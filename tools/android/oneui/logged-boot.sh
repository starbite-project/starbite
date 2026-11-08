#!/usr/bin/env bash
# [HOLD_S=N] logged-boot.sh [WAIT_S] : one fully logged One UI boot (catalog key `android`), unattended, for the bootloop
# triage (docs/ANDROID-ONEUI.md 6). Start with the phone in TWRP or at the loader (menu / crash screen).
#   1. before: TWRP -> pstore + last_kmsg, then `adb reboot` (lands in the loader menu); the loader's crash
#      log (sbfb.py crash --level 3), its dmesg/loader/usb logs.
#   2. `sbfb.py boot android`, then a 2 s timeline (timeline.txt) of every state change: none / loader <mode> /
#      android bc= anim= zygote= ss= / recovery. While Android's adb is up: `logcat -b all` streamed to
#      logcat-N.txt (unbuffered), getprop and dmesg (if allowed) once per Android session.
#   3. outcome:
#      boot_completed=1 -> getprop + logcat snapshot, then `adb reboot recovery` (back to TWRP): RESULT booted
#      back at the loader (reset / watchdog / panic) -> crash log, logs, screen.png, `sbfb.py reboot recovery`
#        (a warm reset keeps Samsung's last_kmsg of the failed kernel), then TWRP collection: RESULT reset
#      TWRP directly (init fatal reboot / boot guard) -> TWRP collection: RESULT recovery
#      nothing for WAIT_S -> RESULT hang (the user's buttons are needed)
#   HOLD_S=N: after boot_completed keep the timeline and logcat (kernel buffer included) running N more seconds
#      while the user uses the phone; a reset in that time is RESULT reset (with the seconds after boot_completed),
#      and logcat holds the last lines before it (USB dies with the kernel, so the very last ones may be missing).
#   TWRP collection = last_kmsg, pstore, tools/android/oneui/diag-boot.sh, ODM's 9 blocks (odm.bin).
# Writes nothing to the phone except what the loader does for `oem reboot-to` (its one-shot next entry).
#   -> build/test-boots/<date>-android-logged/ (RESULT in summary.txt)
set -uo pipefail
cd "$(dirname "$0")/../../.."
WAIT=$(( ${1:-900} + ${HOLD_S:-0} ))
out=build/test-boots/$(date +%Y%m%d-%H%M%S)-android-logged
mkdir -p "$out"
S="$out/summary.txt"; T="$out/timeline.txt"
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$S"; }
SBFB="python3 tools/store/sbfb.py"
adbstate() { adb get-state 2>/dev/null </dev/null | tr -d '\r'; }
fbmode() { fastboot devices 2>/dev/null | grep -q . && $SBFB status 2>/dev/null </dev/null | head -1 | sed -n 's/^mode \([a-z]*\).*/\1/p'; }
state() {
    local s; s=$(adbstate)
    case "$s" in
    device) echo "android $(adb shell 'echo bc=$(getprop sys.boot_completed) anim=$(getprop init.svc.bootanim) zygote=$(getprop init.svc.zygote) ss=$(getprop sys.system_server.start_count)' 2>/dev/null </dev/null | tr -d '\r')" ;;
    recovery) echo recovery ;;
    unauthorized|offline) echo "adb-$s" ;;
    *) local m; m=$(fbmode); [ -n "$m" ] && echo "loader $m" || echo none ;;
    esac
}
loader_logs() {  # $1 = tag
    $SBFB crash --level 3 > "$out/crash-$1.txt" 2>&1
    for w in dmesg loader usb pmsg; do $SBFB logs $w -o "$out/loader-$w-$1.txt" >/dev/null 2>&1; done
    $SBFB screen "$out/screen-$1.png" --half >/dev/null 2>&1
    say "loader logs ($1): crash $(wc -l < "$out/crash-$1.txt" | tr -d ' ') lines; $(grep -a -m3 -i -E 'android|HANG|PANIC|WATCHDOG|reset' "$out/crash-$1.txt" | tr '\n' ' ' | cut -c1-300)"
}
twrp_collect() {  # $1 = tag
    adb exec-out 'cat /proc/last_kmsg' </dev/null > "$out/last_kmsg-$1.txt" 2>/dev/null
    for f in $(adb shell 'ls /sys/fs/pstore' 2>/dev/null </dev/null | tr -d '\r'); do
        adb exec-out "cat /sys/fs/pstore/$f" </dev/null > "$out/pstore-$f-$1" 2>/dev/null; done
    adb exec-out 'cat /proc/reset_reason' </dev/null > "$out/reset_reason-$1.txt" 2>/dev/null
    adb exec-out 'dd if=/dev/block/by-name/ODM bs=4096 count=9 2>/dev/null' </dev/null > "$out/odm-$1.bin"
    say "TWRP ($1): reset_reason $(cat "$out/reset_reason-$1.txt" 2>/dev/null); last_kmsg $(wc -c < "$out/last_kmsg-$1.txt" | tr -d ' ') B"
    grep -a -E "Kernel panic|Unable to|EXT4-fs error|erofs.*(err|fail)|init: .*(fail|abort|reboot)|fs_mgr|Watchdog|BUG:|Oops|SError|first_stage|Restarting system" \
        "$out/last_kmsg-$1.txt" 2>/dev/null | tail -30 | tee -a "$S"
}

say "== One UI logged boot, wait ${WAIT}s -> $out"
st=$(state); say "start: $st"
case "$st" in
recovery)
    twrp_collect pre
    say "adb reboot -> loader menu"; adb reboot </dev/null
    $SBFB wait --timeout 120 >/dev/null 2>&1 || { say "RESULT no-loader: the loader's fastboot did not answer after the reboot"; exit 2; } ;;
loader*) ;;
*) say "RESULT not-ready: phone neither in TWRP nor at the loader ($st)"; exit 2 ;;
esac
loader_logs pre
say "boot android"; $SBFB boot android >> "$S" 2>&1
sleep 4

t0=$(date +%s); last=""; seen_android=0; left=0; n=0; lcpid=""; result=""
while :; do
    now=$(( $(date +%s) - t0 ))
    s=$(state)
    if [ "$s" != "$last" ]; then echo "$(date +%H:%M:%S) +${now}s $s" | tee -a "$T"; last=$s; fi
    case "$s" in none|android*|adb-*) left=1 ;; esac      # the loader handed over (its USB went away)
    case "$s" in
    android*)
        if [ -z "$lcpid" ] || ! kill -0 "$lcpid" 2>/dev/null; then
            n=$((n + 1)); seen_android=1
            adb logcat -b all -v threadtime </dev/null > "$out/logcat-$n.txt" 2>/dev/null & lcpid=$!   # adb writes as it reads: the last lines before a reset are kept
            adb shell getprop </dev/null > "$out/getprop-$n.txt" 2>/dev/null
            adb shell 'dmesg 2>&1 || su -c dmesg' </dev/null > "$out/dmesg-$n.txt" 2>/dev/null
        fi
        case "$s" in *bc=1*)
            if [ "${HOLD_S:-0}" -gt 0 ]; then   # keep streaming while the user uses the phone
                [ -n "${tbc:-}" ] || { tbc=$now; say "boot_completed after ${now}s: holding ${HOLD_S}s, logcat streaming (use the phone now)"; }
                [ $((now - tbc)) -lt "$HOLD_S" ] && { sleep 2; continue; }
            fi
            sleep 20
            adb shell getprop </dev/null > "$out/getprop-final.txt" 2>/dev/null
            adb shell 'dmesg 2>&1 || su -c dmesg' </dev/null > "$out/dmesg-final.txt" 2>/dev/null
            result="booted (boot_completed after ${now}s, $n Android session(s))"; break ;;
        esac ;;
    "loader crash"|"loader menu"|"loader recovery"|"loader fastboot")
        # back at the loader after Android had started = a reset during the boot
        [ "$left" = 1 ] && { result="reset (back at the $s after ${now}s${tbc:+, $((now - tbc))s after boot_completed}, $n Android adb session(s))"; break; } ;;
    recovery) [ "$left" = 1 ] && result="recovery (TWRP after ${now}s: init fatal reboot or boot guard)" && break ;;
    esac
    [ "$now" -ge "$WAIT" ] && { result="hang (state '$s' after ${WAIT}s)"; break; }
    sleep 2
done
[ -n "$lcpid" ] && kill "$lcpid" 2>/dev/null
say "RESULT $result"

case "$result" in
booted*)
    say "logcat lines: $(cat "$out"/logcat-*.txt | wc -l | tr -d ' '); back to TWRP"
    adb reboot recovery </dev/null ;;
reset*)
    loader_logs post
    say "sbfb reboot recovery (warm: last_kmsg keeps the failed kernel)"; $SBFB reboot recovery >/dev/null 2>&1
    for i in $(seq 60); do [ "$(adbstate)" = recovery ] && break; sleep 3; done
    if [ "$(adbstate)" = recovery ]; then sleep 5; twrp_collect post; tools/android/oneui/diag-boot.sh post > /dev/null 2>&1
    else say "TWRP did not come back"; fi ;;
recovery*)
    sleep 5; twrp_collect post; tools/android/oneui/diag-boot.sh post > /dev/null 2>&1 ;;
hang*) say "phone needs the user's buttons (Volume Down + Power ~7 s, then Volume Up + Bixby + Power for TWRP)" ;;
esac
for f in "$out"/logcat-*.txt; do
    [ -s "$f" ] || continue
    say "-- ${f##*/}: last fatal lines"
    grep -a -E " F |FATAL|Fatal signal|beginning of crash|reboot|watchdog|init *:.*(fail|abort)|Kernel panic|Unable to handle|Internal error|BUG:|Oops|SError|mali|decon|Call trace" "$f" | tail -30 | tee -a "$S"
done
say "== done: $out ($(du -sh "$out" | cut -f1))"
