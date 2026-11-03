#!/system/bin/sh
# Starbite first-boot logging (tools/android/starbite-log.rc). Keeps the last 5 boots in
# /metadata/starbite/boot0..boot4 (boot0 = newest). klog runs first (early-init) and rotates.
# Both services stop at sys.boot_completed; nothing keeps running while Android is up.
base=/metadata/starbite
case "$1" in
klog)
    mkdir -p $base
    rm -rf $base/boot4
    for i in 3 2 1 0; do
        [ -d $base/boot$i ] && mv $base/boot$i $base/boot$((i + 1))
    done
    # small /metadata (64 MiB on the card): drop the oldest boots while less than 32 MiB is free
    for i in 4 3 2 1; do
        free=$(df -k /metadata 2>/dev/null | awk 'NR==2 {print $4}')
        [ -n "$free" ] && [ "$free" -lt 32768 ] && rm -rf $base/boot$i
    done
    mkdir -p $base/boot0
    cat /proc/cmdline > $base/boot0/cmdline.txt 2>&1
    getprop > $base/boot0/props-early.txt 2>&1
    sync
    # follow the kernel log only until sys.boot_completed (synced every 2 s so a hang loses
    # little), and never past 4 MiB or 10 minutes: /metadata also holds vold checkpoints and
    # apexd sessions, and on the card it is a 64 MiB partition, so it must not fill up.
    dmesg -w > $base/boot0/dmesg.txt 2>&1 &
    pid=$!
    t=0
    while [ "$(getprop sys.boot_completed)" != 1 ] && [ $t -lt 300 ]; do
        sz=$(stat -c %s $base/boot0/dmesg.txt 2>/dev/null || echo 0)
        [ "$sz" -ge 4194304 ] && { echo "klog: 4 MiB cap reached" > $base/boot0/KLOG_CAPPED; break; }
        sleep 2
        sync
        t=$((t + 1))
    done
    kill $pid 2>/dev/null
    sync
    ;;
logcat)
    d=$base/boot0
    mkdir -p $d
    # at most 3 x 2 MiB of logcat per boot, and only until 60 s after sys.boot_completed
    logcat -b all -v threadtime -r 2048 -n 2 -f $d/logcat.txt &
    pid=$!
    while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 5; getprop > $d/props.txt; sync; done
    getprop > $d/props.txt
    echo boot_completed > $d/DONE
    sleep 60
    kill $pid 2>/dev/null
    sync
    ;;
esac
