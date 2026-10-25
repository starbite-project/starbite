#!/usr/bin/env bash
# W2 UFS read test: put an EFI Shell + startup.nsh (tools/woa/ufstest) on a
# FAT32-ish 4 KiB-sector volume written over CACHE (backed up first), so the
# woa UEFI boots it from the WINDOWS 11 menu entry and logs block reads.
#   tools/woa/ufs-test.sh install     (phone in TWRP)
#   tools/woa/ufs-test.sh results     (phone in TWRP again) -> build/woa/ufstest-results.txt
#   tools/woa/ufs-test.sh restore     put the backed-up CACHE back
set -euo pipefail
cd "$(dirname "$0")/../.."
DEV=/dev/block/by-name/CACHE
bak=build/woa/cache-backup.img
img=build/woa/ufstest-esp.img
adb get-state | grep -q recovery || { echo "phone is not in TWRP"; exit 1; }
case ${1:-} in
install)
    size=$(adb shell "blockdev --getsize64 $DEV" | tr -d '\r')
    [ -f "$bak" ] || { adb exec-out "dd if=$DEV bs=1048576 2>/dev/null" > "$bak"; }
    [ "$(stat -f %z "$bak")" = "$size" ] || { echo "CACHE backup size mismatch"; exit 1; }
    limactl shell starbite -- bash -c "
        set -e
        rm -f '$PWD/$img'; truncate -s $size '$PWD/$img'
        mkfs.fat -S 4096 -s 1 -n STARBITE '$PWD/$img' >/dev/null
        export MTOOLS_SKIP_CHECK=1
        mmd -i '$PWD/$img' ::/EFI ::/EFI/BOOT
        mcopy -i '$PWD/$img' '$PWD/build/woa/Shell-aa64.efi' ::/EFI/BOOT/BOOTAA64.EFI
        mcopy -i '$PWD/$img' '$PWD/tools/woa/ufstest/startup.nsh' ::/EFI/BOOT/startup.nsh
        mcopy -i '$PWD/$img' '$PWD/tools/woa/ufstest/startup.nsh' ::/startup.nsh
        echo starbite | mcopy -i '$PWD/$img' - ::/starbite.mrk
        mdir -i '$PWD/$img' -/ ::/
    "
    adb shell "umount /cache 2>/dev/null; true"
    adb push "$img" /tmp/esp.img >/dev/null
    adb shell "dd if=/tmp/esp.img of=$DEV bs=1048576 conv=fsync 2>/dev/null; sync"
    want=$(shasum -a 256 "$img" | cut -d' ' -f1)
    got=$(adb shell "dd if=$DEV bs=1048576 2>/dev/null | sha256sum" | cut -d' ' -f1)
    [ "$want" = "$got" ] && echo "test ESP on CACHE verified" || { echo "MISMATCH"; exit 1; } ;;
results)
    adb exec-out "dd if=$DEV bs=1048576 2>/dev/null" > build/woa/ufstest-after.img
    limactl shell starbite -- bash -c "MTOOLS_SKIP_CHECK=1 mcopy -o -i '$PWD/build/woa/ufstest-after.img' ::/results.txt '$PWD/build/woa/ufstest-results.txt'"
    ls -l build/woa/ufstest-results.txt ;;
restore)
    adb shell "umount /cache 2>/dev/null; true"
    adb push "$bak" /tmp/cache.img >/dev/null
    adb shell "dd if=/tmp/cache.img of=$DEV bs=1048576 conv=fsync 2>/dev/null; sync"
    [ "$(shasum -a 256 "$bak" | cut -d' ' -f1)" = "$(adb shell "dd if=$DEV bs=1048576 2>/dev/null | sha256sum" | cut -d' ' -f1)" ] \
        && echo "CACHE restored" || { echo "restore MISMATCH"; exit 1; } ;;
*) sed -n 2,9p "$0"; exit 1 ;;
esac
