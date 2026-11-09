#!/usr/bin/env bash
# Pull files from iOS 26's card root to a local folder, read only (phone in TWRP).
#   tools/ios26-pull.sh OUTDIR /path/one /path/two ...       (a path ending in / is listed instead: OUTDIR/ls-*.txt)
# Each file lands at OUTDIR/<path with / -> __>; failures are reported and skipped.
set -uo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
out=${1:?OUTDIR}; shift
mkdir -p "$out"
for p in "$@"; do
    name=$(echo "${p%/}" | sed 's|^/||; s|/|__|g')
    if [ "${p%/}" != "$p" ]; then
        HFS_CARD=1 python3 tools/overlay/hfs-read.py --ls "${p%/}" > "$out/ls-$name.txt" 2>&1 </dev/null && echo "listed $p" || echo "FAILED list $p"
    else
        HFS_CARD=1 python3 tools/overlay/hfs-read.py "$p" "$out/$name" </dev/null >/dev/null 2>"$out/.err" && echo "pulled $p" ||
            echo "FAILED $p: $(tail -1 "$out/.err")"
    fi
done
rm -f "$out/.err"
