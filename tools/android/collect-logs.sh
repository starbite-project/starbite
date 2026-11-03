#!/bin/sh
# Pull every log an Android (LineageOS 24 / GSI) boot attempt leaves behind, from TWRP over adb.
# ODM is mounted -o ro,noload (no journal replay) and unmounted again. CACHE is read where TWRP
# already mounted it (/cache, rw: TWRP itself writes /cache/recovery/log and last_log), else mounted
# ro,noload like ODM. docs/ANDROID-PLAN.md 3.
#   tools/android/collect-logs.sh [TAG]     -> build/android/logs/<date>-<TAG>/
set -eu
tag=${1:-try}
out=build/android/logs/$(date +%Y%m%d-%H%M%S)-$tag
mkdir -p "$out"
adb get-state | grep -q recovery || { echo "phone is not in TWRP (adb get-state)"; exit 1; }
pull() {  # pull REMOTE NAME: raw bytes, empty file when absent
    adb exec-out "cat $1 2>/dev/null" > "$out/$2" || true
    printf '%-28s %8s B\n' "$2" "$(wc -c < "$out/$2" | tr -d ' ')"
}
# previous boot's kernel log (Samsung sec_debug: the exynos-ss kernel ring, kept over a warm reset)
pull /proc/last_kmsg last_kmsg.txt
# pstore: console + pmsg ramoops at 0xFED10000 (read by TWRP's kernel before it reuses the ring)
for f in $(adb shell 'ls /sys/fs/pstore 2>/dev/null' | tr -d '\r'); do pull /sys/fs/pstore/$f "pstore-$f"; done
# S-Boot's own log of this boot (reset reason, boot mode) and the reset/upload reasons
pull /proc/sec_log sec_log.txt
pull /proc/reset_reason reset_reason.txt
pull /proc/store_lastkmsg store_lastkmsg.txt
pull /proc/cmdline twrp-cmdline.txt
# our own boot logs on /metadata (ODM, ext4 "metadata")
adb shell 'mkdir -p /tmp/sbmd && mount -t ext4 -o ro,noload /dev/block/platform/11120000.ufs/by-name/ODM /tmp/sbmd' || true
if adb shell 'test -d /tmp/sbmd/starbite'; then
    adb pull /tmp/sbmd/starbite "$out/metadata-starbite" >/dev/null && echo "metadata/starbite pulled"
fi
adb shell 'ls -la /tmp/sbmd /tmp/sbmd/vold 2>/dev/null' > "$out/metadata-ls.txt" || true
adb shell 'umount /tmp/sbmd' || true
# BCB in MISC (did Android ask recovery to wipe?) and the /cache recovery command
adb exec-out 'dd if=/dev/block/platform/11120000.ufs/by-name/MISC bs=2048 count=1 2>/dev/null' > "$out/misc-bcb.bin" || true
# CACHE (ext4): TWRP mounts it rw at /cache itself (a second ro mount of the same superblock would
# fail with EBUSY), so read it there; only when it is not mounted, mount it ro,noload and unmount.
if adb shell 'grep -q " /cache " /proc/mounts && echo yes' | tr -d '\r' | grep -q yes; then
    echo "CACHE: read from TWRP's /cache mount"
    adb shell 'cat /cache/recovery/command /cache/recovery/last_log 2>/dev/null' > "$out/cache-recovery.txt" || true
else
    adb shell 'mkdir -p /tmp/sbcache && mount -t ext4 -o ro,noload /dev/block/platform/11120000.ufs/by-name/CACHE /tmp/sbcache' ||
        echo "CACHE: ro mount failed (not ext4?)"
    adb shell 'cat /tmp/sbcache/recovery/command /tmp/sbcache/recovery/last_log 2>/dev/null' > "$out/cache-recovery.txt" || true
    adb shell 'umount /tmp/sbcache 2>/dev/null' || true
fi
strings "$out/misc-bcb.bin" | head -5 > "$out/misc-bcb.txt"
# filesystem state of the Android partitions
adb shell 'blkid 2>/dev/null | grep -E "sda1[89]|sda2[018]|mmcblk0"' > "$out/blkid.txt" || true
shasum -a 256 "$out"/* 2>/dev/null > "$out/SHA256SUMS" || true
echo "logs in $out"
grep -h -m 20 -i -E "panic|Kernel panic|init: .*(fatal|reboot)|first_stage|fs_mgr|Failed to mount|switch_root|SELinux" \
    "$out/last_kmsg.txt" "$out"/pstore-console* 2>/dev/null || true
