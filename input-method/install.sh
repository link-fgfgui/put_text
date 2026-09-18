#!/usr/bin/env bash
# Install the input-method integration that put_text.py commits through.
# Both backends speak the same protocol, so installing either one is enough;
# Fcitx5 is preferred. See NOTICE for the upstream source of these files.
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

FCITX5_PACKAGES=(cmake libfcitx5core-dev libfcitx5utils-dev extra-cmake-modules ninja-build)
IBUS_PACKAGES=(python3-gi gir1.2-ibus-1.0)

usage() {
  printf '%s\n' \
    "用法: $0 <fcitx5|ibus|both> [install|deps|check]" \
    "" \
    "  fcitx5  编译并安装 Fcitx5 模块（系统级，需要 sudo，装完重启 fcitx5）" \
    "  ibus    安装 IBus 引擎（用户级，需在设置里添加输入源）" \
    "  both    两个都装" \
    "" \
    "动作（默认 install）:" \
    "  deps    apt 安装该后端所需的系统依赖" \
    "  check   只检查依赖是否齐全，不做安装"
}

target="${1:-}"
case "$target" in
  fcitx5|ibus|both) ;;
  *) usage; exit 2 ;;
esac
action="${2:-install}"
case "$action" in
  install|deps|check) ;;
  *) usage; exit 2 ;;
esac

backend_args=("${@:3}")

apt_install() {
  local packages=("$@")
  printf 'apt install: %s\n' "${packages[*]}"
  if [ "$(id -u)" -eq 0 ]; then
    apt update
    apt install -y "${packages[@]}"
  elif command -v sudo >/dev/null 2>&1; then
    sudo apt update
    sudo apt install -y "${packages[@]}"
  else
    printf '需要 root 权限（既不是 root 也没有 sudo），请手动安装上述软件包。\n' >&2
    return 1
  fi
}

check_fcitx5() {
  local missing=()
  for package in "${FCITX5_PACKAGES[@]}"; do
    if ! dpkg -s "$package" >/dev/null 2>&1; then
      missing+=("$package")
    fi
  done
  if [ ${#missing[@]} -gt 0 ]; then
    printf '缺少依赖: %s\n' "${missing[*]}"
    printf '安装: %s %s deps\n' "$0" "$target"
    return 1
  fi
  printf 'Fcitx5 构建依赖齐全\n'
}

check_ibus() {
  if ! python3 -c 'import gi; gi.require_version("IBus", "1.0")' 2>/dev/null; then
    printf '缺少依赖: python3-gi / IBus typelib\n'
    printf '安装: %s %s deps\n' "$0" "$target"
    return 1
  fi
  printf 'IBus 运行环境齐全\n'
}

install_fcitx5() {
  case "$action" in
    check) check_fcitx5 ;;
    deps) apt_install "${FCITX5_PACKAGES[@]}" ;;
    install)
      check_fcitx5
      bash "${script_dir}/fcitx5/install-system.sh" "${backend_args[@]}"
      ;;
  esac
}

install_ibus() {
  case "$action" in
    check) check_ibus ;;
    deps) apt_install "${IBUS_PACKAGES[@]}" ;;
    install)
      check_ibus
      bash "${script_dir}/ibus/install-user.sh"
      ;;
  esac
}

case "$target" in
  fcitx5) install_fcitx5 ;;
  ibus) install_ibus ;;
  both) install_fcitx5; install_ibus ;;
esac
