#!/system/bin/sh
# Starbite first-boot logging (tools/android/starbite-log.rc). Keeps the last 5 boots in
# /metadata/starbite/boot0..boot4 (boot0 = newest). klog runs first (early-init) and rotates.
base=/metadata/starbite
case "$1" in
klog)
    mkdir -p $base
    rm -rf $base/boot4
    for i in 3 2 1 0; do
        [ -d $base/boot$i ] && mv $base/boot$i $base/boot$((i + 1))
    done
    mkdir -p $base/boot0
    cat /proc/cmdline > $base/boot0/cmdline.txt 2>&1
    getprop > $base/boot0/props-early.txt 2>&1
    sync
    # follow the kernel log for the whole boot (synced every 2 s so a hang loses little)
    dmesg -w > $base/boot0/dmesg.txt 2>&1 &
    while true; do sleep 2; sync; done
    ;;
logcat)
    d=$base/boot0
    mkdir -p $d
    logcat -b all -v threadtime -r 4096 -n 4 -f $d/logcat.txt &
    while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 5; getprop > $d/props.txt; sync; done
    getprop > $d/props.txt
    echo boot_completed > $d/DONE
    sync
    ;;
esac
