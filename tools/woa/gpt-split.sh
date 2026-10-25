#!/usr/bin/env bash
# W3: split sda25 USERDATA for Windows (NOTES.md "Windows 11 multi-boot plan").
#   tools/woa/gpt-split.sh gen BACKUPDIR   build the new GPT offline in lima from
#                                          BACKUPDIR/gpt-sda-{primary,backup}.bin
#                                          -> build/woa/gpt-new/ (nothing touches the phone)
#   tools/woa/gpt-split.sh apply           (phone in TWRP) write build/woa/gpt-new to sda,
#                                          read it back, compare
# Layout (4 KiB blocks, new partitions 1 MiB aligned):
#   25 USERDATA  1698432..6679295   19 GiB + 512 KiB; same start, name, GUID: iOS root 0..16 GiB,
#                                   kiosk ext4 +17 GiB (1.15 GiB) stay where they are
#   26 WINESP    6679296..6744831   256 MiB, EFI System type, FAT32 4K sectors (W4)
#   27 WINDOWS   6744832..13518591  25.84 GiB, basic data, NTFS (W4)
#   28 A16DATA   13518592..15615994 8.0 GiB reserve for Android 16 data
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/woa/gpt-new
BLOCKS=15616000   # sda size in 4 KiB blocks (/sys/block/sda/size / 8)

case ${1:-} in
gen)
    src=${2:?backup dir}
    mkdir -p "$out"
    limactl shell starbite -- sudo bash -c "
        set -e
        img=/tmp/sda-gpt.img
        rm -f \$img; truncate -s $((BLOCKS * 4096)) \$img
        dd if='$PWD/$src/gpt-sda-primary.bin' of=\$img bs=4096 conv=notrunc status=none
        dd if='$PWD/$src/gpt-sda-backup.bin' of=\$img bs=4096 seek=$((BLOCKS - 5)) conv=notrunc status=none
        lo=\$(losetup --sector-size 4096 -f --show \$img)
        trap 'losetup -d \$lo' EXIT
        sgdisk -v \$lo | tail -2
        guid=\$(sgdisk -i 25 \$lo | sed -n 's/^Partition unique GUID: //p')
        sgdisk -d 25 \
            -n 25:1698432:6679295   -t 25:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 -c 25:USERDATA -u 25:\$guid \
            -n 26:6679296:6744831   -t 26:C12A7328-F81F-11D2-BA4B-00A0C93EC93B -c 26:WINESP \
            -n 27:6744832:13518591  -t 27:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 -c 27:WINDOWS \
            -n 28:13518592:15615994 -t 28:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 -c 28:A16DATA \
            \$lo >/dev/null
        sgdisk -v \$lo | tail -2
        sgdisk -p \$lo | tail -6
        for i in 25 26 27 28; do sgdisk -i \$i \$lo | grep -E 'unique GUID|name'; done
        dd if=\$lo of='$PWD/$out/primary.bin' bs=4096 count=6 status=none
        dd if=\$lo of='$PWD/$out/backup.bin' bs=4096 skip=$((BLOCKS - 5)) status=none
    "
    # Only the GPT blocks may differ from the backup (LBA 0 protective MBR untouched).
    cmp -s <(head -c 4096 "$src/gpt-sda-primary.bin") <(head -c 4096 "$out/primary.bin") ||
        { echo "protective MBR changed"; exit 1; }
    ls -l "$out" ;;
apply)
    [ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
    [ "$(adb shell cat /sys/block/sda/size | tr -d '\r')" = $((BLOCKS * 8)) ] || { echo "sda size changed"; exit 1; }
    adb push "$out/primary.bin" "$out/backup.bin" /tmp/ >/dev/null
    adb shell "dd if=/tmp/primary.bin of=/dev/block/sda bs=4096 conv=notrunc,fsync &&
               dd if=/tmp/backup.bin of=/dev/block/sda bs=4096 seek=$((BLOCKS - 5)) conv=notrunc,fsync && sync"
    adb exec-out "dd if=/dev/block/sda bs=4096 count=6 2>/dev/null" | cmp - "$out/primary.bin"
    adb exec-out "dd if=/dev/block/sda bs=4096 skip=$((BLOCKS - 5)) count=5 2>/dev/null" | cmp - "$out/backup.bin"
    echo "GPT written and verified; reboot to TWRP so the kernel rereads it" ;;
*) sed -n 2,16p "$0"; exit 1 ;;
esac
