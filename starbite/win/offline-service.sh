#!/bin/sh
# Offline edits of the phone's Windows SYSTEM hive for the Starbite drivers (docs/WIN11-GPU.md 4.3).
# Runs in the lima VM (hivexsh). Works on a COPY: pull SYSTEM from the WINDOWS partition (TWRP,
# ntfs-3g), run this on the copy, push it back only if the script said OK.
#
#   offline-service.sh check   SYSTEM            is the hive clean (no unflushed log)? which ControlSet?
#   offline-service.sh install SYSTEM sbthermal  add Services\sbthermal (Type 1, Start 1, ErrorControl 0)
#   offline-service.sh disable SYSTEM <name>     Start = 4 (the driver never loads)
#   offline-service.sh enable  SYSTEM <name>     Start = 1 (sbthermal) / 3 (PnP drivers)
#   offline-service.sh guard   SYSTEM <name>     GuardBoots = 0 (after a fixed driver replaces a refused one)
#
# Refuses a dirty hive: when the base block's two sequence numbers differ, Windows replays
# SYSTEM.LOG1/LOG2 at the next boot over whatever we write. Boot Windows once cleanly instead, or use the
# kill file (\Windows\Starbite\<name>.off) or the previous BOOT image.
set -eu
cmd=$1
hive=$2
name=${3:-}

[ -f "$hive" ] || { echo "no hive $hive" >&2; exit 1; }
seq=$(python3 -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read(4096)
assert d[:4] == b"regf", "not a registry hive"
print(*struct.unpack_from("<II", d, 4))' "$hive") || exit 1
s1=${seq% *}; s2=${seq#* }
if [ "$s1" != "$s2" ]; then
    echo "DIRTY hive (sequence $s1 != $s2): Windows would replay its logs over offline edits; not touching it" >&2
    exit 2
fi
cur=$(hivexsh "$hive" <<EOF | tr -d '\r' | awk -F'[:x]' '/Current/ {print $NF}'
cd \\Select
lsval
EOF
)
cur=${cur:-1}
cs=$(printf 'ControlSet%03d' "$((cur))")
echo "hive clean (sequence $s1), current $cs"
[ "$cmd" = check ] && exit 0
[ -n "$name" ] || { echo "which service?" >&2; exit 1; }
case "$name" in sbthermal|sbdisplay|sbmali|sbwddm) ;; *) echo "not a Starbite driver: $name" >&2; exit 1 ;; esac

setdword() {    # key value-name value
    hivexsh -w "$hive" <<EOF
cd \\$cs\\Services\\$1
setval 1
$2
dword:$3
commit
EOF
}

case "$cmd" in
install)
    [ "$name" = sbthermal ] || { echo "only sbthermal is a plain service; PnP drivers install through their INF" >&2; exit 1; }
    hivexsh -w "$hive" <<EOF
cd \\$cs\\Services
add sbthermal
cd sbthermal
setval 5
Type
dword:1
Start
dword:1
ErrorControl
dword:0
ImagePath
expandstring:\\SystemRoot\\System32\\drivers\\sbthermal.sys
DisplayName
string:Starbite temperature watchdog
commit
EOF
    ;;
disable) setdword "$name" Start 4 ;;
enable) [ "$name" = sbthermal ] && setdword "$name" Start 1 || setdword "$name" Start 3 ;;
guard) setdword "$name" GuardBoots 0 ;;
*) echo "unknown command $cmd" >&2; exit 1 ;;
esac
hivexsh "$hive" <<EOF
cd \\$cs\\Services\\$name
lsval
EOF
echo OK
