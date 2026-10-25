#!/usr/bin/env bash
# Flash one image to the BOOT partition over Samsung Download mode (Heimdall).
# Never touches any other partition.
#   tools/flash-boot.sh out/probe-boot.img          flash, then reboot
#   NO_REBOOT=1 tools/flash-boot.sh IMG             flash, stay in Download mode
set -euo pipefail
cd "$(dirname "$0")/.."
H=build/heimdall-src/build/bin/heimdall
img=${1:?usage: tools/flash-boot.sh BOOT_IMAGE}
[ -f "$img" ] || { echo "no such file: $img"; exit 1; }
max=$((14080 * 4096))
size=$(stat -f %z "$img")
[ "$size" -le "$max" ] || { echo "$img is $size bytes, BOOT holds $max"; exit 1; }
head -c 8 "$img" | grep -q 'ANDROID!' || { echo "$img is not an Android boot image"; exit 1; }

$H detect
echo "flashing $img ($size bytes, sha256 $(shasum -a 256 "$img" | cut -c1-16)...) to BOOT"
$H flash --BOOT "$img" ${NO_REBOOT:+--no-reboot}
