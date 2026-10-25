#!/usr/bin/env bash
# Read-only backup of the S9 (starlte) partitions that cannot be regenerated.
# Run with the phone in TWRP and connected over USB. Nothing is written to the phone.
#   tools/backup-starlte.sh            -> backup/<timestamp>/
set -euo pipefail

# Exynos Samsung names (not Qualcomm's persist/modemst*). Missing ones are skipped.
PARTS=(EFS SEC_EFS CPEFS PARAM UP_PARAM BOOT RECOVERY DTB DTBO RADIO CM KEYSTORAGE PERSISTENT STEADY OMR)

out="backup/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"

state=$(adb get-state 2>/dev/null || true)
[ "$state" = recovery ] || { echo "phone not in recovery (adb state: '${state:-none}')"; exit 1; }

{
  echo "# device identity"
  for p in ro.product.device ro.product.model ro.build.product ro.boot.bootloader ro.bootloader ro.twrp.version; do
    echo "$p=$(adb shell getprop $p | tr -d '\r')"
  done
} | tee "$out/device.txt"

adb shell ls -l /dev/block/by-name/ | tr -d '\r' > "$out/by-name.txt"
adb shell cat /proc/partitions | tr -d '\r' > "$out/partitions.txt"
echo "partition list saved to $out/by-name.txt"

for p in "${PARTS[@]}"; do
  if ! adb shell "[ -e /dev/block/by-name/$p ]"; then
    echo "skip $p (not present)"; continue
  fi
  echo -n "pull $p ... "
  adb exec-out "dd if=/dev/block/by-name/$p bs=1M 2>/dev/null" > "$out/$p.img"
  here=$(shasum -a 256 "$out/$p.img" | cut -d' ' -f1)
  there=$(adb shell "sha256sum /dev/block/by-name/$p" | cut -d' ' -f1)
  if [ "$here" = "$there" ]; then echo "ok ($(du -h "$out/$p.img" | cut -f1))"
  else echo "CHECKSUM MISMATCH (local $here, phone $there)"; fi
  echo "$here  $p.img" >> "$out/SHA256SUMS"
done

echo "done: $out"
