#!/system/bin/sh
# Starbite boot guard (see /system/etc/init/starbite-bootguard.rc).
limit=$(getprop ro.starbite.bootguard)
case "$limit" in ''|*[!0-9]*) limit=1200 ;; esac
[ "$limit" -gt 0 ] || exit 0
t=0
while [ "$t" -lt "$limit" ]; do
    if [ "$(getprop sys.boot_completed)" = 1 ]; then
        echo "booted after ${t}s" > /data/local/tmp/starbite-boot-ok 2>/dev/null
        exit 0
    fi
    sleep 10
    t=$((t + 10))
done
log -p e -t starbite "boot guard: not booted after ${limit}s, restarting into recovery"
logcat -d -b all > /data/local/tmp/starbite-bootguard.log 2>&1
getprop > /data/local/tmp/starbite-bootguard.props 2>&1
sync
reboot recovery || setprop sys.powerctl reboot,recovery
