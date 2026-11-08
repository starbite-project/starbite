#!/bin/sh
# 1.1: Survival heat on the host OS: the three / four confirmations, the trip-off order (the trip off only with the
# short watchdog; at the end the loads off, then the trip back exactly, then the watchdog back), the heat loop as the
# watchdog's only feed (never 4 s without one), and the refusals of a TMU that does not take the write and of a
# missing watchdog (nothing left changed). Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
OPEN="w:300 g:1150,2310,0,700 w:400"
HEAT="t:1057,663 w:600"
SURV="t:723,1927 w:500"
SURVX="t:723,2203 w:500 t:723,1927 w:500"
HOLD="g:720,1170,0,1900 w:400"
OFF="t:220,670 w:600"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
order() { a=$(echo "$1" | grep -n -- "$2" | head -1 | cut -d: -f1); b=$(echo "$1" | grep -n -- "$3" | tail -1 | cut -d: -f1)
          if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then echo "ok: $4"; else echo "FAIL: $4 ($2 before $3: $a $b)"; fails=$((fails + 1)); fi; }
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 $ENVS "$out/sbos-host" "$out" /dev/null "$@" q 2>&1; }
ENVS=SBOS_HOST_SMP=m3
r=$(run $OPEN $HEAT $SURV $HOLD $HOLD $HOLD w:7000 $OFF w:3000)
want "$r" "Survival heat confirmed three times" "Survival: three confirmations"
want "$r" "heater: start, level 0x00000007" "Survival started"
want "$r" "heater: stopped: Stopped" "Off stopped Survival"
never "$r" "trip is OFF" "Survival (default) never touches the trip"
want "$r" "host: flexpmu core 4 on (first: cluster on)" "FlexPMU: the first M3 core's cluster on before its CPU_ON"
n=$(echo "$r" | grep -c "(first: cluster on)")
if [ "$n" = 1 ]; then echo "ok: FlexPMU: cluster on once"; else echo "FAIL: cluster on $n times"; fails=$((fails + 1)); fi
want "$r" "host: flexpmu core 7 off (C2)" "FlexPMU: each M3 core's C2 hint at the stop"
never "$r" "cluster off" "FlexPMU: never the cluster-off request (beta2's hang)"
want "$r" "heater: flexpmu at the start" "FlexPMU: logged at the start"
want "$r" "heater: step: PSCI CPU_ON core 0x00000004" "breadcrumbs: before each CPU_ON"
want "$r" "heater: flexpmu after the stop" "FlexPMU: logged after the stop"
r=$(run $OPEN $HEAT $SURV $HOLD $HOLD w:3000)
never "$r" "heater: start, level" "two holds of three do not start it"
r=$(run $OPEN $HEAT $SURVX $HOLD $HOLD $HOLD $HOLD w:9000 $OFF w:4000)
want "$r" "four times" "trip off: four confirmations"
want "$r" "heater: start, level 0x00000008" "trip off: started"
want "$r" "heater: gpu on, 598 MHz" "trip off: the GPU runs its load"
never "$r" "TMU_SUB) is not armed" "trip off: the GPU is not refused for the trip we switched off"
never "$r" "holding" "trip off: flat out, nothing shed while it ran"
want "$r" "heater: core 7 (M3) running" "trip off: every core runs"
order "$r" "host: watchdog shortened to 5 s" "the chip's hardware heat trip is OFF" "the watchdog is short before the trip goes off"
order "$r" "heater: 0 s bat" "the chip's hardware heat trip is OFF" "the trip goes off only after the session's first loop turn"
want "$r" "heater: step: trip off, the watchdog fed" "the short watchdog fed right after the trip write"
want "$r" "after the write: TMU_TOP CONTROL" "the trip's register read back after the write"
order "$r" "heater: stopped: Stopped" "heater: the chip's hardware heat trip back on" "the loads off before the trip comes back"
order "$r" "heater: the chip's hardware heat trip back on" "heater: the watchdog back to the loader's" "the trip back before the watchdog's reload"
want "$r" "heater: restored: TMU_TOP CONTROL" "the trip read back exactly as saved"
never "$r" "DID NOT READ BACK" "no mismatch after the restore"
never "$r" "WATCHDOG WOULD HAVE FIRED" "the heat loop fed the short watchdog in time"
ENVS="SBOS_HOST_SMP=m3 SBOS_HOST_TMU_RO=1"
r=$(run $OPEN $HEAT $SURVX $HOLD $HOLD $HOLD $HOLD w:3000)
want "$r" "not writable from EL1 here" "a TMU that does not take the write is reported"
want "$r" "cannot be switched off from here" "and refused"
want "$r" "host: watchdog back to the loader's" "the watchdog put back after the refusal"
want "$r" "heater: stopped: The chip's heat trip cannot be switched off from here" "the session stopped at its first loop turn"
want "$r" "heater: every worker core left the load" "and its workers left"
never "$r" "the chip's hardware heat trip is OFF" "the trip never reported off"
ENVS="SBOS_HOST_SMP=m3 SBOS_HOST_NOWDT=1"
r=$(run $OPEN $HEAT $SURVX $HOLD $HOLD $HOLD $HOLD w:3000)
want "$r" "No watchdog to back it" "refused without the loader's watchdog"
never "$r" "the chip's hardware heat trip is OFF" "the trip untouched without a watchdog"
echo "survival test: $fails failed"
[ "$fails" = 0 ]
