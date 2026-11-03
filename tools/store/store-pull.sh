#!/usr/bin/env bash
# Read the phone's live OS store back as one image, from TWRP, read-only. Use it as the --import of
# every store rebuild, never an older Mac copy (build/store/ufs-store.img may be behind the phone:
# other workflows flash the store too, and store-flash.sh replaces the whole store area).
#   tools/store/store-pull.sh OUT.img [ufs|sd]     (default ufs: USERDATA +16 GiB; sd: card p2)
# Validates the header first (magic, version 2, header CRC, 7 <= used <= capacity, capacity = what
# the device gives the store, used blocks fit the free space under the output directory), pulls
# exactly the used blocks, lists them (header, directory and entry CRCs), then compares the live
# catalog's menu keys with tools/store/catalog.toml. Before flashing a rebuild, diff both:
#   diff <(python3 tools/store/sbstore2.py list OUT.img) <(python3 tools/store/sbstore2.py list NEW.img)
#   diff <(python3 tools/store/sbstore2.py catalog-keys OUT.img) <(python3 tools/store/sbstore2.py catalog-keys NEW.img)
# The second must show only added keys (sbstore2.py build also refuses a --catalog that drops one).
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
psize=$(adb shell "cat /sys/class/block/${dev##*/}/size" | tr -d '\r')
case "$psize" in ''|*[!0-9]*) echo "cannot read the size of $dev"; exit 1 ;; esac
cap=$(( psize / 8 - skip ))
mkdir -p "$(dirname "$out")"
free_kb=$(df -k "$(dirname "$out")" | awk 'NR == 2 { print $4 }')
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb exec-out "dd if=$dev bs=4096 skip=$((skip + 1)) count=1 2>/dev/null" > "$tmp/hdr"
used=$(python3 - "$tmp/hdr" "$cap" "$free_kb" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sbstore2', 'tools/store/sbstore2.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
d = open(sys.argv[1], 'rb').read()
if len(d) < 52:
    sys.exit('short header read')
n, cap, used, _ = m.check_header(d, 'phone store')
if cap != int(sys.argv[2]):
    sys.exit(f'header capacity {cap} != {sys.argv[2]} blocks the device gives the store: refusing')
if used * 4 + 1048576 > int(sys.argv[3]):
    sys.exit(f'{used * 4 // 1024} MiB to pull, {int(sys.argv[3]) // 1024} MiB free (keeping 1 GiB): refusing')
print(used)
PY
)
echo "live store: $used blocks ($((used * 4 / 1024)) MiB) on $dev +$skip"
adb exec-out "dd if=$dev bs=4096 skip=$skip count=$used 2>/dev/null" > "$out"
[ "$(wc -c < "$out" | tr -d ' ')" = $((used * 4096)) ] || { echo "short read"; exit 1; }
python3 tools/store/sbstore2.py list "$out"          # header, directory and entry CRCs
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
    lk, rk = set(m.catalog_keys(live)), set(m.catalog_keys(ref))
    print('STOP: the live catalog differs from tools/store/catalog.toml. Do NOT build a --catalog from a '
          'copy of catalog.toml: find the TOML the live catalog was built from (or omit --catalog to keep '
          'the live one). sbstore2.py build refuses a catalog that drops a live key.')
    if lk - rk:
        print('  live keys missing from catalog.toml: ' + ' '.join(sorted(lk - rk)))
    if rk - lk:
        print('  catalog.toml keys not on the phone:   ' + ' '.join(sorted(rk - lk)))
PY
echo "pulled to $out"
