#!/usr/bin/env bash
# c-clean-stage.sh: arm the Windows-side C: cleanup (starbite/win/autotest/c-clean-win.cmd) to run once, elevated, at
# the next Windows logon, from TWRP (docs/WIN11-GPU-CAPS.md "C: space"). Run AFTER tools/win/c-cleanup.sh --delete
# (which frees the room DISM and compact need) and only in a fresh TWRP with no other phone job.
#   tools/win/c-clean-stage.sh            push the script + a restart flag to C:\starbite\cleanup, write HKLM RunOnce
#                                         "StarbiteCleanup" into the SOFTWARE hive (only if the hive is clean), verify
#   tools/win/c-clean-stage.sh --undo     remove the RunOnce value again (if Windows has not run it yet)
# Afterwards: boot Windows (the big-core entry: much faster than the A55s) and let it log on. The script runs for
# 30-90 min, logs to C:\starbite\cleanup\c-clean-win.log, writes C:\starbite\cleanup\done and restarts. Then pull that
# log and run tools/win/c-cleanup.sh (survey) to measure. Out: build/win/cleanstage-<date>/ (SOFTWARE.orig kept).
set -uo pipefail
NTFS=/dev/block/sda27
W=/tmp/wwin
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
main=$(cd "$repo" && git rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's#/\.git$##')
[ -d "$main/build" ] || main=/Users/dev/project-starbite
out=$main/build/win/cleanstage-$(date +%Y%m%d-%H%M)
undo=0; [ "${1:-}" = --undo ] && undo=1
mkdir -p "$out"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/stage.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
if pgrep -fl 'gpu-cycle|autotest-cycle|boot-safe|store-flash|c-cleanup' | grep -v pgrep >/dev/null; then
    echo "another phone job is running (one at a time)"; exit 1
fi
mounted() { adb shell "grep ' $W ' /proc/mounts" | tr -d '\r' | awk '{print $4}' | tr ',' '\n' | grep -qx "$1"; }
mount_rw() { adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null; mounted rw || { log "C: did not mount read-write"; exit 2; }; }
umount_w() { adb shell "sync; umount $W 2>/dev/null" >/dev/null 2>&1 || true; }
trap umount_w EXIT
push_check() {
    adb push "$1" "$2" >/dev/null
    local a b
    a=$(shasum -a 256 "$1" | cut -c1-64)
    b=$(adb shell "sha256sum '$2'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $2"; exit 3; }
    log "pushed $(basename "$1") -> $2 (${a:0:8})"
}
mount_rw
H=$W/Windows/System32/config
for f in SOFTWARE SOFTWARE.LOG1 SOFTWARE.LOG2; do adb pull "$H/$f" "$out/" >/dev/null 2>&1 || log "missing $f"; done
cp "$out/SOFTWARE" "$out/SOFTWARE.orig"
if [ "$undo" = 1 ]; then
    python3 "$repo/tools/win/hive-runonce.py" "$out/SOFTWARE" "$out/SOFTWARE.new" remove StarbiteCleanup 2>&1 | tee -a "$out/stage.log"
    [ -f "$out/SOFTWARE.new" ] || { log "nothing changed"; exit 0; }
    push_check "$out/SOFTWARE.new" "$H/SOFTWARE"
    log "RunOnce StarbiteCleanup removed"
    exit 0
fi
adb shell "mkdir -p $W/starbite/cleanup; rm -f $W/starbite/cleanup/done" >/dev/null
push_check "$repo/starbite/win/autotest/c-clean-win.cmd" "$W/starbite/cleanup/c-clean-win.cmd"
adb shell "echo on > $W/starbite/cleanup/restart" >/dev/null
python3 "$repo/tools/win/hive-runonce.py" "$out/SOFTWARE" "$out/SOFTWARE.new" set StarbiteCleanup 'cmd.exe /c C:\starbite\cleanup\c-clean-win.cmd' 2>&1 | tee -a "$out/stage.log"
[ -f "$out/SOFTWARE.new" ] || { log "the hive was not edited (dirty? see above): nothing armed. The script is at C:\\starbite\\cleanup for a manual run."; exit 4; }
push_check "$out/SOFTWARE.new" "$H/SOFTWARE"
log "armed: HKLM RunOnce StarbiteCleanup -> C:\\starbite\\cleanup\\c-clean-win.cmd (runs once at the next logon, elevated); original hive kept in $out/SOFTWARE.orig"
