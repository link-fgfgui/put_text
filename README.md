# put_text

把 HTTP 请求变成「往当前聚焦的输入框里打字」。

在本地起一个 HTTP 服务，收到请求就把正文上屏到当前聚焦的输入框里。
用来把语音识别、大模型、脚本的输出直接送进任意应用，而不用关心那个应用
是什么、有没有命令行接口、是否支持插件。

上屏管线移植自 [SayIt](https://github.com/crosswk/SayIt) 及其 Linux 版
[SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，两个平台各有一套实现。

## 分支

两个后端各自独立成分支，按需要的平台 checkout，互不干扰：

| 分支 | 平台 | 实现 | 上屏通道 |
|---|---|---|---|
| `main` | — | `index.html`，手机端网页（纯前端，单文件） | 通过 HTTP 调后端 |
| [`linux`](../../tree/linux) | Linux（Wayland / X11） | Python 单文件 + Fcitx5 / IBus 输入法插件 | 输入法 socket → 剪贴板 + 合成 `Ctrl+V` |
| [`windows`](../../tree/windows) | Windows 10 / 11 | C++ 单二进制，零第三方依赖 | `WM_COMMAND` / `WM_PASTE` → `SendInput` `Ctrl+V` |

```bash
git clone -b linux   https://github.com/link-fgfgui/put_text.git put_text   # Linux 后端
git clone -b windows https://github.com/link-fgfgui/put_text.git put_text   # Windows 后端
```

网页只用到 `main`，直接拿 `index.html` 一个文件即可，不需要后端代码。

## 网页端

`index.html` 是一个自包含的单文件页面（无构建、无依赖），用手机浏览器打开，
输入的文字会发到后端并上屏到你电脑当前聚焦的输入框。

- **手机排版**：整屏输入区、底部状态栏、`dvh` + 安全区适配；
  输入框 `font-size: 16px` 避免 iOS 自动放大。
- **停 1.5 秒自动发送**：停止输入 1.5 秒后自动上屏并清空输入框，
  适合「说一句、上屏一句」的听写节奏。发送失败时文本保留，不会丢。
- **设备列表**：保存多台后端（名称 + 地址 + 平台），存在 `localStorage`，
  随时切换。首次打开预置了两个模板（Linux `:8787` / Windows `:18765`），
  地址默认是本机回环，连手机要改成电脑的局域网 IP。
- **状态栏显示真实结果**：上屏成功显示「已上屏」，失败显示后端返回的原因
  （如「当前没有聚焦的输入框」「输入法插件没装或没重启」），而不是笼统的「失败」。

### 怎么用

1. 电脑上跑后端，**要监听所有网卡**，否则手机连不上：

   ```bash
   # Linux
   src/put_text.py --host 0.0.0.0
   # Windows
   put_text.exe --host 0.0.0.0
   ```

2. 把 `index.html` 传到手机，用系统浏览器打开（部分内置浏览器如微信不支持
   打开本地文件，用系统浏览器最稳）。
3. 点右上角 ⚙ 添加设备，地址填电脑的局域网 IP，例如
   `http://192.168.1.10:8787`（Linux）或 `http://192.168.1.10:18765`（Windows）。
4. 点一下电脑上要输入的地方，让输入框获得焦点，然后开始打字。

### 跨源与预检

网页和后端不同源，浏览器默认不让 JS 读跨源响应。两个后端都返回了
`Access-Control-Allow-Origin: *`，所以页面能读到完整结果。

页面**只用简单请求**，因此不会触发 `OPTIONS` 预检（后端也刻意没实现 OPTIONS）：

```js
fetch(url + '/text', {
  method: 'POST',
  headers: { 'Content-Type': 'text/plain' },   // 必须保持 text/plain
  body: '要上屏的文字',
});
```

改代码时注意：把 `Content-Type` 换成 `application/json`、或加任何自定义请求头，
浏览器就会先发 `OPTIONS` 探测，请求会直接失败。

### 安全

- 后端**没有任何鉴权**。`--host 0.0.0.0` 意味着同一局域网里任何设备都能往你
  焦点窗口打字；`Access-Control-Allow-Origin: *` 还意味着你浏览器里打开的
  任何网页都能读写它。只在可信网络里这么开，别暴露到公网。
- 页面是 `file://` 打开或放在 `http://` 下都行，但**别放 `https://`** ——
  浏览器会拦截 https 页面发往 `http://` 的请求（混合内容）。

## 相同点

两个后端都遵循同一套设计：

- **本地 HTTP 服务**，默认只监听 `127.0.0.1`，不接受外部连接
  （给手机用要显式改成 `--host 0.0.0.0`）。
- **逐级回退的上屏策略**：优先走能拿到输入上下文的正规通道，失败再退到
  「写剪贴板 + 合成粘贴」。
- **剪贴板还原**：粘贴触发后延迟恢复用户原本的剪贴板内容，并用
  「代数计数器 + 内容复核」避免覆盖用户随后复制的内容。
- **结构化响应**：返回 `ok` / `strategy` / `reason` / `detail`，
  调用方可以判断到底是哪条路径生效、失败在哪一步。
- **带 CORS 头**：所有响应都有 `Access-Control-Allow-Origin: *`，
  网页可以直接 `fetch` 并读到结果；两边都只依赖简单请求，不实现 `OPTIONS`。

## 不同点

接口没有强行统一，两边各自贴合平台习惯：

| | `linux` | `windows` |
|---|---|---|
| 默认端口 | `8787` | `18765` |
| 主接口 | `POST /text` | `POST /paste` |
| GET 提交 | `GET /text?text=...` | `GET /paste?text=...` |
| 状态查询 | `GET /` | `GET /` 或 `GET /health` |
| 请求体 | 原文 / JSON / 表单 | 原文 / JSON |
| 目标窗口 | 只能是当前聚焦的输入框 | 可指定 `target_hwnd` / `focus_hwnd` |
| 额外参数 | `--no-restore-clipboard` | `restore_clipboard` / `force` |
| 上屏失败时的 HTTP 码 | `502` | `200`（只看 body 里的 `ok`） |

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
main/                       linux/                      windows/
  README.md                   README.md                   README.md
  index.html                  setup.sh                    build.bat
                              src/put_text.py             src/main.cpp
                              input-method/               src/inject.cpp
                                fcitx5/                   src/inject.hpp
                                ibus/
```

`linux` 还需要单独安装 `input-method/` 里的输入法插件 —— 那是把文本送上屏的
正规通道，`setup.sh` 会自动编译安装；`windows` 是单个 exe，构建完即安装完。

## 许可

**这个仓库的各个分支许可不同**，checkout 到哪个分支就适用哪个：

| 分支 | 许可 | 原因 |
|---|---|---|
| `main` | **MIT** | `index.html` 与文档是原创，见 [LICENSE](LICENSE) |
| `linux` | **AGPL-3.0** | `input-method/` 逐字复制自 [SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)（AGPL-3.0），`src/put_text.py` 也移植了其上屏逻辑 |
| `windows` | **AGPL-3.0** | `src/inject.cpp` 移植自 [SayIt](https://github.com/crosswk/SayIt) 的 `client/src-tauri/src/inject/mod.rs`（AGPL-3.0） |

两个上游都是 AGPL-3.0，所以**修改和分发两个后端分支时必须继续遵循 AGPL-3.0**，
包括向网络用户提供服务时也要提供源码。各分支根目录有完整 LICENSE 全文，
`linux` 分支的 `input-method/NOTICE` 另记了第三方代码的具体出处。
