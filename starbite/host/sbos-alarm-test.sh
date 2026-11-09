#!/bin/sh
# 1.2-alpha8: wake-up alarms on the host OS (the host clock starts at 16:39): Clock > Alarms > Set time to 16:40 every
# day > Done; it rings within ~80 s (the ringing page, the motor), Power snoozes; Stop ends it. Run on the Mac after
# sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 "$out/sbos-host" "$out" /dev/null "$@" q 2>&1; }
UNLOCK="w:300 g:1150,2310,0,700 w:400"
CLOCK="t:385,1835 w:600"
ALARMS="t:710,1672 w:600"
EDIT0="t:385,888 w:600"
H="t:385,955 w:150"
M10="t:385,1225 w:150"
SET1640="$EDIT0 $H $H $H $H $H $H $H $H $H $M10 $M10 $M10 $M10 t:720,1800 w:300 t:720,2105 w:600"
r=$(run $UNLOCK $CLOCK $ALARMS $SET1640 s:al-set t:220,210 w:600 t:220,210 w:600 w:80000 k:p w:2500 s:al-snoozed)
want "$r" "alarm: ringing 16:40" "the alarm rings at its minute"
want "$r" "vib: (host)\|vib: on" "the motor buzzes"
want "$r" "alarm: snoozed 9 minutes" "Power snoozes it"
never "$r" "alarm: stopped" "not stopped by the snooze"
r2=$(run $UNLOCK $CLOCK $ALARMS $SET1640 t:220,210 w:600 t:220,210 w:600 w:80000 s:al-ring t:720,955 w:800)
want "$r2" "alarm: ringing 16:40" "rings again in a new run"
want "$r2" "alarm: stopped" "Stop on the ringing page stops it"
echo "alarm test: $fails failed"
[ "$fails" = 0 ]
