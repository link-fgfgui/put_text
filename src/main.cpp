// main.cpp — HTTP POST 上屏服务。
// 用法: put_text.exe [--port 18765] [--host 127.0.0.1]
// API:
//   GET  /health                      → {"ok":true,"service":"put_text"}
//   POST /paste                       → 执行上屏
//        Content-Type: application/json
//        {"text":"...", "restore_clipboard":true, "target_hwnd":0, "focus_hwnd":0, "force":false}
//        Content-Type: text/plain     → 请求体原文即 text
//   响应 (HTTP 200/400): {"ok":bool,"strategy":...|null,"reason":...|null,
//                        "detail":...|null,"uncertain":bool}
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

static std::string json_string_of(const JsonVal& v) { return v.t == J_STR ? v.str : std::string(); }
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
    std::string method, path;
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
    // 去掉 query
    size_t q = req.path.find('?');
    if (q != std::string::npos) req.path = req.path.substr(0, q);

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

// ─── 上屏请求处理 ───
static std::string handle_paste(const HttpRequest& req) {
    std::string text;
    std::string ctype = req.headers.count("content-type") ? req.headers.at("content-type") : "";

    JsonVal root;
    bool has_json = false;
    if (ctype.find("text/plain") != std::string::npos) {
        text = req.body; // 原文即文本
    } else if (json_parse(req.body, root) && root.t == J_OBJ) {
        has_json = true;
        auto it = root.obj.find("text");
        if (it != root.obj.end()) text = json_string_of(it->second);
    } else {
        // 兼容性回退：尝试整体当作 JSON，失败则视为纯文本
        text = req.body;
    }

    if (text.empty()) {
        return "{\"ok\":false,\"strategy\":null,\"reason\":\"empty_text\",\"detail\":null,\"uncertain\":false}";
    }

    // 以正确 UTF-16 编码构造文本
    std::wstring wtext;
    {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
        if (n <= 0) {
            return "{\"ok\":false,\"strategy\":null,\"reason\":\"invalid_utf8\",\"detail\":null,\"uncertain\":false}";
        }
        wtext.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &wtext[0], n);
    }

    InjectRequest ireq;
    ireq.text = wtext;
    if (has_json) {
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
        ireq.restore_clipboard = f_bool("restore_clipboard", true);
        ireq.target_hwnd = (uintptr_t)f_num("target_hwnd");
        ireq.focus_hwnd = (uintptr_t)f_num("focus_hwnd");
        ireq.force = f_bool("force", false);
    } else {
        // 纯文本模式（text/plain）:剪贴板还原默认开，目标为前台窗口
        ireq.restore_clipboard = true;
    }

    InjectResult r = inject_text(ireq);
    std::string body = "{\"ok\":" + std::string(r.ok ? "true" : "false") +
                       ",\"strategy\":" + json_nullable(r.strategy) +
                       ",\"reason\":" + json_nullable(r.reason) +
                       ",\"detail\":" + json_nullable(r.detail) +
                       ",\"uncertain\":" + (r.uncertain ? "true" : "false") + "}";
    return body;
}

static void send_http(SOCKET s, int code, const std::string& body) {
    const char* reason = (code == 200) ? "OK" : (code == 400 ? "Bad Request" : "Not Found");
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
                       "Content-Type: application/json; charset=utf-8\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
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
    } else if (req.method == "GET" && (req.path == "/" || req.path == "/health")) {
        body = "{\"ok\":true,\"service\":\"put_text\",\"desc\":\"POST /paste {\\\"text\\\":\\\"...\\\"} to insert text into the focused window\"}";
    } else if (req.method == "POST" && req.path == "/paste") {
        body = handle_paste(req);
    } else {
        code = 404;
        body = "{\"ok\":false,\"reason\":\"not_found\"}";
    }

    send_http(s, code, body);
    closesocket(s);
    return 0;
}

int main(int argc, char* argv[]) {
    int port = 18765;
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--port" || a == "-p") {
            if (i + 1 < argc) port = atoi(argv[++i]);
        } else if (a == "--host" || a == "-h") {
            if (i + 1 < argc) host = argv[++i];
        } else if (a == "--help" || a == "/?") {
            printf("put_text - HTTP POST text-insertion (Type-in) service\n"
                   "usage: put_text.exe [--port 18765] [--host 127.0.0.1]\n"
                   "POST /paste  JSON: {\"text\":\"hi\",\"restore_clipboard\":true,\"target_hwnd\":0,\"focus_hwnd\":0,\"force\":false}\n"
                   "GET  /health\n");
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

    printf("[ok] put_text listening on http://%s:%d  (POST /paste)\n", host.c_str(), port);
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