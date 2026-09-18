# put_text — Linux 后端

把 HTTP POST 变成「往当前聚焦的输入框里打字」。上屏管线移植自
[SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)。

```bash
./setup.sh --serve                     # 装依赖 → 编译安装输入法后端 → 重启 → 起服务
curl -s localhost:8787/text --data-binary '你好，世界'
```

## 上屏原理

`src/put_text.py:231` 的 `commit_text()` 逐级回退：

1. **Fcitx5 模块** —— `$XDG_RUNTIME_DIR/sayit-linux/fcitx5.sock`
2. **IBus 引擎** —— `$XDG_RUNTIME_DIR/sayit-linux/ibus.sock`
3. **兜底：剪贴板 + 合成 `Ctrl+V`** —— Wayland 用 `wtype` / `ydotool`，
   X11 用 `xdotool`；剪贴板用 `wl-copy` 或 `xclip`。
   粘贴后用独立线程延迟 500ms 还原用户原来的剪贴板内容，
   靠「代数计数器 + 内容复核」避免覆盖用户随后复制的内容。

前两条是正规通道：输入法持有输入上下文，能直接把文本提交进编辑器，
不碰剪贴板、不抢焦点。socket 协议为
`4 字节大端长度 + UTF-8 正文`，响应 `OK` / `NO_FOCUS` / `INVALID` / `TIMEOUT` / `ERROR`。

Fcitx5 / IBus 插件代码在 `input-method/`，来自 SayIt-Linux，
**只有装上其中之一，第 3 步才不会被用到**。两者的 socket 协议一致，装一个即可。

## 安装

### 一键

```bash
./setup.sh                 # 自动检测当前在跑的输入法框架并安装
./setup.sh --backend ibus  # 强制用 IBus 引擎
./setup.sh --serve         # 装完顺带把 HTTP 服务跑起来
./setup.sh --port 9000 --serve
```

会依次做：`apt` 装系统依赖 → 编译/安装输入法后端 → 重启输入法 →
等待 socket 出现 →（可选）启动服务。

### 手动

```bash
# 1. 依赖 + 输入法后端（fcitx5 需要 sudo，ibus 是用户级安装）
input-method/install.sh fcitx5 install    # 或 ibus / both
input-method/install.sh fcitx5 check      # 只检查依赖，不安装

# 2. fcitx5 装完要重启；ibus 装完要在 设置 → 键盘 → 输入源 里
#    添加「中文 → SayIt Linux 语音输入」
fcitx5 -rd

# 3. 起服务
src/put_text.py
```

`put_text.py` 本身只用标准库，不需要 `pip install`。

## 使用

```
src/put_text.py [--host 127.0.0.1] [--port 8787] [--no-restore-clipboard] [--verbose]
```

`--no-restore-clipboard` 让上屏后的文本留在剪贴板里，不清回去。

### API

**`POST /text`** —— 上屏。请求体三种写法都认：

```bash
# 原文（curl --data 默认发 x-www-form-urlencoded，无 key 时按原文处理）
curl -s localhost:8787/text --data-binary '你好，世界'

# JSON：字符串，或对象里的 text / content / body 字段
curl -s localhost:8787/text -H 'Content-Type: application/json' \
     -d '{"text":"hello"}'
```

**`GET /`** —— 状态，列出检测到的 socket 和可用的粘贴工具：

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

### 响应

```json
{"ok": true, "strategy": "fcitx5_commit", "reason": null, "detail": "Text committed by the SayIt Fcitx5 module"}
```

成功返回 `200`，上屏失败返回 `502`（`ok:false`）。`strategy` 说明走通了哪条路：

| `strategy` | 含义 |
|---|---|
| `fcitx5_commit` / `ibus_commit` | 输入法正规通道 |
| `clipboard_wtype` / `clipboard_ydotool` / `clipboard_xdotool` | 剪贴板 + 合成按键 |
| `clipboard_only` | 文本已进剪贴板，但没有工具能触发粘贴 |
| `empty_text` | 收到空文本，没做事 |

`reason` 为失败原因，常见的有：`socket_missing`（插件没装/没重启）、
`no_focused_input_context`（当前没有聚焦的输入框）、
`input_method_commit_timeout`、`linux_paste_tool_unavailable`、
`xdg_runtime_dir_unavailable`（例如从 systemd 服务里跑，拿不到 session 环境）。

`detail` 里带完整的输入法错误链，排查时先看它。

## 目录结构

```
setup.sh                 一键安装脚本
src/put_text.py          HTTP 服务 + 上屏管线（单文件，标准库）
input-method/
  install.sh             fcitx5 / ibus 的依赖与安装入口
  NOTICE                 第三方代码来源与许可
  fcitx5/                Fcitx5 模块（C++ / CMake）
  ibus/                  IBus 引擎（Python / GObject）
```

## 排查

- **`GET /` 里 `input_methods` 全是 `available: false`** —— 插件没装上或输入法没重启。
  `fcitx5` 用 `fcitx5 -rd` 重启；`ibus` 需要退出重新登录。
- **返回 `no_focused_input_context`** —— 焦点不在输入框上，或者前台是终端里的
  TUI 程序。先点一下目标输入框再发请求。
- **返回 `xdg_runtime_dir_unavailable`** —— `XDG_RUNTIME_DIR` 没设置，
  常见于 `sudo` 或 systemd service 里执行。用 `systemd --user` 单元，
  或显式带上 `XDG_RUNTIME_DIR=/run/user/$(id -u)`。
- **X11 下从键盘快捷键触发时 `xdotool` 失败** —— 合成按键会和用户按住的修饰键
  叠加；命令里已经带 `--clearmodifiers`，如果还不行就改用输入法通道。

## 许可

`input-method/` 下的 Fcitx5 / IBus 代码来自
[SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，遵循 **AGPL-3.0**，
修改后的版本同样如此。详见 `input-method/NOTICE`。
