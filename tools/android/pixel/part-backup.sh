#!/bin/bash
# part-backup.sh mac NAME OUTDIR [--dry-run]   partition -> OUTDIR/NAME.img.gz on the Mac (small ones)
# part-backup.sh card NAME TAG [--dry-run]     partition -> /external_sd/starbite-backup/TAG/NAME.img.gz
#                                              on the microSD's exFAT (big ones: A16DATA, SYSTEM, WINDOWS)
# part-backup.sh restore-card NAME TAG [--dry-run]   ROLLBACK: the card backup -> the partition, phone-side;
#                                              only SYSTEM VENDOR ODM A16DATA; the backup's sha256 must be the
#                                              one recorded in build/android/pixel/backups.txt, and so must
#                                              the partition's afterwards.
# Backup modes are read-only on the partition. gzip -1 runs on the phone. Verified: sha256 of the raw partition (phone)
# == sha256 of the decompressed backup (Mac or phone). Prints both; exit 1 on any mismatch.
# --dry-run: checks adb state, the by-name link, the size and the free space at the target; copies nothing.
set -euo pipefail
mode=${1:?mac|card}; NAME=${2:?partition}; dest=${3:?outdir|tag}; dry=${4:-}
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
dev=$(adb shell "readlink -f /dev/block/by-name/$NAME" | tr -d '\r')
[ -n "$dev" ] && [ "$dev" != "/dev/block/by-name/$NAME" ] || { echo "no partition $NAME"; exit 1; }
size=$(adb shell "blockdev --getsize64 $dev" | tr -d '\r')
echo "$NAME = $dev, $size B ($((size >> 20)) MiB)"
case $mode in
mac)
  free=$(df -k "$(dirname "$dest")" 2>/dev/null | awk 'NR==2 {print $4 * 1024}')
  echo "Mac free at $dest: $((free >> 20)) MiB (a gzip'd backup is at most the partition size)"
  [ "$dry" = --dry-run ] && { echo "dry run: nothing copied"; exit 0; }
  mkdir -p "$dest"
  adb exec-out "dd if=$dev bs=1048576 2>/dev/null | gzip -1" > "$dest/$NAME.img.gz"
  here=$(gzip -dc "$dest/$NAME.img.gz" | shasum -a 256 | cut -d' ' -f1) ;;
card)
  adb shell 'mount | grep -q " /external_sd "' || { echo "card not mounted at /external_sd in TWRP"; exit 1; }
  free=$(adb shell "df -k /external_sd | tail -1" | tr -d '\r' | awk '{print $(NF-2) * 1024}')
  echo "card free: $((free >> 20)) MiB"
  [ "$free" -gt $((size + (1 << 30))) ] || echo "WARNING: less than partition size + 1 GiB free on the card (gzip usually needs far less)"
  d=/external_sd/starbite-backup/$dest
  [ "$dry" = --dry-run ] && { echo "dry run: would write $d/$NAME.img.gz"; exit 0; }
  adb shell "mkdir -p $d && dd if=$dev bs=1048576 2>/dev/null | gzip -1 > $d/$NAME.img.gz && sync"
  here=$(adb shell "gzip -dc $d/$NAME.img.gz | sha256sum" | tr -d '\r' | cut -d' ' -f1) ;;
restore-card)
  case $NAME in SYSTEM|VENDOR|ODM|A16DATA) ;; *) echo "REFUSED: restore only to SYSTEM VENDOR ODM A16DATA"; exit 1;; esac
  ! adb shell "grep -q '^$dev ' /proc/mounts" || { echo "REFUSED: $dev is mounted"; exit 1; }
  f=/external_sd/starbite-backup/$dest/$NAME.img.gz
  want=$(grep " $NAME.img (raw; stored gzip'd, card:$dest)" "${BACKUP_LOG:-build/android/pixel/backups.txt}" | tail -1 | cut -d' ' -f1)
  [ -n "$want" ] || { echo "REFUSED: no recorded sha256 for $NAME card:$dest"; exit 1; }
  got=$(adb shell "gzip -dc $f | sha256sum" | tr -d '\r' | cut -d' ' -f1)
  [ "$got" = "$want" ] || { echo "REFUSED: $f is $got, recorded $want"; exit 1; }
  echo "backup $f verified ($want)"
  [ "$dry" = --dry-run ] && { echo "dry run: would run gzip -dc $f | dd of=$dev bs=1048576 conv=notrunc,fsync"; exit 0; }
  adb shell "gzip -dc $f | dd of=$dev bs=1048576 conv=notrunc,fsync 2>/dev/null && sync"
  there=$(adb shell "sha256sum $dev" | tr -d '\r' | cut -d' ' -f1)
  [ "$there" = "$want" ] && { echo "$NAME restored and verified"; exit 0; } || { echo "RESTORE MISMATCH: $there"; exit 1; } ;;
*) echo "mode mac|card|restore-card"; exit 1 ;;
esac
there=$(adb shell "sha256sum $dev" | tr -d '\r' | cut -d' ' -f1)
echo "partition sha256 $there"; echo "backup    sha256 $here"
[ "$here" = "$there" ] || { echo "BACKUP MISMATCH: do not go on"; exit 1; }
echo "$there  $NAME.img (raw; stored gzip'd, $mode:$dest)" | tee -a "${BACKUP_LOG:-build/android/pixel/backups.txt}"
