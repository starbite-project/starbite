#!/usr/bin/env bash
# touch.sh: Windows touch on the S9 (docs/WIN11-GPU.md 6.78), from TWRP, no boot. Touch is shared by BOTH rows.
#
#   tools/win/touch.sh apply OUT     (1) the touch service runs s6touchdiag.sys = touchdiag-2 (build/win/touch-1,
#                                        070dad9a, source starbite/win/s6touchdiag = claude/win-m3-clean-947e73 cbd9f5a):
#                                        the same S6SY761 driver plus up to 3 tries of a failed touch I2C read before
#                                        it tells Windows "all fingers up" (one failed read used to cut a drag into
#                                        several touches), and Diag* counters + a press/release/all-up trace in
#                                        Enum\ACPI\SCSY0761\0\Device Parameters. Only the service's ImagePath changes
#                                        (the original is kept in Parameters\SbOrigImagePath); DriverStore untouched.
#                                    (2) HKLM RunOnce StarbiteTouch -> C:\starbite\debloat\touch-online.cmd at the next
#                                        logon: "press and hold for right-click" off for the user (old value saved),
#                                        so a tap in Geometry Dash is a click at once.
#                                (3) WER LocalDumps for sihost.exe, explorer.exe, ShellExperienceHost.exe into
#                                        C:\sbdumps (2 full dumps each): the shell host's abort() after GD (11-14)
#                                        leaves its stack for user-pull.sh.
#   tools/win/touch.sh restore OUT   ImagePath back to the DriverStore driver; RunOnce StarbiteUntouch puts the
#                                    press-and-hold setting back. The .sys file stays (unused).
#   tools/win/touch.sh update OUT    after apply: only the driver FILE becomes touchdiag-3 (build/win/touch-3, fa9f2a52:
#                                    touchdiag-2 + a 25 ms timeout for touch-event reads instead of 250 ms and 5 tries, so a stuck
#                                    read freezes the finger ~25 ms, not ~265 ms, before the retry). Refused unless the
#                                    service already points at s6touchdiag.sys. Check boot as after apply.
#   tools/win/touch.sh update OUT 2  the same, back to the touchdiag-2 file.
#   tools/win/touch.sh orient OUT N  the panel's saved Windows orientation (tools/win/hive-orientation.py): 4 = landscape
#                                    as the user holds it, 2 = landscape (flipped), 1 = portrait. Windows applies it at
#                                    the next start and turns touch with it. Run it last (after check boot + restore).
#   tools/win/touch.sh steam OUT     HKLM RunOnce StarbiteSteamOn -> steam-autostart-online.cmd: Steam back in the user's
#                                    autostart (debloat-1 removed it; a cold Steam start on the big cores = 5+ min to GD).
#   tools/win/touch.sh hsi2c OUT     the touch bus's controller driver (Exynos9810Hsi2c) -> sbhsi2c.sys (build/win/hsi2c-1):
#                                    the same driver, but its interrupt handler acknowledges only the status bits it
#                                    read (the original re-read INT_STATUS and cleared bits it never processed: a lost
#                                    TRANSFER_DONE = a hung touch read). ImagePath switch (tools/win/hive-imagepath.py);
#                                    the touch state before is saved to OUT/hsi2c-before.txt. Check boot PASS = a NEW
#                                    DiagClock line (differs from hsi2c-before.txt), TouchD0Status 0, DiagSelf 200 reads.
#   tools/win/touch.sh hsi2c OUT 2   the same with sbhsi2c-2 (build/win/hsi2c-2): + a 2 ms poll that services an interrupt
#                                    the ISR missed (edge-triggered), and a register snapshot at each cancel
#                                    (Enum\ACPI\SAMS9810\a\Device Parameters: I2cPollRescue, I2cCancelCount/Pending/Regs).
#   tools/win/touch.sh hsi2c OUT 3   sbhsi2c-3 (build/win/hsi2c-3): + a transfer whose bytes all moved, with the master idle
#                                    and nothing pending for 2 polls, is completed as done (I2cPollSynthDone).
#   tools/win/touch.sh hsi2c-restore OUT   the DriverStore HSI2C driver again.
#   tools/win/touch.sh show OUT      read-only: ImagePath and every Diag* value with the decoded trace.
#
# After apply, BEFORE anyone relies on touch, prove the driver loads with one unattended small-core boot:
#   tools/win/gpu-cycle.sh --measure STAGE_T61 OUT/check   then   tools/win/touch.sh show OUT
# (DiagBuild 's6touchdiag cbd9f5a', TouchD0Status 0, DiagSelf 0 errors; otherwise restore at once).
# Refuses a dirty hive (exit 3). Backups in OUT/backup/. Every push read back by sha256 (exit 4).
set -euo pipefail
cd "$(dirname "$0")/../.."
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
mode=${1:?usage: touch.sh apply|update|hsi2c|hsi2c-restore|orient|steam|restore|show OUT}
out=${2:?usage: touch.sh apply|update|restore|show OUT}
DRV=$main/build/win/touch-1/s6touchdiag.sys
DRV_SHA=070dad9aae60debcc2819b873dba92bec1f9a2b944fa70b09d9ec4b1738a61f0
DRV3=$main/build/win/touch-3/s6touchdiag.sys
HSI2C=$main/build/win/hsi2c-1/sbhsi2c.sys
HSI2C_SHA=8489ac43bdef2ce7ee37b7fdb64bae58a7e9e74639c6da1126d740e0679fa9ec
IP=tools/win/hive-imagepath.py
if [ "${1:-}" = hsi2c ] && [ "${3:-1}" = 2 ]; then HSI2C=$main/build/win/hsi2c-2/sbhsi2c.sys; HSI2C_SHA=a8c74b938ebf9baeb07dc2a9d6e728c078b2014186b46c7c8f11ee892f6db040; fi
if [ "${1:-}" = hsi2c ] && [ "${3:-1}" = 3 ]; then HSI2C=$main/build/win/hsi2c-3/sbhsi2c.sys; HSI2C_SHA=5d976386ee07bca410954c3852a3dbc8453bc0f70b4d6f88eecf590c0225df0b; fi
DRV3_SHA=fa9f2a52c9fb0aa866cc7ba66e9f20548ad311d418cddac844531224efa7a2a2
NTFS=/dev/block/sda27
W=/tmp/wwin
SYS=Windows/System32/config/SYSTEM
SOFT=Windows/System32/config/SOFTWARE
HS=tools/win/hive-service.py
TD=tools/woa/win-m3/hive-touchdiag.py
RO=tools/win/hive-runonce.py
LDT=tools/woa/win-m3/hive-localdumps.py
DUMPS="sihost.exe explorer.exe ShellExperienceHost.exe"
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/touch.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "the phone is not in TWRP"; exit 1; }
tmp=$(mktemp -d); trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
push_check() {
    adb push "$1" "$2" >/dev/null
    local a b
    a=$(shasum -a 256 "$1" | cut -c1-64); b=$(adb shell "sha256sum '$2'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $2"; exit 4; }
    log "pushed $(basename "$1") -> $2 (${a:0:8})"
}
case $mode in
show)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$TD" "$tmp/SYSTEM" show | tee -a "$out/touch.log"
    adb shell "cat $W/ProgramData/Starbite/debloat/touch.log 2>/dev/null" | tr -d '\r' | tee -a "$out/touch.log"
    exit 0 ;;
hsi2c|hsi2c-restore)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$HS" "$tmp/SYSTEM" check >> "$out/touch.log" 2>&1 || { log "the SYSTEM hive is dirty: run gpu-cycle.sh --clean-only first"; exit 3; }
    cp "$tmp/SYSTEM" "$out/backup/SYSTEM-$mode-$(date +%Y%m%d-%H%M%S)"
    if [ "$mode" = hsi2c ]; then
        [ "$(shasum -a 256 "$HSI2C" | cut -c1-64)" = "$HSI2C_SHA" ] || { log "$HSI2C is not the expected build ($HSI2C_SHA)"; exit 1; }
        python3 "$TD" "$tmp/SYSTEM" show > "$out/hsi2c-before.txt" 2>&1 || true
        python3 "$IP" "$tmp/SYSTEM" "$tmp/S1" set Exynos9810Hsi2c '\SystemRoot\System32\drivers\sbhsi2c.sys' 2>&1 | tee -a "$out/touch.log"
        push_check "$HSI2C" "$W/Windows/System32/drivers/sbhsi2c.sys"
    else
        python3 "$IP" "$tmp/SYSTEM" "$tmp/S1" restore Exynos9810Hsi2c 2>&1 | tee -a "$out/touch.log"
    fi
    push_check "$tmp/S1" "$W/$SYS"
    log "$mode done. Next: the unattended check boot (touch depends on this bus: verify before anyone relies on touch)"
    exit 0 ;;
steam)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    adb pull "$W/$SOFT" "$tmp/SOFTWARE" >/dev/null
    python3 "$HS" "$tmp/SOFTWARE" check >> "$out/touch.log" 2>&1 || { log "the SOFTWARE hive is dirty: run gpu-cycle.sh --clean-only first"; exit 3; }
    cp "$tmp/SOFTWARE" "$out/backup/SOFTWARE-steam-$(date +%Y%m%d-%H%M%S)"
    python3 "$RO" "$tmp/SOFTWARE" "$tmp/SW1" set StarbiteSteamOn 'cmd /c C:\starbite\debloat\steam-autostart-online.cmd' >> "$out/touch.log" 2>&1
    adb shell "mkdir -p $W/starbite/debloat" >/dev/null
    push_check starbite/win/debloat/steam-autostart-online.cmd "$W/starbite/debloat/steam-autostart-online.cmd"
    push_check "$tmp/SW1" "$W/$SOFT"
    log "steam done: RunOnce StarbiteSteamOn armed (Steam starts with Windows again from the logon after next)"
    exit 0 ;;
orient)
    case ${3:-} in 1|2|3|4) ;; *) log "orient: N must be 1-4"; exit 1 ;; esac
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$HS" "$tmp/SYSTEM" check >> "$out/touch.log" 2>&1 || { log "the SYSTEM hive is dirty: run gpu-cycle.sh --clean-only first"; exit 3; }
    cp "$tmp/SYSTEM" "$out/backup/SYSTEM-orient-$(date +%Y%m%d-%H%M%S)"
    python3 tools/win/hive-orientation.py "$tmp/SYSTEM" "$tmp/S1" set "$3" 2>&1 | tee -a "$out/touch.log"
    push_check "$tmp/S1" "$W/$SYS"
    log "orient done: saved orientation $3 (applies at the next Windows start)"
    exit 0 ;;
update)
    if [ "${3:-3}" = 2 ]; then DRV3=$DRV; DRV3_SHA=$DRV_SHA; fi
    [ "$(shasum -a 256 "$DRV3" | cut -c1-64)" = "$DRV3_SHA" ] || { log "$DRV3 is not the expected build ($DRV3_SHA)"; exit 1; }
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$TD" "$tmp/SYSTEM" show | head -3 | grep -q 'ImagePath .*s6touchdiag.sys' || { log "the touch service does not use s6touchdiag.sys: run apply first"; exit 1; }
    adb pull "$W/Windows/System32/drivers/s6touchdiag.sys" "$out/backup/s6touchdiag-$(date +%Y%m%d-%H%M%S).sys" >/dev/null
    push_check "$DRV3" "$W/Windows/System32/drivers/s6touchdiag.sys"
    log "update done: s6touchdiag.sys = $([ "${3:-3}" = 2 ] && echo touchdiag-2 || echo "touchdiag-3 (25 ms touch-read timeout)"). Next: the unattended check boot"
    exit 0 ;;
apply|restore) ;;
*) echo "usage: touch.sh apply|update|restore|show OUT"; exit 1 ;;
esac
[ "$(shasum -a 256 "$DRV" | cut -c1-64)" = "$DRV_SHA" ] || { log "$DRV is not touchdiag-2 ($DRV_SHA)"; exit 1; }
for f in starbite/win/debloat/touch-online.cmd starbite/win/debloat/untouch-online.cmd; do [ -f "$f" ] || { log "missing $f"; exit 1; }; done
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
adb pull "$W/$SOFT" "$tmp/SOFTWARE" >/dev/null
for h in SYSTEM SOFTWARE; do
    if ! python3 "$HS" "$tmp/$h" check >> "$out/touch.log" 2>&1; then
        log "the $h hive is dirty (the last Windows boot did not end cleanly): run gpu-cycle.sh --clean-only first"; exit 3
    fi
done
stamp=$(date +%Y%m%d-%H%M%S)
cp "$tmp/SYSTEM" "$out/backup/SYSTEM-$stamp"; cp "$tmp/SOFTWARE" "$out/backup/SOFTWARE-$stamp"
log "$mode: hives clean, backed up as *-$stamp"
case $mode in
apply)
    python3 "$TD" "$tmp/SYSTEM" "$tmp/S1" set >> "$out/touch.log" 2>&1
    # shellcheck disable=SC2086
    python3 "$LDT" "$tmp/SOFTWARE" "$tmp/SW0" set $DUMPS >> "$out/touch.log" 2>&1
    python3 "$RO" "$tmp/SW0" "$tmp/SW1" set StarbiteTouch 'cmd /c C:\starbite\debloat\touch-online.cmd' >> "$out/touch.log" 2>&1
    adb shell "mkdir -p $W/starbite/debloat" >/dev/null
    push_check "$DRV" "$W/Windows/System32/drivers/s6touchdiag.sys"
    push_check starbite/win/debloat/touch-online.cmd "$W/starbite/debloat/touch-online.cmd"
    push_check starbite/win/debloat/untouch-online.cmd "$W/starbite/debloat/untouch-online.cmd"
    push_check "$tmp/S1" "$W/$SYS"
    push_check "$tmp/SW1" "$W/$SOFT"
    python3 "$TD" "$tmp/S1" show | head -3 | tee -a "$out/touch.log"
    log "apply done: touch service -> s6touchdiag.sys (touchdiag-2), LocalDumps for the shell, RunOnce StarbiteTouch armed. Next: the unattended check boot" ;;
restore)
    python3 "$TD" "$tmp/SYSTEM" "$tmp/S1" restore >> "$out/touch.log" 2>&1
    # shellcheck disable=SC2086
    python3 "$LDT" "$tmp/SOFTWARE" "$tmp/SWd" remove $DUMPS >> "$out/touch.log" 2>&1 || cp "$tmp/SOFTWARE" "$tmp/SWd"
    python3 "$RO" "$tmp/SWd" "$tmp/SW0" remove StarbiteTouch >> "$out/touch.log" 2>&1 || cp "$tmp/SWd" "$tmp/SW0"
    python3 "$RO" "$tmp/SW0" "$tmp/SW1" set StarbiteUntouch 'cmd /c C:\starbite\debloat\untouch-online.cmd' >> "$out/touch.log" 2>&1
    adb shell "mkdir -p $W/starbite/debloat" >/dev/null
    push_check starbite/win/debloat/untouch-online.cmd "$W/starbite/debloat/untouch-online.cmd"
    push_check "$tmp/S1" "$W/$SYS"
    push_check "$tmp/SW1" "$W/$SOFT"
    python3 "$TD" "$tmp/S1" show | head -3 | tee -a "$out/touch.log"
    log "restore done: touch service back on the DriverStore driver, RunOnce StarbiteUntouch armed" ;;
esac
