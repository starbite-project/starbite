#!/usr/bin/env bash
# Write a version 2 OS store (tools/store/sbstore2.py build --medium sd) to the microSD's partition 2
# (MBR type 0xDA, raw: no filesystem) from TWRP, keeping the card's settings block if it has one.
# Refuses unless partition 2 is type 0xDA, the store's capacity is the partition's size in 4 KiB
# blocks, and the partition holds a store or zeros at its start. SHA-256 readback on the phone.
# The loader reads the card store only with the settings flag on (sbstore2.py settings --sd on, or
# Engineer > Storage > Card store at boot).
#   tools/store/store-flash-sd.sh build/store/sd-store.img
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-build/store/sd-store.img}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
dev=/dev/block/mmcblk0p2
adb shell "[ -b $dev ]" || { echo "no $dev (card not in?)"; exit 1; }
# MBR entry 2 must be type 0xDA
type=$(adb exec-out "dd if=/dev/block/mmcblk0 bs=1 skip=$((446 + 16 + 4)) count=1 2>/dev/null" | xxd -p)
[ "$type" = da ] || { echo "partition 2 is type $type, not da: refusing"; exit 1; }
psize=$(adb shell "cat /sys/class/block/mmcblk0p2/size" | tr -d '\r')
pstart=$(adb shell "cat /sys/class/block/mmcblk0p2/start" | tr -d '\r')
[ $((pstart % 8)) = 0 ] || { echo "partition 2 starts at sector $pstart, not 4 KiB aligned"; exit 1; }
cap=$(( psize / 8 ))
read -r want_cap used < <(python3 - "$img" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(8192)
assert d[4096:4104] == b'SBSTORE2', 'not a version 2 store'
_, _, _, cap, used = struct.unpack_from('<8sIIQQ', d, 4096)
print(cap, used)
PY
)
[ "$want_cap" = "$cap" ] || { echo "store capacity $want_cap, partition 2 has $cap blocks: rebuild with --capacity $cap"; exit 1; }
# A compact image (sbstore2.py --data-at: the card home store) is NOT laid out as on the card: writing it
# from block 0 would put its data over the iOS 26 root. tools/store/card-store.sh writes those.
if [ "$(dd if="$img" bs=1 skip=$((4096 + 64)) count=8 2>/dev/null)" = SBSTHOME ]; then
    echo "$img is a home / compact card store (SBSTHOME): use tools/store/card-store.sh flash"; exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb exec-out "dd if=$dev bs=4096 count=3 2>/dev/null" > "$tmp/head"
python3 - "$tmp/head" "$img" "$tmp/store.img" <<'PY'
import struct, sys, zlib
head = open(sys.argv[1], 'rb').read()
v1, v2, st = head[:8], head[4096:4104], head[8192:12288]
if v1 not in (b'SBSTORE1', bytes(8)) and v2 != b'SBSTORE2':
    sys.exit(f'partition 2 holds unknown data ({v1.hex()}): refusing')
if v2 != b'SBSTORE2' and any(head):
    sys.exit('partition 2 not empty: refusing')
if v2 == b'SBSTORE2' and head[4096 + 64:4096 + 72] == b'SBSTHOME':
    sys.exit('partition 2 holds the HOME card store (SBSTHOME): use tools/store/card-store.sh flash: refusing')
img = bytearray(open(sys.argv[2], 'rb').read())
if v2 == b'SBSTORE2' and st[:8] == b'SBSETTNG' and zlib.crc32(st[16:256]) == struct.unpack_from('<I', st, 12)[0]:
    img[8192:12288] = st
open(sys.argv[3], 'wb').write(img)
PY
blocks=$(( ($(wc -c < "$tmp/store.img") + 4095) / 4096 ))   # wc -c: macOS and Linux
[ "$blocks" = "$used" ] || { echo "image is $blocks blocks, header says $used"; exit 1; }
adb push "$tmp/store.img" /tmp/sdstore.img >/dev/null
adb shell "dd if=/tmp/sdstore.img of=$dev bs=4096 conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/sdstore.img"
back=$(adb shell "dd if=$dev bs=4096 count=$blocks 2>/dev/null | sha256sum" | cut -c1-64)
want=$(shasum -a 256 "$tmp/store.img" | cut -c1-64)
[ "$back" = "$want" ] || { echo "card store readback mismatch"; exit 1; }
echo "card OS store written to $dev (${want:0:16}), $blocks of $cap blocks"
