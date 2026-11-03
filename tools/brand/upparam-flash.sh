#!/usr/bin/env bash
# Write an UP_PARAM image (S-Boot's splash tar) through TWRP's adb, then read it back.
# Refuses unless the phone's current UP_PARAM is one we know (stock VG6, Powered by starbite, the
# native Knox-free one, or Starbite's "One mark"), so a restore from
# backup/20261102-185306/UP_PARAM.img is always possible.
#   tools/brand/upparam-flash.sh [build/brand/upparam-native/up_param.img]
#   tools/brand/upparam-flash.sh build/brand/upparam-onemark/up_param.img   (One mark: flash it in the
#       same session as a loader with the One mark intro, docs/concepts/splash/HANDOFF.md)
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-build/brand/upparam-native/up_param.img}
known="47e56280a5ff888d eac5844ad0988dfd 49788acac4692f46 5bb50d61257b303a"   # stock, starbite, native, onemark (sha256 prefix)
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
n=$(stat -f %z "$img")
[ "$n" = 8388608 ] || { echo "$img is $n bytes, not 8 MiB"; exit 1; }
dev=$(adb shell readlink -f /dev/block/by-name/UP_PARAM | tr -d '\r')
[ "$dev" = /dev/block/sda5 ] || { echo "UP_PARAM is $dev, expected /dev/block/sda5"; exit 1; }
cur=$(adb shell "sha256sum $dev" | cut -c1-16)
case " $known " in *" $cur "*) ;; *) echo "UP_PARAM is $cur, not a known image: refusing"; exit 1 ;; esac
loc=$(shasum -a 256 "$img" | cut -c1-64)
adb push "$img" /tmp/up_param.img >/dev/null
[ "$(adb shell sha256sum /tmp/up_param.img | cut -c1-64)" = "$loc" ] || { echo "push corrupted"; exit 1; }
adb shell "dd if=/tmp/up_param.img of=$dev bs=1048576 conv=fsync 2>/dev/null; sync"
back=$(adb shell "sha256sum $dev" | cut -c1-64)
[ "$back" = "$loc" ] || { echo "UP_PARAM readback mismatch: $back (restore: $0 backup/20261102-185306/UP_PARAM.img)"; exit 1; }
echo "UP_PARAM verified (${loc:0:16}), was ${cur}"
