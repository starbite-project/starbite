#!/usr/bin/env bash
# Read the phone's live OS store back as one image, from TWRP, read-only. Use it as the --import of
# every store rebuild, never an older Mac copy (build/store/ufs-store.img may be behind the phone:
# other workflows flash the store too, and store-flash.sh replaces the whole store area).
#   tools/store/store-pull.sh OUT.img [ufs|sd]     (default ufs: USERDATA +16 GiB; sd: card p2)
# Pulls exactly the used blocks of the version 2 store, lists it, and says whether the live catalog
# is byte-identical to tools/store/catalog.toml packed (so a copy of that file plus new [[entry]]
# rows is "the phone's catalog plus the entry"). Compare with a new build before flashing:
#   diff <(python3 tools/store/sbstore2.py list OUT.img) <(python3 tools/store/sbstore2.py list NEW.img)
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:?usage: store-pull.sh OUT.img [ufs|sd]}
medium=${2:-ufs}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
case "$medium" in
ufs) dev=$(adb shell readlink -f /dev/block/by-name/USERDATA | tr -d '\r'); skip=4194304 ;;
sd)  dev=/dev/block/mmcblk0p2; skip=0 ;;
*)   echo "medium is ufs or sd"; exit 1 ;;
esac
[ -n "$dev" ] || { echo "no store device"; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb exec-out "dd if=$dev bs=4096 skip=$((skip + 1)) count=1 2>/dev/null" > "$tmp/hdr"
used=$(python3 - "$tmp/hdr" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'SBSTORE2', f'no version 2 store on the phone (magic {d[:8]!r})'
print(struct.unpack_from('<8sIIQQ', d, 0)[4])
PY
)
echo "live store: $used blocks ($((used * 4 / 1024)) MiB) on $dev +$skip"
adb exec-out "dd if=$dev bs=4096 skip=$skip count=$used 2>/dev/null" > "$out"
[ "$(wc -c < "$out" | tr -d ' ')" = $((used * 4096)) ] || { echo "short read"; exit 1; }
python3 tools/store/sbstore2.py list "$out"          # also checks every CRC32
python3 tools/store/sbstore2.py catalog tools/store/catalog.toml "$tmp/cat.sbc"
python3 - "$out" "$tmp/cat.sbc" <<'PY'
import sys
import importlib.util
spec = importlib.util.spec_from_file_location('sbstore2', 'tools/store/sbstore2.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
live = {n: b for n, _, b in m.read_store(sys.argv[1])}.get('catalog')
ref = open(sys.argv[2], 'rb').read()
if live is None:
    print('live store has no catalog entry')
elif live == ref:
    print('live catalog == tools/store/catalog.toml (copy it and append the new [[entry]])')
else:
    print('WARNING: live catalog differs from tools/store/catalog.toml: find the TOML it was built '
          'from before passing --catalog, or omit --catalog to keep the live one')
PY
echo "pulled to $out"
