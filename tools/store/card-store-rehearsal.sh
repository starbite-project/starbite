#!/usr/bin/env bash
# Rehearse tools/store/card-store.sh on the installer's fake phone (tools/installer/fakes: sparse UFS
# and 128 GB card, TWRP-style adb), nothing near the real phone. Linux (the Lima VM: GNU dd and
# sha256sum, sparse files); about 2 GB of real disk under $TMPDIR.
#   tools/store/card-store-rehearsal.sh [LIVE-UFS-STORE.img]
# Without an image it builds a small UFS store. Checks: migrate (entries equal, USERDATA untouched,
# the old card store's head replaced, nothing outside p2's head and store data area written), a
# second migrate refused, verify, flash of an updated image (the card's settings kept, only changed
# pieces written), store-flash-sd.sh refusing the home store, release refused without sbstore=card
# in BOOT, then release (old UFS store head zeroed, saved first).
set -euo pipefail
cd "$(dirname "$0")/../.."
repo=$PWD
F=$(mktemp -d "${TMPDIR:-/tmp}/cardstore-fake.XXXXXX")
W=$F/work
trap 'rm -rf "$F"' EXIT
export FAKE_ROOT=$F/phone PATH="$repo/tools/installer/fakes:$PATH"
SB="python3 -I -B tools/store/sbstore2.py"
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; exit 1; }
sum() { dd if="$1" bs=4096 skip="$2" count="$3" 2>/dev/null | sha256sum | cut -c1-64; }

python3 tools/installer/fakes/fakephone.py create "$FAKE_ROOT" --state recovery --card starbite >/dev/null
D=$FAKE_ROOT/dev/block
ud=$(readlink -f "$D/by-name/USERDATA")
mkdir -p "$W"

# the live UFS store (+ its settings record) and the old 1-entry card store
if [ -n "${1:-}" ]; then
    cp "$1" "$F/ufs.img"
else
    head -c 3000000 /dev/urandom > "$F/k1"; head -c 777777 /dev/urandom > "$F/k2"
    $SB build "$F/ufs.img" linux.image="$F/k1" rescue.image="$F/k2" --medium ufs \
        --catalog tools/store/catalog.toml --volatile sbos-state=1048576 >/dev/null
fi
$SB settings "$F/set.bin" --default linux --last utility --timeout 7 >/dev/null
dd if="$F/set.bin" of="$F/ufs.img" bs=4096 seek=2 conv=notrunc 2>/dev/null
dd if="$F/ufs.img" of="$ud" bs=4096 seek=4194304 conv=notrunc 2>/dev/null
# the Apple slot store-flash.sh checks: an HFS+ volume header (block size 4096, 4 GiB) at USERDATA byte 1024
python3 -c 'import struct,sys; sys.stdout.buffer.write(b"H+\x00\x04"+bytes(36)+struct.pack(">II",4096,1048576))' |
    dd of="$ud" bs=1 seek=1024 conv=notrunc 2>/dev/null
$SB build "$F/oldcard.img" --medium sd --catalog tools/store/catalog.toml >/dev/null
dd if="$F/oldcard.img" of="$D/mmcblk0p2" bs=4096 conv=notrunc 2>/dev/null
ufs_before=$(sum "$ud" 0 4915328)
# what lies outside the store's head and data area on p2 (the roots, the tail): marked
for blk in 7 300 4194304 8126719 8372736 8389119; do
    printf 'MARK%08d' "$blk" | dd of="$D/mmcblk0p2" bs=4096 seek="$blk" conv=notrunc 2>/dev/null
done
marks() { for blk in 7 300 4194304 8126719 8372736 8389119; do dd if="$D/mmcblk0p2" bs=4096 skip="$blk" count=1 2>/dev/null | head -c 12; echo; done | sha256sum; }
marks_before=$(marks)

echo "== migrate"
tools/store/card-store.sh migrate "$W" > "$F/migrate.log" 2>&1 || { cat "$F/migrate.log"; fail "migrate"; }
grep -q 'entries: all' "$F/migrate.log" || fail "migrate compared the entries"
[ "$(sum "$ud" 0 4915328)" = "$ufs_before" ] && pass "USERDATA unchanged by migrate" || fail "USERDATA changed"
[ "$(marks)" = "$marks_before" ] && pass "p2 outside the head and the store data area untouched" || fail "p2 marks changed"
$SB header "$W/card-store.img" | grep -q 'HOME store' && pass "card image is a home store" || fail "not home"
dd if="$D/mmcblk0p2" bs=4096 skip=2 count=1 of="$F/cardset.bin" 2>/dev/null
$SB settings-show "$F/cardset.bin" | grep -q 'default *linux' || { $SB settings-show "$F/cardset.bin"; fail "card settings = the UFS record"; }
$SB settings-show "$F/cardset.bin" | grep -qi 'card store.*on' && pass "card settings: the UFS record, card store flag on" || { $SB settings-show "$F/cardset.bin"; fail "card flag"; }
diff <($SB list "$F/ufs.img") <($SB list "$W/card-pulled.img" | grep -v '^  crashlog ') >/dev/null 2>&1 ||
    diff <($SB list "$F/ufs.img") <($SB list "$W/card-pulled.img") || true
python3 -I -B - "$F/ufs.img" "$W/card-pulled.img" "$repo/tools/store/sbstore2.py" <<'PY' || fail "entries equal after the pull"
import importlib.util, sys, zlib
spec = importlib.util.spec_from_file_location('s', sys.argv[3]); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
a = {n: (t, zlib.crc32(b), len(b), f) for n, t, b, f in m.read_store(sys.argv[1], with_flags=True)}
b = {n: (t, zlib.crc32(x), len(x), f) for n, t, x, f in m.read_store(sys.argv[2], with_flags=True)}
b.pop('crashlog', None) if 'crashlog' not in a else None
assert a == b, (set(a) ^ set(b))
print(f'PASS: the card store pulled back has the live store\'s {len(a)} entries (name, type, CRC, size, flags)')
PY

echo "== migrate again: refused"
tools/store/card-store.sh migrate "$W/again" > "$F/m2.log" 2>&1 && fail "second migrate ran" || pass "second migrate refused ($(tail -1 "$F/m2.log"))"

echo "== verify"
tools/store/card-store.sh verify "$W/card-store.img" | tail -1 | grep -q '^card ==' && pass "verify" || fail "verify"

echo "== flash an update (settings on the card changed meanwhile, one entry replaced)"
$SB settings "$F/set2.bin" --from "$F/cardset.bin" --last mac13 >/dev/null
dd if="$F/set2.bin" of="$D/mmcblk0p2" bs=4096 seek=2 conv=notrunc 2>/dev/null
tools/store/store-pull.sh "$F/pull.img" sd >/dev/null
head -c 2000000 /dev/urandom > "$F/k3"
$SB build "$F/new.img" rescue.image="$F/k3" --medium sd --home --data-at layout --import "$F/pull.img" >/dev/null
tools/store/card-store.sh flash "$F/new.img" > "$F/flash.log" 2>&1 || { cat "$F/flash.log"; fail "flash"; }
grep -q "card's record kept" "$F/flash.log" || fail "flash kept the card's settings"
dd if="$D/mmcblk0p2" bs=4096 skip=2 count=1 of="$F/cardset3.bin" 2>/dev/null
cmp -s "$F/cardset3.bin" "$F/set2.bin" && pass "flash kept the card's settings record" || fail "settings replaced"
tools/store/store-pull.sh "$F/pull2.img" sd >/dev/null
python3 -I -B - "$F/pull2.img" "$F/k3" "$repo/tools/store/sbstore2.py" <<'PY' || fail "updated entry"
import importlib.util, sys
spec = importlib.util.spec_from_file_location('s', sys.argv[3]); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
d = {n: b for n, _, b in m.read_store(sys.argv[1])}
assert d['rescue.image'] == open(sys.argv[2], 'rb').read()
print('PASS: the updated entry reads back from the card')
PY
grep -q 'written' "$F/flash.log" && echo "  $(grep 'pieces checked' "$F/flash.log" | tr '\r' '\n' | tail -1)"
[ "$(marks)" = "$marks_before" ] && pass "p2 outside the head and data area still untouched" || fail "marks after flash"

echo "== store-flash-sd.sh refuses the home store"
tools/store/store-flash-sd.sh "$F/oldcard.img" > "$F/fsd.log" 2>&1 && fail "store-flash-sd wrote over the home store" || pass "store-flash-sd.sh refused ($(tail -1 "$F/fsd.log"))"
tools/store/store-flash-sd.sh "$F/new.img" > "$F/fsd2.log" 2>&1 && fail "store-flash-sd wrote a compact image" || pass "store-flash-sd.sh refused a compact image"

echo "== the automation tools, old BOOT (the installed loader still reads the UFS store)"
head -c 123457 /dev/urandom > "$F/k4"
[ "$(tools/store/live-store.sh)" = ufs ] && pass "live-store.sh: ufs (no sbstore=card in BOOT)" || fail "live-store.sh"
tools/store/store-update.sh "$F/upd" newentry.img="$F/k4":image > "$F/su.log" 2>&1 || { cat "$F/su.log"; fail "store-update.sh (ufs)"; }
grep -q 'OS store v2 written (ufs' "$F/su.log" && grep -q 'card copy updated' "$F/su.log" && pass "store-update.sh wrote the UFS store and the card copy" || { cat "$F/su.log"; fail "store-update output"; }
dd if="$ud" bs=4096 skip=4194304 count=721024 of="$F/ufsnow.img" 2>/dev/null
$SB list "$F/ufsnow.img" 2>/dev/null | grep -q '^  newentry.img ' && pass "the new entry is in the UFS store" || fail "not in UFS"
tools/store/store-pull.sh "$F/pull3.img" sd >/dev/null
$SB list "$F/pull3.img" | grep -q '^  newentry.img ' && pass "and in the card copy" || fail "not on the card"
rm -f "$F/ufsnow.img"
key=$(sed -n 's/^key = "\(.*\)"/\1/p' build/store/catalog-live.toml 2>/dev/null | head -1)
bn() {   # $1 card|ufs: where boot-next.sh must have put next=$key
    timeout 120 tools/store/boot-next.sh "$key" 5 > "$F/bn.log" 2>&1 || true
    if [ "$1" = card ]; then dd if="$D/mmcblk0p2" bs=4096 skip=2 count=1 of="$F/set-now.bin" 2>/dev/null
    else dd if="$ud" bs=4096 skip=4194306 count=1 of="$F/set-now.bin" 2>/dev/null; fi
    $SB settings-show "$F/set-now.bin" | grep -q "next *$key" && pass "boot-next.sh set next=$key in the $1 settings" ||
        { cat "$F/bn.log"; $SB settings-show "$F/set-now.bin"; fail "boot-next ($1)"; }
}
[ -n "$key" ] && bn ufs
ufs_before=$(sum "$ud" 0 4915328)

echo "== the automation tools, card-only BOOT"
printf 'xxxx starmenu7=2 sbstore=card yyyy' | dd of="$(readlink -f "$D/by-name/BOOT")" bs=1 seek=4096 conv=notrunc 2>/dev/null
[ "$(tools/store/live-store.sh)" = card ] && pass "live-store.sh: card" || fail "live-store.sh card"
tools/store/store-flash.sh "$F/ufs.img" > "$F/sf.log" 2>&1 && fail "store-flash.sh wrote USERDATA" || pass "store-flash.sh refused: $(tail -1 "$F/sf.log" | cut -c1-80)"
head -c 54321 /dev/urandom > "$F/k5"
tools/store/store-update.sh "$F/upd" newer.img="$F/k5":image > "$F/su2.log" 2>&1 || { cat "$F/su2.log"; fail "store-update.sh (card)"; }
grep -q 'OS store v2 written (card)' "$F/su2.log" && pass "store-update.sh wrote the card store" || fail "store-update card output"
[ -n "$key" ] && bn card
[ "$(sum "$ud" 0 4915328)" = "$ufs_before" ] && pass "USERDATA unchanged by the card-only tools" || fail "USERDATA changed"

echo "== release"
tools/store/card-store.sh release > "$F/r0.log" 2>&1 && fail "release without --i-mean-it" || pass "release needs --i-mean-it"
printf 'xxxx starmenu7=2 sbstore=nope yyyy' | dd of="$(readlink -f "$D/by-name/BOOT")" bs=1 seek=4096 conv=notrunc 2>/dev/null
tools/store/card-store.sh release --i-mean-it > "$F/r1.log" 2>&1 && fail "release without sbstore=card in BOOT" || pass "release refused: $(tail -1 "$F/r1.log")"
[ "$(sum "$ud" 0 4915328)" = "$ufs_before" ] || fail "USERDATA changed by a refused release"
printf 'xxxx starmenu7=2 sbstore=card yyyy' | dd of="$(readlink -f "$D/by-name/BOOT")" bs=1 seek=4096 conv=notrunc 2>/dev/null
(cd "$F" && mkdir -p build/store)
tools/store/card-store.sh release --i-mean-it > "$F/r2.log" 2>&1 || { cat "$F/r2.log"; fail "release"; }
[ "$(sum "$ud" 4194304 7)" = "$(dd if=/dev/zero bs=4096 count=7 2>/dev/null | sha256sum | cut -c1-64)" ] && pass "old UFS store head zeroed" || { cat "$F/r2.log"; dd if="$ud" bs=4096 skip=4194304 count=7 2>/dev/null | od -An -tx1 | head -3; fail "not zeroed"; }
[ "$(sum "$ud" 4194311 100)" = "$(sum "$F/ufs.img" 7 100)" ] && pass "only blocks 0..6 of the old store touched" || fail "more than the head"
saved=$(sed -n 's/^saved to \(.*\) (.*/\1/p' "$F/r2.log")
cmp -s "$saved" <(dd if="$F/ufs.img" bs=4096 count=7 2>/dev/null) && pass "old head saved to $saved" || fail "saved head"
rm -f "$saved"
echo "card-store rehearsal: all passed"
