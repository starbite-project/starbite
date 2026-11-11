#!/bin/sh
# crash-report.sh DIR...: sbwdog's crash ledger (docs/WIN11-GPU.md 6.45) from pulled SYSTEM hives.
# DIR is a cycle folder (reads pre/ and post/), or any folder with SYSTEM (+ SYSTEM.LOG1/LOG2).
# Each hive says how the Windows boot BEFORE it ended: pre/ = the boot before the test boot (often the user's),
# post/ = the test boot itself. Also prints the =UFSBUGCHECK lines of the pmsg snapshots next to them.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
main=$(cd "$(git -C "$here" rev-parse --git-common-dir)/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
one() {  # $1 = folder holding SYSTEM
    d=$1; h=$d/SYSTEM
    [ -f "$h" ] || return 0
    if [ -f "$d/SYSTEM.LOG1" ] && python3 "$main/tools/woa/hive-replay.py" "$h" "$tmp/r" "$d/SYSTEM.LOG1" "$d/SYSTEM.LOG2" >/dev/null 2>&1; then
        h=$tmp/r
    fi
    echo "== $d"
    python3 "$main/tools/woa/hive.py" "$h" 'ControlSet001\Services\sbwdog\State' 2>/dev/null |
        grep -E 'CrashLedger|BugCheckCallback|ShutdownNotify|BootSeq|PrevBoot|PrevBugCheck|PrevP[1-4]|CrashTest|History' |
        sed 's/^ *//' || echo "(no sbwdog State)"
    for p in "$d/loader-pmsg.txt" "$d/pmsg.txt"; do
        [ -f "$p" ] && grep -a -o '=UFSBUGCHECK [ -~]*' "$p" | sed "s#^#$(basename "$p"): #" || true
    done
}
for a in "$@"; do
    if [ -d "$a/pre" ] || [ -d "$a/post" ]; then
        one "$a/pre"; one "$a/post"
        [ -f "$a/clean/loader-pmsg.txt" ] && { echo "== $a/clean (the loader's snapshot at the clean boot = the test boot's pmsg)"; grep -a -o '=UFSBUGCHECK [ -~]*' "$a/clean/loader-pmsg.txt" || true; }
    else
        one "$a"
    fi
done
