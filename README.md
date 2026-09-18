# put_text — Linux 后端

在 Linux 本地起个 HTTP 服务，收到请求就把正文直接打进当前聚焦的输入框里。

上屏管线移植自 [SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，优先通过 Fcitx5 / IBus 输入法模块直接提交文本（零侵入、不碰剪贴板、不抢焦点），未安装插件时自动退回到「写剪贴板 + 合成 Ctrl+V」兜底。

```bash
./setup.sh --serve                     # 装依赖 → 编译安装输入法后端 → 重启 → 起服务
curl -s localhost:8787/text --data-binary '你好，世界'
```

___

## 上屏原理

`src/put_text.py` 的 `commit_text()` 会按以下优先级逐级尝试：

1. **Fcitx5 模块** —— `$XDG_RUNTIME_DIR/sayit-linux/fcitx5.sock`
2. **IBus 引擎** —— `$XDG_RUNTIME_DIR/sayit-linux/ibus.sock`
3. **兜底：剪贴板 + 合成 `Ctrl+V`** —— Wayland 下使用 `wtype` / `ydotool`，X11 下使用 `xdotool`；剪贴板使用 `wl-copy` 或 `xclip`。粘贴触发后由独立线程延迟 500ms 恢复用户原剪贴板内容，通过「代数计数器 + 内容复核」避免覆盖随后复制的新内容。

前两条是正规通道：输入法自身持有目标编辑框的上下文，能直接向当前输入框提交文本，不抢焦点、不污染剪贴板。socket 协议为 `4 字节大端长度 + UTF-8 正文`。

插件源码在 `input-method/` 目录，编译安装其中任意一个即可打通正规通道；只有两者皆不可用时才会触发第 3 步兜底。

___

## 安装

### 一键安装（推荐）

```bash
./setup.sh                 # 自动检测当前运行的输入法框架并编译安装
./setup.sh --backend ibus  # 强制指定 IBus 引擎
./setup.sh --serve         # 装完顺带把 HTTP 服务跑起来
./setup.sh --port 9000 --serve
```

脚本会依次执行：`apt` 安装必要系统依赖 → 编译/安装输入法插件 → 重启输入法 → 等待 socket 就绪 →（可选）启动服务。

### 手动安装

```bash
# 1. 检查或安装依赖（fcitx5 需要 sudo 写入系统目录，ibus 为用户级安装）
input-method/install.sh fcitx5 install    # 或 ibus / both
input-method/install.sh fcitx5 check      # 仅检查依赖，不安装

# 2. 重启生效
fcitx5 -rd                                # fcitx5 需重启
# ibus 装完需要在系统设置 → 键盘 → 输入源里手动添加「中文 → SayIt Linux 语音输入」，并注销重登

# 3. 启动服务
src/put_text.py
```

`put_text.py` 纯使用 Python 标准库编写，无需 `pip install` 任何额外依赖。

___

## 运行与参数

```bash
src/put_text.py [--host 127.0.0.1] [--port 8787] [--no-restore-clipboard] [--verbose]
```

- `--host 0.0.0.0`：监听所有网卡。默认只听 `127.0.0.1`，手机或局域网设备要访问必须指定 `0.0.0.0`。
- `--port 8787`：默认监听端口。
- `--no-restore-clipboard`：降级走剪贴板模式时，上屏后保留内容，不恢复原剪贴板。
- `--verbose`：输出调试日志。

> **安全注意**：服务为了轻量没有任何鉴权。一旦指定 `--host 0.0.0.0`，同局域网的任何设备都能向你电脑的焦点窗口打字，切勿映射到公网。

___

## 接口说明

### 1. 提交上屏 (`POST /text`)

支持三种常见的请求体格式：

```bash
# 原文（最推荐，直接把文本丢进 body）
curl -s localhost:8787/text --data-binary '你好，世界'

# JSON 格式（读取 text / content / body 字段）
curl -s localhost:8787/text -H 'Content-Type: application/json' \
     -d '{"text":"hello"}'
```

### 2. URL 参数快捷提交 (`GET /text?text=...`)

和 `POST /text` 效果完全相同，主要方便在浏览器地址栏或简单脚本中快速测试：

```bash
curl -s 'localhost:8787/text?text=%E4%BD%A0%E5%A5%BD'
```

> 注：正文会暴露在 URL、浏览器历史和访问日志中，请勿用于敏感内容。

### 3. 查看状态 (`GET /`)

检查服务运行状态、已连接的输入法 socket 与可用的粘贴工具：

```bash
curl -s localhost:8787/
```

响应示例：

```json
{
  "ok": true,
  "service": "put_text",
  "socket_directory": "/run/user/1000/sayit-linux",
  "input_methods": [
    {"strategy": "fcitx5_commit", "socket": ".../fcitx5.sock", "available": true},
    {"strategy": "ibus_commit",   "socket": ".../ibus.sock",   "available": false}
  ],
  "paste_tools": ["wtype", "wl-copy", "wl-paste"]
}
```

### 响应状态码与策略

上屏成功返回 HTTP `200`，失败返回 HTTP `502`（`ok: false`）：

```json
{"ok": true, "strategy": "fcitx5_commit", "reason": null, "detail": "Text committed by the SayIt Fcitx5 module"}
```

- `strategy`：说明具体命中了哪条上屏路径：
  - `fcitx5_commit` / `ibus_commit`：输入法正规通道（最稳定、无感）。
  - `clipboard_wtype` / `clipboard_ydotool` / `clipboard_xdotool`：剪贴板 + 合成按键兜底。
  - `clipboard_only`：文字已写入剪贴板，但缺少模拟按键工具无法触发粘贴。
  - `clipboard_write`：写剪贴板失败。
- `reason`：失败原因标识，常见有：`socket_missing`（插件未就绪）、`no_focused_input_context`（没有焦点窗口）、`input_method_commit_timeout`、`linux_paste_tool_unavailable`、`xdg_runtime_dir_unavailable`。
- `detail`：包含底层异常或输入法返回的原始错误信息，排查问题时直接看这里。

### 跨域调用须知

所有响应均附带 `Access-Control-Allow-Origin: *` 头，前端静态页面（如 `file://` 打开的单文件网页）可直接跨域 `fetch`。

由于后端未实现 `OPTIONS` 预检处理，前端请求**必须保持简单请求**，即 `POST` + `Content-Type: text/plain`：

```js
await fetch('http://192.168.1.10:8787/text', {
  method: 'POST',
  headers: { 'Content-Type': 'text/plain' },
  body: '你好，世界',
});
```

若误将 `Content-Type` 设为 `application/json` 或添加自定义 Header，浏览器会先行发送 `OPTIONS` 探测，导致请求直接失败。

___

## 目录结构

```
setup.sh                 一键依赖检测、编译与安装脚本
LICENSE                  AGPL-3.0 协议全文
src/put_text.py          HTTP 服务与上屏核心管线（纯标准库单文件）
input-method/
  install.sh             输入法后端编译与安装底层脚本
  NOTICE                 第三方移植来源与授权声明
  fcitx5/                Fcitx5 模块源码（C++ / CMake）
  ibus/                  IBus 引擎源码（Python / GObject）
```

___

## FAQ / 排查

**Q: `GET /` 显示 `input_methods` 全部为 `available: false`？**  
**A:** 输入法插件未正确安装，或者安装后未重启输入法。Fcitx5 执行 `fcitx5 -rd` 重启即可；IBus 安装后需在系统设置中添加输入源并重新登录系统桌面。

**Q: 上屏失败，返回 `no_focused_input_context`？**  
**A:** 当前桌面上没有获得光标焦点的输入框，或者前台正在运行终端 TUI 程序。在发送请求前，先用鼠标点击一下目标输入框使光标处于闪烁激活状态。

**Q: 报 `xdg_runtime_dir_unavailable` 错误？**  
**A:** 环境变量中缺少 `XDG_RUNTIME_DIR`，常见于使用 `sudo` 运行或配置了不完整的 systemd 系统服务。建议以普通用户权限运行，或在 systemd 中使用 `--user` 用户服务单元。

**Q: X11 环境下快捷键触发 `xdotool` 容易失灵？**  
**A:** 系统快捷键激活时，物理修饰键（如 Ctrl/Alt）未完全释放会与合成按键产生冲突。虽然脚本已附带 `--clearmodifiers`，但最根本的解决方式是安装并启用 Fcitx5/IBus 输入法正规通道。

___

## 许可

本分支遵循 **AGPL-3.0** 协议，全文见 [LICENSE](LICENSE)。

- `input-method/` 目录下的输入法实现逐字复制自 [SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)（AGPL-3.0），详细出处见 `input-method/NOTICE`。
- `src/put_text.py` 的输入法通信逻辑同样移植自 SayIt-Linux。
- 修改和分发本分支代码必须严格遵守 AGPL-3.0 协议，包含通过网络向用户提供服务时同样需公开源码。
