#!/bin/sh
# 1.2-alpha26 (the user: "starbite os didn't know the time"): the PMIC clock read at boot and logged; a lost PMIC clock
# (2000-01-01 after a brownout) with a saved network: Wi-Fi once for the time, the PMIC clock written, Wi-Fi off; the
# last known time kept in the state slot and shown as approximate when the PMIC clock is lost. Host model:
# SBOS_HOST_RTC_LOST=1. Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_NORMAL=1 $ENVS "$out/sbos-host" "$out" /dev/null "$@" q 2>&1; }
NET="test-ap
correct horse"
ENVS=
r=$(run w:600)
want "$r" "clock: the PMIC clock reads 2026-" "the PMIC clock read and logged at boot"
want "$r" "clock: the PMIC clock is used" "a good PMIC clock is used"
never "$r" "no clock at boot" "a good clock: no Wi-Fi at boot"
ENVS="SBOS_HOST_RTC_LOST=1"
r=$(run w:600)
want "$r" "LOST (before 2020" "a lost PMIC clock is named"
want "$r" "clock: no time yet" "no saved time and no network: no time"
never "$r" "no clock at boot: on once" "no saved network: no Wi-Fi at boot"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_NORMAL=1 SBOS_HOST_RTC_LOST=1 "SBOS_HOST_WIFINET=$NET" "$out/sbos-host" "$out" /dev/null w:60000 q 2>&1)
want "$r" "wifi: no clock at boot: on once for the time" "a lost clock and a saved network: Wi-Fi once"
want "$r" "clock: the PMIC clock set from the network" "the network's time written into the PMIC clock"
want "$r" "host: the PMIC clock written" "(the model saw the write)"
want "$r" "the boot's time check ends: the clock is set; Wi-Fi off again" "then Wi-Fi off again"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_NORMAL=1 SBOS_HOST_RTC_LOST=1 SBOS_HOST_SELFTEST=1 "SBOS_HOST_WIFINET=$NET" "$out/sbos-host" "$out" /dev/null w:3000 q 2>&1)
never "$r" "no clock at boot: on once" "never in a self test (it has its own Wi-Fi step)"
ST=/tmp/sbos-clock-state.bin
limactl shell starbite rm -f "$ST"
ENVS="SBOS_HOST_STATE=$ST"
r=$(run w:600 g:720,1375,0,800 w:6000)
want "$r" "settings: saved" "a settings save keeps the time"
ENVS="SBOS_HOST_STATE=$ST SBOS_HOST_RTC_LOST=1"
r=$(run w:600)
want "$r" "clock: the last known time (the state slot) 2026-" "the last known time read back from the slot"
want "$r" "clock: the last known time, shown as approximate" "and used, marked approximate"
limactl shell starbite rm -f "$ST"
echo "clock test: $fails failed"
