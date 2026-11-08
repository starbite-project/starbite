#!/bin/sh
# Makes ext4 test images for the EFS reader (sb/ext4.c + sb/jbd2.c), in the Lima VM (needs sudo for a loop
# mount of a scratch file; nothing else is touched):
#   $OUT/efs-<kind>-dirty.img   snapshot taken while mounted, after sync: needs_recovery, the newest
#                               nv_data.bin (a temp file renamed over the old one, as Android does) only
#                               complete through the journal
#   $OUT/efs-<kind>-clean.img   the same file system after a clean unmount
#   $OUT/efs-nv-expected.bin    the newest nv_data.bin
# kinds: csum (this e2fsprogs's defaults: metadata_csum, journal csum v3) and plain (^metadata_csum).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
mnt="$OUT/efs-mnt"
mkdir -p "$mnt"
head -c 1048576 /dev/urandom > "$OUT/efs-nv-old.bin"
head -c 1048576 /dev/urandom > "$OUT/efs-nv-expected.bin"
for kind in csum plain; do
    img="$OUT/efs-$kind.img"
    rm -f "$img"
    truncate -s 16M "$img"
    if [ "$kind" = plain ]; then opts="-O ^metadata_csum,^orphan_file"; else opts=""; fi
    # shellcheck disable=SC2086
    mkfs.ext4 -q -F -b 4096 -J size=4 $opts "$img" 2>/dev/null || mkfs.ext4 -q -F -b 4096 -J size=4 "$img"
    sudo mount -o loop "$img" "$mnt"
    sudo mkdir -p "$mnt/imei" "$mnt/Battery"
    sudo cp "$OUT/efs-nv-old.bin" "$mnt/nv_data.bin"
    echo old | sudo tee "$mnt/nv.log" >/dev/null
    sudo umount "$mnt"
    sudo mount -o loop,data=journal,commit=600 "$img" "$mnt"
    # commits only through fsync (sync FILE): jbd2 writes the transaction to the journal, the checkpoint (the
    # blocks in place) waits for writeback, so the snapshot's own blocks are stale
    sudo cp "$OUT/efs-nv-old.bin" "$mnt/scratch.tmp"
    sudo sync "$mnt/scratch.tmp"
    sudo rm "$mnt/scratch.tmp"                                   # freed journaled blocks: revoke records
    sudo cp "$OUT/efs-nv-expected.bin" "$mnt/.nv_data.tmp"
    sudo sync "$mnt/.nv_data.tmp"
    sudo mv "$mnt/.nv_data.tmp" "$mnt/nv_data.bin"               # Android's write: temp + rename
    echo new | sudo tee -a "$mnt/nv.log" >/dev/null
    sudo sync "$mnt/nv.log" "$mnt/nv_data.bin" "$mnt"
    sudo cp "$img" "$OUT/efs-$kind-dirty.img"                    # while mounted: the journal is not replayed
    sudo umount "$mnt"
    cp "$img" "$OUT/efs-$kind-clean.img"
    sudo chown "$(id -u):$(id -g)" "$OUT/efs-$kind-dirty.img" "$OUT/efs-$kind-clean.img"
    rm -f "$img"
    dumpe2fs -h "$OUT/efs-$kind-dirty.img" 2>/dev/null | grep -iE '^Filesystem features|needs_recovery|Journal features' | sed "s/^/$kind dirty: /" || true
done
rmdir "$mnt"
echo "efs fixtures in $OUT"
