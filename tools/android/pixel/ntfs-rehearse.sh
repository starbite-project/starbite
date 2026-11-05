#!/bin/bash
# ntfs-rehearse.sh NTFSPROGS_DIR : rehearse Phase 2's NTFS shrink on a throwaway 2 GiB volume in lima
# (mkntfs from the same build, 4 KiB clusters like WINDOWS): --info, --no-action, the real shrink to
# 1 GiB, the container cut to 1 GiB (as the GPT step does), then a fresh --info and ntfsfix -n. Nothing outside a temp dir is touched.
set -euo pipefail
D=${1:?dir with ntfsresize etc.}
MK=$(find "$D/src" -path '*ntfsprogs/mkntfs' -type f | head -1)
T=$(mktemp -d /tmp/ntfsreh.XXXXXX); trap 'rm -rf "$T"' EXIT
truncate -s 2G "$T/n.img"
"$MK" -f -q -c 4096 -F "$T/n.img"
echo "-- info"; "$D/ntfsresize" --info --force --no-action "$T/n.img" | tail -3
echo "-- no-action shrink"; "$D/ntfsresize" --no-action --force --size 1073741824 "$T/n.img" </dev/null | tail -2
echo "-- shrink"; "$D/ntfsresize" --force --force --size 1073741824 "$T/n.img" </dev/null | tail -2
truncate -s 1073741824 "$T/n.img"     # the partition shrinks to the same size (the GPT step)
echo "-- after"; "$D/ntfsresize" --info --force --no-action "$T/n.img" | grep -i 'volume size'
"$D/ntfsfix" -n "$T/n.img" | tail -2
