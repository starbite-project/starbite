#!/usr/bin/env bash
# a16data-ext4.sh TAG [--dry-run] : make A16DATA a fresh ext4 for One UI 8's /data, from TWRP, ONLY after
# tools/android/oneui/diag-boot.sh showed A16DATA is not a clean ext4 (no 53ef magic, F2FS, FS errors, or
# e2fsck -n errors), which with the vendor fstab's errors=panic / ext4-or-f2fs /data entries makes One UI loop.
#
# Requires a verified backup of A16DATA made first with
#   tools/android/pixel/part-backup.sh card A16DATA TAG     (microSD exFAT, sha256 recorded in backups.txt)
# and refuses without that record. Then: the by-name node must be layout starbite-oneui v3 (start 12790528,
# 2825467 blocks of 4 KiB) and unmounted; TWRP's own mke2fs (its mke2fs.conf matches the 4.9 kernel) makes
# ext4 of 2825462 blocks (5 short of the partition = the fstab's length=-20480), label "data"; e2fsck -fn
# must then be clean. Writes nothing but A16DATA. One UI then does a first boot (about 2.5 min plus one
# restart), with Samsung's setup: tools/android/oneui/first-boot.sh over adb skips it.
set -euo pipefail
cd "$(dirname "$0")/../../.."
tag=${1:?usage: a16data-ext4.sh TAG [--dry-run]  (TAG = the part-backup.sh card tag)}; dry=${2:-}
BN=/dev/block/platform/11120000.ufs/by-name
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
sh() { adb shell "$1" </dev/null | tr -d '\r'; }
grep -q " A16DATA.img (raw; stored gzip'd, card:$tag)" "${BACKUP_LOG:-build/android/pixel/backups.txt}" \
    || { echo "REFUSED: no verified A16DATA backup with tag $tag in backups.txt (run part-backup.sh card A16DATA $tag)"; exit 1; }
dev=$(sh "readlink -f $BN/A16DATA"); s=${dev##*/}
[ -n "$s" ] && [ "$s" != A16DATA ] || { echo "no A16DATA"; exit 1; }
st=$(( $(sh "cat /sys/class/block/$s/start") / 8 )); bl=$(( $(sh "cat /sys/class/block/$s/size") / 8 ))
[ "$st" = 12790528 ] && [ "$bl" = 2825467 ] || { echo "REFUSED: A16DATA $dev is $st + $bl blocks, not layout v3 (12790528 + 2825467)"; exit 1; }
sh "grep -q '^$dev ' /proc/mounts && echo mounted" | grep -q mounted && { echo "A16DATA is mounted: umount /data in TWRP first"; exit 1; }
mk=$(sh "command -v mke2fs || command -v mkfs.ext4")
[ -n "$mk" ] || { echo "no mke2fs in TWRP"; exit 1; }
cmd="$mk -t ext4 -F -b 4096 -L data $dev 2825462"
echo "A16DATA = $dev ($st + $bl blocks), backup card:$tag recorded"
echo "command: $cmd"
[ "$dry" = --dry-run ] && { echo "dry run: nothing written"; exit 0; }
sh "$cmd 2>&1 | tail -4"
sh "e2fsck -fn $dev 2>&1 | tail -3"
m=$(adb exec-out "dd if=$dev bs=1 skip=1080 count=2 2>/dev/null" </dev/null | xxd -p)
[ "$m" = 53ef ] || { echo "FAILED: no ext4 magic after mke2fs"; exit 1; }
echo "A16DATA is a fresh ext4 (2825462 blocks). Next: tools/store/boot-next.sh android"
