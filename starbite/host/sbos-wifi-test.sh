#!/bin/sh
# Starbite OS Wi-Fi on the host (docs/STARBITE-OS.md 9.4): the real core against its model. An open network joined,
# DHCP and the router's ping; the saved WPA2 network (SBOS_HOST_WIFINET) joined the same way; a wrong passphrase
# refused; 20 on / off cycles reuse the DMA blocks and still join; the heater refused while Wi-Fi is on; no names
# or addresses in the log. Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
OPEN="w:300 g:1150,2310,0,700 w:400 t:1057,1738 w:600"       # the lock screen's Open, then the Wi-Fi tile
ON="t:720,625 w:3000"                                           # the toggle, the bring-up and the scan
JOIN="t:720,905 w:9000"                                         # the first network
fails=0
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 ${NET:+"SBOS_HOST_WIFINET=$NET"} "$out/sbos-host" "$out" /dev/null "$@" s:wifi-test q 2>&1; }
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
nowant() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
NET=
a=$(run $OPEN $ON $JOIN)
want "$a" "wifi: the scan heard 1 networks" "open: the scan"
want "$a" "wifi: an address from DHCP" "open: DHCP"
want "$a" "wifi: the gateway answers a ping" "open: the router's ping"
want "$a" "wifi: the clock was checked over the network" "open: the clock checked over the internet (NTP)"
nowant "$a" "test-ap\|192\.168\|02:11:22:33" "open: no name or address logged"
nowant "$a" "a loop turn took" "open: no slow loop turn"
NET="test-ap
correct horse"
b=$(run $OPEN $ON $JOIN)
want "$b" "wifi: a saved network is set" "saved: read"
want "$b" "THE NETWORK IS JOINED" "saved: the WPA2 handshake"
want "$b" "wifi: an address from DHCP" "saved: DHCP"
want "$b" "wifi: the gateway answers a ping" "saved: the router's ping"
nowant "$b" "correct horse\|test-ap" "saved: no name or passphrase logged"
NET="test-ap
wrong passphrase"
c=$(run $OPEN $ON t:720,905 w:17000)                            # 0.6.1: the join's 15 s timeout runs in the loop
want "$c" "the network did not take the phone" "a wrong passphrase refused"
nowant "$c" "a loop turn took" "a wrong passphrase: the loop never blocked during the join (0.6.1)"
nowant "$c" "wifi: an address from DHCP" "no address with a wrong passphrase"
# 1.2-alpha21: the automatic join (a known network heard after the scan: once per power-on) and its switch
NET="test-ap
correct horse"
d=$(run $OPEN $ON w:9000)
want "$d" "joining it automatically" "auto: the saved network joined without a tap"
want "$d" "THE NETWORK IS JOINED" "auto: the WPA2 handshake"
want "$d" "wifi: an address from DHCP" "auto: DHCP"
nowant "$d" "correct horse\|test-ap" "auto: no name or passphrase logged"
KNOWN="t:720,1184 w:500"; AUTO="t:720,624 w:400"; DONE="h:b:900 w:300 t:1057,1738 w:600"   # Back: home, Wi-Fi again
e=$(run $OPEN $KNOWN $AUTO $DONE $ON w:9000)
want "$e" "settings: Wi-Fi never joins by itself" "auto off: the switch"
nowant "$e" "joining it automatically" "auto off: no join by itself"
nowant "$e" "THE NETWORK IS JOINED" "auto off: not joined"
NET=
CYC=""
i=0
while [ $i -lt 20 ]; do CYC="$CYC t:720,625 w:1500 t:720,625 w:600"; i=$((i + 1)); done
# shellcheck disable=SC2086
d=$(run $OPEN $CYC $ON $JOIN)
want "$d" "wifi: DMA blocks: 0 new, 10 reused" "on / off: the DMA blocks reused"
want "$d" "wifi: an address from DHCP" "on / off: a join after 20 cycles"
nowant "$d" "the bring-up failed" "on / off: no failed bring-up"
# a typed passphrase on the on-screen keyboard (the model's WPA2 network, "correct horse"); then Back and in again:
# the network is remembered ("Saved") for this session
R0=2246; R1=2434; R2=2622; R3=2810
TYPE="t:580,$R2 w:150 t:1210,$R0 w:150 t:510,$R0 w:150 t:510,$R0 w:150 t:370,$R0 w:150 t:580,$R2 w:150 t:650,$R0 w:150 t:545,$R3 w:150 t:860,$R1 w:150 t:1210,$R0 w:150 t:510,$R0 w:150 t:300,$R1 w:150 t:370,$R0 w:300"
NET=
# shellcheck disable=SC2086
g=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_WPA2=1 "$out/sbos-host" "$out" /dev/null $OPEN $ON t:720,905 w:800 $TYPE t:1245,$R3 w:10000 s:wifi-test q 2>&1)
want "$g" "THE NETWORK IS JOINED" "typed: the WPA2 handshake"
want "$g" "wifi: an address from DHCP" "typed: DHCP"
nowant "$g" "correct horse" "typed: the passphrase not logged"
# a short passphrase: refused before any join
h=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_WPA2=1 "$out/sbos-host" "$out" /dev/null $OPEN $ON t:720,905 w:800 t:580,$R2 w:150 t:1245,$R3 w:3000 s:wifi-test q 2>&1)
nowant "$h" "wifi: joining a secured" "typed: a 1-character passphrase refused"
# no carry-over (0.5.5): 13 characters typed, Cancel, the same network again, Join at once: nothing joins
# (the field came back empty: under 8 characters is refused before any join)
CANCEL="t:382,1437 w:500"
k=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_WPA2=1 "$out/sbos-host" "$out" /dev/null $OPEN $ON t:720,905 w:800 $TYPE $CANCEL t:720,905 w:800 t:1245,$R3 w:3000 s:wifi-test q 2>&1)
nowant "$k" "wifi: joining a secured" "typed: the passphrase does not carry over after Cancel"
e=$(run $OPEN $ON h:b:900 w:500 t:1057,663 w:600 t:720,670 w:600)
want "$e" "heater: refused: Wi-Fi is on" "the heater refused while Wi-Fi is on"
nowant "$e" "heater: start, level" "the heater did not start"
echo "wifi test: $fails failed"
[ "$fails" = 0 ]
