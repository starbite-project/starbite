#!/usr/bin/env bash
# Queue files for iOS 26's root on the microSD card (kernel 0401, bsd/dev/starlte_overlay.c): the
# overlay (tools/overlay/sboverlay.py format) goes to the card's overlay area, sector 249933824 = the
# upper half of the sdlog range on the 128 GB card (nothing else writes there on a normal boot). At the
# next iOS 26 boot the kernel writes each file into the root before launchd starts, once per overlay
# (stamp /private/var/db/starbite-overlay-sd). Phone in TWRP.
#   tools/overlay/card-overlay.sh OVERLAY.bin     write + read back
#   tools/overlay/card-overlay.sh --clear         zero the header block (no overlay)
# Build one:  python3 tools/overlay/sboverlay.py build/ios/card-ovl.bin LOCAL=/ios/path[:MODE] ...
set -euo pipefail
cd "$(dirname "$0")/../.."
FIRST=249933824 MAXSECT=65535
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
[ "$(adb shell cat /sys/block/mmcblk0/size | tr -d '\r')" = 249999360 ] || { echo "not the 128 GB card (sd128 layout): refusing"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
if [ "${1:-}" = --clear ]; then
    head -c 4096 /dev/zero > "$tmp/ovl.bin"
else
    src=${1:?usage: card-overlay.sh OVERLAY.bin | --clear}
    case "$(head -c 8 "$src")" in SBOVL001|SBOVL002) ;; *) echo "$src: not an SBOVL overlay"; exit 1 ;; esac
    cp "$src" "$tmp/ovl.bin"
    n=$(stat -f %z "$tmp/ovl.bin")
    pad=$(( (512 - n % 512) % 512 )); [ $pad -eq 0 ] || head -c $pad /dev/zero >> "$tmp/ovl.bin"
fi
sect=$(( $(stat -f %z "$tmp/ovl.bin") / 512 ))
[ $sect -le $MAXSECT ] || { echo "overlay is $sect sectors, the area holds $MAXSECT"; exit 1; }
adb push "$tmp/ovl.bin" /tmp/sbovl.bin >/dev/null
adb shell "dd if=/tmp/sbovl.bin of=/dev/block/mmcblk0 bs=512 seek=$FIRST count=$sect conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/sbovl.bin"
back=$(adb exec-out "dd if=/dev/block/mmcblk0 bs=512 skip=$FIRST count=$sect 2>/dev/null" | shasum -a 256 | cut -c1-64)
want=$(shasum -a 256 "$tmp/ovl.bin" | cut -c1-64)
[ "$back" = "$want" ] || { echo "readback mismatch ($back != $want)"; exit 1; }
echo "card overlay: $sect sectors at $FIRST, read back ok (sha256 $want)"
