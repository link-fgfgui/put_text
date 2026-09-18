#!/usr/bin/env bash
# 一键安装：装依赖 -> 编译/安装输入法后端 -> 重启后端 -> 校验 socket -> （可选）启动服务
set -euo pipefail

root_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
backend=""
serve=0
host="127.0.0.1"
port=8787

usage() {
  printf '%s\n' \
    "用法: $0 [选项]" \
    "" \
    "  --backend <fcitx5|ibus>  指定后端（默认自动检测当前在跑的输入法框架）" \
    "  --host <addr>            --serve 时的监听地址（默认 127.0.0.1）" \
    "  --port <n>               --serve 时的监听端口（默认 8787）" \
    "  --serve                  安装完成后直接启动 HTTP 服务（前台运行）" \
    "  -h, --help               显示本帮助" \
    "" \
    "示例:" \
    "  $0                 # 自动检测后端并安装" \
    "  $0 --serve         # 装完顺带把服务跑起来" \
    "  $0 --backend ibus  # 强制用 IBus 引擎"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --backend) backend="${2:-}"; shift 2 ;;
    --backend=*) backend="${1#*=}"; shift ;;
    --host) host="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --serve) serve=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf '未知参数: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$backend" in
  ""|fcitx5|ibus) ;;
  *) printf '后端只能是 fcitx5 或 ibus\n' >&2; exit 2 ;;
esac

detect_backend() {
  if command -v fcitx5 >/dev/null 2>&1 && { pgrep -x fcitx5 >/dev/null 2>&1; }; then
    printf 'fcitx5'
  elif command -v ibus >/dev/null 2>&1 && pgrep -x ibus-daemon >/dev/null 2>&1; then
    printf 'ibus'
  elif command -v fcitx5 >/dev/null 2>&1; then
    printf 'fcitx5'
  elif command -v ibus >/dev/null 2>&1; then
    printf 'ibus'
  fi
}

if [ -z "$backend" ]; then
  backend="$(detect_backend)"
fi
if [ -z "$backend" ]; then
  printf '未检测到 Fcitx5 或 IBus，请先用 --backend 指定。\n' >&2
  exit 1
fi
printf '==> 后端: %s\n' "$backend"

# 缓存一次 sudo 凭据，后面的 apt / cmake --install 不再重复询问
if [ "$(id -u)" -ne 0 ]; then
  sudo -v
fi

printf '==> 安装系统依赖\n'
bash "${root_dir}/input-method/install.sh" "$backend" deps

printf '==> 安装 %s 后端\n' "$backend"
bash "${root_dir}/input-method/install.sh" "$backend" install --install

runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
socket="${runtime_dir}/sayit-linux/$( [ "$backend" = fcitx5 ] && echo fcitx5.sock || echo ibus.sock )"

printf '==> 重启 %s\n' "$backend"
case "$backend" in
  fcitx5)
    if pgrep -x fcitx5 >/dev/null 2>&1; then
      fcitx5 -rd
    else
      printf 'Fcitx5 未在运行，跳过重启（启动 Fcitx5 后插件会自动加载）。\n'
    fi
    ;;
  ibus)
    if command -v ibus >/dev/null 2>&1 && ibus restart >/dev/null 2>&1; then
      printf '已执行 ibus restart。\n'
    else
      printf '无法自动重启 IBus，请退出并重新登录。\n' >&2
    fi
    printf '还需要在 设置 → 键盘 → 输入源 中添加“中文 → SayIt Linux 语音输入”。\n'
    ;;
esac

printf '==> 等待 socket: %s\n' "$socket"
for _ in $(seq 1 20); do
  if [ -S "$socket" ]; then
    printf '==> 就绪\n'
    break
  fi
  sleep 0.5
done
if [ ! -S "$socket" ]; then
  printf 'socket 未出现，请确认后端已重启并加载插件，然后查看: %s\n' "$socket" >&2
fi

if [ "$serve" -eq 1 ]; then
  printf '==> 启动服务 http://%s:%s\n' "$host" "$port"
  exec python3 "${root_dir}/src/put_text.py" --host "$host" --port "$port"
fi

printf '%s\n' \
  "" \
  "完成。启动服务：" \
  "  ${root_dir}/src/put_text.py" \
  "测试：" \
  "  curl -s localhost:${port}/" \
  "  curl -s localhost:${port}/text --data-binary '你好'"
