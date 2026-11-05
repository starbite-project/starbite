#!/bin/bash
# make-a16data.sh BYTES OUT.img : a fresh, empty f2fs for Android's /data (A16DATA), made in lima exactly
# like today's (NOTES.md 2026-10-27: mkfs.f2fs -g android -O encrypt,quota,project_quota,extra_attr,casefold
# -C utf8 -l data), as a sparse file of the partition's exact size (8 GiB layout: 8590962688 B = 2097403
# blocks; 16 GiB layout: 17180897280 B = 4194555 blocks). It costs about 40 MB of disk. Written to the
# phone with part-write.py A16DATA (zero chunks come from /dev/zero there, so stale data is cleared).
set -euo pipefail
B=${1:?bytes}; OUT=${2:?out.img}
case $B in 8590962688|17180897280) ;; *) echo "REFUSED: $B is neither A16DATA size (8590962688 / 17180897280)"; exit 1;; esac
[ ! -e "$OUT" ] || { echo "REFUSED: $OUT exists"; exit 1; }
truncate -s "$B" "$OUT"
mkfs.f2fs -f -g android -O encrypt,quota,project_quota,extra_attr,casefold -C utf8 -l data "$OUT" | tail -3
fsck.f2fs --dry-run "$OUT" | tail -2
du -h "$OUT"; sha256sum "$OUT"
