# put_text

在本地起个 HTTP 服务，收到请求就把正文直接打进当前聚焦的输入框里。

主要用来把语音识别、大模型、本地脚本的输出直接送进任意应用，不用关心目标软件有没有命令行接口或是否支持插件。上屏管线分别移植自 [SayIt](https://github.com/crosswk/SayIt) 与 [SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，两个平台各有一套原生实现。

___

## 分支与获取

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

网页只用到 `main` 分支，直接拿走 `index.html` 这一个文件即可，不需要任何后端代码或构建环境。

___

## 手机端网页 (`index.html`)

一个自包含的单文件 HTML，没有构建流程、零第三方依赖。传到手机用浏览器打开，敲字或用手机自带语音输入，内容就会自动上屏到电脑当前聚焦的窗口。

配合手机系统的语音输入法，相当于把手机当成电脑的无线语音听写麦克风。

- **手机排版体验**：整屏输入区、底部状态栏、适配 `dvh` 与手势安全区；输入框锁定 `font-size: 16px` 防止 iOS 聚焦时强制放大页面。
- **停 1.5 秒自动发送**：输入停顿 1.5 秒后自动上屏并清空输入框，适合「说一句、打一句」的节奏。如果发送失败文本会留在框里，不会丢字。
- **设备切换**：支持保存多台设备配置（名称 + 地址 + 平台），存在 `localStorage`。首次打开预置了模板，默认地址为本机回环，连电脑时换成电脑局域网 IP。
- **直白的状态反馈**：上屏成功显示「已上屏」，失败直接显示后端给出的具体原因（比如「当前没有聚焦的输入框」「输入法插件没装」），绝不只报一个模糊的失败。

### 使用步骤

1. **电脑启动后端**：必须显式加上 `--host 0.0.0.0` 监听所有网卡，否则只监听本机回环，局域网内的手机连不上：

   ```bash
   # Linux
   src/put_text.py --host 0.0.0.0

   # Windows
   put_text.exe --host 0.0.0.0
   ```

2. **手机打开网页**：把 `index.html` 传到手机，用系统自带浏览器（如 Safari / Chrome）打开。部分 App 内置网页视图（如微信）禁止打开本地文件，用系统独立浏览器最稳妥。
3. **添加设备**：点右上角 ⚙，地址填电脑在局域网中的 IP，例如 `http://192.168.1.10:8787`（Linux 和 Windows 端口统一为 8787）。
4. **聚焦与输入**：鼠标在电脑上点一下目标输入框让光标闪烁，然后在手机端打字或语音输入。

### 踩坑与注意事项

- **不要走 HTTPS**：网页用本地 `file://` 打开或挂在普通 `http://` 静态服务器下均可。**绝对不要**把页面放到 `https://` 域名下，否则浏览器会触发 Mixed Content 拦截，拒绝向电脑的 `http://` 局域网地址发请求。
- **别改请求类型（CORS 预检）**：页面与后端不同源，但后端已经返回了 `Access-Control-Allow-Origin: *`。之所以只发 `text/plain` 纯文本，是为了保持 CORS「简单请求」，避免浏览器发起 `OPTIONS` 预检（后端刻意没做预检处理）。如果二次开发改成 `application/json` 或加了自定义 Header，浏览器会先发 OPTIONS 探测，请求会直接暴毙。
- **局域网安全**：后端为了轻量**没有任何鉴权**。`--host 0.0.0.0` 意味着同局域网内的任何人都能往你电脑正选中的窗口注入文字。只在可信的家庭或私人热点里开，严禁映射到公网。

___

## 后端接口与平台差异

两个平台的后端遵循同一套 HTTP 契约，默认均监听 `8787` 端口：

- **统一接口**：
  - `POST /text`：上屏主入口。支持 原文（`text/plain`）、JSON（`application/json`）和表单（`application/x-www-form-urlencoded`）。
  - `GET /text?text=...`：URL 参数等价提交。
  - `GET /`：健康检查与状态查询。
- **状态码逻辑**：上屏成功返回 `200`，失败返回 `502`（均伴随带 `ok: true/false` 的 JSON 明细）。
- **无感剪贴板还原**：当不得不退回「写剪贴板 + 合成 Ctrl+V」兜底通道时，后台线程会在粘贴触发后延迟恢复原本的剪贴板内容，并通过「代数计数器 + 内容复核」防止意外覆盖用户随后手动复制的新内容。

### 平台差异对比

两端 HTTP 请求方式完全一致：

```bash
# Linux / Windows 通用测试命令
curl -s localhost:8787/text --data-binary '你好，世界'
```

实际差异仅源于底层系统机制：

| 平台 | 目标窗口限制 | 上屏核心通道 | 专有能力 |
|---|---|---|---|
| **Linux** | 只能送入「当前正聚焦的输入框」 | 优先走 Fcitx5 / IBus 插件 socket，失败再回退剪贴板 + `Ctrl+V` | 纯 Python 单文件，不抢焦点，输入法通道零侵入 |
| **Windows** | 默认当前焦点窗口，支持传参指定句柄 | 优先 conhost 命令 / `WM_PASTE` 消息，兜底抢前台 + `SendInput` | C++ 零依赖二进制；支持 JSON 传 `target_hwnd` 定向注入或加 `force: true` 跳过可编辑检测 |

Windows 专属的定向注入示例：

```bash
curl -s -X POST localhost:8787/text \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好，世界","target_hwnd":123456}'
```

___

## 目录结构

各分支代码结构保持对称：核心实现都在 `src/`，平台构建与安装脚本置于根目录。

```
main/                       linux/                      windows/
  README.md                   README.md                   README.md
  index.html                  setup.sh                    build.bat
                              src/put_text.py             src/main.cpp
                              input-method/               src/inject.cpp
                                fcitx5/                   src/inject.hpp
                                ibus/
```

- `linux`：需要编译安装 `input-method/` 下的插件以打通无感输入法通道（根目录 `./setup.sh` 会自动处理）。
- `windows`：单个 exe 无任何第三方运行时依赖，构建完成即是绿色单文件。

___

## FAQ

**Q: 手机上点了发送，电脑没反应，手机提示「连不上，检查地址和端口」？**  
**A:** 按这三步排查：
1. 电脑端命令是否漏了 `--host 0.0.0.0`（没加就只会监听 127.0.0.1，局域网访问必挂）。
2. 电脑防火墙是否拦截了 `8787` 端口的入站连接（特别是 Windows Defender，初次运行需点允许放行）。
3. 手机与电脑是否在同一个局域网，公司或学校公共 WiFi 常常开启了 AP 隔离（禁止内网设备互联），这种情况开手机热点让电脑连。

**Q: 提示「当前没有聚焦的输入框」或「没有前台窗口」？**  
**A:** 程序不会凭空猜你要往哪打字。在手机发送前，先在电脑上用鼠标点一下目标编辑框，确保文本光标（I-beam）正在其中闪烁。

**Q: Linux 下上屏有半秒延迟，或者偶尔漏字？**  
**A:** 说明没装输入法插件，程序自动掉进了「写剪贴板 + 合成 Ctrl+V」的兜底降级方案。请在 `linux` 分支下跑一遍 `./setup.sh` 编译并重启 Fcitx5 或 IBus，走 socket 原生通道上屏是毫无感知的瞬时触发。

**Q: 为什么不做身份验证或 Token 密码？**  
**A:** 本身就是个人局域网小工具，不想把请求流程搞臃肿。如果在共享网络有安全要求，建议配合内网反向代理加权，或者绑定指定网卡。

___

## 许可

**仓库各分支适用不同的开源协议**：

| 分支 | 协议 | 说明 |
|---|---|---|
| `main` | **MIT** | `index.html` 与文档为原创，见 [LICENSE](LICENSE) |
| `linux` | **AGPL-3.0** | `input-method/` 复制自 [SayIt-Linux](https://github.com/Kishibe-Miru/SayIt-Linux)，`src/put_text.py` 移植了其上屏逻辑 |
| `windows` | **AGPL-3.0** | `src/inject.cpp` 移植自 [SayIt](https://github.com/crosswk/SayIt)（AGPL-3.0） |

两端上游均为 AGPL-3.0，因此**修改与分发两个后端分支时必须严格继承 AGPL-3.0**（网络提供服务同样需要开源）。各分支根目录均附带对应的协议全文。
