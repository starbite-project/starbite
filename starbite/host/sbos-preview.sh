#!/bin/sh
# Starbite OS host preview (docs/STARBITE-OS.md 12.2): the real kernel, UI and apps built for aarch64 Linux (the
# Lima VM) on host.c's fake hardware, driven by scripted keys, taps and holds, PNGs to starbite/sbos/preview/.
# Run on the Mac from the repo root:   sh starbite/host/sbos-preview.sh
# Holds are drags that do not move: g:X,Y,0,MS. Keys: k:b Bixby (select), h:b:900 Bixby held (Back), k:p Power
# (screen), h:p:1200 Power held (the power menu), k:d / k:u Volume (focus).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
png="$root/starbite/sbos/preview"
py="$root/build/venv/bin/python"
[ -x "$py" ] || py="$root/../../../build/venv/bin/python"
mkdir -p "$out" "$png"
lib="$root/starbite/libsb"
k="$root/starbite/sbos/kern"
sdk="$root/starbite/sbos/sdk"
apps="$root/starbite/sbos/apps"
SB7=""; EXTRA=""
for c in "$root/build/brand" "$root/../../../build/brand"; do
    [ -z "$SB7" ] && [ -f "$c/menu7.sb7" ] && SB7="$c/menu7.sb7"
    [ -z "$EXTRA" ] && [ -f "$c/sbos-extra.sb7" ] && EXTRA="$c/sbos-extra.sb7"
done
[ -n "$SB7" ] && [ -n "$EXTRA" ] || { echo "needs build/brand/menu7.sb7 and sbos-extra.sb7"; exit 1; }
V="limactl shell --workdir $root starbite"
$V python3 starbite/tools/mkassets.py "$SB7" "$out/libsb-assets.sb7" >/dev/null
$V python3 starbite/sbos/tools/mergeassets.py "$out/sbassets.sb7" "$out/libsb-assets.sb7" "$EXTRA" >/dev/null
CF="-O1 -g -DSB_HOST -DSB_HOST_UTILITY -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing -I$lib -I$k -I$sdk -I$root/starbite/utility -I$root/starbite/engineer"
aobjs=""
for a in light heat sos clock battery settings; do
    $V gcc $CF -c -Dapp_event=${a}_app_event -Dsbapp_header=${a}_header -o "$out/app_$a.o" "$apps/$a.c"
    aobjs="$aobjs $out/app_$a.o"
done
$V gcc $CF -DSBOS_KERNEL "-DSB_ASSETS=\"$out/sbassets.sb7\"" -o "$out/sbos-host" \
    "$root/starbite/host/host.c" "$root/starbite/host/sbos-host-stubs.c" "$root/starbite/host/sbos-host-eng.c" \
    "$lib/str.c" "$lib/log.c" "$lib/sbui.c" "$lib/sbui_input.c" "$lib/sbui_page.c" "$lib/sbui_w.c" "$lib/sb7.c" \
    "$lib/fdt.c" "$lib/catalog.c" "$lib/fence.c" "$lib/tmu.c" "$lib/adc.c" "$lib/mali.c" "$lib/mali_desc.c" \
    "$lib/dvfs_levels.c" "$lib/assets.S" \
    "$k/main.c" "$k/timer.c" "$k/appfmt.c" "$k/state.c" "$k/batmodel.c" "$k/heatctl.c" "$k/heater.c" "$k/heatk.c" \
    "$k/services.c" "$k/input.c" "$k/ui.c" "$k/app.c" "$root/starbite/utility/tz.c" $aobjs -lm -lpthread

scene() {
    name=$1
    shift
    if [ -n "${SCENES:-}" ] && ! echo " $SCENES " | grep -q " $name "; then return 0; fi
    $V env ${SBOS_DBG:+SBOS_DBG=1} "$out/sbos-host" "$out" /dev/null "$@" "s:$name" q | grep -E "shot|FAIL|CRASH|dbg|host: (power|reboot|boot)" || true
    "$py" -c "from PIL import Image; Image.open('$out/$name.ppm').save('$png/$name.png')"
}
OPEN="g:1150,2310,0,700 w:400"                      # hold Open on the lock screen
BACK="h:b:900 w:300"                                # Bixby held: Back
scene lock          w:600
scene home          w:300 $OPEN
scene light         w:300 $OPEN t:380,600 w:400
scene light-on      w:300 $OPEN t:380,600 w:400 t:720,650 w:400
scene heat          w:300 $OPEN t:1050,600 w:600
scene heat-confirm  w:300 $OPEN t:1050,600 w:600 t:550,670 w:400
scene heat-running  w:300 $OPEN t:1050,600 w:600 t:550,670 w:400 g:720,1070,0,1900 w:6000
scene heat-off      w:300 $OPEN t:1050,600 w:600 t:550,670 w:400 g:720,1070,0,1900 w:4000 t:220,670 w:3000
scene sos           w:300 $OPEN t:380,1250 w:500
scene sos-countdown w:300 $OPEN t:380,1250 w:500 g:720,815,0,1300 w:600
scene sos-flash     w:300 $OPEN t:380,1250 w:500 g:720,815,0,1300 w:3600
scene clock         w:300 $OPEN t:1050,1250 w:600
scene world         w:300 $OPEN t:1050,1250 w:600 t:720,1100 w:500
scene stopwatch     w:300 $OPEN t:1050,1250 w:600 t:380,1400 w:500 t:720,950 w:2300
scene timer         w:300 $OPEN t:1050,1250 w:600 t:1050,1400 w:500 t:380,950 w:300 t:720,1220 w:2300
scene battery       w:300 $OPEN t:380,1900 w:600
scene settings      w:300 $OPEN t:1050,1900 w:600
scene power-menu    w:300 h:p:1300 w:500
scene focus-keys    w:300 $OPEN k:d w:200 k:d w:200 k:d w:300
scene lock-hold     w:300 g:290,2310,0,300
echo "PNGs in $png"
