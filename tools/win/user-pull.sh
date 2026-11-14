#!/usr/bin/env bash
# user-pull.sh OUT [--report]: READ-ONLY pull of the user's own Windows session from TWRP (docs/WIN11-GPU.md 6.70).
# Run it BEFORE any gpu-cycle (every install empties umdlog). Mounts the WINDOWS volume read-only; changes nothing.
# Pulls: umdlog (GD's glstats/gllows), SYSTEM + LOG1/LOG2, the System / Application / Diagnostics-Performance /
# Defender / Resource-Exhaustion event logs, WER report folder names + GD/Steam Report.wer, Minidump listing, the GD
# folder's GL files, pmsg. Then tools/win/user-report.py OUT (frame times, guard state, boot/shutdown durations,
# what delayed the shutdown, service and app crashes).
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:?usage: user-pull.sh OUT}
NTFS=/dev/block/sda27
W=/tmp/wwin
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "the phone is not in TWRP"; exit 1; }
mkdir -p "$out/umdlog" "$out/evtx" "$out/wer"
adb shell "cat /sys/fs/pstore/pmsg-ramoops-0" 2>/dev/null | LC_ALL=C tr -c '[:print:]\n' '.' > "$out/pmsg.txt" || true
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
trap 'adb shell "umount '"$W"'" >/dev/null 2>&1 || true' EXIT
adb pull "$W/ProgramData/Starbite/umdlog/." "$out/umdlog/" >/dev/null 2>&1 || echo "no umdlog"
for f in SYSTEM SYSTEM.LOG1 SYSTEM.LOG2; do adb pull "$W/Windows/System32/config/$f" "$out/" >/dev/null 2>&1 || echo "missing $f"; done
L="$W/Windows/System32/winevt/Logs"
for f in System.evtx Application.evtx "Microsoft-Windows-Diagnostics-Performance%4Operational.evtx" \
         "Microsoft-Windows-Windows Defender%4Operational.evtx" "Microsoft-Windows-Resource-Exhaustion-Detector%4Operational.evtx"; do
    adb pull "$L/$f" "$out/evtx/" >/dev/null 2>&1 || echo "missing $f"
done
for d in ReportArchive ReportQueue; do
    adb shell "ls -la --time-style=+%Y-%m-%dT%H:%M '$W/ProgramData/Microsoft/Windows/WER/$d' 2>/dev/null || ls -la '$W/ProgramData/Microsoft/Windows/WER/$d'" | tr -d '\r' > "$out/wer/$d.txt" 2>&1 || true
    for r in $(grep -o -E '(AppCrash|AppHang)_(GeometryDash|steam|sihost|explorer|ShellExperienceHost|dwm|msedge)[^ ]*' "$out/wer/$d.txt" | tail -6); do
        adb pull "$W/ProgramData/Microsoft/Windows/WER/$d/$r/Report.wer" "$out/wer/$r.wer" >/dev/null 2>&1 || true
    done
done
adb shell "ls -la '$W/Windows/Minidump' 2>&1" | tr -d '\r' > "$out/minidump.txt" || true
adb shell "ls -la '$W/Program Files (x86)/Steam/steamapps/common/Geometry Dash/' 2>&1 | grep -i -E 'opengl|gallium|starbite|GeometryDash.exe'; sha256sum '$W/Program Files (x86)/Steam/steamapps/common/Geometry Dash/opengl32.dll' '$W/Program Files (x86)/Steam/steamapps/common/Geometry Dash/libgallium_wgl.dll' 2>&1" | tr -d '\r' > "$out/gd-folder.txt" || true
adb shell "ls -la $W/pagefile.sys $W/hiberfil.sys $W/swapfile.sys 2>&1; df -h $W" | tr -d '\r' > "$out/disk.txt" || true
echo "pulled into $out"
# 11-14: WER LocalDumps of the shell processes (tools/win/touch.sh apply sets them), newest 4
adb shell "ls -la '$W/sbdumps' 2>/dev/null" | tr -d '\r' > "$out/wer/sbdumps.txt" 2>&1 || true
for f in $(adb shell "ls -t '$W/sbdumps' 2>/dev/null | head -4" | tr -d '\r'); do
    mkdir -p "$out/dumps"; adb pull "$W/sbdumps/$f" "$out/dumps/$f" >/dev/null 2>&1 || true
done
python3 tools/win/user-report.py "$out" | tee "$out/report.txt"
