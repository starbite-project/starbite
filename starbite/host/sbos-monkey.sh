#!/bin/sh
# Starbite OS monkey test (docs/STARBITE-OS.md 12.2): random taps, holds, Volume / Bixby presses and Back over the
# whole UI (every app, the keyboard, Wi-Fi on the core's model, the heater's confirmation, SOS, the light) for
# several seeds; each run must end without a crash, an unpresented frame, a full event queue or a loop turn of
# 200 ms or more. Power is never held (the power menu could end the run). Run on the Mac after sbos-preview.sh.
#   sh starbite/host/sbos-monkey.sh [seeds] [actions]
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
seeds=${1:-6}
n=${2:-600}
fails=0
s=1
while [ "$s" -le "$seeds" ]; do
    script=$(python3 - "$s" "$n" <<'PY'
import random, sys
r = random.Random(int(sys.argv[1]))
a = ['w:300', 'g:1150,2310,0,700', 'w:400']          # open home first
for _ in range(int(sys.argv[2])):
    p = r.random()
    if p < 0.62:
        a.append(f't:{r.randrange(20, 1420)},{r.randrange(130, 2940)}')
    elif p < 0.70:
        a.append(f'g:{r.randrange(20, 1420)},{r.randrange(130, 2940)},0,{r.randrange(200, 2200)}')
    elif p < 0.80:
        a.append(r.choice(['k:d', 'k:u', 'k:b']))
    elif p < 0.86:
        a.append('h:b:900')
    elif p < 0.88:
        a.append('k:p')                               # the screen off / on (a short Power press)
    a.append(f'w:{r.choice([30, 80, 150, 300, 600, 1200])}')
print(' '.join(a))
PY
)
    log="$out/monkey-$s.txt"
    # shellcheck disable=SC2086
    limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 ${MONKEY_NORMAL:+SBOS_HOST_NORMAL=1} "$out/sbos-host" "$out" /dev/null $script "s:monkey-$s" q > "$log" 2>&1
    bad=""
    grep -q "CRASH\|Segmentation\|Abort\|AddressSanitizer\|runtime error" "$log" && bad="$bad [crash]"
    grep -q "event queue full" "$log" && bad="$bad [event queue full]"
    grep -q "a loop turn took" "$log" && bad="$bad [slow loop: $(grep -m1 'a loop turn took' "$log" | sed 's/.*took //')]"
    grep "panel vs back buffer" "$log" | grep -v " 0 px differ" | grep -q . && bad="$bad [unpresented pixels]"
    grep -q "host: done\|host: reboot\|host: power" "$log" || bad="$bad [did not finish]"
    if [ -n "$bad" ]; then
        echo "monkey $s: FAIL$bad"
        fails=$((fails + 1))
    else
        echo "monkey $s: ok ($(grep -c '^log: ' "$log") log lines, $(grep -o 'frames [0-9]*' "$log" | tail -1))"
    fi
    s=$((s + 1))
done
echo "monkey: $fails failed"
[ "$fails" = 0 ]
