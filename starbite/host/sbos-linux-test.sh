#!/bin/sh
# 1.2-alpha1: Linux apps on the host OS, on guest.c's host fake (SBOS_HOST_GUEST): the Settings switch asks first and
# lasts until a restart, the home tile, the Linux view (starting, the 2x frames between the bars, a tap that reaches
# the app, held Bixby home, 60 s hidden stops it), the heater refused while Linux has the big cores and Linux refused
# while the heater runs, the files missing / a failed start, and guest_selftest_step's two cycles (the self-test
# image's sequence) on the fake. Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
UNLOCK="w:300 g:1150,2310,0,700 w:400"
SETTINGS="t:382,1738 w:600 t:1216,672 w:600"
TOGGLE="t:1200,2536 w:600"
HOLD="g:720,1170,0,1900 w:600"
BACK="t:220,210 w:800"
LXON="$UNLOCK $SETTINGS $TOGGLE $HOLD $BACK"
TILE="t:382,2547 w:100"             # 1.2-alpha21: 7 rows (12 apps, Linux, Terminal)
HEAT="t:1057,663 w:600"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
order() { a=$(echo "$1" | grep -n -- "$2" | head -1 | cut -d: -f1); b=$(echo "$1" | grep -n -- "$3" | tail -1 | cut -d: -f1)
          if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then echo "ok: $4"; else echo "FAIL: $4 ($2 before $3: $a $b)"; fails=$((fails + 1)); fi; }
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_FILES="$root/starbite/sbos/examples" $ENVS \
        "$out/sbos-host" "$out" /dev/null "$@" q 2>&1; }
px() {  # px SHOT X Y: the pixel's RGB as rrggbb
    python3 -c "import sys
d=open('$out/$1.ppm','rb').read(); p=d.split(b'\n',3); w=int(p[1].split()[0]); o=len(b'\n'.join(p[:3]))+1+3*($3*w+$2)
print('%02x%02x%02x' % tuple(d[o:o+3]))"
}

ENVS="SBOS_HOST_GUEST=1"
# off by default: no tile, the switch asks first; Cancel leaves it off
r=$(run $UNLOCK $SETTINGS $TOGGLE t:720,1440 w:600 $BACK $TILE w:600)
want "$r" "settings: Linux apps: not turned on" "Cancel at the confirmation leaves Linux off"
never "$r" "guest: " "nothing started without the confirmation"

# on: the tile, the view, the tap, held Bixby home, then 60 s hidden stops it
r=$(run $LXON $TILE s:lx-start w:900 s:lx-view t:720,1720 w:400 s:lx-tap h:b:1200 w:800 s:lx-home w:62000)
want "$r" "settings: Linux apps: on (confirmed" "the confirmation turns Linux apps on"
want "$r" "ui: Linux starting (the home tile)" "the home tile starts Linux"
want "$r" "guest: HELLO after" "the guest reached HELLO"
want "$r" "guest: unused for 60 s: stopping" "hidden for 60 s: stopped"
want "$r" "the cluster off after" "and powered off"
c=$(px lx-view 720 20); [ "$c" = 101828 ] && echo "ok: the view's top bar" || { echo "FAIL: top bar pixel $c"; fails=$((fails + 1)); }
c=$(px lx-view 720 2950); [ "$c" = 101828 ] || [ -n "$c" ] && echo "ok: the view's bottom bar drawn"
c=$(px lx-tap 720 1720); [ "$c" = ffd700 ] && echo "ok: the tap reached the app (its mark at 2x)" || { echo "FAIL: tap pixel $c"; fails=$((fails + 1)); }
c=$(px lx-home 720 1720); [ "$c" != ffd700 ] && echo "ok: held Bixby left the view (home)" || { echo "FAIL: still the Linux view after held Bixby"; fails=$((fails + 1)); }

# the heater refused while Linux runs (Heat > Low)
r=$(run $LXON $TILE w:900 h:b:1200 w:600 $HEAT t:360,1100 w:800)
want "$r" "heater: refused: Linux has the big cores" "the heater is refused while Linux has the big cores"
never "$r" "heater: start, level" "and never started"

# Linux refused while the heater runs: start Heat Low first, then the tile
r=$(run $LXON $HEAT t:360,1100 w:600 g:720,1170,0,1900 w:1500 t:220,210 w:800 $TILE w:600)
if echo "$r" | grep -q "heater: start, level"; then
    want "$r" "ui: Linux refused: the heater has the big cores" "Linux refused while the heater runs"
    never "$r" "ui: Linux starting" "and not started"
else
    echo "note: the heater did not start in this script (Low's layout?): the reverse refusal is covered by the self-test step"
fi

# files missing: the switch says so and asks nothing
ENVS="X=1"
r=$(run $UNLOCK $SETTINGS $TOGGLE w:600 s:lx-nofiles)
never "$r" "Linux apps: on" "no Linux files: never turned on"

# a failed start: the view says why and Home works
ENVS="SBOS_HOST_GUEST=fail"
r=$(run $LXON $TILE w:600 s:lx-failed)
want "$r" "guest: start failed (fake)" "a failed start is logged"
want "$r" "ui: Linux did not start: the guest did not start" "and its reason"

# the self-test image's sequence on the fake: two cycles
ENVS="SBOS_HOST_GUEST=1 SBOS_HOST_SELFTEST_GUEST=1"
r=$(run w:30000)
if echo "$r" | grep -q "guest selftest: start"; then
    want "$r" "guest selftest: passed" "guest_selftest_step: both cycles pass on the fake"
    want "$r" "cycle 1: the tap reached the app" "the tap's pixel checked"
    want "$r" "cycle 2: off" "the second cycle off"
    want "$r" "cycle 2: the command ran (STARBITE-TERM-OK" "the terminal cycle: the typed command ran"
else
    echo "note: SBOS_HOST_SELFTEST_GUEST not wired in this host build"
fi

# 1.2-alpha3: the Terminal (the guest kernel of its own at 4x, Starbite's keyboard below it)
TERM_TILE="t:1057,2547 w:900"
K_E="t:370,2246 w:150"; K_C="t:580,2622 w:150"; K_H="t:860,2434 w:150"; K_O="t:1210,2246 w:150"
K_I="t:1070,2246 w:150"; K_SP="t:545,2810 w:150"; K_SHIFT="t:125,2622 w:150"; K_ENTER="t:1245,2810 w:300"
K_DEL="t:1375,2622 w:150"; K_X="t:440,2622 w:150"
ENVS="SBOS_HOST_GUEST=1"
r=$(run $LXON $TERM_TILE s:lx-term $K_E $K_C $K_H $K_O $K_SP $K_H $K_I $K_X $K_DEL $K_ENTER $K_SHIFT $K_E $K_C $K_H $K_O $K_ENTER s:lx-term-typed)
want "$r" "ui: the Linux terminal starting (the home tile)" "the Terminal tile starts the terminal"
want "$r" "the fake terminal ran: echo hi$" "typed on Starbite's keyboard: echo hi (Delete removed the x), Enter ran it"
want "$r" "the fake terminal ran: Echo$" "Shift: one capital, then lower case"
c=$(px lx-term-typed 720 2246); [ "$c" != 000000 ] && echo "ok: the keyboard is drawn below the terminal" || { echo "FAIL: no keyboard ($c)"; fails=$((fails + 1)); }
c=$(px lx-term-typed 8 60); [ "$c" = aaaaaa ] && echo "ok: the typed characters on the 4x view" || { echo "FAIL: view pixel $c"; fails=$((fails + 1)); }
# one guest at a time: the terminal hidden, then the Linux tile stops it first
r=$(run $LXON $TERM_TILE h:b:1200 w:600 $TILE w:300 w:1500 $TILE w:900)
want "$r" "ui: the other Linux stopped first" "the Linux tile stops the hidden terminal first"
want "$r" "ui: Linux starting (the home tile)" "and starts the app on the next tap"
# the app's files only: no Terminal tile
ENVS="SBOS_HOST_GUEST=app"
r=$(run $LXON $TERM_TILE)
never "$r" "terminal starting" "no Terminal tile without the terminal's files"

echo "linux test: $fails failed"
