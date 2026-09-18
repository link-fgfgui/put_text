// main.cpp — HTTP POST 上屏服务。
// 用法: put_text.exe [--port 8787] [--host 127.0.0.1] [--no-restore-clipboard] [--verbose]
// API（与 linux 分支统一；/paste 与 /health 保留为旧别名，POST / 与 linux 一致）:
//   GET  /                           → 状态 {"ok":true,"service":"put_text",...}
//   POST /text                       → 执行上屏（POST / 与 POST /paste 等价）
//        Content-Type: application/json
//        {"text":"...","restore_clipboard":true,"target_hwnd":0,"focus_hwnd":0,"force":false}
//        Content-Type: text/plain    → 请求体原文即 text
//        Content-Type: application/x-www-form-urlencoded → text=/content=/body= 键值
//   响应: 上屏成功 HTTP 200、失败 HTTP 502（与 linux 一致），body 均为
//        {"ok":bool,"strategy":...|null,"reason":...|null,"detail":...|null,"uncertain":bool}
//   GET  /text?text=...              → 执行上屏（等价 POST，方便地址栏/脚本直接测）
//        无 text/content/body 参数时等同 GET / 返回状态
//
// 跨源：所有响应都带 Access-Control-Allow-Origin: *，浏览器页面可以直接 fetch
//       读结果。页面只用简单请求（POST + Content-Type: text/plain），
//       不触发 OPTIONS 预检，所以这里不需要处理 OPTIONS。
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "inject.hpp"

// CLI 全局状态：--no-restore-clipboard 时默认不还原剪贴板；--verbose 时逐请求打日志
static bool g_restore_default = true;
static bool g_verbose = false;

// ─── 极简 JSON（足够覆盖本服务收发） ───
enum JsonType { J_NULL, J_BOOL, J_NUM, J_STR, J_OBJ, J_ARR };
struct JsonVal {
    JsonType t = J_NULL;
    bool b = false;
    long long num = 0;
    std::string str;
    std::map<std::string, JsonVal> obj;
    std::vector<JsonVal> arr;
};

static void json_skip_ws(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
}

static void utf8_push(unsigned cp, std::string& out) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// 解析 "..."，*p 指向开引号；结束后指向引号之后的字符。返回 false 表示用了代理对。
static void json_string(const char*& p, std::string& out) {
    p++; // skip "
    out.clear();
    unsigned prev_high = 0; // 未配对的 \uD800-\uDBFF
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            char c = *p;
            switch (c) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p[1 + i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp += h - '0';
                        else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                    }
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) { prev_high = cp; }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        unsigned full = 0x10000 + ((prev_high - 0xD800) << 10) + (cp - 0xDC00);
                        utf8_push(full, out);
                        prev_high = 0;
                    } else {
                        utf8_push(cp, out);
                        prev_high = 0;
                    }
                    break;
                }
                default: out += '\\'; break;
            }
        } else {
            out += *p;
        }
        if (*p) p++;
    }
    if (*p == '"') p++;
}

static JsonVal json_val(const char*& p) {
    json_skip_ws(p);
    JsonVal v;
    if (*p == '"') { v.t = J_STR; json_string(p, v.str); }
    else if (*p == 't') { p += 4; v.t = J_BOOL; v.b = true; }
    else if (*p == 'f') { p += 5; v.t = J_BOOL; v.b = false; }
    else if (*p == 'n') { p += 4; v.t = J_NULL; }
    else if (*p == '{') {
        p++;
        v.t = J_OBJ;
        for (;;) {
            json_skip_ws(p);
            if (*p == '}') { p++; break; }
            std::string key;
            if (*p == '"') json_string(p, key);
            json_skip_ws(p);
            if (*p == ':') p++;
            JsonVal sub = json_val(p);
            v.obj[key] = sub;
            json_skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            break;
        }
    } else if (*p == '[') {
        p++;
        v.t = J_ARR;
        for (;;) {
            json_skip_ws(p);
            if (*p == ']') { p++; break; }
            v.arr.push_back(json_val(p));
            json_skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; break; }
            break;
        }
    } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
        v.num = strtoll(p, (char**)&p, 10);
        v.t = J_NUM;
    }
    return v;
}

static bool json_parse(const std::string& s, JsonVal& out) {
    const char* p = s.c_str();
    out = json_val(p);
    json_skip_ws(p);
    return *p == '\0';
}

static bool json_bool_of(const JsonVal& v, bool def) { return v.t == J_BOOL ? v.b : def; }
static long long json_num_of(const JsonVal& v) { return v.t == J_NUM ? v.num : 0; }

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// 可空字符串字段: 空 → null
static std::string json_nullable(const std::string& s) {
    return s.empty() ? std::string("null") : "\"" + json_escape(s) + "\"";
}

// ─── 极简 HTTP/1.1 客户端解析 ───
struct HttpRequest {
    std::string method, path, query;
    std::map<std::string, std::string> headers;
    std::string body;
};

static bool http_parse(const std::string& raw, HttpRequest& req) {
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;
    size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string request_line = raw.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return false;
    req.method = request_line.substr(0, sp1);
    req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    // 拆出 query：GET /paste 把正文放在 ?text= 里
    size_t q = req.path.find('?');
    if (q != std::string::npos) {
        req.query = req.path.substr(q + 1);
        req.path = req.path.substr(0, q);
    }

    size_t pos = line_end + 2;
    while (pos < header_end) {
        size_t e = raw.find("\r\n", pos);
        if (e == std::string::npos || e > header_end) break;
        std::string line = raw.substr(pos, e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            for (auto& c : k) if (c >= 'A' && c <= 'Z') c += 32; // 小写
            req.headers[k] = v;
        }
        pos = e + 2;
    }
    req.body = raw.substr(header_end + 4);
    return true;
}

// ─── query 解析 ───
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX 还原成原始字节，'+' 还原成空格（与浏览器 encodeURIComponent 之外的表单编码一致）
static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]), lo = hex_val(s[i + 2]);
            if (hi < 0 || lo < 0) {
                out += s[i];
            } else {
                out += (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string query_param(const std::string& query, const char* key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        size_t end = (amp == std::string::npos) ? query.size() : amp;
        std::string pair = query.substr(pos, end - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, key) == 0) {
            return url_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

// 参数名是否精确出现（含无值的裸参数，如 ?text=）
static bool query_has_param(const std::string& query, const char* key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        size_t end = (amp == std::string::npos) ? query.size() : amp;
        std::string pair = query.substr(pos, end - pos);
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            if (pair == key) return true;
        } else if (pair.compare(0, eq, key) == 0) {
            return true;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// ─── 上屏 ───

// UTF-8 正文 → UTF-16 → 上屏。POST 与 GET 共用。
static InjectResult commit_text(const std::string& text, bool restore_clipboard,
                                uintptr_t target_hwnd, uintptr_t focus_hwnd, bool force) {
    InjectResult out;
    std::wstring wtext;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) {
        out.reason = "invalid_utf8";
        return out;
    }
    wtext.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &wtext[0], n);

    InjectRequest ireq;
    ireq.text = wtext;
    ireq.restore_clipboard = restore_clipboard;
    ireq.target_hwnd = target_hwnd;
    ireq.focus_hwnd = focus_hwnd;
    ireq.force = force;
    return inject_text(ireq);
}

static std::string result_to_json(const InjectResult& r) {
    return "{\"ok\":" + std::string(r.ok ? "true" : "false") +
           ",\"strategy\":" + json_nullable(r.strategy) +
           ",\"reason\":" + json_nullable(r.reason) +
           ",\"detail\":" + json_nullable(r.detail) +
           ",\"uncertain\":" + (r.uncertain ? "true" : "false") + "}";
}

// POST /text 的可选参数（仅 JSON 对象体里有效）
struct PasteOptions {
    bool restore_clipboard = true;
    uintptr_t target_hwnd = 0;
    uintptr_t focus_hwnd = 0;
    bool force = false;
};

static std::string empty_text_json() {
    return "{\"ok\":true,\"strategy\":\"empty_text\",\"reason\":null,\"detail\":\"nothing to commit\",\"uncertain\":false}";
}

static std::string error_json(const char* reason) {
    return std::string("{\"ok\":false,\"strategy\":null,\"reason\":\"") + reason +
           "\",\"detail\":null,\"uncertain\":false}";
}

// 按 Content-Type 提取正文（与 linux 分支 put_text.py 的 extract_text 对齐）：
//   application/json               → 字符串整体即文本，对象取 text/content/body 键（失败 400）
//   application/x-www-form-urlencoded → 取 text=/content=/body= 键值，无键时整体即文本
//   其余（text/plain 等）          → 请求体原文即文本
// 返回 true 表示已拿到 text；false 时 body 里已是 400 错误响应。
static bool extract_text(const HttpRequest& req, std::string& text, PasteOptions& opt, std::string& body) {
    std::string ctype = req.headers.count("content-type") ? req.headers.at("content-type") : "";
    size_t semi = ctype.find(';');
    if (semi != std::string::npos) ctype = ctype.substr(0, semi);
    for (auto& c : ctype) if (c >= 'A' && c <= 'Z') c += 32;
    while (!ctype.empty() && ctype[0] == ' ') ctype.erase(0, 1);
    while (!ctype.empty() && ctype.back() == ' ') ctype.pop_back();

    if (ctype == "application/json") {
        JsonVal root;
        if (!json_parse(req.body, root)) {
            body = error_json("invalid_json");
            return false;
        }
        if (root.t == J_STR) {
            text = root.str;
            return true;
        }
        if (root.t == J_OBJ) {
            bool got = false;
            for (const char* key : {"text", "content", "body"}) {
                auto it = root.obj.find(key);
                if (it != root.obj.end() && it->second.t == J_STR) { text = it->second.str; got = true; break; }
            }
            if (!got) {
                body = error_json("json_missing_text_field");
                return false;
            }
            auto f_bool = [&](const char* key, bool def) {
                auto f = root.obj.find(key);
                if (f == root.obj.end()) return def;
                return json_bool_of(f->second, def);
            };
            auto f_num = [&](const char* key) {
                auto f = root.obj.find(key);
                if (f == root.obj.end()) return 0ll;
                return json_num_of(f->second);
            };
            opt.restore_clipboard = f_bool("restore_clipboard", opt.restore_clipboard);
            opt.target_hwnd = (uintptr_t)f_num("target_hwnd");
            opt.focus_hwnd = (uintptr_t)f_num("focus_hwnd");
            opt.force = f_bool("force", false);
            return true;
        }
        body = error_json("json_body_not_object_or_string");
        return false;
    }
    if (ctype == "application/x-www-form-urlencoded") {
        // curl --data 默认用这个类型；键值形式取 text/content/body，无键时整体视为纯文本
        size_t pos = 0;
        while (pos <= req.body.size()) {
            size_t amp = req.body.find('&', pos);
            size_t end = (amp == std::string::npos) ? req.body.size() : amp;
            std::string pair = req.body.substr(pos, end - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                for (auto& c : k) if (c >= 'A' && c <= 'Z') c += 32;
                if (k == "text" || k == "content" || k == "body") {
                    text = url_decode(pair.substr(eq + 1));
                    return true;
                }
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        text = req.body;
        return true;
    }
    text = req.body; // 原文即文本
    return true;
}

// 执行上屏并写响应；code 成功 200、失败 502（与 linux 分支一致）。
static void handle_text(const HttpRequest& req, int& code, std::string& body, bool restore_default) {
    PasteOptions opt;
    opt.restore_clipboard = restore_default;
    std::string text;
    if (!extract_text(req, text, opt, body)) {
        code = 400;
        return;
    }
    if (text.empty()) {
        body = empty_text_json(); // 空文本视为「无事可做」的成功（与 linux 一致）
        code = 200;
        return;
    }
    InjectResult r = commit_text(text, opt.restore_clipboard, opt.target_hwnd, opt.focus_hwnd, opt.force);
    body = result_to_json(r);
    code = r.ok ? 200 : 502;
}

// GET /text?text=...（或 content=/body=）—— 等价于 POST，方便浏览器地址栏和脚本直接测；
// 无这些参数时等同 GET / 返回状态。
static void handle_get_text(const HttpRequest& req, int& code, std::string& body) {
    std::string text;
    bool has = false;
    for (const char* key : {"text", "content", "body"}) {
        if (query_has_param(req.query, key)) {
            text = query_param(req.query, key);
            has = true;
            break;
        }
    }
    if (!has) {
        body = "{\"ok\":true,\"service\":\"put_text\",\"desc\":\"POST /text {\\\"text\\\":\\\"...\\\"} to insert text into the focused window\"}";
        code = 200;
        return;
    }
    if (text.empty()) {
        body = empty_text_json();
        code = 200;
        return;
    }
    InjectResult r = commit_text(text, true, 0, 0, false);
    body = result_to_json(r);
    code = r.ok ? 200 : 502;
}

static void send_http(SOCKET s, int code, const std::string& body) {
    const char* reason = (code == 200) ? "OK"
                       : (code == 400) ? "Bad Request"
                       : (code == 502) ? "Bad Gateway" : "Not Found";
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
                       "Content-Type: application/json; charset=utf-8\r\n"
                       // 网页可能来自别的源（手机上的 file:// 或另一个 http 服务），
                       // 跨源读响应需要这个头。前端只用简单请求，不会触发预检。
                       "Access-Control-Allow-Origin: *\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Cache-Control: no-store\r\n"
                       "Connection: close\r\n\r\n";
    std::string resp = head + body;
    send(s, resp.data(), (int)resp.size(), 0);
}

static DWORD WINAPI worker(LPVOID p) {
    SOCKET s = (SOCKET)(intptr_t)p;
    std::string raw;
    char buf[8192];
    int n;
    long long content_length = -1;

    // 读完请求头
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, n);
        size_t header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            HttpRequest req;
            if (http_parse(raw, req)) {
                auto it = req.headers.find("content-length");
                if (it != req.headers.end()) content_length = strtoll(it->second.c_str(), nullptr, 10);
            }
            if (content_length <= 0) break;
            // 继续读满 body
            size_t has = raw.size() - (header_end + 4);
            while (has < (size_t)content_length) {
                n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) break;
                raw.append(buf, n);
                has += n;
            }
            break;
        }
        if (raw.size() > 16 * 1024 * 1024) break; // 防御上限
    }

    HttpRequest req;
    std::string body;
    int code = 200;
    if (!http_parse(raw, req)) {
        code = 400;
        body = "{\"ok\":false,\"reason\":\"bad_request\"}";
    } else if (req.method == "GET" && (req.path == "/" || req.path == "/health" ||
                                       req.path == "/text" || req.path == "/paste")) {
        handle_get_text(req, code, body);
    } else if (req.method == "POST" && (req.path == "/text" || req.path == "/paste" || req.path == "/")) {
        handle_text(req, code, body, g_restore_default);
    } else {
        code = 404;
        body = "{\"ok\":false,\"reason\":\"not_found\"}";
    }
    if (g_verbose) {
        printf("[req] %s %s -> %d %s\n", req.method.c_str(), req.path.c_str(), code, body.c_str());
    }

    send_http(s, code, body);
    closesocket(s);
    return 0;
}

int main(int argc, char* argv[]) {
    int port = 8787;
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--port" || a == "-p") {
            if (i + 1 < argc) port = atoi(argv[++i]);
        } else if (a == "--host" || a == "-h") {
            if (i + 1 < argc) host = argv[++i];
        } else if (a == "--no-restore-clipboard") {
            g_restore_default = false;
        } else if (a == "--verbose" || a == "-v") {
            g_verbose = true;
        } else if (a == "--help" || a == "/?") {
            printf("put_text - HTTP text-insertion (Type-in) service\n"
                   "usage: put_text.exe [--port 8787] [--host 127.0.0.1] [--no-restore-clipboard] [--verbose]\n"
                   "POST /text   plain: body is the text\n"
                   "             JSON: {\"text\":\"hi\",\"restore_clipboard\":true,\"target_hwnd\":0,\"focus_hwnd\":0,\"force\":false}\n"
                   "             form: text=hi\n"
                   "GET  /text?text=hi   same as POST\n"
                   "GET  /              status\n"
                   "--no-restore-clipboard   leave the text on the clipboard after pasting\n");
            return 0;
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[error] WSAStartup failed\n");
        return 1;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        printf("[error] socket failed\n");
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[error] bind %s:%d failed (%d)\n", host.c_str(), port, WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }
    if (listen(listen_sock, 8) == SOCKET_ERROR) {
        printf("[error] listen failed\n");
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    printf("[ok] put_text listening on http://%s:%d  (POST /text)\n", host.c_str(), port);
    for (;;) {
        SOCKET client = accept(listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEINTR) continue;
            printf("[error] accept failed (%d)\n", WSAGetLastError());
            continue;
        }
        HANDLE h = CreateThread(nullptr, 0, worker, (LPVOID)(intptr_t)client, 0, nullptr);
        if (h) CloseHandle(h);
        else closesocket(client);
    }
}