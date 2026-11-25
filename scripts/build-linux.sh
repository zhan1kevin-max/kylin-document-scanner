#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${1:-"$ROOT/build-linux"}
if [[ $(uname -s) != Linux ]]; then
    printf '%s\n' '请在目标系统兼容的 Linux 构建环境运行此脚本。' >&2
    exit 1
fi
for cmd in cmake c++ ctest cpack dpkg-deb; do
    command -v "$cmd" >/dev/null || { printf '缺少构建工具：%s\n' "$cmd" >&2; exit 1; }
done
printf '构建架构：%s / %s\n' "$(uname -m)" "$(dpkg --print-architecture)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON -DSCANNER_QT_PKGCONFIG="${SCANNER_QT_PKGCONFIG:-ON}"
cmake --build "$BUILD" --parallel "${BUILD_JOBS:-2}"
(cd -- "$BUILD" && QT_QPA_PLATFORM=offscreen ctest --output-on-failure)
(cd -- "$BUILD" && cpack -G DEB)
printf '构建和测试成功。DEB 位于：%s\n' "$BUILD"
printf '%s\n' '该 DEB 尚需在目标麒麟实机上验证；仅 DEB 本体不等于完整离线安装包。'
