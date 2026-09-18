// inject.cpp — 上屏管线实现。逻辑移植自 SayIt inject/mod.rs，改用 Win32 C API。
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "inject.hpp"

// ─── 常量（与 SayIt 一致） ───
enum {
    MSG_WM_PASTE       = 0x0302,
    MSG_WM_COMMAND     = 0x0111,
    ID_CONSOLE_PASTE   = 0xFFF1,  // conhost 系统菜单「粘贴」
    CLIP_CF_UNICODETEXT = 13,
    VK_CTRL            = 0x11,
    VK_V               = 0x56,
    VKCODE_F24         = 0x87,    // 用于满足 SetForegroundWindow 的输入条件
    SHOW_FLAG          = 5,
    WIN_ACCESS_DENIED  = 5,
};

// 每次剪贴板粘贴递增。旧恢复线程看到新一代粘贴后会放弃，避免覆盖新内容。
static std::atomic<uint64_t> g_paste_generation{0};

// ─── UTF-8 <-> UTF-16 ───
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ─── 剪贴板 ───

// 标记本次剪贴板内容不进入 Win+V 历史 / 不上传云剪贴板（与 SayIt 一致）。
// 必须在 OpenClipboard/EmptyClipboard 之后、CloseClipboard 之前调用。结果可忽略。
static void mark_clipboard_history_excluded() {
    struct Spec { const wchar_t* name; UINT value; };
    const Spec specs[] = {
        {L"CanIncludeInClipboardHistory", 0},
        {L"CanUploadToCloudClipboard", 0},
        {L"ExcludeClipboardContentFromMonitorProcessing", 0},
    };
    for (const auto& sp : specs) {
        UINT fmt = RegisterClipboardFormatW(sp.name);
        if (fmt == 0) continue;
        HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, 4);
        if (!hmem) continue;
        void* locked = GlobalLock(hmem);
        if (!locked) { GlobalFree(hmem); continue; }
        std::memcpy(locked, &sp.value, 4);
        GlobalUnlock(hmem);
        SetClipboardData(fmt, hmem); // 失败时按 Windows 惯例不释放（所有物归剪贴板）
    }
}

static bool native_set_clipboard_text(const std::wstring& text, std::string* err) {
    if (!OpenClipboard(nullptr)) {
        if (err) *err = "step=OpenClipboard failed";
        return false;
    }
    bool ok = false;
    do {
        EmptyClipboard();
        // 以 NUL 结尾的 UTF-16，与 SayIt 一致
        std::vector<unsigned short> wide(text.begin(), text.end());
        wide.push_back(0);
        size_t bytes = wide.size() * 2;

        HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!hmem) { if (err) *err = "step=GlobalAlloc failed"; break; }
        void* locked = GlobalLock(hmem);
        if (!locked) { GlobalFree(hmem); if (err) *err = "step=GlobalLock failed"; break; }
        std::memcpy(locked, wide.data(), bytes);
        GlobalUnlock(hmem);

        if (!SetClipboardData(CLIP_CF_UNICODETEXT, hmem)) {
            if (err) *err = "step=SetClipboardData failed";
            break;
        }
        mark_clipboard_history_excluded();
        ok = true;
    } while (false);

    CloseClipboard();
    return ok;
}

static bool set_clipboard_with_retry(const std::wstring& text, int attempts, int delay_ms, std::string* err) {
    for (int i = 0; i < attempts; i++) {
        if (native_set_clipboard_text(text, err)) return true;
        if (i < attempts - 1) Sleep(delay_ms);
    }
    return false;
}

// 读取剪贴板文本（CF_UNICODETEXT）。无文本/非文本时返回空串。
static std::wstring get_clipboard_text_wide() {
    if (!OpenClipboard(nullptr)) return std::wstring();
    std::wstring out;
    HANDLE h = static_cast<HANDLE>(GetClipboardData(CLIP_CF_UNICODETEXT));
    if (h) {
        const wchar_t* locked = static_cast<const wchar_t*>(GlobalLock(h));
        if (locked) {
            size_t len = 0;
            const wchar_t* p = locked;
            while (p[len] != 0 && len < 10000000) len++;
            out.assign(p, len); // 不复制末尾 NUL
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

static bool clear_clipboard() {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = EmptyClipboard();
    CloseClipboard();
    return ok;
}

// 剪贴板还原作业（后台线程执行，400ms 后恢复原内容）
struct RestoreJob {
    uint64_t paste_id;
    std::wstring previous;
    bool has_previous = false;
    std::wstring injected;
};

static DWORD WINAPI restore_thread(LPVOID p) {
    std::unique_ptr<RestoreJob> job(static_cast<RestoreJob*>(p));
    Sleep(400);
    if (g_paste_generation.load(std::memory_order_acquire) != job->paste_id)
        return 0; // 已被新一代粘贴取代
    if (get_clipboard_text_wide() != job->injected)
        return 0; // 剪贴板已被用户/目标程序改写，不再用旧内容覆盖
    if (job->has_previous)
        set_clipboard_with_retry(job->previous, 3, 20, nullptr);
    else
        clear_clipboard();
    return 0;
}

// ─── 窗口/上下文探测 ───

static std::string class_name_of(HWND hwnd) {
    wchar_t buf[256] = {0};
    int n = GetClassNameW(hwnd, buf, 255);
    if (n <= 0) return std::string();
    std::wstring ws(buf, n);
    std::string out;
    for (wchar_t c : ws) out += (char)((c >= L'A' && c <= L'Z') ? (c - L'A' + L'a') : c);
    return out;
}

static std::string process_name_of(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return std::string();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::string();
    wchar_t buf[512] = {0};
    DWORD size = 511;
    DWORD len = size;
    std::wstring path;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        std::wstring ws(buf, len);
        size_t slash = ws.find_last_of(L'\\');
        path = (slash == std::wstring::npos) ? ws : ws.substr(slash + 1);
    }
    CloseHandle(h);
    std::string out;
    for (wchar_t c : path) out += (char)((c >= L'A' && c <= L'Z') ? (c - L'A' + L'a') : c);
    return out;
}

uintptr_t get_focus_hwnd(uintptr_t target_hwnd) {
    HWND target = reinterpret_cast<HWND>(target_hwnd);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(target, &pid);
    if (tid == 0) return 0;
    GUITHREADINFO gti;
    std::memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(tid, &gti)) return 0;
    return reinterpret_cast<uintptr_t>(gti.hwndFocus);
}

ContextInfo capture_context(uintptr_t target_hwnd, uintptr_t focus_hwnd) {
    ContextInfo ctx;
    ctx.hwnd = target_hwnd;
    ctx.focus_hwnd = focus_hwnd;
    HWND target = reinterpret_cast<HWND>(target_hwnd);
    HWND focus = reinterpret_cast<HWND>(focus_hwnd);

    ctx.window_class = class_name_of(target);
    if (focus && focus != target) ctx.focus_class = class_name_of(focus);
    if (ctx.focus_class.empty()) ctx.focus_class = ctx.window_class;
    ctx.process_name = process_name_of(target);

    DWORD tid = GetWindowThreadProcessId(target, nullptr);
    if (tid != 0) {
        GUITHREADINFO gti;
        std::memset(&gti, 0, sizeof(gti));
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(tid, &gti)) {
            ctx.has_caret = (gti.hwndCaret != nullptr) || (gti.flags & GUI_CARETBLINKING) != 0;
            if (ctx.focus_hwnd == 0 && gti.hwndFocus != nullptr) {
                ctx.focus_hwnd = reinterpret_cast<uintptr_t>(gti.hwndFocus);
                ctx.focus_class = class_name_of(gti.hwndFocus);
                if (ctx.focus_class.empty()) ctx.focus_class = ctx.window_class;
            }
        }
    }
    return ctx;
}

// ─── 可编辑性检测（移植 is_likely_editable_pub，不含 UIA） ───
static bool string_contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool is_likely_editable(const ContextInfo& ctx) {
    const std::string& fc = ctx.focus_class;
    const std::string& wc = ctx.window_class;
    const std::string& proc = ctx.process_name;

    // Windows 桌面(explorer)的图标列表不是文本输入目标，必须明确拒绝
    static const char* explorer_desktop_classes[] = {
        "progman", "workerw", "shelldll_defview", "syslistview32"};
    if (string_contains(proc, "explorer")) {
        for (const char* cls : explorer_desktop_classes)
            if (fc == cls || wc == cls) return false;
    }

    if (ctx.has_caret) return true;

    // 原生 Win32 可编辑控件
    static const char* native_editable_classes[] = {
        "edit", "richedit", "richedit20w", "richedit50w",
        "scintilla", "texteditorsid", "_wwg"};  // _wwg = Office Word 编辑器控件
    for (const char* cls : native_editable_classes)
        if (string_contains(fc, cls) || string_contains(wc, cls)) return true;

    // Chromium 类窗口（含 AI IDE 等基于 CEF 的产品通常也以 Chrome 组件承载体为主）
    static const char* chromium_classes[] = {
        "chrome_widgetwin_1", "chrome_renderwidgethostview", "intermediate d3d window"};
    static const char* editable_procs[] = {
        "notepad", "winword", "excel", "powerpnt", "outlook",
        "code", "devenv", "idea64",
        "trae", "cursor", "windsurf", "kiro",
        "chrome", "msedge", "firefox", "opera", "brave",
        "teams", "wechat", "dingtalk", "slack",
        "windowsterminal", "cmd", "powershell",
        "mobaxterm", "putty", "securecrt", "xshell"};
    bool is_chromium = false;
    for (const char* cls : chromium_classes)
        if (string_contains(fc, cls) || string_contains(wc, cls)) { is_chromium = true; break; }
    if (is_chromium) return true; // Chromium 下不做悲观判定：Ctrl+V 打错也无害

    for (const char* p : editable_procs)
        if (string_contains(proc, p)) return true;

    return false;
}

// ─── 前台抢占辅助（移植 force_foreground） ───
static bool force_foreground(HWND target) {
    DWORD my_tid = GetCurrentThreadId();
    DWORD target_tid = GetWindowThreadProcessId(target, nullptr);
    bool attached = false;
    if (target_tid != 0 && target_tid != my_tid)
        attached = AttachThreadInput(my_tid, target_tid, TRUE) != 0;

    // 用无害键 VK_F24 满足 SetForegroundWindow 的"调用者必须收到过输入"限制
    INPUT f24 = {0};
    f24.type = INPUT_KEYBOARD;
    f24.ki.wVk = VKCODE_F24;
    SendInput(1, &f24, sizeof(INPUT));
    f24.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &f24, sizeof(INPUT));

    ShowWindow(target, SHOW_FLAG);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    Sleep(50);
    if (GetForegroundWindow() != target) {
        SetForegroundWindow(target);
        Sleep(30);
    }
    if (attached) AttachThreadInput(my_tid, target_tid, FALSE);
    return GetForegroundWindow() == target;
}

static void attach_and_set_focus(HWND target, HWND focus) {
    DWORD my_tid = GetCurrentThreadId();
    DWORD target_tid = GetWindowThreadProcessId(target, nullptr);
    bool attached = false;
    if (target_tid != 0 && target_tid != my_tid)
        attached = AttachThreadInput(my_tid, target_tid, TRUE) != 0;
    SetFocus(focus);
    Sleep(10);
    if (attached) AttachThreadInput(my_tid, target_tid, FALSE);
}

// ─── 释放卡住的修饰键（Alt/Shift/Ctrl/Win） ───
static void release_modifiers() {
    static const WORD mods[] = {0xA4, 0xA5, 0xA0, 0xA1, 0xA2, 0xA3};
    std::vector<INPUT> inputs;
    inputs.reserve(6);
    for (WORD vk : mods) {
        INPUT in = {0};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(in);
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

static BOOL send_message_nowait(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, PDWORD_PTR result) {
    return SendMessageTimeoutW(hwnd, msg, wp, lp, SMTO_ABORTIFHUNG, 2000 /*2s*/, result);
}

// ─── 核心注入流程（移植 do_inject） ───
static InjectResult do_inject(HWND target, HWND focus, const std::wstring& text, bool restore_clipboard) {
    InjectResult out;
    uint64_t paste_id = ++g_paste_generation;

    // 保存原剪贴板文本（写入新内容之前）
    std::wstring previous_text;
    bool has_previous = false;
    if (restore_clipboard) {
        previous_text = get_clipboard_text_wide();
        has_previous = !previous_text.empty();
    }

    // Step 1: 写入剪贴板（5 次重试，30ms 间隔 —— 应对安全软件剪贴板保护竞争）
    std::string clip_err;
    if (!set_clipboard_with_retry(text, 5, 30, &clip_err)) {
        out.ok = false;
        out.strategy = "clipboard";
        out.reason = "clipboard_blocked";
        out.detail = "pasteId=" + std::to_string(paste_id) + " " + clip_err;
        return out;
    }

    // 剪贴板还原守卫：函数返回时起算 400ms，由独立线程恢复
    auto* job = new RestoreJob();
    job->paste_id = paste_id;
    job->injected = text;
    job->previous = previous_text;
    job->has_previous = has_previous;
    HANDLE restore_handle = CreateThread(nullptr, 0, restore_thread, job, 0, nullptr);
    if (restore_handle) CloseHandle(restore_handle);
    else { delete job; job = nullptr; } // 线程创建失败时放弃还原（写出的内容保留）

    // Step 1.5: 经典控制台（conhost）——用系统菜单「粘贴」命令而非模拟 Ctrl+V。
    // raw 模式下合成按键会被 TUI 程序吞掉造成"假成功"，WM_COMMAND 则不经过按键流。
    HWND paste_target = (focus != nullptr) ? focus : target;
    std::string target_class = class_name_of(target);
    if (string_contains(target_class, "consolewindowclass")) {
        force_foreground(target);
        DWORD_PTR result = 0;
        if (send_message_nowait(target, MSG_WM_COMMAND, ID_CONSOLE_PASTE, 0, &result)) {
            out.ok = true;
            out.strategy = "console_paste";
            out.detail = "hwnd=" + std::to_string((uintptr_t)target) +
                         " class=" + target_class + " textLen=" + std::to_string(text.size());
            return out;
        }
        // 失败则继续走通用流程
    }

    // Step 2: WM_PASTE 直发焦点控件（跨进程，目标无需前台）。原生编辑控件可靠。
    std::string focus_class = class_name_of(paste_target);
    bool try_wm_paste = string_contains(focus_class, "edit") ||
                        string_contains(focus_class, "richedit") ||
                        string_contains(focus_class, "scintilla");
    if (try_wm_paste) {
        DWORD_PTR result = 0;
        if (send_message_nowait(paste_target, MSG_WM_PASTE, 0, 0, &result)) {
            out.ok = true;
            out.strategy = "wm_paste";
            out.detail = "hwnd=" + std::to_string((uintptr_t)paste_target) +
                         " class=" + focus_class + " textLen=" + std::to_string(text.size());
            return out;
        }
    }

    // Step 3: 兜底——前台抢占 + SendInput 模拟 Ctrl+V
    bool fg_ok = force_foreground(target);
    release_modifiers();
    Sleep(15);

    if (focus != target && focus != nullptr)
        attach_and_set_focus(target, focus);

    INPUT keys[4] = {0};
    for (auto& k : keys) k.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_CTRL;                    // Ctrl down
    keys[1].ki.wVk = VK_V;                       // V down
    keys[2].ki.wVk = VK_V;                       // V up
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_CTRL;                    // Ctrl up
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, keys, sizeof(INPUT));
    // 立刻抓 GetLastError（后续调用会冲掉它）。err=5 => UIPI 权限拦截
    DWORD last_err = GetLastError();

    Sleep(10);
    release_modifiers();

    std::string detail = "sent=" + std::to_string(sent) + " err=" + std::to_string(last_err) +
                         " class=" + focus_class + " target=" + std::to_string((uintptr_t)target) +
                         " focus=" + std::to_string((uintptr_t)focus) +
                         " textLen=" + std::to_string(text.size()) + " fgOk=" + (fg_ok ? "1" : "0");

    if (sent >= 4) {
        out.ok = true;
        out.strategy = "send_input";
        out.detail = detail;
        return out;
    }

    // Step 4: SendInput 被系统拦（err=5 UIPI / 安全软件防键盘模拟），最后一搏 WM_PASTE
    if (!try_wm_paste) {
        DWORD_PTR result = 0;
        if (send_message_nowait(paste_target, MSG_WM_PASTE, 0, 0, &result)) {
            out.ok = true;
            out.strategy = "wm_paste_last_resort";
            out.detail = detail;
            return out;
        }
    }

    out.ok = false;
    out.strategy = "send_input";
    out.reason = (sent == 0 && last_err == WIN_ACCESS_DENIED) ? "input_blocked_access_denied"
                                                              : "send_input_short_write";
    out.detail = detail;
    return out;
}

// ─── 入口 ───
InjectResult inject_text(const InjectRequest& req) {
    InjectResult out;
    HWND target = reinterpret_cast<HWND>(req.target_hwnd);
    if (target == nullptr) target = GetForegroundWindow();
    if (target == nullptr) {
        out.reason = "no_foreground_window";
        return out;
    }

    HWND focus = reinterpret_cast<HWND>(req.focus_hwnd);
    if (focus == nullptr) focus = reinterpret_cast<HWND>(get_focus_hwnd(reinterpret_cast<uintptr_t>(target)));
    if (focus == nullptr) focus = target;

    if (!req.force) {
        ContextInfo ctx = capture_context(reinterpret_cast<uintptr_t>(target),
                                          reinterpret_cast<uintptr_t>(focus));
        if (!is_likely_editable(ctx)) {
            out.ok = false;
            out.strategy = "overlay_fallback";
            out.reason = "not_editable";
            out.detail = "class=" + ctx.window_class + " focusClass=" + ctx.focus_class +
                         " hasCaret=" + (ctx.has_caret ? "1" : "0") + " process=" + ctx.process_name;
            return out;
        }
    }
    return do_inject(target, focus, req.text, req.restore_clipboard);
}