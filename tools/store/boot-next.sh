#!/usr/bin/env bash
# Unattended test boot from TWRP: set the store settings' one-shot "next" entry (the loader starts
# it without the menu and clears it, shim/starlte/menu.c boot_next), reboot, wait for TWRP to come
# back (an XNU test kernel with sweetreboot=1 resets into TWRP after its panic), save pstore.
#   tools/store/boot-next.sh KEY [WAIT_S]     -> build/test-boots/<time>-KEY/{pmsg.txt,dmesg*}
# Writes one 4 KiB block: the settings record at USERDATA +16 GiB + 2 blocks, read back.
set -euo pipefail
cd "$(dirname "$0")/../.."
key=${1:?usage: boot-next.sh KEY [WAIT_S]}
wait_s=${2:-600}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
dev=$(adb shell readlink -f /dev/block/by-name/USERDATA | tr -d '\r')
SET=$(( 4194304 + 2 ))
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
adb exec-out "dd if=$dev bs=4096 skip=4194305 count=2 2>/dev/null" > "$tmp/head"
python3 - "$tmp/head" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
assert len(d) == 8192 and d[:8] == b'SBSTORE2', 'no SBSTORE2 header at USERDATA +16 GiB + 1: refusing'
PY
dd if="$tmp/head" of="$tmp/cur.bin" bs=4096 skip=1 count=1 2>/dev/null
if head -c 8 "$tmp/cur.bin" | grep -q SBSETTNG; then from=(--from "$tmp/cur.bin"); else from=(); fi
python3 tools/store/sbstore2.py settings "$tmp/new.bin" "${from[@]}" --next "$key" >/dev/null
adb push "$tmp/new.bin" /tmp/sbset.bin >/dev/null
adb shell "dd if=/tmp/sbset.bin of=$dev bs=4096 seek=$SET count=1 conv=notrunc,fsync 2>/dev/null; sync"
back=$(adb exec-out "dd if=$dev bs=4096 skip=$SET count=1 2>/dev/null" | shasum -a 256 | cut -c1-64)
[ "$back" = "$(shasum -a 256 "$tmp/new.bin" | cut -c1-64)" ] || { echo "settings readback mismatch"; exit 1; }
echo "next = $key; rebooting"
adb reboot
sleep 20
out=build/test-boots/$(date +%Y%m%d-%H%M%S)-$key
mkdir -p "$out"
for ((t = 0; t < wait_s; t += 5)); do
    [ "$(adb get-state 2>/dev/null)" = recovery ] && break
    sleep 5
done
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "no TWRP after ${wait_s}s (the phone may be hung or in another OS)"; exit 2; }
sleep 3
adb shell 'cat /sys/fs/pstore/pmsg-ramoops-0' 2>/dev/null | LC_ALL=C tr -c '[:print:]\n' '.' > "$out/pmsg.txt" || true
adb shell 'ls -l /sys/fs/pstore/' > "$out/pstore-ls.txt" 2>&1 || true
echo "back in TWRP after ~$((t + 20)) s; log: $out/pmsg.txt ($(wc -l < "$out/pmsg.txt") lines)"
grep -a -m3 'panic(' "$out/pmsg.txt" || true
