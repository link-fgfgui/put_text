# put_text — Windows 后端

在 Windows 本地起个 HTTP 服务，收到请求就把正文直接打进当前前台窗口或指定的控件句柄。

上屏管线移植自 [SayIt](https://github.com/crosswk/SayIt)（`client/src-tauri/src/inject/mod.rs`），C++ 编写，纯 Win32 API 零第三方运行时依赖，单文件绿色 exe 仅约 48KB。

```bat
build.bat
put_text.exe --host 0.0.0.0
```

___

## 上屏原理

`src/inject.cpp` 的 `inject_text()` 会按以下链路逐级回退：

1. **写剪贴板**：写入 UTF-16 文本，重试 5 次（间隔 30ms）。同时打上 `CanIncludeInClipboardHistory`、`CanUploadToCloudClipboard` 与 `ExcludeClipboardContentFromMonitorProcessing` 格式标记，避免污染系统的 `Win+V` 历史记录和云剪贴板。
2. **策略优先级**：
   - **conhost 传统控制台**：发送 `WM_COMMAND` 触发系统控制台内部的「粘贴」指令（`0xFFF1`）。TUI 程序在 raw 模式下会吞掉常规合成按键，走 conhost 原生菜单指令可避免假成功。
   - **原生编辑控件（Edit / RichEdit / Scintilla 等）**：直接投递 `WM_PASTE`（`0x0302`）消息直达目标句柄，无需强抢前台焦点。
   - **兜底：前台抢占 + 合成按键**：通过 `AttachThreadInput` + `VK_F24` 占位键 + `SetForegroundWindow` 唤醒前台并释放残留修饰键，再调用 `SendInput` 模拟 `Ctrl+V`。
   - **最后一搏**：若 `SendInput` 因权限或安全软件拦截返回 `ERROR_ACCESS_DENIED (5)`，补发一次 `WM_PASTE` 尽力抢救。
3. **延迟还原剪贴板**：按键触发后，后台独立线程延迟 400ms 还原剪贴板原本的内容；通过「代数计数器 + 内容哈希复核」机制，确保不会冲掉用户在这一期间手动复制的新内容。
4. **可编辑性检测**：结合 `GetGUIThreadInfo` 光标状态、原生控件类名、Chromium 渲染类名以及进程名启发式综合判断；明确拒绝桌面空白处（`Progman` / `WorkerW` 等）。

___

## 构建

构建脚本使用 [Zig](https://ziglang.org/download/)（0.16+）充当极简 C++ 编译器，省去配置繁重的 MSVC 工具链：

```bat
build.bat
```

产物为 `put_text.exe`，仅动态链接系统自带的 `user32` / `kernel32` / `ws2_32`，无任何额外运行时。

___

## 运行与参数

```bat
put_text.exe [--port 18765] [--host 127.0.0.1]
```

- `--host 0.0.0.0`：监听所有网卡。给手机端网页或局域网设备调用时**必须**加此参数。
- `--port 18765`：监听端口（默认 18765）。

> **安全警示**：服务为了轻量没有任何身份鉴权。加上 `--host 0.0.0.0` 后局域网内任意设备都能往你前台打字，切勿映射到公网。

___

## 接口说明

### 1. 提交上屏 (`POST /paste`)

支持纯文本原文或精细控制的 JSON：

```bash
# 原文直接提交（默认还原剪贴板、打到前台窗口）
curl -s -X POST http://localhost:18765/paste \
  -H 'Content-Type: text/plain' \
  --data-binary '你好，世界'

# JSON 精细控制
curl -s -X POST http://localhost:18765/paste \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好","target_hwnd":123456,"force":true}'
```

JSON 关键字段说明：
- `text`：待输入的文本（UTF-8，必填）。
- `target_hwnd`：目标窗口句柄。默认为 `0`（自动获取当前前台焦点窗口）；传指定非 0 句柄时可实现跨窗口精准注入。
- `force`：是否跳过可编辑性检测。默认为 `false`；遇到未识别的自绘富文本编辑器误判时设为 `true` 强行投递。
- `restore_clipboard`：是否在注入后延迟恢复原剪贴板内容。默认为 `true`。

### 2. URL 参数快捷提交 (`GET /paste?text=...`)

与 `POST /paste` 等价，方便在浏览器地址栏或测试脚本里快速触发：

```bash
curl -s 'http://127.0.0.1:18765/paste?text=%E4%BD%A0%E5%A5%BD'
```

### 3. 健康检查 (`GET /` 或 `GET /health`)

```bash
curl -s http://localhost:18765/
```

返回服务基础状态：`{"ok":true,"service":"put_text",...}`。

### 响应判断与跨域

上屏响应结构体直接镜像 SayIt 的 `InjectResult`：

```json
{"ok":true,"strategy":"wm_paste","reason":null,"detail":"hwnd=... class=scintilla textLen=10","uncertain":false}
```

- `ok`：`true` 表示成功，`false` 表示失败。
- `strategy`：命中的通道（`console_paste` / `wm_paste` / `send_input` / `wm_paste_last_resort` / `clipboard`）。
- `reason`：失败原因（`no_foreground_window` / `not_editable` / `clipboard_blocked` / `send_input_short_write` / `input_blocked_access_denied` / `invalid_utf8` / `empty_text`）。
- **注意**：除 400（参数解析错误）和 404 外，接口均返回 HTTP 200，实际结果必须读取 body 中的 `ok` 字段判定。

#### 跨域注意事项

所有响应均附带 `Access-Control-Allow-Origin: *`。服务**未实现 OPTIONS 预检**，网页请求必须保持简单请求（`POST` + `Content-Type: text/plain`），改用 `application/json` 会触发浏览器 OPTIONS 探测导致 404 失败。

___

## 目录结构

```
build.bat      zig c++ 构建脚本
LICENSE        AGPL-3.0 协议全文
src/
  main.cpp     Winsock HTTP 监听与轻量路由解析
  inject.cpp   核心上屏管线（剪贴板 / WM_PASTE / SendInput / 还原与检测）
  inject.hpp   结构体定义与函数声明
```

___

## FAQ / 局限

**Q: 手机或外部机器无法连接？**  
**A:** Windows 首次运行该服务时，Windows Defender 防火墙通常会弹出网络访问拦截提示，请勾选专用网络放行。同时务必确认启动时带上了 `--host 0.0.0.0`。

**Q: 返回 `not_editable` 且未输入任何文字？**  
**A:** 程序检测到当前前台不是常规可编辑窗口（如桌面空白、任务栏、资源管理器窗口等）。如果是某些使用纯 DirectUI / 自绘框架的特殊富文本编辑器被误判，可以在 JSON 请求中加上 `"force": true` 绕过检测。

**Q: 为什么没有做完整的 UIA (UI Automation) 支持？**  
**A:** SayIt 原生实现中使用了体积庞大的 UIA 接口进行深层节点分析。本 Windows 后端追求轻量与绿色单文件（编译后仅 48KB），因此采用光标检测 + 控件类名 + 进程名启发式算法覆盖常见场景。

**Q: 在没有交互式桌面（如远程无桌面的 SSH 会话）下运行？**  
**A:** 涉及 `SendInput` 和前台切换的 API 要求处于活跃的交互式桌面 session 中，无交互式桌面下仅支持直接向已知 `target_hwnd` 投递 `WM_PASTE` 消息。

___

## 许可

本分支遵循 **AGPL-3.0** 协议，全文见 [LICENSE](LICENSE)。

`src/inject.cpp` 移植自 [SayIt](https://github.com/crosswk/SayIt)（AGPL-3.0）的 `client/src-tauri/src/inject/mod.rs`。修改和分发本分支代码时必须继续遵循 AGPL-3.0 协议。
