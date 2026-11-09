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
SURVX="g:720,2400,-1500,600 w:800 t:1184,1872 w:500 t:718,589 w:500"   # 1.2-alpha17: scrolled (the 4.5 A row)
BATLIM="t:723,2213 w:500"
HOLD="g:720,1170,0,1900 w:400"
OFF="t:720,330 w:600 t:220,670 w:600"   # 1.2-alpha5: the first tap leaves the white screen (on the Heat page: the title)
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
never "$r" "notif: heat" "the user's Off posts no notification (1.2-alpha9)"
never "$r" "trip is OFF" "Survival (default) never touches the trip"
want "$r" "host: flexpmu core 4 on (first: cluster on)" "FlexPMU: the first M3 core's cluster on before its CPU_ON"
n=$(echo "$r" | grep -c "(first: cluster on)")
if [ "$n" = 1 ]; then echo "ok: FlexPMU: cluster on once"; else echo "FAIL: cluster on $n times"; fails=$((fails + 1)); fi
want "$r" "host: flexpmu core 5 off (C2)" "FlexPMU: each M3 core's C2 hint at the stop"
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
want "$r" "heater: core 5 (M3) running" "trip off: every core runs (2 M3s)"
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
want "$r" "notif: heat: Heat stopped" "a heat stop by itself posts a notification (1.2-alpha9)"
want "$r" "heater: every worker core left the load" "and its workers left"
never "$r" "the chip's hardware heat trip is OFF" "the trip never reported off"
ENVS="SBOS_HOST_SMP=m3 SBOS_HOST_NOWDT=1"
r=$(run $OPEN $HEAT $SURVX $HOLD $HOLD $HOLD $HOLD w:3000)
want "$r" "No watchdog to back it" "refused without the loader's watchdog"
never "$r" "the chip's hardware heat trip is OFF" "the trip untouched without a watchdog"
# 1.1-beta5: the battery limit above 55 C only after its own confirmation
ENVS=
r=$(run $OPEN $HEAT $BATLIM $HOLD w:500)
want "$r" "settings: Survival's battery limit raised (confirmed), 0.1 C: 0x00000244" "58 C after its confirmation"
r=$(run $OPEN $HEAT $BATLIM w:300 k:p w:500)
never "$r" "raised (confirmed)" "no raise without the hold"
# 1.2-alpha5: no M3 measurement (every load at once, 4 @ 1794), never past Samsung's table; no LED; the screen white
# with the status strip, a tap brings the Heat page back
px() {  # px SHOT X Y: the pixel's RGB as rrggbb
    python3 -c "import sys
d=open('$out/$1.ppm','rb').read(); p=d.split(b'\\n',3); w=int(p[1].split()[0]); o=len(b'\\n'.join(p[:3]))+1+3*($3*w+$2)
print('%02x%02x%02x' % tuple(d[o:o+3]))"
}
ENVS=SBOS_HOST_SMP=m3
r=$(run $OPEN $HEAT $SURV $HOLD $HOLD $HOLD w:2500 s:surv-white t:720,1500 w:600 s:surv-back w:3000 $OFF w:3000)
never "$r" "heater: survival M3 candidate" "no M3 measurement"
want "$r" "heater: core 5 (M3) running" "every core running at once (2 M3s)"
never "$r" "heater: core 6 (M3) running" "no third M3 core"
want "$r" "heater: heat log on" "the heat log is on"
n=$(echo "$r" | grep -n "heater: core 5 (M3) running" | head -1 | cut -d: -f1)
m=$(echo "$r" | grep -n "heater: start, level" | head -1 | cut -d: -f1)
[ -n "$n" ] && [ -n "$m" ] && echo "note: start at log line $m, core 7 at line $n"
want "$r" "heater: Survival: the screen dark with the readings (no LED, no boost)" "Survival: the dark readings screen by default, no boost, no LED"
never "$r" "the LED on" "Survival never lights the LED"
never "$r" "M3 OCP TABLE VIOLATION" "never more M3 cores than the table allows at the clock asked"
never "$r" "acpm dvfs (m3 ocp table) refused" "the libsb table never had to refuse"
c=$(px surv-white 720 1500); [ "$c" = 000000 ] && echo "ok: the screen is dark (1.2-alpha19 default)" || { echo "FAIL: dark screen pixel $c"; fails=$((fails + 1)); }
c=$(px surv-white 20 40); [ "$c" = 1c1c1e ] && echo "ok: the dark status strip on top" || { echo "FAIL: strip pixel $c"; fails=$((fails + 1)); }
c=$(px surv-back 720 1200); [ "$c" = 1c1c1e ] && echo "ok: a tap brings the normal screen back" || { echo "FAIL: still the dark screen after a tap"; fails=$((fails + 1)); }
want "$r" "heater: the panel level, the white screen and the LED put back" "everything put back at the stop"
# 1.1-beta7: the read-only OCP probe (Settings > About): readable, trapped, no M3 core
SETT_ABOUT="w:300 g:1150,2310,0,700 w:400 t:382,2015 w:800 t:1216,671 w:600"
PROBE="t:723,2252 w:1500"
ENVS=SBOS_HOST_SMP=m3
r=$(run $SETT_ABOUT $PROBE)
want "$r" "ocp probe: PMIC OCP_WARN1 0xD3 0x80" "the PMIC's OCP_WARN read"
want "$r" "ocp probe: MIDR 0x531F0020 OCPTHROTTCTL" "the M3 registers read on core 4"
want "$r" "readable on the M3 (nothing written)" "reported readable"
want "$r" "ocp probe: flexpmu before" "FlexPMU around the probe core"
never "$r" "TRAPPED" "no trap reported"
ENVS="SBOS_HOST_SMP=m3 SBOS_HOST_OCP=trap"
r=$(run $SETT_ABOUT $PROBE)
want "$r" "ocp probe: TRAPPED on the M3" "a trap is caught and reported"
want "$r" "Survival stays inside Samsung's core-count table" "and the fallback said"
ENVS=
r=$(run $SETT_ABOUT $PROBE)
want "$r" "ocp probe: the M3 core did not report" "no M3 core: reported, nothing read"
# 1.2-alpha16: "No limit" asks once (was 8 holds) and is remembered with the trip-off switch across a restart
ST=/tmp/sbos-survival-state.bin         # in the VM: the Mac-shared folder showed a just-written slot late
limactl shell starbite rm -f "$ST"
ENVS="SBOS_HOST_SMP=m3 SBOS_HOST_STATE=$ST"
r=$(run $OPEN $HEAT g:720,2400,-1500,600 w:800 t:1184,1872 w:600 g:720,2400,-1500,600 w:800 g:718,1594,0,1700 w:800 g:720,1170,0,1900 w:1500 w:12000)
want "$r" "settings: Survival (trip off): NO SoC LIMIT (confirmed; saved)" "No limit: one hold, one confirmation"
r=$(run $OPEN $HEAT w:600 s:nl-after)
want "$r" "settings: Survival's SoC limit restored, 0.1 C: 0x00002706" "No limit remembered across a restart"
c=$(px nl-after 720 2000); echo "note: after the restart the Heat page was drawn ($c)"
# 1.2-alpha17: the 4.5 A stop's switch: one hold, one confirmation, remembered
limactl shell starbite rm -f "$ST"
ENVS="SBOS_HOST_SMP=m3 SBOS_HOST_STATE=$ST"
r=$(run $OPEN $HEAT g:720,2400,-1500,600 w:800 g:718,1610,0,1700 w:800 g:720,1170,0,1900 w:1500 w:16000)
want "$r" "settings: Survival's 4.5 A stop: OFF (confirmed; saved; a 6 A / 1 s backstop stays)" "4.5 A stop off: asked once"
r=$(run $OPEN $HEAT w:600)
want "$r" "settings: Survival's current stops restored (bit 0: 4.5 A off, bit 1: 3.5 A off): 0x00000001" "and remembered"
# 1.2-alpha18: with the 4.5 A stop off, B2SOVRC off for the run and back at the stop; a run that never stops: back
# at the next boot (the slot's note)
r=$(run $OPEN $HEAT $SURV $HOLD $HOLD $HOLD w:4000 $OFF w:3000)
want "$r" "heater: B2SOVRC OFF for this Survival run (the user's choice), CHG_CNFG_05 was 0x0000003C" "B2SOVRC off at the start"
want "$r" "heater: B2SOVRC back (the stop), CHG_CNFG_05 0x3C" "and back at the stop"
want "$r" "fuel gauge's lowest-voltage / highest-current trackers cleared" "the gauge's trackers cleared at the start"
r=$(run $OPEN $HEAT $SURV $HOLD $HOLD $HOLD w:4000)
want "$r" "heater: B2SOVRC OFF for this Survival run" "B2SOVRC off again"
r=$(run w:1500)
want "$r" "heater: B2SOVRC back (the boot after a Survival run that did not stop), CHG_CNFG_05 0x3C" "a run that never stopped: back at the next boot"
want "$r" "boot: since the last Survival start: lowest cell" "the boot's readout"
ENVS=
echo "survival test: $fails failed"
[ "$fails" = 0 ]
