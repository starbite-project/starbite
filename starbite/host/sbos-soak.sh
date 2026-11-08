#!/bin/sh
# Starbite OS host soak (docs/STARBITE-OS.md 12.5): the whole UI driven by keys and holds for minutes of fake time
# while the heater runs, under injected worker-core failures, on host.c's fake hardware. Every run must end in a
# reboot from the power menu ("Boot menu"), with no crash, no unpresented frame, no "event queue full", no loop
# turn of 200 ms or more, and the case's own expected log lines. Run on the Mac (after sbos-preview.sh built the
# host binary):   sh starbite/host/sbos-soak.sh
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
V="limactl shell --workdir $root starbite"
OPEN="g:1150,2310,0,700 w:500"
D="k:d w:150"
SEL="k:b w:400"
BACK="h:b:900 w:500"
HOLDSEL="k:b w:200 k:b w:600"                  # a hold element by keys: Bixby twice
home_to() { n=$1; s=""; i=0; while [ $i -le "$n" ]; do s="$s $D"; i=$((i + 1)); done; echo "$s $SEL w:400"; }
U="k:u w:150"
# home (0.3): the Emergency tile first, then the apps
EMERG=$(home_to 0)
LIGHT=$(home_to 1)
HEAT=$(home_to 2)
SOSA=$(home_to 3)
INFO=$(home_to 4)
CLOCK=$(home_to 5)
BATT=$(home_to 6)
SETT=$(home_to 7)
# heat: focus the level (Back is 0), Bixby steps Off -> Low -> High -> Off; the confirmation's hold by keys
HEAT_LOW="$HEAT $D $D $SEL $D $HOLDSEL w:500"
LEVEL_UP="$D $D $SEL"
# the last heat step taps Low (0.5.13): a first session that hit a hard stop (the model's 85 C) left the level at Off,
# and three Bixby presses then ended on Off instead of a running level
# Settings (0.3): Back, the section level, then Display's brightness (never selected here), screen-off level,
# the two panel toggles; the Alarm section's LED toggle
SCRIPT="w:600 $OPEN $HEAT_LOW w:5000 $LEVEL_UP w:15300 $BACK
 $EMERG $D $D $HOLDSEL w:600 $BACK $EMERG $D $D $D $SEL w:400 $D $SEL w:400 $BACK
 $SETT $D $D $D $D $D $SEL $D $SEL $U $U $U $U $SEL $D $SEL w:800 $BACK
 $LIGHT $D $D $SEL w:800 $SEL w:300 $D $SEL w:300 $D $SEL w:1200 $SEL w:400 $BACK
 $CLOCK $D $D $D $SEL $D $D $SEL w:2500 $SEL w:300 $BACK
 $BATT w:3000 $BACK $INFO w:800 $BACK
 $SOSA $D $D $HOLDSEL w:500 $D $HOLDSEL w:800
 h:p:1300 w:500 $D $D $D $D $D $SEL w:500
 k:p w:4000 k:p w:800 $OPEN
 $HEAT w:2000 t:720,670 w:6000
 h:p:1300 w:500 $D $D $D $SEL w:2000"
# the GPU case: Settings' GPU switch (focus 5) and its confirmation first, then Heat Low -> High
GPU_SCRIPT="w:600 $OPEN $SETT $D $D $SEL $SEL $D $SEL $D $HOLDSEL w:800 $BACK
 $HEAT_LOW w:3000 $LEVEL_UP w:20300 $BACK
 h:p:1300 w:500 $D $D $D $SEL w:2000"
# the parked case: Low, then Off (core 1 leaves and, injected, parks), then Low again (its relaunch must be refused
# at once as parked, the other cores start), then the power menu's Boot menu while heating
PARKED_SCRIPT="w:600 $OPEN $HEAT_LOW w:5000 $D $D $SEL w:600 $SEL w:3000 $SEL w:6000
 h:p:1300 w:500 $D $D $D $SEL w:2000"
fails=0
run_case() {
    name=$1
    smp=$2
    shift 2
    script=$SCRIPT
    if [ "$name" = gpu ]; then script=$GPU_SCRIPT; fi
    if [ "$name" = parked1 ]; then script=$PARKED_SCRIPT; fi
    log="$out/soak-$name.txt"
    # shellcheck disable=SC2086
    $V env SBOS_HOST_DUMPLOG=1 SBOS_HOST_SMP="$smp" SBOS_HOST_FILES="$root/starbite/sbos/examples" "$out/sbos-host" "$out" /dev/null $script "s:soak-$name" q > "$log" 2>&1
    bad=""
    grep -q "host: reboot" "$log" || bad="$bad [no reboot from the power menu]"
    grep -q "CRASH" "$log" && bad="$bad [crash]"
    grep -q "event queue full" "$log" && bad="$bad [event queue full]"
    grep -q "settings: brightness" "$log" && bad="$bad [the brightness was sent]"
    grep -q "a loop turn took" "$log" && bad="$bad [slow loop: $(grep -m1 'a loop turn took' "$log" | sed 's/.*took //')]"
    grep "panel vs back buffer" "$log" | grep -v " 0 px differ" | grep -q . && bad="$bad [unpresented pixels]"
    grep -q "heater: start, level" "$log" || bad="$bad [the heater never started]"
    grep -q "heater: stopped" "$log" || bad="$bad [the heater never stopped]"
    # the power menu's Boot menu with the heater running (the case may also stop on its own before that)
    if [ "$name" != allfail ]; then
        grep -q "heater: stopped: Restarting" "$log" || bad="$bad [Boot menu was not reached with the heater running]"
    fi
    grep -q "acpm dvfs domain 2 to 1794000" "$log" && ! grep -q "acpm dvfs domain 2 to 1053000" "$log" &&
        bad="$bad [the A55 clock was raised and never restored]"
    for want in "$@"; do
        grep -q -- "$want" "$log" || bad="$bad [missing: $want]"
    done
    if [ -n "$bad" ]; then
        echo "soak $name: FAIL$bad"
        fails=$((fails + 1))
    else
        echo "soak $name: ok ($(grep -c '^log: ' "$log") log lines, max cores $(grep -o 'cores [0-9]' "$log" | sort -k2 -n | tail -1))"
    fi
}
run_case clean "" "settings: panel refresh only on change: on" "settings: panel off with the screen: on" "settings: LED in SOS: on" "sos: countdown" "sos: off" "emergency: Call 112 held"
run_case all8 "m3" "core 7 (M3) running, 8 cores with cpu0"
run_case timeout1 "timeout:1" "core 1: no check-in within 1 s (code -100): trying again later" "core 2 (A55) running" "core 1: no check-in within 1 s (code -100): not used until a restart"
run_case timeout1noctx "timeout:1,noctx" "core 1: no check-in within 1 s; no more cores start in this session"
run_case alreadyon2 "already_on:2" "core 2: PSCI says it is still on"
run_case fault3 "fault:3" "core 3: faulted at start"
run_case allfail "timeout:1,timeout:2,timeout:3" "The processor cores did not start"
run_case parked1 "parked:1" "core 1: parked and PSCI says it is still on" "heater: Off (asked)"
run_case gpu "m3" "settings: GPU in High: on (confirmed)" "heater: gpu on" "gpu 572" "heater: gpu ran" "core 7 (M3) running, 8 cores with cpu0"
echo "soak: $fails failed"
[ "$fails" = 0 ]
