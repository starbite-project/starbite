#!/bin/bash
# gpt-crosscheck.sh DIR : independent check of gpt-a16grow.py's output with sgdisk (lima, needs sudo for
# losetup). Builds a sparse 4 KiB-sector disk of the phone's size from DIR/primary.bin + DIR/backup.bin
# and runs `sgdisk -v` and `sgdisk -p`. Same for the restore set (primary-orig.bin + backup-orig.bin).
set -euo pipefail
D=${1:?dir with primary.bin backup.bin}
BLOCKS=15616000
img=$(mktemp /tmp/gptcheck.XXXXXX)
trap 'rm -f "$img"' EXIT
for set in "" "-orig"; do
  rm -f "$img"; truncate -s $((BLOCKS * 4096)) "$img"
  dd if="$D/primary$set.bin" of="$img" bs=4096 conv=notrunc status=none
  dd if="$D/backup$set.bin" of="$img" bs=4096 seek=$((BLOCKS - 5)) conv=notrunc status=none
  lo=$(sudo losetup --sector-size 4096 -f --show "$img")
  echo "== primary$set.bin + backup$set.bin"
  sudo sgdisk -v "$lo" | tail -3
  sudo sgdisk -p "$lo" | tail -5
  sudo sgdisk -i 28 "$lo" | grep -E 'GUID code|unique GUID|First|Last|name'
  sudo losetup -d "$lo"
done
