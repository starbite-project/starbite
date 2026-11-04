#!/usr/bin/env bash
# Write a version 2 OS store (tools/store/sbstore2.py build) to USERDATA +16 GiB from TWRP, keeping
# the phone's settings record (block 2). Refuses unless the iOS root (HFS+) ends by 16 GiB, the
# store's capacity is what USERDATA leaves after 16 GiB, and the target holds a store or zeros.
# Kiosk's root at +17 GiB is retired: a store may grow over it, but not into the last 1025 blocks
# (iOS 26's herolog ring). Checked by a SHA-256 on the phone.
#   tools/store/store-flash.sh build/store/ufs-store.img
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-build/store/ufs-store.img}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
dev=$(adb shell readlink -f /dev/block/by-name/USERDATA | tr -d '\r')
[ -n "$dev" ] || { echo "no USERDATA"; exit 1; }
STORE=4194304           # 16 GiB in 4 KiB blocks
rd() { adb exec-out "dd if=$dev bs=4096 skip=$1 count=${2:-1} 2>/dev/null"; }

psize=$(adb shell "cat /sys/class/block/${dev##*/}/size" | tr -d '\r')
cap=$(( psize / 8 - STORE ))
read -r want_cap used < <(python3 - "$img" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(8192)
assert d[4096:4104] == b'SBSTORE2', 'not a version 2 store'
_, _, _, cap, used = struct.unpack_from('<8sIIQQ', d, 4096)
print(cap, used)
PY
)
[ "$want_cap" = "$cap" ] || { echo "store capacity $want_cap, USERDATA leaves $cap blocks: rebuild with --capacity $cap"; exit 1; }
# The iOS 26 kernel (rd=ufsroot0 ufsrw=1) writes its herolog ring into the last 1025 blocks of
# USERDATA (bsd/dev/herolte_ufs.c: 4 MiB ring + header block); the store must end before it.
HEROLOG=1025
[ "$used" -le $(( cap - HEROLOG )) ] || { echo "store needs $used blocks: past $(( cap - HEROLOG )) it would overlap the USERDATA herolog ring: refusing"; exit 1; }

# The Apple slot (USERDATA's first 16 GiB) must end before the store: either the old iOS 26 root (HFS+ volume
# header at byte 1024) or iOS 16's (an earlier port's) system APFS container at block 0, whose 8 GiB SBIOSSYS part it fills
# at most (the HFSX /var volume behind it, SBIOSVAR, ends at 16 GiB by construction: tools/refport/slot-write.py).
python3 - <(rd 0) <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
b = d[1024:1024 + 512]
if b[:2] in (b'H+', b'HX'):
    bs, total = struct.unpack('>II', b[40:48])
    assert bs * total <= 16 << 30, f'HFS+ volume ends at {bs * total} > 16 GiB'
    print(f'Apple slot: HFS+ {b[:2].decode()} ends at {bs * total / 2**30:.2f} GiB')
elif d[32:36] == b'NXSB':
    bs, count = struct.unpack_from('<IQ', d, 36)
    assert bs * count <= 8 << 30, f'APFS container ends at {bs * count} > 8 GiB (SBIOSSYS)'
    print(f'Apple slot: iOS 16 APFS system, {bs * count / 2**30:.2f} GiB (SBIOSSYS 8 GiB)')
else:
    sys.exit(f'Apple slot holds neither an HFS+ root nor the iOS 16 APFS system (signature {b[:2]!r}): refusing')
PY

# Target: a store (either version) or zeros.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rd $STORE 3 > "$tmp/head"
python3 - "$tmp/head" "$img" "$tmp/store.img" <<'PY'
import struct, sys, zlib
head = open(sys.argv[1], 'rb').read()
v1, v2, st = head[:8], head[4096:4104], head[8192:12288]
if v1 not in (b'SBSTORE1', bytes(8)) and v2 != b'SBSTORE2':
    sys.exit(f'store area holds unknown data ({v1.hex()}): refusing')
if v1 == bytes(8) and v2 != b'SBSTORE2' and any(head):
    sys.exit('store area not empty: refusing')
img = bytearray(open(sys.argv[2], 'rb').read())
if v2 == b'SBSTORE2' and st[:8] == b'SBSETTNG' and zlib.crc32(st[16:256]) == struct.unpack_from('<I', st, 12)[0]:
    img[8192:12288] = st
    keys = [st[o:o + 24].split(b'\0')[0].decode() or '-' for o in (32, 56, 80)]
    print(f'settings kept: default {keys[0]}, last {keys[1]}, next {keys[2]}')
else:
    print('no settings record on the phone: defaults')
open(sys.argv[3], 'wb').write(img)
PY
blocks=$(( ($(stat -f %z "$tmp/store.img") + 4095) / 4096 ))
[ "$blocks" = "$used" ] || { echo "image is $blocks blocks, header says $used"; exit 1; }

adb push "$tmp/store.img" /tmp/sbstore2.img >/dev/null
adb shell "dd if=/tmp/sbstore2.img of=$dev bs=4096 seek=$STORE conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/sbstore2.img"
back=$(adb shell "dd if=$dev bs=4096 skip=$STORE count=$blocks 2>/dev/null | sha256sum" | cut -c1-64)
want=$(shasum -a 256 "$tmp/store.img" | cut -c1-64)
[ "$back" = "$want" ] || { echo "store readback mismatch"; exit 1; }
echo "OS store v2 written at USERDATA +16 GiB (${want:0:16}), $blocks of $cap blocks"
