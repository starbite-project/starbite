#!/usr/bin/env bash
# c-cleanup.sh: free space on Windows' C: (sda27) from TWRP, safely (docs/WIN11-GPU-CAPS.md 2, GD run 1: the
# phone ran out of commit memory because the pagefile could not grow on a C: with ~850 MB free).
#
#   tools/win/c-cleanup.sh            SURVEY only (read-only mount): free space, the big folders, what --delete
#                                     would free, the pagefile / hibernation / Windows.old state. Changes nothing.
#   tools/win/c-cleanup.sh --delete   the survey, then deletes ONLY caches and our own test leftovers (list below),
#                                     after copying the Geometry Dash crash reports and our driver logs to the Mac.
#   --lkr                             also Windows\LiveKernelReports (off by default: on 2026-11-11 22:57 deleting
#                                     its 1.8 MB hung TWRP's mount.ntfs for 20+ min and needed a TWRP restart)
# Hardened after that hang: files are deleted 20 at a time, biggest folder first, each `rm` under a 30 s timeout
# in TWRP and a 45 s deadline on the Mac; the first one that does not return stops all deleting (the mount is then
# suspect: restart TWRP before the next phone job), and the script says so and exits 4.
#
# Deleted with --delete (contents only; the folders stay):
#   every user's AppData\Local\Temp, Windows\Temp, ProgramData\Microsoft\Windows\WER\{ReportArchive,ReportQueue,
#   Temp}, Windows\LiveKernelReports (only with --lkr), Windows\Minidump, Windows\MEMORY.DMP, Windows\SoftwareDistribution\Download
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
del=0; lkr=0
for a in "$@"; do case $a in --delete) del=1 ;; --lkr) lkr=1 ;; *) echo "usage: c-cleanup.sh [--delete [--lkr]]"; exit 64 ;; esac; done
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
umount_w() { adb shell "timeout -s KILL 60 sync; timeout -s KILL 30 umount $W 2>/dev/null" >/dev/null 2>&1 || true; }
trap umount_w EXIT
free_mb() { adb shell "df -m $W | tail -1" | tr -d '\r' | awk '{print $4}'; }
kb() { adb shell "du -sk \"$W/$1\" 2>/dev/null | tail -1" | tr -d '\r' | awk '{print $1+0}'; }

# ---- the deletable set (contents only), in this order after the users' Temp folders ---------------------------
TARGETS=(
"Windows/Temp"
"ProgramData/Microsoft/Windows/WER/ReportArchive"
"ProgramData/Microsoft/Windows/WER/ReportQueue"
"ProgramData/Microsoft/Windows/WER/Temp"
"Windows/Minidump"
"Windows/SoftwareDistribution/Download"
"starbite/auto/gpu"
)
[ "$lkr" = 1 ] && TARGETS+=("Windows/LiveKernelReports")

mount_ro
log "SURVEY (read-only). C: free: $(free_mb) MB"
adb shell "df -h $W | tail -1" | tr -d '\r' | tee -a "$out/cleanup.log"
log "top level of C: (MB):"
adb shell "cd $W && for d in *; do du -sm \"\$d\" 2>/dev/null; done | sort -rn | head -14" | tr -d '\r' | tee -a "$out/cleanup.log"
log "Program Files (x86) and Windows, the biggest parts (MB, report only):"
adb shell "cd '$W/Program Files (x86)' && for d in *; do du -sm \"\$d\" 2>/dev/null; done | sort -rn | head -6" | tr -d '\r' | sed 's/^/  x86: /' | tee -a "$out/cleanup.log"
adb shell "cd $W/Windows && for d in WinSxS SoftwareDistribution Installer System32 SysWOW64 SyChpe32 Temp LiveKernelReports; do du -sm \"\$d\" 2>/dev/null; done | sort -rn" | tr -d '\r' | sed 's/^/  Windows: /' | tee -a "$out/cleanup.log"
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
# one remote command under a deadline on the Mac too (an rm stuck in the FUSE driver ignores TWRP's timeout)
rsh() {  # rsh SECONDS 'command' -> its output; exit 124 if the Mac-side deadline passed
    local t=$1; shift
    local f; f=$(mktemp)
    adb shell "$1" < /dev/null > "$f" 2>&1 & local p=$!
    local i=0
    while kill -0 $p 2>/dev/null; do
        sleep 1; i=$((i + 1))
        if [ $i -ge "$t" ]; then kill $p 2>/dev/null; cat "$f"; rm -f "$f"; return 124; fi
    done
    wait $p; local rc=$?
    tr -d '\r' < "$f"; rm -f "$f"; return $rc
}
stuck() {
    log "STUCK: '$1' did not return in time. Stopped deleting. The mount is suspect: restart TWRP before the next phone job."
    exit 4
}
clear_dir() {  # the contents of C:\<arg>, 20 files at a time; never the folder itself
    local rel="${1:-}"   # flush() below is a function too: its $1 is not this one
    case "$rel" in ""|/*|*..*) log "refused: '$rel'"; return ;; esac
    local d="$W/$rel" n=0 f
    rsh 30 "[ -d \"$d\" ]" >/dev/null || { log "C:/$rel: not there"; return; }
    local list; list=$(mktemp)
    rsh 60 "cd \"$d\" && find . -mindepth 1 \\( -type f -o -type l \\) 2>/dev/null" > "$list" || stuck "find in C:/$rel"
    # batches of 20 names per timed rm: one adb round trip per file would take half an hour for a big Temp
    local batch="" nb=0 first=""
    flush() {
        [ -n "$batch" ] || return 0
        local r; r=$(rsh 45 "cd \"$d\" && timeout -s KILL 30 rm -f --$batch; echo rc=\$?") || stuck "rm in C:/$rel (from ${first#./})"
        case "$r" in *rc=137*) stuck "rm in C:/$rel (from ${first#./})" ;; *rc=0*) ;; *) log "  some files in C:/$rel could not be deleted (batch from ${first#./}: $r)" ;; esac
        batch=""; nb=0; first=""
    }
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        case "$f" in ./*) ;; *) continue ;; esac
        local q; q=$(printf '%s' "$f" | sed "s/'/'\\\\''/g")   # single-quoted for TWRP's shell
        batch="$batch '$q'"; nb=$((nb + 1)); n=$((n + 1)); [ -n "$first" ] || first=$f
        [ $nb -lt 20 ] || flush
    done < "$list"
    flush
    rm -f "$list"
    rsh 60 "cd \"$d\" && find . -mindepth 1 -depth -type d -empty -exec rmdir {} \\; 2>/dev/null; true" >/dev/null || stuck "rmdir in C:/$rel"
    log "cleared C:/$rel ($n files)"
}
for u in "${users[@]}"; do
    case $u in "All Users"|Default|"Default User"|Public|desktop.ini) continue ;; esac
    clear_dir "Users/$u/AppData/Local/Temp"
done
for t in "${TARGETS[@]}"; do clear_dir "$t"; done
rsh 45 "timeout -s KILL 30 rm -f $W/Windows/MEMORY.DMP; true" >/dev/null || stuck "rm MEMORY.DMP"
rsh 45 "cd $W/ProgramData/Starbite/umdlog 2>/dev/null && timeout -s KILL 30 rm -f *.log dump-* shot-* sweep-*; true" >/dev/null || stuck "rm in umdlog"
log "cleared Windows/MEMORY.DMP and umdlog's logs and dumps (its flag files stay)"
umount_w; mount_ro
after=$(free_mb)
log "C: free: $before MB -> $after MB (freed $((after - before)) MB)"
adb shell "df -h $W | tail -1" | tr -d '\r' | tee -a "$out/cleanup.log"
