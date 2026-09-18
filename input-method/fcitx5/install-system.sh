#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
build_dir="${script_dir}/build"

generator="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
  generator="Ninja"
fi

cmake -S "$script_dir" -B "$build_dir" -G "$generator" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$build_dir"

if [ "${1:-}" = "--install" ]; then
  if [ "$(id -u)" -eq 0 ]; then
    cmake --install "$build_dir"
  else
    sudo cmake --install "$build_dir"
  fi
  printf '%s\n' "已安装。重启 Fcitx5 生效：fcitx5 -rd"
else
  printf '%s\n' \
    "构建完成。请用下面的命令安装，然后重启 Fcitx5：" \
    "  sudo cmake --install \"${build_dir}\"" \
    "  fcitx5 -rd"
fi
