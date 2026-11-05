#!/bin/bash
# metadata-reset.sh [--dry-run] : a fresh, empty /metadata for the GSI (LineageOS 24's apexd sessions, vold
# checkpoint and log dirs must not carry over). ODM (sda20) is Android's /metadata on this phone. Runs
# TWRP's own mke2fs (its mke2fs.conf matches the 4.9 kernel; a lima mke2fs 1.47 may enable orphan_file,
# which 4.9 cannot mount), with exactly krazey90's command (lineage-24.0-initialize-metadata-sideload.zip).
# Refuses unless ODM -> sda20, it is not mounted, and an ODM backup line is in build/android/pixel/backups.txt.
set -euo pipefail
DRY=${1:-}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
dev=$(adb shell readlink -f /dev/block/by-name/ODM | tr -d '\r')
[ "$dev" = /dev/block/sda20 ] || { echo "REFUSED: ODM -> $dev, not sda20"; exit 1; }
! adb shell "grep -q '^$dev ' /proc/mounts" || { echo "REFUSED: ODM is mounted (umount /tmp/sbmd or TWRP's mount)"; exit 1; }
grep -q ' ODM.img (raw' "${BACKUP_LOG:-build/android/pixel/backups.txt}" 2>/dev/null || { echo "REFUSED: no ODM backup recorded (part-backup.sh mac ODM DIR)"; exit 1; }
echo "now: $(adb shell "blkid $dev" | tr -d '\r')"
cmd="mke2fs -F -t ext4 -b 4096 -m 0 -L metadata $dev"
if [ "$DRY" = --dry-run ]; then echo "dry run: would run: $cmd"; exit 0; fi
adb shell "$cmd" | tail -2
adb shell "e2fsck -fn $dev" | tail -1
b=$(adb shell "blkid $dev" | tr -d '\r'); echo "after: $b"
echo "$b" | grep -q 'LABEL="metadata"' && echo "$b" | grep -q 'TYPE="ext4"' || { echo "CHECK FAILED"; exit 1; }
