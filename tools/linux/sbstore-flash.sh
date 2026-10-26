#!/usr/bin/env bash
# Write the OS store (tools/linux/sbstore.py) into USERDATA at +16 GiB from
# TWRP, between the iOS root (HFS+, must end by 16 GiB) and the Kiosk root
# (ext4 at +17 GiB, must be intact before and after). Refuses unless the
# target is empty or an older store. Reads the write back.
#   tools/linux/sbstore-flash.sh build/linux/sbstore.img
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-build/linux/sbstore.img}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
dev=$(adb shell readlink -f /dev/block/by-name/USERDATA | tr -d '\r')
[ -n "$dev" ] || { echo "no USERDATA"; exit 1; }
STORE=4194304           # 16 GiB in 4 KiB blocks
KIOSK=4456448          # 17 GiB
size=$(stat -f %z "$img")
blocks=$(( (size + 4095) / 4096 ))
[ $((STORE + blocks)) -le $KIOSK ] || { echo "store larger than the 1 GiB gap"; exit 1; }

rd() { adb exec-out "dd if=$dev bs=4096 skip=$1 count=${2:-1} 2>/dev/null"; }
psize=$(adb shell "cat /sys/class/block/${dev##*/}/size" | tr -d '\r')
[ $((psize / 8)) -gt $((KIOSK + 262144)) ] || { echo "USERDATA too small ($psize sectors)"; exit 1; }

# iOS root: HFS+ volume header at byte 1024, must end by 16 GiB.
python3 - <(rd 0) <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()[1024:1024 + 512]
sig = b[:2]
assert sig in (b'H+', b'HX'), f'no HFS+ volume at USERDATA (signature {sig!r})'
bs, total = struct.unpack('>II', b[40:48])
end = bs * total
assert end <= 16 << 30, f'HFS+ volume ends at {end} > 16 GiB'
print(f'iOS root: HFS+ {sig.decode()} ends at {end / 2**30:.2f} GiB')
PY
kiosk_ok() { rd $KIOSK | python3 -c 'import sys; b=sys.stdin.buffer.read(); sys.exit(0 if b[1024+56:1024+58]==b"\x53\xef" else 1)'; }
kiosk_ok || { echo "no ext4 superblock at +17 GiB: refusing"; exit 1; }
echo "Kiosk root: ext4 at +17 GiB intact"

# Target: zeros or an earlier store.
first=$(rd $STORE | head -c 8 | xxd -p)
case $first in
534253544f524531|0000000000000000) ;;
*) echo "store area holds unknown data ($first): refusing"; exit 1 ;;
esac
if [ "$first" = 0000000000000000 ]; then
	nz=$(rd $STORE $blocks | python3 -c 'import sys; print(sum(1 for x in sys.stdin.buffer.read() if x))')
	[ "$nz" = 0 ] || { echo "store area not empty ($nz nonzero bytes): refusing"; exit 1; }
fi

adb push "$img" /tmp/sbstore.img >/dev/null
adb shell "dd if=/tmp/sbstore.img of=$dev bs=4096 seek=$STORE conv=notrunc,fsync 2>/dev/null; sync"
back=$(rd $STORE $blocks | head -c "$size" | shasum -a 256 | cut -c1-64)
want=$(shasum -a 256 "$img" | cut -c1-64)
[ "$back" = "$want" ] || { echo "store readback mismatch"; exit 1; }
kiosk_ok || { echo "WARNING: Kiosk superblock changed"; exit 1; }
echo "OS store written at USERDATA +16 GiB (${want:0:16}), $blocks blocks; Kiosk intact"
