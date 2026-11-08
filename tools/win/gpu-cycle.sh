#!/usr/bin/env bash
# One unattended Windows GPU test cycle (docs/WIN11-GPU.md 6.17), started with the phone in TWRP:
#
#   tools/win/gpu-cycle.sh STAGE OUT [--test-key win11-gpu] [--clean-key win11-auto] [--keep-unattended]
#   tools/win/gpu-cycle.sh --clean-only OUT [--clean-key win11-auto]    a dirty hive -> one clean display-only boot
#   tools/win/gpu-cycle.sh --measure STAGE OUT                          first run: sbwdog alone, one display-only boot
#                                                                       that must restart itself; the boot-time gap
#   tools/win/gpu-cycle.sh --deadman-test OUT [--clean-key win11-auto]  proves the dead-man: sbwdog.off, expect a reset
#
# The two keys are hidden catalog rows with the loader's watchdog (wdt = 82, wdt_feed = "own"): win11-gpu (the
# GPU firmware) and win11-auto (the same files as win11). The user's own win11 row keeps no watchdog, so a normal
# boot can never be reset by it (e.g. at a boot-manager screen waiting for a key).
#
# STAGE = a build/win/stage/sbwddm-t13x directory (sbwddm.sys, optional sbumd.dll, sbwdog.sys, SHA256SUMS).
#  1. Refuses unless both catalog rows (build/store/catalog-live.toml) arm the loader's watchdog for Windows
#     (wdt > 0, wdt_feed = "own"): no boot without a hardware dead-man. Refuses a dirty hive (exit 3).
#  2. Backs up the hive to OUT/hive-before/, then installs: sbwddm.sys, sbumd.dll, sbwdog.sys (boot-start
#     service), FullGuardBoots=0, sbwdog Unattended=1 (full mode: clean restart at 300 s, feeding stops at
#     360 s; display-only: 120 s / 180 s). Removes sbwddm.full.off, empties umdlog (keeps trace) and the
#     Temp sbumd logs. Every pushed file is read back by sha256.
#  3. Boots TEST_KEY (one-shot "next", tools/store/boot-next.sh), waits for the loader menu (Windows restarted
#     itself, or the watchdog reset it), saves the loader's logs, goes back to TWRP.
#  4. Pulls OUT/pre/: SYSTEM + SYSTEM.LOG1/LOG2, umdlog, Temp sbumd logs, pmsg; sbwdog/sbwddm state summary.
#  5. Boots CLEAN_KEY (the combo3b firmware: no GPU interrupts, so sbwddm cannot go full mode; the guard says
#     display-only anyway), which restarts itself cleanly at ~120 s; waits for the menu; back to TWRP.
#  6. Checks the hive is clean, pulls OUT/post/SYSTEM, then sets Unattended=0 (unless --keep-unattended).
# The phone ends in TWRP. Exit: 0 done; 1 usage or precondition (phone untouched or still in TWRP); 2 the phone
# did not come back (it needs the user); 3 the hive is dirty (run --clean-only first); 4 an install read-back
# failed (phone in TWRP, files as listed).
set -euo pipefail
cd "$(dirname "$0")/../.."
here=$(pwd)
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)     # build/store lives in the main checkout
mode=cycle
case "${1:-}" in
--clean-only|--deadman-test) mode=${1#--}; shift; stage= ;;
--measure) mode=measure; stage=${2:?usage: gpu-cycle.sh --measure STAGE OUT}; shift 2 ;;
*) stage=${1:?usage: gpu-cycle.sh STAGE OUT [...]}; shift ;;
esac
out=${1:?usage: gpu-cycle.sh STAGE OUT [...]}; shift
tkey=win11-gpu; ckey=win11-auto; keep=0
while [ $# -gt 0 ]; do case $1 in --test-key) tkey=$2; shift 2;; --clean-key) ckey=$2; shift 2;; --keep-unattended) keep=1; shift;; *) echo "bad arg $1"; exit 1;; esac; done
mkdir -p "$out"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/cycle.log"; }
NTFS=/dev/block/sda27
W=/tmp/wwin
HIVE=Windows/System32/config/SYSTEM
HS=tools/win/hive-service.py
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# ---- preconditions -----------------------------------------------------------------------------------
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "phone not in TWRP"; exit 1; }
python3 - "$main/build/store/catalog-live.toml" "$tkey" "$ckey" <<'PY' || exit 1
import sys, tomllib
cat = tomllib.load(open(sys.argv[1], 'rb'))
rows = {e['key']: e for e in cat.get('entry', [])}
bad = []
for k in sys.argv[2:]:
    e = rows.get(k)
    if e is None:
        bad.append(f"{k}: not a catalog key (the loader would show the menu)")
    elif e.get('method') != 'uefi':
        bad.append(f"{k}: method {e.get('method')}, not uefi")
    elif not (e.get('wdt') or 0) or e.get('wdt_feed') != 'own':
        bad.append(f"{k}: wdt={e.get('wdt')} wdt_feed={e.get('wdt_feed')}: the loader arms no watchdog for it "
                   "(needs wdt = 82, wdt_feed = \"own\")")
    elif e.get('no_launch'):
        bad.append(f"{k}: no_launch rows get no watchdog")
if bad:
    print("refusing, no hardware dead-man:\n  " + "\n  ".join(bad))
    sys.exit(1)
PY
if [ "$mode" = cycle ]; then
    for f in sbwddm.sys sbwdog.sys; do [ -f "$stage/$f" ] || { log "no $stage/$f"; exit 1; }; done
fi
[ "$mode" != measure ] || [ -f "$stage/sbwdog.sys" ] || { log "no $stage/sbwdog.sys"; exit 1; }

mount_rw() { adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null; }
mount_ro() { adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null; }
umount_w() { adb shell "sync; umount $W 2>/dev/null" >/dev/null || true; }
hive_clean() {  # $1 = local copy; hive-service.py check exits 0 only when both sequence numbers match
    python3 "$HS" "$1" check >> "$out/cycle.log" 2>&1
}
# push $1 to $2 on the phone and read it back
push_check() {
    adb push "$1" "$2" >/dev/null
    local a b
    a=$(shasum -a 256 "$1" | cut -c1-64)
    b=$(adb shell "sha256sum '$2'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $2"; return 1; }
    log "pushed $(basename "$1") -> $2 (${a:0:8})"
}
# boot KEY, wait for the loader menu (fastboot) up to $2 s, save the loader's logs into $3, back to TWRP
boot_and_return() {
    local key=$1 limit=$2 dir=$3 start
    mkdir -p "$dir"
    start=$(date +%s)
    # the main checkout's boot-next.sh (it reads that checkout's build/store/catalog-live.toml); it waits for
    # TWRP to come back, so the 25 s timeout (124) is the normal end; 1 = it refused (nothing rebooted)
    local rc=0
    timeout 25 "$main/tools/store/boot-next.sh" "$key" 5 >>"$out/cycle.log" 2>&1 || rc=$?
    if [ $rc = 1 ]; then log "boot-next.sh refused $key (see cycle.log): phone still in TWRP"; exit 1; fi
    log "booted $key (boot-next rc $rc); waiting up to ${limit}s for the loader menu"
    sleep 60
    if [ "$(adb get-state 2>/dev/null)" = recovery ]; then
        log "the phone is still in TWRP 60 s after boot-next: $key did not start"; exit 2
    fi
    until fastboot devices 2>/dev/null | grep -q fastboot; do
        if [ $(( $(date +%s) - start )) -gt "$limit" ]; then
            log "NO MENU after ${limit}s: the phone is stuck (needs the user)"; exit 2
        fi
        sleep 5
    done
    log "menu after $(( $(date +%s) - start )) s"
    fastboot oem log dmesg > "$dir/loader-dmesg.txt" 2>&1 || true
    fastboot get_staged "$dir/zone0.bin" >/dev/null 2>&1 || true
    python3 "$main/tools/store/sbfb.py" reboot recovery >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do [ "$(adb get-state 2>/dev/null)" = recovery ] && break; sleep 3; done
    [ "$(adb get-state 2>/dev/null)" = recovery ] || { log "no TWRP after the menu (needs the user)"; exit 2; }
    sleep 3
    adb shell 'cat /sys/fs/pstore/pmsg-ramoops-0' 2>/dev/null | LC_ALL=C tr -c '[:print:]\n' '.' > "$dir/loader-pmsg.txt" || true
    grep -a "STARBITE: watchdog" "$dir/loader-pmsg.txt" | tail -2 | sed 's/^/  pmsg: /' | tee -a "$out/cycle.log" || true
}
pull_hive() {  # into $1
    mkdir -p "$1"
    for f in SYSTEM SYSTEM.LOG1 SYSTEM.LOG2; do adb pull "$W/Windows/System32/config/$f" "$1/" >/dev/null 2>&1 || log "missing $f"; done
}
set_unattended() {  # $1 = 0|1 ; the hive must be clean; phone mounted rw
    adb pull "$W/$HIVE" "$tmp/SYSTEM" >/dev/null
    hive_clean "$tmp/SYSTEM" || { log "hive dirty: Unattended not changed"; return 3; }
    python3 "$HS" "$tmp/SYSTEM" "$tmp/SYSTEM.new" param sbwdog Unattended="$1" >/dev/null
    push_check "$tmp/SYSTEM.new" "$W/$HIVE"
}
clean_boot() {  # display-only boot that restarts itself; ends in TWRP with the hive checked
    boot_and_return "$ckey" 900 "$out/clean"
    mount_ro
    pull_hive "$out/post"
    umount_w
    if hive_clean "$out/post/SYSTEM"; then log "hive clean after the display-only boot"; else log "hive STILL DIRTY after the display-only boot"; return 3; fi
}

# the hive's sbwdog parameter (read only; works on a dirty hive too): prints the value or nothing
wdog_param() { python3 tools/woa/hive.py "$1" 'ControlSet001\Services\sbwdog\Parameters' 2>/dev/null | awk -v n="$2" '$1 == n { print $NF }' | tr -d '()'; }
need_unattended() {  # the display-only boot restarts itself only with sbwdog installed and Unattended=1
    mount_ro; adb pull "$W/$HIVE" "$tmp/cur" >/dev/null; umount_w
    [ "$(wdog_param "$tmp/cur" Unattended)" = 1 ] || { log "sbwdog is not installed with Unattended=1: a display-only boot would not restart itself (run --measure from a clean hive first)"; exit 1; }
}
gap_report() {  # $1 = hive
    python3 tools/woa/hive.py "$1" 'ControlSet001\Services\sbwdog\State' > "$1.sbwdog.txt" 2>&1 || true
    log "sbwdog: $(grep -E 'ArmedAtStart|GapMsAtStart|UptimeMsAtStart|WtDat |Mode |RestartAt|RestartStatus|CapAt|KillFile' "$1.sbwdog.txt" | sed 's/DWORD 0x[0-9a-f]* //' | tr -s ' ' | tr '\n' ' ')"
}
# install into the mounted (rw) volume: $1 = 1 with the GPU driver files, 0 sbwdog only
install() {
    adb pull "$W/$HIVE" "$tmp/SYSTEM" >/dev/null
    if ! hive_clean "$tmp/SYSTEM"; then umount_w; log "the hive is dirty (the last Windows boot did not end cleanly): run --clean-only, or restart Windows cleanly once"; exit 3; fi
    mkdir -p "$out/hive-before"; cp "$tmp/SYSTEM" "$out/hive-before/SYSTEM"
    if [ "$1" = 1 ]; then python3 "$HS" "$tmp/SYSTEM" "$tmp/S1" param sbwddm --root FullGuardBoots=0 >/dev/null; else cp "$tmp/SYSTEM" "$tmp/S1"; fi
    if python3 tools/woa/hive.py "$tmp/S1" 'ControlSet001\Services\sbwdog' 2>/dev/null | grep -q "Start"; then cp "$tmp/S1" "$tmp/S2"
    else python3 "$HS" "$tmp/S1" "$tmp/S2" add sbwdog --start 0 --oneshot 0 --image 'System32\drivers\sbwdog.sys' --display "Starbite watchdog feeder" >/dev/null; fi
    python3 "$HS" "$tmp/S2" "$tmp/S3" param sbwdog Unattended=1 FullRestartSec=300 FullCapSec=360 DisplayRestartSec=120 DisplayCapSec=180 >/dev/null
    local rc=0
    push_check "$tmp/S3" "$W/$HIVE" || rc=4
    push_check "$stage/sbwdog.sys" "$W/Windows/System32/drivers/sbwdog.sys" || rc=4
    if [ "$1" = 1 ]; then
        push_check "$stage/sbwddm.sys" "$W/Windows/System32/drivers/sbwddm.sys" || rc=4
        [ -f "$stage/sbumd.dll" ] && { push_check "$stage/sbumd.dll" "$W/Windows/System32/sbumd.dll" || rc=4; }
        adb shell "rm -f $W/Windows/Starbite/sbwddm.full.off $W/Windows/Temp/sbumd-*.log; mkdir -p $W/ProgramData/Starbite/umdlog; cd $W/ProgramData/Starbite/umdlog && for f in *; do [ \"\$f\" = trace ] || rm -f \"\$f\"; done; touch trace" >/dev/null
    fi
    adb shell "rm -f $W/Windows/Starbite/sbwdog.off" >/dev/null
    umount_w
    [ $rc = 0 ] || { log "install read-back failed: phone left in TWRP"; exit 4; }
    log "installed from $(basename "$stage"): sbwdog (Start 0, Unattended=1)$([ "$1" = 1 ] && echo ", sbwddm/sbumd, FullGuardBoots=0")"
}
finish() {  # after a clean display-only boot: disarm unless asked not to
    if [ $keep = 0 ]; then mount_rw; set_unattended 0 || true; umount_w; log "Unattended=0"; fi
    log "done; phone in TWRP"
}

# ---- modes -----------------------------------------------------------------------------------------------
case $mode in
measure)
    # sbwdog alone, then one display-only boot of CLEAN_KEY: it must restart itself cleanly at ~120 s.
    # Measures the loader -> Windows gap (GapMsAtStart) the 82 s watchdog has to cover. If the gap is longer,
    # the watchdog resets the phone before Windows and it comes back to the menu all the same.
    mount_rw
    install 0
    clean_boot || { gap_report "$out/post/SYSTEM"; log "the self-restart did not leave the hive clean"; exit 3; }
    gap_report "$out/post/SYSTEM"
    keep=1; finish
    exit 0 ;;
clean-only)
    need_unattended
    clean_boot
    finish
    exit 0 ;;
deadman-test)
    # sbwdog loaded but not feeding: the loader's watchdog must reset the phone back to the menu by itself,
    # then a clean display-only boot repairs the hive
    need_unattended
    mount_rw
    adb shell "mkdir -p $W/Windows/Starbite; echo test > $W/Windows/Starbite/sbwdog.off" >/dev/null
    umount_w
    log "dead-man test: sbwdog.off in place, booting $ckey; expect the menu ~82 s after the loader"
    boot_and_return "$ckey" 400 "$out/deadman"
    mount_rw; adb shell "rm -f $W/Windows/Starbite/sbwdog.off" >/dev/null; umount_w
    log "dead-man test passed: the phone came back by itself"
    clean_boot || exit 3
    keep=1; finish
    exit 0 ;;
esac

# ---- install ---------------------------------------------------------------------------------------------
mount_rw
install 1

# ---- the test boot, the pull, the clean boot ----------------------------------------------------------------
# full mode: Windows restarts itself at 300 s, the feeder stops at 360 s, the watchdog fires <= 82 s later
boot_and_return "$tkey" 900 "$out/pre"
mount_ro
pull_hive "$out/pre"
mkdir -p "$out/pre/umdlog"
adb pull "$W/ProgramData/Starbite/umdlog/." "$out/pre/umdlog/" >/dev/null 2>&1 || true
adb shell "ls $W/Windows/Temp/ | grep '^sbumd-'" 2>/dev/null | tr -d '\r' | while read -r f; do adb pull "$W/Windows/Temp/$f" "$out/pre/umdlog/" >/dev/null 2>&1; done || true
umount_w
log "pulled pre/ ($(du -sh "$out/pre" | cut -f1)); the values below are from the unreplayed hive"
gap_report "$out/pre/SYSTEM"
clean_boot || { log "leaving Unattended=1: the hive is dirty (run --clean-only)"; exit 3; }
finish
