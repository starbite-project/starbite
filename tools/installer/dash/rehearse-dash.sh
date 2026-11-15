#!/bin/bash
# tools/installer/dash/rehearse-dash.sh [WORKDIR]: the night dashboard (sbdash-mac.sh + sbdash.sh) against a FAKE
# S9 #2 (tools/installer/fakes: the installer environment over the fake ssh) and a fake night log. No phone.
# Checks: the watcher starts the dashboard only for the right serial and not inside a GPT / card-store write; the
# frame shows the steps, the substep's bytes and ETA, the USB counters and rates, the storage I/O, the battery, the
# night log and the substep output; the backlight dims when idle and comes back on a touch or a new step; --stop puts
# the backlight back and ends it; nothing under the fake phone's /dev/block changes.
set -uo pipefail
cd "$(dirname "$0")/../../.."
REPO=$PWD
W=${1:-build/dash-rehearsal}
rm -rf "$W"; mkdir -p "$W"; W=$(cd "$W" && pwd)
export SBINST_STATE=$W/state SBDASH_LOG=$W/sbdash-mac.log SBDASH_PIDFILE=$W/sbdash-mac.pid
export SBDASH_NIGHT_LOG=$W/night.log SBDASH_NIGHT_DIR=$W/nb SBDASH_USBPEEK=/nonexistent
pass=0 fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
R=$W/s2
python3 -B tools/installer/fakes/fakephone.py create "$R" --state env --gpt starbite-oneui --card starbite --layout sd64l --card-sectors 124735488 >/dev/null
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=S9TWO02/' "$R/props" && rm "$R/props.bak"
mkdir -p "$W/state/env" "$W/nb/logs"
ssh-keygen -q -t ed25519 -N '' -f "$W/state/env/id_ed25519"; cp "$W/state/env/id_ed25519.pub" "$R/authorized_keys"
# the fake phone's /sys: usb0 counters, the UDC, the battery, a thermal zone, the backlight
N=$R/sys/class/net/usb0/statistics; mkdir -p "$N" "$R/sys/class/udc/10c00000.dwc3" "$R/sys/class/power_supply/battery" \
	"$R/sys/class/thermal/thermal_zone0" "$R/sys/class/backlight/panel"
for f in rx_bytes tx_bytes rx_packets tx_packets rx_errors tx_errors rx_dropped tx_dropped; do echo 0 > "$N/$f"; done
echo configured > "$R/sys/class/udc/10c00000.dwc3/state"; echo high-speed > "$R/sys/class/udc/10c00000.dwc3/current_speed"
echo 1500 > "$R/sys/class/net/usb0/mtu"; echo 1 > "$R/sys/class/net/usb0/carrier"
B=$R/sys/class/power_supply/battery; echo Battery > "$B/type"; echo 64 > "$B/capacity"; echo 3987000 > "$B/voltage_now"
echo 812000 > "$B/current_now"; echo 312 > "$B/temp"; echo Charging > "$B/status"
echo 41500 > "$R/sys/class/thermal/thermal_zone0/temp"
echo 200 > "$R/sys/class/backlight/panel/brightness"; echo 255 > "$R/sys/class/backlight/panel/max_brightness"
touch "$W/marker"; sleep 1          # nothing under /dev/block may be written after this
# a night in the middle of step G: WINDOWS being streamed
cat > "$W/night.log" <<'LOG'
######## night-s9b (real) 2026-11-15 22:14:02: S9 #1 = source (TWRP, read only), S9 #2 = phone-x (installer environment)
### step P started (22:14:02)
### step P done (22:16:40)
### step C started (22:16:40)
### step C done (22:18:11)
### step G started (22:40:00)
   [22:40:01] G-esp-dry: python3 -B tools/installer/stream-copy.py ...
   [22:40:05] G-esp: python3 -B tools/installer/stream-copy.py ...
      ok   part:WINESP on S9TWO02 = part:WINESP of S9ONE01: 512.0 MiB, sha256 0123456789abcdef... (both phones agree)
   [22:41:10] G-windows-dry: python3 -B tools/installer/stream-copy.py ...
   [22:41:14] G-windows: python3 -B tools/installer/stream-copy.py ...
LOG
cat > "$W/nb/logs/G-windows.txt" <<'TXT'
== stream-copy (REAL) ==
   source: S9ONE01 part:WINDOWS = /dev/block/sda27 at byte 0, 22.0 GiB (23622320128 bytes)
   512 / 22528 MiB (24 MiB/s over USB, the rest zeros)
   1024 / 22528 MiB (25 MiB/s over USB, the rest zeros)
TXT
WATCH=(bash tools/installer/dash/sbdash-mac.sh S9TWO02 --fake-root "$R")

echo "######## refusals"
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=OTHER/' "$R/props"
"${WATCH[@]}" --once 1
grep -q 'is OTHER, not S9TWO02: nothing pushed' "$W/sbdash-mac.log" && [ ! -e "$R/tmp/sbdash/pid" ] && ok "another phone's environment: nothing pushed, nothing started" || bad "serial check"
mv "$R/props.bak" "$R/props"
cp "$W/night.log" "$W/night.keep"; echo '   [22:42:00] G-store: python3 -B tools/installer/store-update.py ...' >> "$W/night.log"
"${WATCH[@]}" --once 1
grep -q 'inside a GPT or card-store write: starting it later' "$W/sbdash-mac.log" && [ ! -e "$R/tmp/sbdash/pid" ] && ok "inside a card-store write: not started (later)" || bad "store-write deferral"
mv "$W/night.keep" "$W/night.log"

echo; echo "######## the dashboard"
"${WATCH[@]}" --once 1
sleep 1
[ -e "$R/tmp/sbdash/pid" ] && ok "started between writes (G-windows is a stream, not a GPT/store write)" || bad "not started"
( for i in 1 2 3 4 5 6 7 8; do echo $((i * 25000000)) > "$N/rx_bytes"; echo $((i * 1300000)) > "$N/tx_bytes"
	echo $((i * 17000)) > "$N/rx_packets"; echo $((i * 9000)) > "$N/tx_packets"; sleep 1; done ) &
"${WATCH[@]}" --once 3
sleep 3
S=$R/tmp/sbdash/screen
for want in 'STARBITE INSTALLER' '== STEPS' 'G: Windows 11 \(started\)' 'substep: .*G-windows' 'WRITING' '1.00 GiB of 22.00 GiB   25.0 MiB/s   ETA 14m' 'ETA ' \
            '== USB' 'UDC .*configured' 'speed high-speed' 'rx .* in [0-9]+ pkts' 'Mac -> phone' '== RAW usb0 FRAMES' \
            '== STORAGE I/O' '== PHONE' 'battery 64% Charging  3.99 V  \+812 mA  31.2 C' 'SoC temp 41.5 C' \
            '== NIGHT LOG' 'step C done' '== SUBSTEP OUTPUT  \(G-windows\)' '1024 / 22528 MiB'; do
	sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$S" | grep -qE -- "$want" && ok "frame: $want" || bad "frame lacks /$want/"
done
grep -q $'\033\\[1;30;42m P ' "$S" && grep -q $'\033\\[1;30;43m G ' "$S" && ok "steps coloured: P done (green), G running (yellow)" || bad "step colours"
kill %1 2>/dev/null; wait 2>/dev/null

sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$S" | awk -v w=90 'length($0) > w { bad = 1 } END { exit bad }' && ok "every frame line fits 90 columns (16x32 font on 1440 px)" || bad "a line is wider than 90 columns"
echo; echo "######## the dim, the touch, a new step (a dashboard started with 3 s / 4 s instead of 30 / 10 min)"
touch "$R/tmp/sbdash/stop"; sleep 3
bash -c "cd '$R' && sh '$REPO/tools/installer/dash/sbdash.sh' '$R/tmp/sbdash' '$R/tmp/sbdash/screen' '$R/sys/' '/proc/' 3 4 </dev/null >/dev/null 2>&1 &"
sleep 8
b=$(cat "$R/sys/class/backlight/panel/brightness")
[ "$b" -lt 200 ] && ok "idle: the backlight is down to $b of 200" || bad "no dim ($b)"
sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$R/tmp/sbdash/screen" | grep -q 'dimmed: tap to wake' && ok "the frame says: dimmed, tap to wake" || bad "no dim note"
date +%s > "$R/tmp/sbdash/touch"; sleep 2
[ "$(cat "$R/sys/class/backlight/panel/brightness")" = 200 ] && ok "a touch: back to 200 at once" || bad "touch did not wake ($(cat "$R/sys/class/backlight/panel/brightness"))"
sleep 7
b=$(cat "$R/sys/class/backlight/panel/brightness")
sed -i.bak 's/^step=.*/step=H: iOS 27 (started)/' "$R/tmp/sbdash/feed"; sleep 2
[ "$b" -lt 200 ] && [ "$(cat "$R/sys/class/backlight/panel/brightness")" = 200 ] && ok "dimmed again ($b), then a new step: back to 200" || bad "new-step wake ($b -> $(cat "$R/sys/class/backlight/panel/brightness"))"

echo; echo "######## --stop"
SBDASH_PIDFILE=$W/sbdash-mac.pid bash tools/installer/dash/sbdash-mac.sh S9TWO02 --fake-root "$R" --stop >/dev/null
sleep 3
[ ! -e "$R/tmp/sbdash/pid" ] && ok "the dashboard ended" || bad "still running"
[ "$(cat "$R/sys/class/backlight/panel/brightness")" = 200 ] && ok "the backlight is back at 200" || bad "backlight not restored"
[ -z "$(find "$R/dev/block" -type f -newer "$W/marker")" ] && ok "nothing under the fake phone's /dev/block changed" || bad "a block device changed"
echo; echo "--- the last frame (colours removed) ---"
sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$S" | head -60
echo; echo "######## result: $pass passed, $fail failed"
[ "${KEEP:-0}" = 1 ] || [ "$fail" != 0 ] || rm -rf "$W"
[ "$fail" = 0 ]
