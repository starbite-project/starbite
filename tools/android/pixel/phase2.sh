#!/bin/bash
# phase2.sh STEP [--dry-run] : Phase 2 of docs/ANDROID-PIXEL.md, A16DATA 8 -> 16 GiB by shrinking WINDOWS.
# Run from the repo root on the Mac, phone in TWRP. One step per call; each prints what it checks.
#   check        read-only: the phone's GPT is byte-identical to build/android/pixel/gpt-a16-16g/*-orig.bin
#                (the set gpt-a16grow.py made from today's backup), WINDOWS not mounted, NTFS clean and not
#                hibernated, its minimum size leaves >= 2 GiB free inside the new 17.84 GiB WINDOWS
#   ntfs-backup  ntfsclone image of WINDOWS (used clusters only) -> card /external_sd/starbite-backup/TAG/
#   ntfs-shrink  ntfsresize --no-action, then the real shrink to exactly the new WINDOWS size
#   gpt          write primary.bin + backup.bin (only if the phone still has the -orig set), read back
#   verify       after a reboot to TWRP: kernel partition sizes, by-name links, NTFS consistent
#   gpt-restore  ROLLBACK: write primary-orig.bin + backup-orig.bin (only if the phone has the new or the
#                orig set), read back. The shrunk NTFS still fits the old, larger WINDOWS.
# --dry-run on any step: every read-only check, then the write commands are printed, not run.
set -euo pipefail
STEP=${1:?step}; DRY=${2:-}
G=build/android/pixel/gpt-a16-16g
NP=build/android/pixel/ntfsprogs
TAG=${TAG:-$(date +%Y%m%d)}
BLOCKS=15616000
run() { if [ "$DRY" = --dry-run ]; then echo "  [dry-run] adb shell '$1'"; else adb shell "$1"; fi; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
[ -s "$G/MANIFEST.txt" ] || { echo "no $G: run gpt-a16grow.py gen on a fresh head/tail backup first"; exit 1; }
[ "$(adb shell cat /sys/block/sda/size | tr -d '\r')" = $((BLOCKS * 8)) ] || { echo "sda size changed: STOP"; exit 1; }
WIN=$(adb shell readlink -f /dev/block/by-name/WINDOWS | tr -d '\r')
[ "$WIN" = /dev/block/sda27 ] || { echo "WINDOWS -> $WIN, not sda27: STOP"; exit 1; }
SIZE=$(sed -n 's/^ntfsresize_size \([0-9]*\).*/\1/p' "$G/MANIFEST.txt")
phone_gpt() {  # prints "orig", "new" or "other"
  local p b
  p=$(adb exec-out "dd if=/dev/block/sda bs=4096 count=6 2>/dev/null" | shasum -a 256 | cut -d' ' -f1)
  b=$(adb exec-out "dd if=/dev/block/sda bs=4096 skip=$((BLOCKS - 5)) count=5 2>/dev/null" | shasum -a 256 | cut -d' ' -f1)
  if [ "$p $b" = "$(shasum -a 256 < $G/primary-orig.bin | cut -d' ' -f1) $(shasum -a 256 < $G/backup-orig.bin | cut -d' ' -f1)" ]; then echo orig
  elif [ "$p $b" = "$(shasum -a 256 < $G/primary.bin | cut -d' ' -f1) $(shasum -a 256 < $G/backup.bin | cut -d' ' -f1)" ]; then echo new
  else echo other; fi
}
push_tools() {
  (cd $NP && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "ntfsprogs hashes wrong"; exit 1; }
  adb push $NP/ntfsresize $NP/ntfsclone $NP/ntfsinfo $NP/ntfsfix /tmp/ >/dev/null
  adb shell 'chmod 0755 /tmp/ntfsresize /tmp/ntfsclone /tmp/ntfsinfo /tmp/ntfsfix'
}
not_mounted() { ! adb shell "grep -q -E '^(/dev/block/sda27|$WIN) ' /proc/mounts" || { echo "WINDOWS is mounted: umount it in TWRP"; exit 1; }; }
case $STEP in
check)
  echo "GPT on the phone: $(phone_gpt) (must be orig)"
  not_mounted; push_tools
  adb shell "/tmp/ntfsfix -n $WIN" | tail -3
  info=$(adb shell "/tmp/ntfsresize --info --no-action $WIN" | tr -d '\r') || true
  echo "$info" | tail -6
  min=$(echo "$info" | sed -n 's/.*You might resize at \([0-9]*\) bytes.*/\1/p')
  [ -n "$min" ] || { echo "STOP: ntfsresize gave no minimum (unclean or hibernated volume: boot Windows, Shut down, retry)"; exit 1; }
  echo "minimum $min B, new size $SIZE B, room after shrink $(( (SIZE - min) >> 20 )) MiB"
  [ $((SIZE - min)) -ge $((2 << 30)) ] || { echo "STOP: less than 2 GiB would stay free in Windows"; exit 1; }
  echo "check passed" ;;
ntfs-backup)
  not_mounted; push_tools
  adb shell 'mount | grep -q " /external_sd "' || { echo "card not mounted at /external_sd"; exit 1; }
  adb shell 'df -h /external_sd | tail -1'
  d=/external_sd/starbite-backup/$TAG
  run "mkdir -p $d && /tmp/ntfsclone --save-image --output - $WIN | gzip -1 > $d/WINDOWS.ntfsclone.gz && sync && gzip -t $d/WINDOWS.ntfsclone.gz && ls -l $d/WINDOWS.ntfsclone.gz && sha256sum $d/WINDOWS.ntfsclone.gz"
  echo "restore (only if Windows is broken): gzip -dc $d/WINDOWS.ntfsclone.gz | /tmp/ntfsclone --restore-image --overwrite $WIN -" ;;
ntfs-shrink)
  [ "$(phone_gpt)" = orig ] || { echo "STOP: GPT is not the orig set"; exit 1; }
  not_mounted; push_tools
  adb shell "/tmp/ntfsresize --no-action --size $SIZE $WIN </dev/null" | tail -3
  run "echo y | /tmp/ntfsresize --size $SIZE $WIN"
  run "/tmp/ntfsresize --info --no-action $WIN | grep -i 'volume size'" ;;
gpt)
  st=$(phone_gpt); echo "GPT on the phone: $st"
  [ "$st" = orig ] || { echo "STOP: expected the orig set"; exit 1; }
  [ "$DRY" = --dry-run ] || adb shell "/tmp/ntfsresize --info --no-action $WIN" | grep -i 'current volume size' || true
  [ "$DRY" = --dry-run ] || adb push $G/primary.bin $G/backup.bin /tmp/ >/dev/null
  run "dd if=/tmp/primary.bin of=/dev/block/sda bs=4096 conv=notrunc,fsync && dd if=/tmp/backup.bin of=/dev/block/sda bs=4096 seek=$((BLOCKS - 5)) conv=notrunc,fsync && sync"
  [ "$DRY" = --dry-run ] || { st=$(phone_gpt); echo "read back: $st"; [ "$st" = new ] || { echo "READ-BACK FAILED: run gpt-restore"; exit 1; }; }
  echo "next: adb reboot recovery, then phase2.sh verify" ;;
verify)
  echo "GPT on the phone: $(phone_gpt) (must be new)"
  adb shell 'grep -E "sda2[5-8]$" /proc/partitions'
  echo "expect: sda27 $(( SIZE / 1024 )) KiB, sda28 $(( 4194555 * 4 )) KiB"
  adb shell 'for p in USERDATA WINESP WINDOWS A16DATA; do echo "$p -> $(readlink -f /dev/block/by-name/$p)"; done'
  push_tools; adb shell "/tmp/ntfsfix -n $WIN" | tail -2 ;;
gpt-restore)
  st=$(phone_gpt); echo "GPT on the phone: $st"
  [ "$st" = new ] || [ "$st" = orig ] || { echo "STOP: neither set; restore by hand from the sda-head/tail backup"; exit 1; }
  [ "$DRY" = --dry-run ] || adb push $G/primary-orig.bin $G/backup-orig.bin /tmp/ >/dev/null
  run "dd if=/tmp/primary-orig.bin of=/dev/block/sda bs=4096 conv=notrunc,fsync && dd if=/tmp/backup-orig.bin of=/dev/block/sda bs=4096 seek=$((BLOCKS - 5)) conv=notrunc,fsync && sync"
  [ "$DRY" = --dry-run ] || { st=$(phone_gpt); echo "read back: $st"; [ "$st" = orig ] || { echo "READ-BACK FAILED"; exit 1; }; }
  echo "next: adb reboot recovery; A16DATA is 8 GiB again: part-write.py A16DATA build/android/pixel/a16data-8g.img.gz" ;;
*) sed -n 2,13p "$0"; exit 1 ;;
esac
