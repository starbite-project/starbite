#!/bin/bash
# tools/installer/rehearse-store-update.sh [WORKDIR]: tools/installer/store-update.py on a FAKE S9 #2 (the installer
# environment, an sd64l card holding a home store with marked volatile entries and a settings block). No phone.
#   refusals (another phone's serial, no catalog, a catalog that drops a key, no dry run, an sd128 card);
#   a dry run (nothing changes); an update adding android.cmdline and a catalog row, with a cable pull in the data and
#   the resume; the volatile entries' bytes, the settings block and every CRC afterwards; the rollback (--restore);
#   UFS untouched. About a minute; WORKDIR (default build/store-update-rehearsal) is removed unless KEEP=1.
set -uo pipefail
cd "$(dirname "$0")/../.."
W=${1:-build/store-update-rehearsal}
rm -rf "$W"; mkdir -p "$W"; W=$(cd "$W" && pwd)
export SBINST_STATE=$W/state
unset ANDROID_SERIAL SBINST_SERIAL FAKE_FAIL SBINST_FAKE_USB_EXTRA
FP=(python3 -B tools/installer/fakes/fakephone.py)
SU=(python3 -B tools/installer/store-update.py --serial S9TWO02 --state-dir "$W/state" --fake-root "$W/s2")
pass=0 fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
run() {
	local want=$1 desc=$2; shift 3
	"$@" > "$W/last.txt" 2>&1
	local rc=$?
	grep -E '^(ok|REFUSED|STOPPED)' "$W/last.txt" | sed 's/^/     /' | head -4
	if [ "$rc" = "$want" ]; then ok "$desc (exit $rc)"; else bad "$desc (exit $rc, wanted $want)"; sed 's/^/     | /' "$W/last.txt" | tail -8; fi
}
has() { if grep -qE -- "$1" "$W/last.txt"; then ok "$2"; else bad "$2 (no /$1/)"; fi; }
p2hash() { shasum -a 256 < "$W/s2/dev/block/mmcblk0p2" | cut -c1-64; }

echo "######## a fake S9 #2: the installer environment, an sd64l card with a home store (marked volatile entries)"
"${FP[@]}" create "$W/s2" --state env --gpt starbite-oneui --card starbite --layout sd64l --card-sectors 124735488 >/dev/null
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=S9TWO02/' "$W/s2/props" && rm "$W/s2/props.bak"
mkdir -p "$W/state/env" "$W/in"
ssh-keygen -q -t ed25519 -N '' -C su-rehearsal -f "$W/state/env/id_ed25519"
cp "$W/state/env/id_ed25519.pub" "$W/s2/authorized_keys"
cat > "$W/in/catalog.toml" <<'TOML'
[[entry]]
key = "starbite-next-gpu"
name = "Starbite OS Next (GPU)"
version = "test"
category = "starbite"
method = "bare"
tag = "experimental"
files = ["ufs:starbite-next-gpu.img|sd:starbite-next-gpu.img"]
root = "none (RAM only)"

[[entry]]
key = "engineer"
name = "Engineer OS"
version = "test"
category = "starbite"
method = "bare"
tag = "beta"
files = ["ufs:engineer.img|sd:engineer.img"]
root = "none (RAM only)"
TOML
head -c $((3 << 20)) /dev/urandom > "$W/in/starbite-next-gpu.img"
head -c $((1 << 20)) /dev/urandom > "$W/in/engineer.img"
python3 tools/store/sbstore2.py build "$W/in/store.img" --medium sd64l --home --catalog "$W/in/catalog.toml" \
	--volatile sbos-state=1048576 --volatile sbos-photos=$((64 << 20)) \
	starbite-next-gpu.img="$W/in/starbite-next-gpu.img":image engineer.img="$W/in/engineer.img":image > /dev/null
python3 tools/store/sbstore2.py settings "$W/in/settings.bin" --default starbite-next-gpu > /dev/null
python3 - "$W/in/store.img" "$W/s2/dev/block/mmcblk0p2" "$W/in/settings.bin" <<'PY'
# the store onto the fake p2, then what the phone would have written: markers in the volatile entries, the settings
import struct, sys
img, p2, st = sys.argv[1], sys.argv[2], open(sys.argv[3], 'rb').read()
d = bytearray(open(img, 'rb').read())
n = struct.unpack_from('<8sII', d, 4096)[2]
for i in range(n):
    name, blk, size, crc, typ, fl = struct.unpack_from('<32sQQIHH', d, 3 * 4096 + 64 * i)
    name = name.rstrip(b'\0').decode()
    if fl & 1:
        mark = f'PHONE WROTE {name}'.encode()
        d[blk * 4096:blk * 4096 + len(mark)] = mark
d[2 * 4096:3 * 4096] = st
with open(p2, 'r+b') as f:
    f.write(d)
PY
UFS=$(shasum -a 256 < "$W/s2/dev/block/sda" | cut -c1-64)
cp "$W/in/catalog.toml" "$W/in/catalog2.toml"
cat >> "$W/in/catalog2.toml" <<'TOML'

[[entry]]
key = "android"
name = "One UI 8"
version = "test"
category = "android"
method = "android"
tag = "daily"
files = ["android.kernel", "android.ramdisk", "android.cmdline", "android.dtb"]
root = "ufs:SYSTEM + VENDOR + ODM (EROFS) + A16DATA (ext4)"
TOML
printf 'console=ram loglevel=4' > "$W/in/android.cmdline"
head -n 9 "$W/in/catalog.toml" > "$W/in/catalog-lost.toml"       # only starbite-next-gpu: engineer would vanish

echo; echo "######## refusals"
run 2 "another phone's serial" -- python3 -B tools/installer/store-update.py --serial OTHER --state-dir "$W/state" --fake-root "$W/s2" --catalog "$W/in/catalog2.toml"
run 2 "no --catalog" -- "${SU[@]}"
run 2 "a catalog that drops a key" -- "${SU[@]}" --catalog "$W/in/catalog-lost.toml"
has 'lacks menu keys .*engineer' "...names the key (engineer)"
run 0 "...allowed with --drop engineer (dry run)" -- "${SU[@]}" --catalog "$W/in/catalog-lost.toml" --drop engineer
run 2 "--rehearse without a dry run of this result" -- "${SU[@]}" --catalog "$W/in/catalog2.toml" android.cmdline="$W/in/android.cmdline" --rehearse --yes
has 'no dry run gave this exact store' "...asks for it"

echo; echo "######## the update: android.cmdline + the One UI row"
B=$(p2hash)
run 0 "dry run" -- "${SU[@]}" --catalog "$W/in/catalog2.toml" android.cmdline="$W/in/android.cmdline"
has 'add +android.cmdline' "plan: android.cmdline added"
has 'volatile, kept +sbos-state' "plan: sbos-state kept"
has 'volatile, kept +crashlog' "plan: the crash log kept"
[ "$(p2hash)" = "$B" ] && ok "dry run: card p2 byte-identical" || bad "dry run changed p2"
[ -z "$(ls "$W"/state/phone-*/store/*.img 2>/dev/null)" ] && ok "dry run: no images left on the Mac" || bad "dry run left images"
FAKE_FAIL='sbsu.piece of=' run 3 "the write, with a cable pull in the data" -- "${SU[@]}" --catalog "$W/in/catalog2.toml" android.cmdline="$W/in/android.cmdline" --rehearse --yes
rm -f "$W/s2/fail-count"
python3 - "$W/s2/dev/block/mmcblk0p2" <<'PY' && ok "after the pull: the card's head is still the old store (2 + the crash log + 3 entries... unchanged header)" || bad "the head changed before the data was written"
import struct, sys
d = open(sys.argv[1], 'rb').read(8192)
assert struct.unpack_from('<8sII', d, 4096)[2] == 6, struct.unpack_from('<8sII', d, 4096)
PY
run 0 "the same command again: resumes from the saved image" -- "${SU[@]}" --catalog "$W/in/catalog2.toml" android.cmdline="$W/in/android.cmdline" --rehearse --yes
has 'interrupted: writing it again' "...without a new pull"
has 'reads back as the new image' "the whole store area read back"
python3 - "$W/s2/dev/block/mmcblk0p2" "$W/in/settings.bin" "$W/in/android.cmdline" <<'PY' && ok "on the card: android.cmdline, the volatile entries' phone bytes, the settings block, every CRC" || bad "card store content"
import importlib.util, struct, sys, tempfile
spec = importlib.util.spec_from_file_location('sb', 'tools/store/sbstore2.py'); sb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sb)
d = open(sys.argv[1], 'rb').read()
used = struct.unpack_from('<8sIIQQ', d, 4096)[4]
with tempfile.NamedTemporaryFile(suffix='.img') as t:
    t.write(d[:used * 4096]); t.flush()
    e = {n: (b, fl) for n, _, b, fl in sb.read_store(t.name, with_flags=True)}      # every CRC checked
assert e['android.cmdline'][0] == open(sys.argv[3], 'rb').read()
for n in ('crashlog', 'sbos-state', 'sbos-photos'):
    assert e[n][1] & 1 and e[n][0].startswith(f'PHONE WROTE {n}'.encode()), n
assert d[2 * 4096:3 * 4096] == open(sys.argv[2], 'rb').read()
assert b'android' in e['catalog'][0]
PY
PULL=$(ls "$W"/state/phone-*/store/pull-*.img | head -1)
[ -f "$PULL" ] && ok "the rollback image stays: $(basename "$PULL")" || bad "no rollback image"

echo; echo "######## rollback: the pulled image back"
run 0 "--restore dry run" -- "${SU[@]}" --restore "$PULL"
run 0 "--restore" -- "${SU[@]}" --restore "$PULL" --rehearse --yes
python3 - "$W/s2/dev/block/mmcblk0p2" "$PULL" <<'PY' && ok "the card store is the pulled one again (its settings block kept)" || bad "restore"
import sys
d = open(sys.argv[1], 'rb').read(); p = open(sys.argv[2], 'rb').read()
assert d[:2 * 4096] == p[:2 * 4096] and d[3 * 4096:len(p)] == p[3 * 4096:]
PY
[ "$(shasum -a 256 < "$W/s2/dev/block/sda" | cut -c1-64)" = "$UFS" ] && ok "UFS untouched" || bad "UFS changed"

echo; echo "######## an sd128 card"
"${FP[@]}" create "$W/s3" --state env --gpt starbite-oneui --card starbite --layout sd128 >/dev/null
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=S9TWO02/' "$W/s3/props"; cp "$W/state/env/id_ed25519.pub" "$W/s3/authorized_keys"
run 2 "refused: card-store.sh's compact store" -- python3 -B tools/installer/store-update.py --serial S9TWO02 --state-dir "$W/state" --fake-root "$W/s3" --catalog "$W/in/catalog2.toml"
has 'card-store.sh' "...names the right tool"

echo; echo "######## result: $pass passed, $fail failed"
[ "${KEEP:-0}" = 1 ] || rm -rf "$W"
[ "$fail" = 0 ]
