#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
BUNDLE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
DRY_RUN=false
if [[ $# == 1 && $1 == --dry-run ]]; then
    DRY_RUN=true
elif [[ $# != 0 ]]; then
    printf '%s\n' '用法：sudo bash install-offline.sh [--dry-run]' >&2
    exit 2
fi
if [[ ${EUID:-$(id -u)} != 0 ]]; then
    printf '%s\n' '请使用管理员权限运行：sudo bash install-offline.sh' >&2
    exit 1
fi
for cmd in apt-get dpkg dpkg-query sha256sum mktemp; do
    command -v "$cmd" >/dev/null || { printf '缺少系统工具：%s\n' "$cmd" >&2; exit 1; }
done
[[ -f "$BUNDLE/SHA256SUMS" ]] || { echo '缺少 SHA256SUMS，安装包不完整。' >&2; exit 1; }
(cd -- "$BUNDLE" && sha256sum --check --strict SHA256SUMS) || { echo '文件校验失败，未开始安装。' >&2; exit 1; }
ARCH=$(tr -d '\r\n' < "$BUNDLE/architecture")
VERSION=$(tr -d '\r\n' < "$BUNDLE/version")
[[ $ARCH == "$(dpkg --print-architecture)" ]] || { echo '安装包与本机 CPU 架构不匹配。' >&2; exit 1; }
[[ $VERSION =~ ^[0-9][A-Za-z0-9.+:~_-]*$ ]] || { echo '安装包版本格式无效。' >&2; exit 1; }
# Do not source a transported os-release file as shell code.
read_os_value() { sed -n "s/^$2=//p" "$1" | head -n 1 | tr -d '\"'; }
for key in ID VERSION_ID; do
    [[ $(read_os_value /etc/os-release "$key") == "$(read_os_value "$BUNDLE/build-os-release" "$key")" ]] || { echo '系统发行版或版本与构建环境不同，请重新准备兼容离线包。' >&2; exit 1; }
done
AUDIT=$(dpkg --audit)
if [[ -n "$AUDIT" ]]; then
    printf '%s\n' "$AUDIT" >&2
    echo '系统已有未完成的软件安装，请先处理。' >&2
    exit 1
fi
WORK=$(mktemp -d /tmp/kylin-scan-install.XXXXXXXX)
trap 'printf "安装诊断记录保留在：%s\n" "$WORK"' EXIT
mkdir -p "$WORK/lists/partial" "$WORK/cache/archives/partial" "$WORK/empty-sources" "$WORK/empty-config"
: > "$WORK/empty-main.conf"
# apt is constrained to this file:// repository. No configured network source,
# proxy, or global APT hook is loaded. Spaces in source paths are URI encoded.
URI=${BUNDLE//%/%25}
URI=${URI// /%20}
printf 'deb [trusted=yes] file:%s ./\n' "$URI" > "$WORK/sources.list"
cat > "$WORK/apt.conf" <<EOF
Dir::Etc::parts "$WORK/empty-config";
Dir::Etc::main "$WORK/empty-main.conf";
Dir::Etc::sourcelist "$WORK/sources.list";
Dir::Etc::sourceparts "$WORK/empty-sources";
Dir::State::lists "$WORK/lists";
Dir::Cache "$WORK/cache";
APT::Sandbox::User "root";
APT::Install-Recommends "false";
APT::Install-Suggests "false";
Acquire::Languages "none";
EOF
export APT_CONFIG="$WORK/apt.conf"
apt-get update > "$WORK/update.log" 2>&1 || { cat "$WORK/update.log" >&2; exit 1; }
apt-get --simulate --no-remove install "kylin-document-scanner=$VERSION" > "$WORK/simulate.log" 2>&1 || { cat "$WORK/simulate.log" >&2; echo '依赖无法离线满足，未开始安装。' >&2; exit 1; }
if grep -q '^Remv ' "$WORK/simulate.log"; then
    echo '安装将移除现有软件，已停止。' >&2
    exit 1
fi
# Only the application itself may replace an installed version. A base-system
# upgrade/downgrade signals a mismatched preparation host, and must be rebuilt.
if awk '/^Inst / && $3 ~ /^\[/ && $2 !~ /^kylin-document-scanner(:[^ ]+)?$/ { found=1; print } END { exit !found }' "$WORK/simulate.log"; then
    echo '依赖将升级或降级现有系统软件；请在匹配系统上重新制作离线包。' >&2
    exit 1
fi
cat "$WORK/simulate.log"
if $DRY_RUN; then
    printf '%s\n' '离线包校验和依赖模拟通过；未安装或修改系统软件。'
    exit 0
fi
apt-get --yes --no-remove install "kylin-document-scanner=$VERSION" 2>&1 | tee "$WORK/install.log"
[[ $(dpkg-query -W -f='${Status}' kylin-document-scanner) == 'install ok installed' ]] || { echo '软件包状态异常，安装未完成。' >&2; exit 1; }
tesseract --list-langs 2>&1 | tee "$WORK/languages.log"
grep -qx 'chi_sim' "$WORK/languages.log" && grep -qx 'eng' "$WORK/languages.log" || { echo 'OCR 模型不完整，请检查离线包。' >&2; exit 1; }
printf '%s\n' '安装完成。请以普通用户从应用菜单启动“文档高拍工具”。'
