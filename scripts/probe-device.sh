#!/bin/sh
# Read-only diagnostic report. No installation, privilege escalation, or user-file reads.
# Usage: sh probe-device.sh > device-report.txt 2>&1
set -u
export LC_ALL=C

section() { printf '\n===== %s =====\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }
run_limited() {
    if have timeout; then timeout 15 "$@"; else "$@"; fi
}

section 'System architecture and release'
uname -m
uname -r
if [ -r /etc/os-release ]; then cat /etc/os-release; fi
if have dpkg; then dpkg --print-architecture; fi
if have ldd; then ldd --version 2>&1 | head -n 1; fi

section 'Installed runtime package versions'
if have dpkg-query; then
    dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' \
        libc6 libstdc++6 libqt5core5a libqt5widgets5 libqt5gui5 \
        libopencv-core* libopencv-imgproc* libopencv-imgcodecs* \
        tesseract-ocr tesseract-ocr-chi-sim tesseract-ocr-eng v4l-utils 2>/dev/null || :
else
    printf 'dpkg-query is not installed.\n'
fi

section 'Required tools'
for tool in v4l2-ctl lsusb tesseract cmake c++ pkg-config; do
    if have "$tool"; then command -v "$tool"; else printf '%s: not installed\n' "$tool"; fi
done
if have tesseract; then
    run_limited tesseract --version 2>&1 | head -n 2
    run_limited tesseract --list-langs 2>&1
fi

section 'Deli USB device 2e5a:1719'
if have lsusb; then
    run_limited lsusb -d 2e5a:1719
else
    printf 'lsusb is not installed; checking sysfs instead.\n'
fi
found_usb=0
for node in /sys/bus/usb/devices/*; do
    [ -r "$node/idVendor" ] && [ -r "$node/idProduct" ] || continue
    vendor=$(cat "$node/idVendor")
    product=$(cat "$node/idProduct")
    if [ "$vendor:$product" = '2e5a:1719' ]; then
        found_usb=1
        printf 'USB device: %s (%s:%s)\n' "${node##*/}" "$vendor" "$product"
        if [ -r "$node/product" ]; then cat "$node/product"; fi
        if [ -r "$node/speed" ]; then printf 'USB speed Mbps: '; cat "$node/speed"; fi
    fi
done
if [ "$found_usb" -eq 0 ]; then printf 'Target USB ID was not found.\n'; fi

section 'Camera kernel module'
if [ -d /sys/module/uvcvideo ]; then printf 'uvcvideo module is present.\n'; else printf 'uvcvideo module not visible in sysfs.\n'; fi

section 'Video devices and access'
found_video=0
for node in /sys/class/video4linux/video*; do
    [ -e "$node" ] || continue
    found_video=1
    name=${node##*/}
    device=/dev/$name
    printf '\n--- %s ---\n' "$device"
    if [ -r "$node/name" ]; then cat "$node/name"; fi
    if [ -e "$device" ]; then
        ls -l "$device"
        if [ -r "$device" ]; then printf 'Current user: read=yes '; else printf 'Current user: read=no '; fi
        if [ -w "$device" ]; then printf 'write=yes\n'; else printf 'write=no\n'; fi
    fi
    if have readlink; then readlink -f "$node/device/driver" 2>/dev/null || :; fi
    if have v4l2-ctl; then
        run_limited v4l2-ctl --device "$device" --all 2>&1
        run_limited v4l2-ctl --device "$device" --list-formats-ext 2>&1
        run_limited v4l2-ctl --device "$device" --list-ctrls 2>&1
    fi
done
if [ "$found_video" -eq 0 ]; then printf 'No video nodes found.\n'; fi

section 'Diagnostic limits'
printf '%s\n' \
    'This report does not open a video stream or change controls.' \
    'A successful video capture on the target computer is still required.' \
    'Missing tools are reported only; this script never installs packages.'
