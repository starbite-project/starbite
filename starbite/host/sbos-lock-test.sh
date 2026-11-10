#!/bin/sh
# 1.2-alpha25: the lock screen as the user asked (2026-11-10): swipe up unlocks; the other OSes are cards (from the
# store's catalog, read only); a hold on one asks, and yes is the loader's one-shot next boot (a dry run here: logged,
# no restart). Run on the Mac after sbos-preview.sh built the host binary. 1.2-alpha26: only with SBOS_LOCK_V25 1
# (kern.h; the alpha25 lock screen is off while the UI is redesigned).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_NORMAL=1 $ENVS "$out/sbos-host" "$out" /dev/null "$@" q 2>&1; }
ENVS=
CARD="g:720,1525,0,800 w:600"   # the first OS card, under the Emergency hold
HOLD="g:720,1092,0,1900 w:600"
r=$(run w:600)
want "$r" "lock: OS cards from the catalog" "the cards are read from the catalog"
never "$r" "lock: OS cards from the catalog.*: 0$" "at least one card (the host's built-in catalog)"
r=$(run w:600 g:720,2800,-900,300 w:800)
want "$r" "ui: unlocked by a swipe up" "a swipe up from the bottom unlocks"
r=$(run w:600 g:720,1500,-900,300 w:800)
never "$r" "unlocked by a swipe up" "a swipe that starts mid-screen does not"
r=$(run w:600 t:720,1525 w:800)
never "$r" "lock: OS card:" "a tap on a card does nothing (a hold, for pockets)"
r=$(run w:600 $CARD $HOLD w:800)
want "$r" "lock: OS card: would restart into " "a card's hold and the confirmation: the restart (a dry run)"
never "$r" "power: restart\|host: reboot" "the dry run restarts nothing"
r=$(run w:600 $CARD t:720,1361 w:800)
want "$r" "lock: OS card: not restarted" "Cancel: nothing happens"
# Emergency mode from the lock screen (heat and the power features live there): in, and out again
r=$(run w:600 g:720,1375,0,800 w:2000 s:lock-em g:720,1375,0,800 w:1500)
want "$r" "ui: Emergency Mode on" "the lock screen's Emergency hold: in"
want "$r" "ui: Emergency Mode off" "and out again"
echo "lock test: $fails failed"
