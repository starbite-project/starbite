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
CF="-O1 -g -DSB_HOST -DSB_HOST_UTILITY -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing -I$lib -I$k -I$sdk -I$root/starbite/utility -I$root/starbite/engineer -I$root/starbite/wifi -I$root/starbite/guest/sbos"
aobjs=""
for a in emergency light heat sos info clock battery settings wifi calc notes; do
    $V gcc $CF -c -Dapp_event=${a}_app_event -Dsbapp_header=${a}_header -o "$out/app_$a.o" "$apps/$a.c"
    aobjs="$aobjs $out/app_$a.o"
done
$V gcc $CF -DSBOS_KERNEL "-DSB_ASSETS=\"$out/sbassets.sb7\"" -o "$out/sbos-host" \
    "$root/starbite/host/host.c" "$root/starbite/host/sbos-host-stubs.c" "$root/starbite/host/sbos-host-eng.c" \
    "$lib/str.c" "$lib/log.c" "$lib/sbui.c" "$lib/sbui_input.c" "$lib/sbui_page.c" "$lib/sbui_w.c" "$lib/sb7.c" \
    "$lib/fdt.c" "$lib/catalog.c" "$lib/fence.c" "$lib/tmu.c" "$lib/adc.c" "$lib/mali.c" "$lib/mali_desc.c" \
    "$lib/dvfs_levels.c" "$lib/assets.S" \
    "$k/main.c" "$k/timer.c" "$k/appfmt.c" "$k/state.c" "$k/batmodel.c" "$k/heatctl.c" "$k/heater.c" "$k/heatk.c" \
    "$k/services.c" "$k/input.c" "$k/ui.c" "$k/app.c" "$k/torch.c" "$k/vib.c" "$k/alarm.c" "$k/notif.c" "$k/panel.c" "$k/gpuheat.c" "$k/uitest.c" "$k/settings.c" "$k/slot.c" "$k/wifi.c" "$k/look.c" "$k/fled.c" "$k/audio.c" "$root/starbite/guest/sbos/guest.c" \
    "$root/starbite/wifi/core.c" "$root/starbite/wifi/exynos_pcie.c" "$root/starbite/wifi/chip.c" "$root/starbite/wifi/fw.c" \
    "$root/starbite/wifi/ring.c" "$root/starbite/wifi/msgbuf.c" "$root/starbite/wifi/link.c" "$root/starbite/wifi/wpa.c" \
    "$root/starbite/wifi/net.c" "$root/starbite/wifi/host/model.c" "$root/starbite/wifi/host/model_ap.c" "$root/starbite/utility/tz.c" $aobjs -lm -lpthread

scene() {
    name=$1
    shift
    if [ -n "${SCENES:-}" ] && ! echo " $SCENES " | grep -q " $name "; then return 0; fi
    $V env ${SBOS_DBG:+SBOS_DBG=1} SBOS_HOST_FILES=$root/starbite/sbos/examples ${SCENE_ENV:-X=1} "$out/sbos-host" "$out" /dev/null "$@" "s:$name" q | grep -E "shot|FAIL|CRASH|dbg|host: (power|reboot|boot)" || true
    "$py" -c "from PIL import Image; Image.open('$out/$name.ppm').save('$png/$name.png')"
}
OPEN="g:1150,2310,0,700 w:400"                      # hold Open on the lock screen
BACK="h:b:900 w:300"                                # Bixby held: Back
# home (0.3): Emergency across the top (720,245); tiles: Light 382,663 Heat 1057,663 SOS 382,1250 Info 1057,1250
# Clock 382,1545 Battery 1057,1545 Settings 382,2015 Wi-Fi 1057,2015 Calculator 382,2485 Notes 1057,2485 (5 rows, 1.2-alpha20)
scene lock          w:600
scene home          w:300 $OPEN
scene emergency     w:300 $OPEN t:720,245 w:500
scene emergency-call w:300 $OPEN t:720,245 w:500 g:720,635,0,1500 w:500
scene emergency-msg w:300 $OPEN t:720,245 w:500 t:382,925 w:400 t:720,725 w:400
scene light         w:300 $OPEN t:382,663 w:400
scene light-on      w:300 $OPEN t:382,663 w:400 t:720,1395 w:400
scene light-red-low w:300 $OPEN t:382,663 w:400 t:220,1070 w:300 t:1057,1705 w:400
scene light-led     w:300 $OPEN t:382,663 w:400 t:720,655 w:600
scene heat          w:300 $OPEN t:1057,663 w:600
scene heat-confirm  w:300 $OPEN t:1057,663 w:600 t:720,670 w:400
scene heat-running  w:300 $OPEN t:1057,663 w:600 t:720,670 w:400 g:720,1070,0,1900 w:25000
scene heat-manual   w:300 $OPEN t:1057,663 w:600 t:1057,1302 w:500
scene heat-manual-2 w:300 $OPEN t:1057,663 w:600 t:1057,1302 w:500 t:720,1150 w:300 t:720,1150 w:300
scene heat-manual-scrolled w:300 $OPEN t:1057,663 w:600 t:1057,1302 w:500 g:720,2400,-1500,600 w:400
scene control-em    w:300 $OPEN t:700,50 w:500
scene aod-clock     w:300 $OPEN t:700,50 w:500 t:1184,1066 w:400 t:220,215 w:400 w:33000
scene heat-pocket   w:300 $OPEN t:1057,663 w:600 t:720,670 w:400 g:720,1070,0,1900 w:36000 t:720,1400 w:400
scene heat-unlocked w:300 $OPEN t:1057,663 w:600 t:720,670 w:400 g:720,1070,0,1900 w:36000 k:p w:500
scene home-busy     w:300 $OPEN t:1057,663 w:600 t:720,670 w:400 g:720,1070,0,1900 w:3000 $BACK w:1200
scene heat-off      w:300 $OPEN t:1057,663 w:600 t:720,670 w:400 g:720,1070,0,1900 w:4000 t:220,670 w:3000
scene sos           w:300 $OPEN t:382,1250 w:500
scene sos-countdown w:300 $OPEN t:382,1250 w:500 g:720,635,0,1300 w:600
scene sos-flash     w:300 $OPEN t:382,1250 w:500 g:720,635,0,1300 w:3600
scene info          w:300 $OPEN t:1057,1250 w:600
scene clock         w:300 $OPEN t:382,1545 w:600
scene world         w:300 $OPEN t:382,1545 w:600 t:720,1100 w:500
scene stopwatch     w:300 $OPEN t:382,1545 w:600 t:380,1400 w:500 t:720,950 w:2300
scene timer         w:300 $OPEN t:382,1545 w:600 t:1050,1400 w:500 t:380,950 w:300 t:720,1220 w:2300
scene battery       w:300 $OPEN t:1057,1545 w:600
scene battery-history w:300 $OPEN w:3700000 k:p w:500 $OPEN t:1057,1545 w:600
scene settings      w:300 $OPEN t:382,2015 w:600
scene settings-on   w:300 $OPEN t:382,2015 w:600 t:720,1789 w:300 t:720,2039 w:300
scene settings-alarm w:300 $OPEN t:382,2015 w:600 t:557,670 w:400
scene settings-heat w:300 $OPEN t:382,2015 w:600 t:882,670 w:400
scene settings-about w:300 $OPEN t:382,2015 w:600 t:1207,670 w:400
scene wifi          w:300 $OPEN t:1057,2015 w:600
scene wifi-list     w:300 $OPEN t:1057,2015 w:600 t:720,625 w:3000
scene wifi-joined   w:300 $OPEN t:1057,2015 w:600 t:720,625 w:3000 t:720,905 w:6000
SCENE_ENV=SBOS_HOST_WPA2=1
scene wifi-passphrase w:300 $OPEN t:1057,2015 w:600 t:720,625 w:3000 t:720,905 w:800 t:580,2622 w:150 t:1210,2246 w:150 t:510,2246 w:300
SCENE_ENV=
SCENE_ENV=SBOS_HOST_FILES=/nonexistent
scene info-empty    w:300 $OPEN t:1057,1250 w:600
scene info-edit     w:300 $OPEN t:1057,1250 w:600 t:720,1150 w:500
scene info-field    w:300 $OPEN t:1057,1250 w:600 t:720,1150 w:500 t:720,655 w:500 t:930,2434 w:150 t:1210,2246 w:150 t:510,2246 w:300
CARD="t:1057,1250 w:600 t:720,1150 w:500 t:720,655 w:500 t:930,2434 w:150 t:1210,2246 w:150 t:1245,2810 w:400 t:720,965 w:500 t:125,2810 w:150 t:1350,2246 w:150 t:1245,2810 w:400 t:382,2515 w:500"
scene lock-card     w:300 $OPEN $CARD k:p w:600 k:p w:800
scene lock-card-open w:300 $OPEN $CARD k:p w:600 k:p w:800 g:720,1715,0,900 w:600
SCENE_ENV=SBOS_HOST_NORMAL=1
# 0.6: normal mode (wallpaper, glass); home icons: Light 232,730 Heat 557,730 SOS 882,730 Info 1207,730
# Clock 232,1090 Battery 557,1090 Settings 882,1090 Wi-Fi 1207,1090; Emergency pill 720,2525; status bar tap 700,50
scene n-lock        w:600
scene n-home        w:300 $OPEN
scene n-control     w:300 $OPEN t:700,50 w:500
scene n-heat        w:300 $OPEN t:557,730 w:600
scene n-light       w:300 $OPEN t:232,730 w:600
scene n-battery     w:300 $OPEN t:557,1090 w:600
scene n-settings    w:300 $OPEN t:882,1090 w:600
scene n-wifi-list   w:300 $OPEN t:1207,1090 w:600 t:720,625 w:3000
scene n-emergency   w:300 $OPEN t:720,2525 w:600
scene n-emergency-mode w:300 $OPEN t:700,50 w:500 t:1220,625 w:500 h:b:900 w:1000
scene n-dusk        w:300 $OPEN t:700,50 w:500 t:551,2182 w:800 h:b:900 w:1000
scene n-ocean-lock  w:300 $OPEN t:700,50 w:500 t:882,2182 w:800 h:b:900 w:800 k:p w:600 k:p w:800
SCENE_ENV=
scene power-menu    w:300 h:p:1300 w:500
scene focus-keys    w:300 $OPEN k:d w:200 k:d w:200 k:d w:300
scene lock-hold     w:300 g:290,2310,0,300
# 1.2-alpha20: Calculator (keys: columns x 224 552 884 1212, rows y 988 1328 1668 2008) and Notes
CALC="t:382,2485 w:600"
scene calc          w:300 $OPEN $CALC
scene calc-chain    w:300 $OPEN $CALC t:224,1668 w:150 t:552,1668 w:150 t:1212,2008 w:150 t:224,988 w:150 t:1212,1328 w:150 t:552,1668 w:150 t:884,2008 w:400
scene calc-third    w:300 $OPEN $CALC t:224,1668 w:150 t:1212,988 w:150 t:884,1668 w:150 t:884,2008 w:400
scene calc-op       w:300 $OPEN $CALC t:224,1668 w:150 t:552,1668 w:150 t:1212,2008 w:150 t:224,988 w:400
NOTES="t:1057,2485 w:600"
scene notes         w:300 $OPEN $NOTES
scene notes-new     w:300 $OPEN $NOTES t:720,636 w:600
scene notes-saved   w:300 $OPEN $NOTES t:720,636 w:600 t:125,2622 w:150 t:860,2434 w:150 t:1070,2246 w:150 t:1245,2810 w:600
scene notes-note    w:300 $OPEN $NOTES t:720,636 w:600 t:125,2622 w:150 t:860,2434 w:150 t:1070,2246 w:150 t:1245,2810 w:600 t:720,636 w:600
echo "PNGs in $png"
# SBOS_SCREENS=DIR: every screen and one contact sheet there too (the main checkout's build/starbite-os/screens/)
if [ -n "${SBOS_SCREENS:-}" ]; then
    mkdir -p "$SBOS_SCREENS"
    cp "$png"/*.png "$SBOS_SCREENS"/
    "$py" "$root/starbite/host/sbos-contact.py" "$SBOS_SCREENS/contact-sheet.png" "$SBOS_SCREENS" 8 288
fi
