#!/usr/bin/env bash
# Write a BOOT image through TWRP's adb (phone in TWRP), verify it, reboot.
# Avoids Download mode and its one-time RECOVERY boot after a flash.
#   tools/twrp-flash-boot.sh [out/xnu-boot-root.img]
set -euo pipefail
cd "$(dirname "$0")/.."
img=${1:-out/xnu-boot-root.img}
n=$(stat -f %z "$img")
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
adb push "$img" /tmp/boot.img >/dev/null
adb shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=1048576 conv=fsync 2>/dev/null; sync"
dev=$(adb shell "dd if=/dev/block/by-name/BOOT bs=1048576 2>/dev/null | head -c $n | sha256sum" | cut -c1-64)
loc=$(shasum -a 256 "$img" | cut -c1-64)
[ "$dev" = "$loc" ] || { echo "BOOT readback mismatch: $dev vs $loc"; exit 1; }
echo "BOOT verified (${loc:0:16}), rebooting"
adb reboot
