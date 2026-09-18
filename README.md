# put_text — Windows 后端

接收 HTTP POST，按 SayIt 的上屏原理把文本输入到当前聚焦窗口或指定窗口的
Windows 服务。零第三方依赖，单个 exe。

上屏管线移植自 [SayIt](https://github.com/crosswk/SayIt)
（`client/src-tauri/src/inject/mod.rs`）。

```bat
build.bat
put_text.exe          # 默认监听 127.0.0.1:8787
```

## 上屏原理

`src/inject.cpp:453` 的 `inject_text()` 逐级回退：

1. **写剪贴板** —— `CF_UNICODETEXT`（UTF-16），5 次重试 × 30ms；同时写入
   `CanIncludeInClipboardHistory` / `CanUploadToCloudClipboard` /
   `ExcludeClipboardContentFromMonitorProcessing` 标记，
   避免污染 Win+V 历史与云剪贴板。
2. **策略优先级**：
   - **conhost 控制台** —— `WM_COMMAND` + 系统菜单「粘贴」(0xFFF1)。
     raw 模式（TUI 程序）下合成按键会被吞掉造成假成功，
     而这个命令由 conhost 自身处理、不经过按键流。
   - **原生编辑控件**（`Edit` / `RichEdit` / `Scintilla`）—— 直接投递
     `WM_PASTE` 消息（0x0302），跨进程直达目标句柄，无需目标处于前台。
   - **兜底：前台抢占 + 合成按键** —— `AttachThreadInput` + `VK_F24` 占位键
     + `SetForegroundWindow` → 释放卡住的修饰键 → `SendInput` 模拟 `Ctrl+V`。
   - **最后一搏** —— `SendInput` 被安全软件 / UIPI 拦截
     （`ERROR_ACCESS_DENIED=5`）时再补一次 `WM_PASTE`。
3. **剪贴板还原** —— 粘贴触发后由独立线程延迟 400ms 恢复原剪贴板内容；
   通过「代数计数器 + 内容复核」防止覆盖用户随后复制的内容或下一次粘贴。
4. **可编辑性检测** —— `GetGUIThreadInfo` 光标 → 原生可编辑控件类 →
   Chromium 窗口类 → 进程名启发式；明确拒绝 explorer 桌面
   （`Progman` / `WorkerW` 等）。

## 构建

需要 [Zig 0.16+](https://ziglang.org/download/)（充当 C++ 编译器，
零配置交叉链接 Windows 系统库）。

```bat
build.bat
```

产物：`put_text.exe`（约 48KB，仅依赖系统库 `user32` / `kernel32` / `ws2_32`）。

## 使用

```bat
put_text.exe [--port 8787] [--host 127.0.0.1] [--no-restore-clipboard] [--verbose]
```

`--no-restore-clipboard` 上屏后不还原剪贴板（等价于每次请求 `restore_clipboard:false`），
`--verbose` 逐请求打印日志到 stdout。

### API

接口与 `linux` 分支统一：主端点 `POST /text`，默认端口同为 `8787`。
`POST /` 与 `POST /paste` 保留为兼容别名（对应 linux 的 `POST /` 别名），
`GET /health` 同理。

**`GET /`（或 `GET /health`、无参数的 `GET /text`）** —— 状态查询，
返回 `{"ok":true,"service":"put_text",...}`。

**`POST /text`** —— 上屏。请求体按 `Content-Type` 解释（与 linux 分支一致）：

- `Content-Type: application/json` —— 顶层是字符串时整体即正文；顶层是对象时
  取 `text` / `content` / `body` 键作为正文，并支持以下 Windows 扩展字段：

```json
{
  "text": "要输入的内容",
  "restore_clipboard": true,
  "target_hwnd": 0,
  "focus_hwnd": 0,
  "force": false
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `text` / `content` / `body` | — | 待上屏文本（UTF-8，至少给一个） |
| `restore_clipboard` | `true` | 上屏后是否还原剪贴板 |
| `target_hwnd` | `0`（=前台窗口） | 目标窗口句柄 |
| `focus_hwnd` | `0`（=自动探测） | 焦点子控件句柄 |
| `force` | `false` | `true` 时跳过可编辑性检测 |

- `Content-Type: text/plain` —— 请求体原文即正文，扩展字段取默认值。
- `Content-Type: application/x-www-form-urlencoded` —— 取 `text=` / `content=` /
  `body=` 键值（`curl --data` 的默认格式）；无这些键时整体视为纯文本。

响应镜像 SayIt 的 `InjectResult`：

```json
{"ok":true,"strategy":"wm_paste","reason":null,"detail":"hwnd=... class=scintilla textLen=10","uncertain":false}
```

`strategy` 说明走通了哪条路：

| `strategy` | 含义 |
|---|---|
| `console_paste` | conhost 系统菜单粘贴 |
| `wm_paste` | 直接投递 `WM_PASTE` 给原生编辑控件 |
| `send_input` | 前台抢占 + 合成 `Ctrl+V` |
| `wm_paste_last_resort` | `SendInput` 被拦后的最后一次 `WM_PASTE` |
| `clipboard` | 只写进了剪贴板，没找到可粘贴的窗口 |

失败时 `ok:false`，`reason` 说明卡在哪一步：`no_foreground_window`、
`not_editable`、`clipboard_blocked`、`send_input_short_write`、
`input_blocked_access_denied`、`invalid_utf8`、`invalid_json`、
`json_missing_text_field`、`json_body_not_object_or_string`。
空文本不算失败，返回 `{"ok":true,"strategy":"empty_text",...}`（与 linux 一致）。
`detail` 里带句柄、窗口类名、错误码等，排查时先看它。

**HTTP 状态码与上屏结果一致**（与 linux 分支相同）：上屏成功返回 `200`，
失败返回 `502`；只有 `400`（请求解析失败，如非法 JSON、缺正文字段）和
`404`（路由不存在）例外。body 里的 `ok` 是同一件事的另一面，两边都判断即可。

**`GET /text?text=...`** —— 等价于 POST，方便浏览器地址栏和脚本直接测
（也认 `content=` / `body=`；无这些参数时等同 `GET /` 返回状态）：

```bash
curl 'http://127.0.0.1:8787/text?text=%E4%BD%A0%E5%A5%BD'
```

其余参数取默认值（还原剪贴板、前台窗口、不跳过可编辑性检测）。
正文会进 URL，浏览器历史和日志里会留一份，别用它传敏感内容。

### 跨源调用

所有响应都带：

```
Access-Control-Allow-Origin: *
```

所以网页（哪怕是从 `file://` 打开的，或者另一个端口上的静态服务）
可以直接 `fetch` 这个接口并读到结果。

**没有处理 `OPTIONS`**，因为不需要：网页只要用简单请求就不会触发预检。
具体就是 `POST` + `Content-Type: text/plain`，正文直接放请求体：

```js
await fetch('http://192.168.1.10:8787/text', {
  method: 'POST',
  headers: { 'Content-Type': 'text/plain' },
  body: '你好，世界',
});
// → {"ok":true,"strategy":"wm_paste","reason":null,"detail":"...","uncertain":false}
```

一旦改成 `application/json`、或者加了任何自定义请求头，浏览器就会先发
`OPTIONS` 探测，这个服务会回 `404`，请求直接失败。要保持 `text/plain` 不变。

`*` 意味着你浏览器里打开的**任何网页**都能读这个服务的响应，而这服务能往你
当前焦点窗口打字。只在可信局域网里这么开，别暴露到公网。要收紧就把 `*`
换成具体来源（如 `http://192.168.1.5:8000`）。

### 示例

```bash
# 上屏到当前前台窗口
curl -X POST http://127.0.0.1:8787/text \
  -H "Content-Type: application/json" \
  -d '{"text":"你好，世界","restore_clipboard":true}'

# 上屏到指定窗口（并绕过可编辑性检查）
curl -X POST http://127.0.0.1:8787/text \
  -d '{"text":"hi","target_hwnd":123456,"force":true}'
```

## 目录结构

```
build.bat      zig c++ 构建脚本
LICENSE        AGPL-3.0 全文
src/
  main.cpp     Winsock HTTP 服务 + 极简 JSON 解析
  inject.cpp   上屏管线（剪贴板 / WM_PASTE / SendInput / 还原 / 检测）
  inject.hpp
```

## 局限

- 仅支持 Windows。
- **服务没有任何鉴权**。默认只监听 `127.0.0.1`，别人碰不到；
  用 `--host 0.0.0.0` 开放给手机之后就不同了：同一局域网里的任何设备都能往
  你当前焦点的窗口里打字。再加上响应带 `Access-Control-Allow-Origin: *`，
  你浏览器里打开的任何一个网页也能读写它。只在可信网络里这么开。
- UIA 可编辑性判定未移植（SayIt 中用于更精细地识别富文本编辑器），
  本实现以光标 + 控件类 + 进程名启发式覆盖常见场景；
  遇到误判可以用 `force:true` 跳过检测。
- `WM_PASTE` 收到消息后是否真的插入了文本无法确认，
  所以 `strategy` 只代表「消息投递成功」。
- conhost / `SendInput` 分支建议在真实交互桌面下验证
  （受限 shell 中无法切换前台窗口）。

## 许可

本分支遵循 **AGPL-3.0**，全文见 [LICENSE](LICENSE)。

`src/inject.cpp` 移植自 [SayIt](https://github.com/crosswk/SayIt) 的
`client/src-tauri/src/inject/mod.rs`，上游是 AGPL-3.0，本分支是它的衍生作品，
所以修改和分发时**必须继续遵循 AGPL-3.0**，包括通过网络提供服务时也要
向用户提供源码。
