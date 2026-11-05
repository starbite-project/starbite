#!/bin/bash
# preflight.sh : read-only checkpoint before any write of docs/ANDROID-PIXEL.md (phone in TWRP). Writes nothing
# to the phone. Prints PASS/WARN/STOP lines; exit 1 on any STOP.
#  - TWRP + device identity, sda size, by-name -> sdaN for every partition this plan reads or writes
#  - which of SYSTEM / VENDOR / ODM / A16DATA / WINDOWS are mounted in TWRP (must not be, for writes)
#  - SYSTEM's first 4561305600 B == LineageOS 24's system.img (318f76ad...) - the rollback image
#  - VENDOR sha256 (0d67327a... = the Oct 27 patched image; anything else = pull it fresh, it changed)
#  - the card mounted at /external_sd with its free space; the Mac's free space (>= 4 GB margin)
set -u
stop=0
say() { printf '%-5s %s\n' "$1" "$2"; [ "$1" = STOP ] && stop=1; return 0; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { say STOP "phone not in TWRP"; exit 1; }
say PASS "TWRP $(adb shell getprop ro.twrp.version | tr -d '\r'), $(adb shell getprop ro.boot.bootloader | tr -d '\r')"
[ "$(adb shell getprop ro.product.device | tr -d '\r')" = starlte ] && say PASS "device starlte" || say STOP "not starlte"
[ "$(adb shell cat /sys/block/sda/size | tr -d '\r')" = 124928000 ] && say PASS "sda 15616000 x 4 KiB" || say STOP "sda size changed"
for p in SYSTEM:sda18 VENDOR:sda19 ODM:sda20 CACHE:sda21 USERDATA:sda25 WINESP:sda26 WINDOWS:sda27 A16DATA:sda28 EFS:sda3 PERSISTENT:sda14 STEADY:sda16 MISC:sda15; do
  r=$(adb shell readlink -f /dev/block/by-name/${p%%:*} | tr -d '\r')
  [ "$r" = /dev/block/${p##*:} ] && say PASS "${p%%:*} -> $r" || say STOP "${p%%:*} -> $r (expected ${p##*:}): re-read the GPT"
done
for n in 18 19 20 27 28; do
  m=$(adb shell "grep '^/dev/block/sda$n ' /proc/mounts" | tr -d '\r')
  [ -z "$m" ] && say PASS "sda$n not mounted" || say WARN "sda$n mounted: $m (umount before writing it)"
done
s=$(adb shell 'head -c 4561305600 /dev/block/by-name/SYSTEM | sha256sum' | tr -d '\r' | cut -d' ' -f1)
case $s in
  318f76ad43beeef43c234f3dba18c2ccd73b944062e1c353767e94ecdf19ac20) say PASS "SYSTEM = LineageOS 24 system.img (rollback image matches)";;
  bddf5ad5a664e2f5d90d6e8cbe630ddbee3fd41ddb0711ebb5a4125c7e32d71c) say PASS "SYSTEM already = the GSI";;
  *) say WARN "SYSTEM sha256 $s: neither LineageOS 24 nor the GSI; back it up whole (part-backup.sh card SYSTEM) before writing";;
esac
v=$(adb shell 'sha256sum /dev/block/by-name/VENDOR' | tr -d '\r' | cut -d' ' -f1)
case $v in
  0d67327abd842b81ea8e92b62467a262ef4baffdd00d999a135c03d829e3cbe6) say PASS "VENDOR = the 2026-10-27 patched image";;
  *) say WARN "VENDOR sha256 $v (changed since 2026-10-27, e.g. ANDROID-PLAN P1.5): build-vendor.sh must start from a fresh pull";;
esac
if adb shell 'mount | grep -q " /external_sd "'; then
  say PASS "card: $(adb shell 'df -h /external_sd | tail -1' | tr -d '\r')"
else say WARN "card not mounted at /external_sd (needed for the A16DATA / WINDOWS backups)"; fi
free=$(df -k . | awk 'NR==2 {print $4}')
[ "$free" -ge $((4 * 1024 * 1024)) ] && say PASS "Mac free $((free >> 20)) GiB" || say STOP "Mac free $((free >> 20)) GiB < 4 GiB margin"
[ $stop = 0 ] && echo "preflight: no STOP" || { echo "preflight: STOP above"; exit 1; }
