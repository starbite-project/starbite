#!/usr/bin/env bash
# c-cleanup.sh: free space on Windows' C: (sda27) from TWRP, safely (docs/WIN11-GPU-CAPS.md 2, GD run 1: the
# phone ran out of commit memory because the pagefile could not grow on a C: with ~850 MB free).
#
#   tools/win/c-cleanup.sh            SURVEY only (read-only mount): free space, the big folders, what --delete
#                                     would free, the pagefile / hibernation / Windows.old state. Changes nothing.
#   tools/win/c-cleanup.sh --delete   the survey, then deletes ONLY caches and our own test leftovers (list below),
#                                     after copying the Geometry Dash crash reports and our driver logs to the Mac.
#
# Deleted with --delete (contents only; the folders stay):
#   Windows\Temp, every user's AppData\Local\Temp, ProgramData\Microsoft\Windows\WER\{ReportArchive,ReportQueue,
#   Temp}, Windows\LiveKernelReports, Windows\Minidump, Windows\MEMORY.DMP, Windows\SoftwareDistribution\Download
#   (Windows Update's download cache), C:\starbite\auto\gpu (gpu-cycle's test files; it recreates them), and in
#   ProgramData\Starbite\umdlog only *.log, dump-*, shot-*, sweep-* (the flag files there stay).
# Never touched, only reported: pagefile.sys / swapfile.sys / hiberfil.sys and their settings, Windows.old,
#   Steam and its games and caches, anything under Users\* other than AppData\Local\Temp, any hive.
# Needs: the phone in TWRP, no other phone job running (one at a time). Output: build/win/cleanup-<date>/.
set -uo pipefail   # not -e: a du/ls of a missing folder is normal; the steps that matter are checked
NTFS=/dev/block/sda27
W=/tmp/wwin
here=$(cd "$(dirname "$0")" && pwd)
main=$(cd "$here/../.." && git rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's#/\.git$##')
[ -d "$main/build" ] || main=/Users/dev/project-starbite
out=$main/build/win/cleanup-$(date +%Y%m%d-%H%M)
del=0; [ "${1:-}" = --delete ] && del=1
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/cleanup.log"; }
mkdir -p "$out"

[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
if pgrep -fl 'gpu-cycle|autotest-cycle|boot-safe|store-flash' | grep -v pgrep >/dev/null; then
    echo "another phone job is running (one at a time):"; pgrep -fl 'gpu-cycle|autotest-cycle|boot-safe|store-flash'; exit 1
fi
mounted() {  # $1 = ro|rw: is C: mounted at $W with that mode?
    adb shell "grep ' $W ' /proc/mounts" | tr -d '\r' | awk '{print $4}' | tr ',' '\n' | grep -qx "$1"
}
mount_ro() { adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null; mounted ro || { echo "C: did not mount read-only"; exit 2; }; }
mount_rw() { adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null; mounted rw || { echo "C: did not mount read-write (dirty NTFS? boot Windows once and shut down cleanly)"; exit 2; }; }
umount_w() { adb shell "sync; umount $W 2>/dev/null" >/dev/null || true; }
trap umount_w EXIT
free_mb() { adb shell "df -m $W | tail -1" | tr -d '\r' | awk '{print $4}'; }
kb() { adb shell "du -sk \"$W/$1\" 2>/dev/null | tail -1" | tr -d '\r' | awk '{print $1+0}'; }

# ---- the deletable set: path under C:, how its contents are matched -----------------------------------------
TARGETS=(
"Windows/Temp"
"ProgramData/Microsoft/Windows/WER/ReportArchive"
"ProgramData/Microsoft/Windows/WER/ReportQueue"
"ProgramData/Microsoft/Windows/WER/Temp"
"Windows/LiveKernelReports"
"Windows/Minidump"
"Windows/SoftwareDistribution/Download"
"starbite/auto/gpu"
)

mount_ro
log "SURVEY (read-only). C: free: $(free_mb) MB"
adb shell "df -h $W | tail -1" | tr -d '\r' | tee -a "$out/cleanup.log"
log "top level of C: (MB):"
adb shell "cd $W && for d in *; do du -sm \"\$d\" 2>/dev/null; done | sort -rn | head -14" | tr -d '\r' | tee -a "$out/cleanup.log"
log "users (MB):"
adb shell "cd $W/Users && for d in *; do du -sm \"\$d\" 2>/dev/null; done | sort -rn | head -8" | tr -d '\r' | tee -a "$out/cleanup.log"
total=0
log "deletable with --delete (KB):"
for t in "${TARGETS[@]}"; do k=$(kb "$t"); total=$((total + k)); printf '  %8s  %s\n' "$k" "$t" | tee -a "$out/cleanup.log"; done
users=()
while IFS= read -r u; do [ -n "$u" ] && users+=("$u"); done < <(adb shell "ls -1 $W/Users" | tr -d '\r')
for u in "${users[@]}"; do
    case $u in "All Users"|Default|"Default User"|Public|desktop.ini) continue ;; esac
    k=$(kb "Users/$u/AppData/Local/Temp"); total=$((total + k)); printf '  %8s  %s\n' "$k" "Users/$u/AppData/Local/Temp" | tee -a "$out/cleanup.log"
done
k=$(adb shell "ls -l $W/Windows/MEMORY.DMP 2>/dev/null" | tr -d '\r' | awk '{print int($5/1024)}'); k=${k:-0}; total=$((total + k))
printf '  %8s  %s\n' "$k" "Windows/MEMORY.DMP" | tee -a "$out/cleanup.log"
k=$(adb shell "cd $W/ProgramData/Starbite/umdlog 2>/dev/null && du -ck *.log dump-* shot-* sweep-* 2>/dev/null | tail -1" | tr -d '\r' | awk '{print $1+0}'); total=$((total + k))
printf '  %8s  %s\n' "$k" "ProgramData/Starbite/umdlog (*.log dump-* shot-* sweep-*)" | tee -a "$out/cleanup.log"
log "--delete would free about $((total / 1024)) MB"
log "REPORT ONLY (never changed by this script):"
adb shell "ls -l $W/pagefile.sys $W/swapfile.sys $W/hiberfil.sys 2>/dev/null" | tr -d '\r' | awk '{printf "  %6d MB  %s\n", $5/1048576, $NF}' | tee -a "$out/cleanup.log"
adb shell "[ -d $W/Windows.old ] && du -sm $W/Windows.old" | tr -d '\r' | sed 's/^/  Windows.old MB: /' | tee -a "$out/cleanup.log" || true
adb shell "cd '$W/Program Files (x86)/Steam' 2>/dev/null && du -sm steamapps/common/* steamapps/downloading steamapps/shadercache appcache depotcache 2>/dev/null | sort -rn | head -10" | tr -d '\r' | sed 's/^/  Steam MB: /' | tee -a "$out/cleanup.log" || true
adb pull "$W/Windows/System32/config/SYSTEM" "$out/SYSTEM" >/dev/null 2>&1 || true
if [ -f "$out/SYSTEM" ]; then
    python3 "$main/tools/woa/hive.py" "$out/SYSTEM" 'ControlSet001\Control\Session Manager\Memory Management' 2>/dev/null |
        grep -E 'PagingFiles|ExistingPageFiles|TempPageFile' | sed 's/^/  /' | tee -a "$out/cleanup.log" || true
    python3 "$main/tools/woa/hive.py" "$out/SYSTEM" 'ControlSet001\Control\Power' 2>/dev/null |
        grep -E 'HibernateEnabled ' | sed 's/^/  /' | tee -a "$out/cleanup.log" || true
fi
[ "$del" = 1 ] || { log "survey only: nothing changed. Run with --delete to free the space above."; exit 0; }

# ---- delete --------------------------------------------------------------------------------------------------
log "copying the Geometry Dash crash reports and our driver logs to $out first"
mkdir -p "$out/wer" "$out/umdlog"
for d in ReportArchive ReportQueue; do
    adb shell "ls '$W/ProgramData/Microsoft/Windows/WER/$d' 2>/dev/null" | tr -d '\r' | grep -i GeometryDash | while read -r r; do
        adb pull "$W/ProgramData/Microsoft/Windows/WER/$d/$r" "$out/wer/" >/dev/null 2>&1 || true
    done
done
adb shell "ls $W/ProgramData/Starbite/umdlog 2>/dev/null" | tr -d '\r' | grep -E '\.log$' | while read -r f; do
    adb pull "$W/ProgramData/Starbite/umdlog/$f" "$out/umdlog/" >/dev/null 2>&1 || true
done
log "copied: $(ls "$out/wer" | wc -l | tr -d ' ') GD reports, $(ls "$out/umdlog" | wc -l | tr -d ' ') logs"
before=$(free_mb)
mount_rw
clear_dir() {  # the contents of C:\$1, never the folder itself; refuses anything outside $W or too short
    case "$1" in ""|/*|*..*) log "refused: '$1'"; return ;; esac
    adb shell "d=\"$W/$1\"; [ -d \"\$d\" ] && cd \"\$d\" && [ \"\$(pwd)\" != \"$W\" ] && rm -rf -- ./* ./.[!.]* 2>/dev/null; true" >/dev/null
    log "cleared C:/$1"
}
for t in "${TARGETS[@]}"; do clear_dir "$t"; done
for u in "${users[@]}"; do
    case $u in "All Users"|Default|"Default User"|Public|desktop.ini) continue ;; esac
    clear_dir "Users/$u/AppData/Local/Temp"
done
adb shell "rm -f $W/Windows/MEMORY.DMP; cd $W/ProgramData/Starbite/umdlog 2>/dev/null && rm -f *.log dump-* shot-* sweep-*; true" >/dev/null
log "cleared Windows/MEMORY.DMP and umdlog's logs and dumps (its flag files stay)"
umount_w; mount_ro
after=$(free_mb)
log "C: free: $before MB -> $after MB (freed $((after - before)) MB)"
adb shell "df -h $W | tail -1" | tr -d '\r' | tee -a "$out/cleanup.log"
