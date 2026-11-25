#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# != 2 ]]; then
    echo "Usage: bash $0 /absolute/path/app.deb /absolute/path/new-bundle-directory" >&2
    exit 2
fi
for cmd in apt-get dpkg dpkg-deb sha256sum realpath gzip; do
    command -v "$cmd" >/dev/null || { echo "Missing tool: $cmd" >&2; exit 1; }
done
DEB=$(realpath -- "$1")
OUT=$(realpath -m -- "$2")
[[ -f "$DEB" ]] || { echo 'Application DEB not found.' >&2; exit 1; }
[[ ! -e "$OUT" ]] || { echo 'Output directory must not already exist.' >&2; exit 1; }
ARCH=$(dpkg --print-architecture)
PKG=$(dpkg-deb -f "$DEB" Package)
VERSION=$(dpkg-deb -f "$DEB" Version)
[[ $PKG == kylin-document-scanner ]] || { echo 'Unexpected package name.' >&2; exit 1; }
[[ $(dpkg-deb -f "$DEB" Architecture) == "$ARCH" ]] || { echo 'Build host and DEB architectures differ.' >&2; exit 1; }
mkdir -p -- "$OUT/packages/partial" "$OUT/.apt/lists/partial"
cp -- "$DEB" "$OUT/packages/${PKG}_${VERSION}_${ARCH}.deb"
# An empty dpkg status forces a complete dependency closure, including dependencies
# already installed on this preparation machine. APT state stays inside OUT.
APT_OPTS=(-o "Dir::State::lists=$OUT/.apt/lists" -o "Dir::Cache::archives=$OUT/packages" -o "Dir::Cache::pkgcache=$OUT/.apt/pkgcache.bin" -o "Dir::Cache::srcpkgcache=$OUT/.apt/srcpkgcache.bin" -o 'Dir::State::status=/dev/null' -o 'Debug::NoLocking=1' -o "APT::Sandbox::User=$(id -un)" -o 'APT::Install-Recommends=false' -o 'APT::Install-Suggests=false')
apt-get "${APT_OPTS[@]}" update
apt-get "${APT_OPTS[@]}" --download-only --yes install "$DEB"
cp -- "$ROOT/scripts/install-offline.sh" "$OUT/install-offline.sh"
mkdir -p -- "$OUT/docs"
cp -- "$ROOT/README.md" "$OUT/README.md"
cp -- "$ROOT"/docs/*.md "$OUT/docs/"
cp -- /etc/os-release "$OUT/build-os-release"
printf '%s\n' "$ARCH" > "$OUT/architecture"
printf '%s\n' "$VERSION" > "$OUT/version"
dpkg-query -W -f='${Version}\n' libc6 > "$OUT/build-libc-version"
# Build a flat, local APT repository without requiring dpkg-dev on the target.
: > "$OUT/Packages"
for deb in "$OUT"/packages/*.deb; do
    filename=$(basename -- "$deb")
    [[ $filename != *$'\n'* ]] || { echo 'Invalid package filename.' >&2; exit 1; }
    {
        # dpkg-deb may end the control output with a blank line. Remove it so
        # Filename/Size/SHA256 remain in the same RFC822 package stanza.
        control=$(dpkg-deb -f "$deb")
        printf '%s\n' "$control"
        printf 'Filename: ./packages/%s\n' "$filename"
        printf 'Size: %s\n' "$(stat -c %s "$deb")"
        printf 'SHA256: %s\n\n' "$(sha256sum "$deb" | cut -d ' ' -f 1)"
    } >> "$OUT/Packages"
done
awk 'BEGIN { RS=""; FS="\n" } { p=0; f=0; h=0; for(i=1;i<=NF;i++){if($i~/^Package: /)p++;if($i~/^Filename: /)f++;if($i~/^SHA256: /)h++} if(p!=1 || f!=1 || h!=1)bad=1 } END { exit bad }' "$OUT/Packages" || { echo 'Invalid local package index.' >&2; exit 1; }
gzip -n -c -- "$OUT/Packages" > "$OUT/Packages.gz"
(cd -- "$OUT" && sha256sum architecture version build-os-release build-libc-version install-offline.sh README.md docs/*.md Packages Packages.gz packages/*.deb > SHA256SUMS)
printf 'Offline bundle prepared at: %s\n' "$OUT"
printf '%s\n' 'Verify this bundle on a clean, matching Kylin machine without network access before release.'
