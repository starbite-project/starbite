#!/usr/bin/env bash
# sbparam.sh: a DWORD of Services\sbwddm\Parameters in the Windows SYSTEM hive, from TWRP (e.g. ScratchUnmap).
#   tools/win/sbparam.sh show NAME OUT         read-only: the value now (absent = the driver's default)
#   tools/win/sbparam.sh set NAME=VALUE OUT    set it (hive clean check, backup in OUT/backup, read-back)
# gpu-cycle.sh's install keeps parameters it does not manage, so a value set here holds for the next test boot AND
# the user's boots after it: set it back after the test. Refuses a dirty hive (exit 3). No boot.
set -euo pipefail
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
cd "$(dirname "$0")/../.."
mode=${1:-}; arg=${2:-}; out=${3:-}
[ -n "$mode" ] && [ -n "$arg" ] && [ -n "$out" ] || { sed -n '2,6p' "$self"; exit 1; }
case $mode in
  show) case $arg in *[!A-Za-z0-9_]*) echo "bad name $arg"; exit 1 ;; esac; name=$arg ;;
  set) name=${arg%%=*}; val=${arg#*=}; case $name in ''|*[!A-Za-z0-9_]*) echo "bad name $name"; exit 1 ;; esac
       case $val in ''|*[!0-9]*) echo "value must be a decimal number"; exit 1 ;; esac ;;
  *) sed -n '2,6p' "$self"; exit 1 ;;
esac
NTFS=/dev/block/sda27; W=/tmp/wwin; SYS=Windows/System32/config/SYSTEM; HS=tools/win/hive-service.py
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/sbparam.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "the phone is not in TWRP"; exit 1; }
tmp=$(mktemp -d); trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
value_of() { python3 - "$1" "$name" <<'PY'
import sys, struct
sys.path.insert(0, 'tools/woa')
from hive import Hive
h = Hive(sys.argv[1])
_, sel = h.find('Select')
cs = 'ControlSet%03d' % struct.unpack('<I', dict((n, d) for n, _, d in h.values(sel))['Current'])[0]
try:
    _, k = h.find(cs + r'\Services\sbwddm\Parameters')
    v = {n: (t, d) for n, t, d in h.values(k)}
    print(struct.unpack('<I', v[sys.argv[2]][1])[0] if sys.argv[2] in v and v[sys.argv[2]][0] == 4 else 'absent')
except SystemExit:
    print('absent (no Parameters key)')
PY
}
if [ "$mode" = show ]; then
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    log "sbwddm Parameters\\$name = $(value_of "$tmp/SYSTEM")"
    exit 0
fi
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
python3 "$HS" "$tmp/SYSTEM" check >> "$out/sbparam.log" 2>&1 || { log "the SYSTEM hive is dirty: run gpu-cycle.sh --clean-only first"; exit 3; }
cp "$tmp/SYSTEM" "$out/backup/SYSTEM-sbparam-$name-$(date +%Y%m%d-%H%M%S)"
before=$(value_of "$tmp/SYSTEM")
python3 "$HS" "$tmp/SYSTEM" "$tmp/S1" param sbwddm "$name=$val" >> "$out/sbparam.log" 2>&1 || { log "the hive edit failed: nothing pushed"; exit 1; }
after=$(value_of "$tmp/S1")
[ "$after" = "$val" ] || { log "read-back says $after, not $val: nothing pushed"; exit 4; }
adb push "$tmp/S1" "$W/$SYS" >/dev/null
a=$(shasum -a 256 "$tmp/S1" | cut -c1-64); b=$(adb shell "sha256sum '$W/$SYS'" | tr -d '\r' | cut -c1-64)
[ "$a" = "$b" ] || { log "READ-BACK MISMATCH: SYSTEM"; exit 4; }
log "sbwddm Parameters\\$name: $before -> $val"
