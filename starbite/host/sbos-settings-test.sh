#!/bin/sh
# Starbite OS settings across a restart (docs/STARBITE-OS.md 6.4) on the host's file-backed state slot: a change is
# saved 3 s later into the other half, the next start restores it, a damaged newest half falls back to the older
# copy, and without a slot nothing is saved. Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
st="$out/settings-test.bin"
rm -f "$st"
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 ${SLOT:+SBOS_HOST_STATE=$st} ${NOFILES:+SBOS_HOST_FILES=/nonexistent} "$out/sbos-host" "$out" /dev/null "$@" s:settings-test q 2>&1 | grep -E 'settings:|fence:|slot:'; }
OPEN="w:300 g:1150,2310,0,700 w:400 t:382,2424 w:600"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $2"; else echo "FAIL: $2"; echo "$1" | sed 's/^/    /'; fails=$((fails + 1)); fi; }
SLOT=1
a=$(run $OPEN t:882,1494 w:300 t:720,1789 w:4000)          # 1 min, refresh only on change on
want "$a" "saved (half 0, sequence 1"
want "$a" "fence: standing grant for 256 blk at ufs:17303808"
want "$a" "fence: wrote 1 blk at ufs:17303808 (sbos-state, tier 1): FUA + cache flush"
b=$(run $OPEN t:720,2039 w:4000)                            # panel off with the screen on
want "$b" "restored (slot sequence 1): screen off 60 s, refresh on change on"
want "$b" "saved (half 1, sequence 2"
want "$b" "fence: wrote 1 blk at ufs:17303936"
c=$(run w:300)
want "$c" "restored (slot sequence 2): screen off 60 s, refresh on change on, panel off on"
# damage the newest half (sequence 2, half 1): the older copy (sequence 1) wins
printf 'XX' | dd of="$st" bs=1 seek=$((512 * 1024 + 40)) conv=notrunc 2>/dev/null
d=$(run w:300)
want "$d" "restored (slot sequence 1): screen off 60 s, refresh on change on, panel off off"
# 0.5.3: an app's state across a restart: the Info app's card (Name typed on the keyboard) saved, then restored
INFO="w:300 g:1150,2310,0,700 w:400 t:1057,1250 w:600 t:720,1150 w:500 t:720,655 w:500 t:930,2434 w:150 t:1210,2246 w:150 t:1245,2810 w:4000"
NOFILES=1
f=$(run $INFO)
want "$f" "settings: saved" "the card saved with the settings"
g=$(run w:300)
want "$g" "settings: an app's state restored: info" "the card restored at the next start"
NOFILES=
SLOT=
e=$(run $OPEN t:720,1789 w:4000)
want "$e" "no \"sbos-state\" store entry"
want "$e" "no state slot yet"
if echo "$e" | grep -q "fence: wrote"; then echo "FAIL: a write without the entry"; fails=$((fails + 1)); fi
if echo "$e" | grep -q "saved ("; then echo "FAIL: saved without a slot"; fails=$((fails + 1)); fi
echo "settings test: $fails failed"
[ "$fails" = 0 ]
