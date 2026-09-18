# put_text

把 HTTP 请求变成「往当前聚焦的输入框里打字」。

在本地起一个 HTTP 服务，收到 POST 就把正文上屏到前台窗口的输入框里。
用来把语音识别、大模型、脚本的输出直接送进任意应用，而不用关心那个应用
是什么、有没有命令行接口、是否支持插件。

上屏管线移植自 [SayIt](https://github.com/sayitapp/sayit) 及其 Linux 版
[SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，两个平台各有一套实现。

## 分支

两个后端各自独立成分支，按需要的平台 checkout，互不干扰：

| 分支 | 平台 | 实现 | 上屏通道 |
|---|---|---|---|
| [`linux`](../../tree/linux) | Linux（Wayland / X11） | Python 单文件 + Fcitx5 / IBus 输入法插件 | 输入法 socket → 剪贴板 + 合成 `Ctrl+V` |
| [`windows`](../../tree/windows) | Windows 10 / 11 | C++ 单二进制，零第三方依赖 | `WM_COMMAND` / `WM_PASTE` → `SendInput` `Ctrl+V` |
| `main` | — | 只有本文档 | 无可运行代码 |

```bash
git clone -b linux   <repo-url> put_text   # Linux 后端
git clone -b windows <repo-url> put_text   # Windows 后端
```

## 相同点

两个后端都遵循同一套设计：

- **本地 HTTP 服务**，只监听 `127.0.0.1`，不接受外部连接。
- **逐级回退的上屏策略**：优先走能拿到输入上下文的正规通道，失败再退到
  「写剪贴板 + 合成粘贴」。
- **剪贴板还原**：粘贴触发后延迟恢复用户原本的剪贴板内容，并用
  「代数计数器 + 内容复核」避免覆盖用户随后复制的内容。
- **结构化响应**：返回 `ok` / `strategy` / `reason` / `detail`，
  调用方可以判断到底是哪条路径生效、失败在哪一步。

## 不同点

接口没有强行统一，两边各自贴合平台习惯：

| | `linux` | `windows` |
|---|---|---|
| 默认端口 | `8787` | `18765` |
| 主接口 | `POST /text` | `POST /paste` |
| 状态查询 | `GET /` | `GET /health` |
| 请求体 | 原文 / JSON / 表单 | 原文 / JSON |
| 目标窗口 | 只能是当前聚焦的输入框 | 可指定 `target_hwnd` / `focus_hwnd` |
| 额外参数 | `--no-restore-clipboard` | `restore_clipboard` / `force` |
| 失败 HTTP 码 | `502` | `502` |

示例：

```bash
# Linux
curl -s localhost:8787/text --data-binary '你好，世界'

# Windows
curl -s -X POST localhost:18765/paste \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好，世界"}'
```

## 目录结构

各分支 checkout 后的布局是对称的：`src/` 放核心实现，平台相关的构建/安装脚本
放根目录。

```
main/
  README.md

linux/                      windows/
  README.md                   README.md
  setup.sh                    build.bat
  src/put_text.py             src/main.cpp
  input-method/               src/inject.cpp
    fcitx5/                   src/inject.hpp
    ibus/
```

`linux` 还需要单独安装 `input-method/` 里的输入法插件 —— 那是把文本送上屏的
正规通道，`setup.sh` 会自动编译安装；`windows` 是单个 exe，构建完即安装完。

## 许可

- `linux` 分支下 `input-method/` 中的 Fcitx5 / IBus 代码来自
  [SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，遵循 **AGPL-3.0**，
  修改后的版本同样如此，详见该目录内的 `NOTICE`。
- `windows` 分支的 C++ 实现移植自 [SayIt](https://github.com/sayitapp/sayit) 的
  Windows 注入管线。
