#!/usr/bin/env bash
# Write a Starbite BOOT image from the phone's running Linux, like
# tools/twrp-flash-boot.sh does from TWRP: the phone fetches the image over
# the USB network, checks the Android boot header and the partition size, and
# only sda and BOOT are made writable for the dd. Reads it back. RECOVERY
# (TWRP) is never touched, so a bad BOOT still has a way back.
#   tools/linux/boot-flash-linux.sh out/xnu-boot-linux3.img
set -euo pipefail
cd "$(dirname "$0")/../.."
img=$1
P=tools/linux/phone.py
want=$(shasum -a 256 "$img" | cut -c1-64)
size=$(stat -f %z "$img")
[ "$(head -c 8 "$img")" = "ANDROID!" ] || { echo "not an Android boot image"; exit 1; }
part=$($P 'for d in /sys/class/block/sda*; do grep -q "^PARTNAME=BOOT$" $d/uevent 2>/dev/null && echo ${d##*/}; done; true' | tr -d '\r\n')
[ -n "$part" ] || { echo "no BOOT partition"; exit 1; }
dev=/dev/$part
psize=$($P "blockdev --getsize64 $dev" | tr -d '\r\n')
[ "$size" -le "$psize" ] || { echo "image larger than BOOT ($psize)"; exit 1; }
echo "BOOT = $dev ($psize bytes)"
python3 -m http.server 8642 --bind 172.16.42.2 --directory "$(dirname "$img")" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null' EXIT
sleep 1
got=$($P "rm -f /tmp/boot.img; wget -q -O /tmp/boot.img http://172.16.42.2:8642/$(basename "$img"); sha256sum /tmp/boot.img | cut -c1-64" | tr -d '\r\n')
[ "$got" = "$want" ] || { echo "transfer mismatch"; exit 1; }
disk=${dev%%[0-9]*}
$P "blockdev --setrw $disk && blockdev --setrw $dev && dd if=/tmp/boot.img of=$dev bs=1048576 conv=fsync 2>/dev/null; r=\$?; sync; blockdev --setro $dev; blockdev --setro $disk; echo dd \$r ro \$(blockdev --getro $disk)\$(blockdev --getro $dev)"
back=$($P "head -c $size $dev | sha256sum | cut -c1-64" | tr -d '\r\n')
[ "$back" = "$want" ] || { echo "BOOT readback mismatch: reflash from TWRP"; exit 1; }
echo "BOOT written from Linux (${want:0:16})"
