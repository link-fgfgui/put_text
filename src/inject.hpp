#pragma once
// inject.hpp — 移植自 SayIt (client/src-tauri/src/inject/mod.rs) 的 Windows 文本上屏管线。
// 策略优先级（与 SayIt 一致）:
//   1. 控制台窗口(conhost) 使用系统菜单「粘贴」命令 (WM_COMMAND + ID_CONSOLE_PASTE)
//   2. 原生编辑控件直接投递 WM_PASTE 消息（跨进程直达，无需前台）
//   3. 前台抢占 + SendInput 模拟 Ctrl+V（兜底）
//   4. SendInput 被系统拦截时最后一搏再试 WM_PASTE
// 辅助机制: 剪贴板内容备份与延时 400ms 还原（带代数保护）、
//           排除剪贴板历史(Win+V)/云剪贴板、可编辑性检测、修饰键释放。

#include <cstdint>
#include <string>

struct ContextInfo {
    uintptr_t hwnd = 0;        // 目标窗口句柄
    uintptr_t focus_hwnd = 0;  // 焦点子控件句柄（0 表示未知）
    std::string window_class;  // 目标窗口类名（小写）
    std::string focus_class;   // 焦点控件类名（小写）
    std::string process_name;  // 目标进程名（小写）
    bool has_caret = false;    // GUI 线程是否显示着文本光标
};

struct InjectRequest {
    std::wstring text;            // 要上屏的文本（UTF-16）
    bool restore_clipboard = true;  // 上屏后是否还原剪贴板
    uintptr_t target_hwnd = 0;    // 目标窗口句柄，0 = 取前台窗口
    uintptr_t focus_hwnd = 0;     // 焦点控件句柄，0 = 自动探测
    bool force = false;           // true = 跳过可编辑性检查
};

struct InjectResult {
    bool ok = false;          // 是否判定成功
    std::string strategy;     // 使用的策略: wm_paste / send_input / console_paste / overlay_fallback ...
    std::string reason;       // 失败原因（成功时为空）
    std::string detail;       // 附加细节（句柄、类名、错误码等）
    bool uncertain = false;   // 注入结果是否无法确认（与 SayIt 语义一致，当前恒为 false）
};

// 探测 target 窗口的焦点子控件（GetGUIThreadInfo.hwndFocus）
uintptr_t get_focus_hwnd(uintptr_t target_hwnd);

// 抓取目标窗口上下文（类名、进程名、光标状态）
ContextInfo capture_context(uintptr_t target_hwnd, uintptr_t focus_hwnd);

// 可编辑性启发式检测（移植 is_likely_editable_pub，去掉 UIA 部分）
bool is_likely_editable(const ContextInfo& ctx);

// 上屏入口
InjectResult inject_text(const InjectRequest& req);