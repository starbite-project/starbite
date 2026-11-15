#!/usr/bin/env bash
# The home OS store on the microSD card (docs/STORE-ON-CARD.md), from TWRP. Card p2 (MBR type 0xDA):
# the store's blocks 0..6 (v1 view, header with SBSTHOME, settings, directory) at p2's start, its data
# in the layout's store data area (sd128: p2 block 8126720.., 961 MiB). Images are the COMPACT ones
# sbstore2.py builds with --home --data-at layout (file blocks 0..6, then the data).
#
#   card-store.sh migrate WORKDIR       pull the live UFS store (USERDATA +16 GiB) and its settings record,
#                                       build the home card store with the same entries (CRC-compared one by
#                                       one), the UFS settings record as the card's (card store flag on),
#                                       write it to the card, read everything back. Writes only card p2.
#   card-store.sh flash IMG             update the home card store: IMG built with --import of a fresh
#                                       `store-pull.sh OUT sd` (same capacity and data block); the card's own
#                                       settings record is kept; only changed 16 MiB pieces are written.
#   card-store.sh verify IMG            card == IMG, piece by piece (SHA-256), read only.
#   card-store.sh settings [OPTIONS]    the card store's settings record: shown, or changed with sbstore2.py
#                                       settings options (e.g. --next '' clears a one-shot next), read back.
#   card-store.sh release --i-mean-it   USERDATA is given to iOS: the OLD UFS store's blocks 0..6 (views,
#                                       header, settings, directory) are saved to WORKDIR-less ./build/store/
#                                       and zeroed, so no loader, Engineer OS, Starbite OS or tool finds a
#                                       store there again. Refuses unless the card holds a home store whose
#                                       entries all pass their CRCs and BOOT's loader has sbstore=card.
#
# Every write: data pieces first, the 7 head blocks last (until then the card's old header stays valid),
# each piece pushed, written with conv=fsync and read back by SHA-256 on the phone. Pieces are 16 MiB.
set -euo pipefail
cd "$(dirname "$0")/../.."
SB=tools/store/sbstore2.py
PIECE=4096                       # blocks of 4 KiB per piece (16 MiB)
UFS_STORE=4194304                # the old UFS store: USERDATA +16 GiB, in 4 KiB blocks
dev=/dev/block/mmcblk0p2

die() { echo "card-store: $*" >&2; exit 1; }
twrp() { [ "$(adb get-state 2>/dev/null)" = recovery ] || die "phone not in TWRP"; }
psh() { adb shell "$1" | tr -d '\r'; }
sha_host() { dd if="$1" bs=4096 skip="$2" count="$3" 2>/dev/null | shasum -a 256 | cut -c1-64; }
sha_card() { psh "dd if=$dev bs=4096 skip=$1 count=$2 2>/dev/null | sha256sum" | cut -c1-64; }

# The card: p2 type 0xDA, its geometry from cardlayout.py; prints "cap data_lo data_hi layout".
card_geometry() {
    [ "$(psh "[ -b $dev ] && echo yes")" = yes ] || die "no $dev (card not in?)"
    local type sectors start size
    type=$(adb exec-out "dd if=/dev/block/mmcblk0 bs=1 skip=$((446 + 16 + 4)) count=1 2>/dev/null" | xxd -p)
    [ "$type" = da ] || die "partition 2 is type $type, not da"
    sectors=$(psh "cat /sys/block/mmcblk0/size")
    start=$(psh "cat /sys/class/block/mmcblk0p2/start")
    size=$(psh "cat /sys/class/block/mmcblk0p2/size")
    python3 -I -B - "$sectors" "$start" "$size" "$PWD/tools/store" <<'PY'
import sys
sys.path.insert(0, sys.argv[4])
import cardlayout as cl
sectors, start, size = map(int, sys.argv[1:4])
name = cl.for_card(sectors)
lay = cl.get(name) if name else None
if lay is None or (lay.p2_start, lay.p2_sectors) != (start, size):
    sys.exit(f'card p2 {start}+{size} ({sectors} sectors) matches no card layout')
d = lay.store_data
if d is None:
    sys.exit(f'the {lay.name} layout has no store data area')
lo = (d.start - start) // cl.BLOCK_SECTORS
print(size // cl.BLOCK_SECTORS, lo, lo + d.sectors // cl.BLOCK_SECTORS, lay.name)
PY
}

# "cap used data_at home" of a compact image; exits unless it is a valid home store.
img_info() {
    python3 -I -B - "$1" "$PWD/$SB" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sbstore2', sys.argv[2]); m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
d = open(sys.argv[1], 'rb').read(8192)
n, cap, used, _ = m.check_header(d[4096:4096 + 52], sys.argv[1])
flags, data_at = m.home_ext(d[4096:8192])
if not flags & m.HOME_FLAG:
    sys.exit(f'{sys.argv[1]}: not a home store (sbstore2.py build --home --data-at layout)')
m.read_store(sys.argv[1])                     # header, directory and every entry's CRC
print(cap, used, data_at, 1)
PY
}

# The card's head as it is: "none" (zeros), "plain" (a store without SBSTHOME), "home DATA_AT", else exits.
card_head() {
    local t; t=$(mktemp)
    adb exec-out "dd if=$dev bs=4096 count=3 2>/dev/null" > "$t"
    python3 -I -B - "$t" "$PWD/$SB" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sbstore2', sys.argv[2]); m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
d = open(sys.argv[1], 'rb').read()
if len(d) != 12288:
    sys.exit('short read of the card head')
if not any(d):
    print('none')
elif d[4096:4104] == b'SBSTORE2':
    m.check_header(d[4096:4096 + 52], 'card store')
    flags, data_at = m.home_ext(d[4096:8192])
    print(f'home {data_at}' if flags & m.HOME_FLAG else 'plain')
elif d[:8] == b'SBSTORE1':
    print('plain')
else:
    sys.exit(f'card p2 holds unknown data ({d[:8].hex()}): refusing')
PY
    rm -f "$t"
}

# Write a compact image: data pieces (only those that differ), then the head; read back each piece.
write_img() {
    local img=$1 used=$2 data_at=$3 n f b want back k=0 w=0
    n=$((used - data_at))                         # data blocks
    for ((f = 0; f < n; f += PIECE)); do
        b=$PIECE; [ $((f + b)) -le "$n" ] || b=$((n - f))
        want=$(sha_host "$img" $((7 + f)) "$b")
        if [ "$(sha_card $((data_at + f)) "$b")" != "$want" ]; then
            dd if="$img" bs=4096 skip=$((7 + f)) count="$b" of="$tmp/piece" 2>/dev/null
            adb push "$tmp/piece" /tmp/cs.piece >/dev/null
            psh "dd if=/tmp/cs.piece of=$dev bs=4096 seek=$((data_at + f)) conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/cs.piece"
            back=$(sha_card $((data_at + f)) "$b")
            [ "$back" = "$want" ] || die "piece at p2 block $((data_at + f)) read back wrong"
            w=$((w + 1))
        fi
        k=$((k + 1))
        printf '\r  data: %d/%d pieces checked, %d written' "$k" $(((n + PIECE - 1) / PIECE)) "$w"
    done
    echo
    want=$(sha_host "$img" 0 7)
    dd if="$img" bs=4096 count=7 of="$tmp/head" 2>/dev/null
    adb push "$tmp/head" /tmp/cs.head >/dev/null
    psh "dd if=/tmp/cs.head of=$dev bs=4096 conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/cs.head"
    [ "$(sha_card 0 7)" = "$want" ] || die "head (p2 blocks 0..6) read back wrong"
    echo "  head written (p2 blocks 0..6)"
    # the head's backup at p2 blocks 64..70 (Starbite Recovery > Repair and reset > repair the card
    # store's head, shim/starlte/repair.c RP_HEAD_BAK), only where the data starts beyond it
    if [ "$data_at" -gt 71 ]; then
        adb push "$tmp/head" /tmp/cs.head >/dev/null
        psh "dd if=/tmp/cs.head of=$dev bs=4096 seek=64 conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/cs.head"
        [ "$(sha_card 64 7)" = "$want" ] || die "head backup (p2 blocks 64..70) read back wrong"
        echo "  head backup written (p2 blocks 64..70)"
    fi
}

verify_img() {
    local img=$1 used=$2 data_at=$3 n f b bad=0
    [ "$(sha_card 0 7)" = "$(sha_host "$img" 0 7)" ] || { echo "  head differs"; bad=1; }
    n=$((used - data_at))
    for ((f = 0; f < n; f += PIECE)); do
        b=$PIECE; [ $((f + b)) -le "$n" ] || b=$((n - f))
        [ "$(sha_card $((data_at + f)) "$b")" = "$(sha_host "$img" $((7 + f)) "$b")" ] ||
            { echo "  data piece at p2 block $((data_at + f)) differs"; bad=1; }
    done
    return $bad
}

# Image vs the card's geometry: capacity = p2, data inside the store data area.
check_fit() {
    local cap=$1 used=$2 data_at=$3 geo
    geo=$(card_geometry)
    read -r gcap glo ghi glay <<< "$geo"
    [ "$cap" = "$gcap" ] || die "image capacity $cap, card p2 has $gcap blocks"
    [ "$data_at" = "$glo" ] && [ "$used" -le "$ghi" ] ||
        die "image data $data_at..$used is not inside the $glay store data area $glo..$ghi"
    echo "card: $glay layout, store data area p2 blocks $glo..$ghi; image data $data_at..$used ($(((used - data_at) * 4 / 1024)) MiB)"
}

cmd=${1:-}; shift || true
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
case "$cmd" in
migrate)
    W=${1:?usage: card-store.sh migrate WORKDIR}
    mkdir -p "$W"
    twrp
    head=$(card_head)
    case "$head" in
    none|plain) echo "card head: $head (replaced by the home store's head)" ;;
    home*) die "the card already holds a home store: use card-store.sh flash" ;;
    esac
    ud=$(psh "readlink -f /dev/block/by-name/USERDATA")
    [ -n "$ud" ] || die "no USERDATA"
    echo "== 1. the live UFS store"
    tools/store/store-pull.sh "$W/ufs-live.img" ufs
    adb exec-out "dd if=$ud bs=4096 skip=$((UFS_STORE + 2)) count=1 2>/dev/null" > "$W/ufs-settings.bin"
    echo "== 2. the home card store"
    read -r gcap glo ghi glay <<< "$(card_geometry)"
    python3 -I -B $SB build "$W/card-store.img" --medium sd --home --data-at layout --import "$W/ufs-live.img" | tail -1
    if python3 -I -B $SB settings-show "$W/ufs-settings.bin" >/dev/null 2>&1; then
        python3 -I -B $SB settings "$W/card-settings.bin" --from "$W/ufs-settings.bin" --sd on >/dev/null
        echo "settings: the UFS record, card store flag on"
    else
        python3 -I -B $SB settings "$W/card-settings.bin" --sd on >/dev/null
        echo "settings: no valid UFS record: defaults, card store flag on"
    fi
    dd if="$W/card-settings.bin" of="$W/card-store.img" bs=4096 seek=2 count=1 conv=notrunc 2>/dev/null
    python3 -I -B - "$W/ufs-live.img" "$W/card-store.img" "$PWD/$SB" <<'PY'
import importlib.util, sys, zlib
spec = importlib.util.spec_from_file_location('sbstore2', sys.argv[3]); m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
a = m.read_store(sys.argv[1], with_flags=True)
b = m.read_store(sys.argv[2], with_flags=True)
bad = 0
bmap = {n: (t, x, f) for n, t, x, f in b}
for n, t, x, f in a:
    if n not in bmap:
        print(f'  MISSING on the card: {n}'); bad += 1; continue
    t2, x2, f2 = bmap.pop(n)
    if (t, len(x), zlib.crc32(x), f) != (t2, len(x2), zlib.crc32(x2), f2):
        print(f'  DIFFERS: {n}'); bad += 1
extra = [n for n in bmap if n != 'crashlog']
if extra:
    print('  only on the card: ' + ' '.join(extra)); bad += 1
if bad:
    sys.exit(f'{bad} entries differ: not writing')
print(f'entries: all {len(a)} equal (name, type, size, CRC32, flags)' + ('' if 'crashlog' not in bmap else '; crashlog added'))
PY
    read -r cap used data_at _ <<< "$(img_info "$W/card-store.img")"
    check_fit "$cap" "$used" "$data_at"
    echo "== 3. write (data, then head)"
    write_img "$W/card-store.img" "$used" "$data_at"
    echo "== 4. read back"
    verify_img "$W/card-store.img" "$used" "$data_at" || die "card differs from the image"
    tools/store/store-pull.sh "$W/card-pulled.img" sd >/dev/null
    cmp -s "$W/card-pulled.img" "$W/card-store.img" || die "the card store pulled back differs from the image"
    shasum -a 256 "$W/card-store.img" | tee "$W/card-store.img.sha256"
    echo "home card store written: $(python3 -I -B $SB header "$W/card-store.img" | head -1)"
    echo "USERDATA untouched. Next: the chain test (docs/STORE-ON-CARD.md section 5)."
    ;;
flash)
    img=${1:?usage: card-store.sh flash IMG}
    twrp
    read -r cap used data_at _ <<< "$(img_info "$img")"
    head=$(card_head)
    [ "$head" = "home $data_at" ] || die "the card holds '$head', not a home store with data at $data_at (first time: migrate)"
    check_fit "$cap" "$used" "$data_at"
    cp "$img" "$tmp/store.img"
    adb exec-out "dd if=$dev bs=4096 skip=2 count=1 2>/dev/null" > "$tmp/settings"
    if python3 -I -B $SB settings-show "$tmp/settings" >/dev/null 2>&1; then
        dd if="$tmp/settings" of="$tmp/store.img" bs=4096 seek=2 count=1 conv=notrunc 2>/dev/null
        echo "settings: the card's record kept"
    else
        echo "settings: no valid record on the card: the image's"
    fi
    write_img "$tmp/store.img" "$used" "$data_at"
    verify_img "$tmp/store.img" "$used" "$data_at" || die "card differs from the image"
    echo "home card store updated ($(shasum -a 256 "$tmp/store.img" | cut -c1-16))"
    ;;
verify)
    img=${1:?usage: card-store.sh verify IMG}
    twrp
    read -r cap used data_at _ <<< "$(img_info "$img")"
    check_fit "$cap" "$used" "$data_at"
    verify_img "$img" "$used" "$data_at" && echo "card == $img" || die "card differs from $img"
    ;;
settings)
    # the card store's settings record: show it, or change it with sbstore2.py settings options (read back)
    twrp
    case "$(card_head)" in home*|plain) ;; *) die "no store on the card" ;; esac
    adb exec-out "dd if=$dev bs=4096 skip=2 count=1 2>/dev/null" > "$tmp/cur.bin"
    if [ $# = 0 ]; then python3 -I -B $SB settings-show "$tmp/cur.bin"; exit 0; fi
    from=(); python3 -I -B $SB settings-show "$tmp/cur.bin" >/dev/null 2>&1 && from=(--from "$tmp/cur.bin")
    python3 -I -B $SB settings "$tmp/new.bin" "${from[@]}" "$@" >/dev/null
    adb push "$tmp/new.bin" /tmp/cs.set >/dev/null
    psh "dd if=/tmp/cs.set of=$dev bs=4096 seek=2 count=1 conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/cs.set"
    [ "$(sha_card 2 1)" = "$(sha_host "$tmp/new.bin" 0 1)" ] || die "settings read back wrong"
    python3 -I -B $SB settings-show "$tmp/new.bin"
    ;;
release)
    [ "${1:-}" = --i-mean-it ] || die "usage: card-store.sh release --i-mean-it (gives USERDATA to iOS)"
    twrp
    head=$(card_head)
    case "$head" in home*) ;; *) die "the card holds '$head', not a home store: nothing released" ;; esac
    echo "== 1. the home card store, every entry's CRC"
    tools/store/store-pull.sh "$tmp/card.img" sd | tail -2
    echo "== 2. BOOT's loader has sbstore=card"
    boot=$(psh "readlink -f /dev/block/by-name/BOOT")
    adb exec-out "dd if=$boot bs=1048576 2>/dev/null" > "$tmp/boot.img"
    grep -a -q ' sbstore=card' "$tmp/boot.img" || die "BOOT has no sbstore=card: flash the card-only BOOT first"
    rm -f "$tmp/boot.img" "$tmp/card.img"
    echo "== 3. the old UFS store's blocks 0..6"
    ud=$(psh "readlink -f /dev/block/by-name/USERDATA")
    mkdir -p build/store
    save=build/store/ufs-store-head-$(date +%Y%m%d-%H%M%S).bin
    adb exec-out "dd if=$ud bs=4096 skip=$UFS_STORE count=7 2>/dev/null" > "$save"
    [ "$(wc -c < "$save" | tr -d ' ')" = 28672 ] || die "short read of the old store head"
    echo "saved to $save ($(shasum -a 256 "$save" | cut -c1-16))"
    # (cmp, not "od | grep -q": under pipefail grep's early exit SIGPIPEs od and reads as "no match")
    if cmp -s "$save" <(dd if=/dev/zero bs=4096 count=7 2>/dev/null); then
        echo "already zero: nothing to do"
    else
        psh "dd if=/dev/zero of=$ud bs=4096 seek=$UFS_STORE count=7 conv=notrunc,fsync 2>/dev/null; sync"
        [ "$(psh "dd if=$ud bs=4096 skip=$UFS_STORE count=7 2>/dev/null | sha256sum" | cut -c1-64)" = \
          "$(dd if=/dev/zero bs=4096 count=7 2>/dev/null | shasum -a 256 | cut -c1-64)" ] || die "zeroing did not read back"
        echo "released: USERDATA +16 GiB holds no store; the card's is the only one"
    fi
    ;;
*)
    sed -n '2,25p' "$0"; exit 1 ;;
esac
