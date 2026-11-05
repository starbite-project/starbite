#!/bin/bash
# hal-compat.sh SYSTEM.img VENDOR.img : does the GSI's framework still speak the legacy HIDL HALs this
# vendor ships? Greps the client libraries for the interface names (lima, read-only).
set -u
S=${1:?system.img}; V=${2:?vendor.img}
T=$(mktemp -d /tmp/halcompat.XXXXXX); trap 'rm -rf "$T"' EXIT
has() {  # has IMG PATH PATTERN...
  local img=$1 p=$2; shift 2
  debugfs -R "dump $p $T/x" "$img" >/dev/null 2>&1 || true
  if [ ! -s "$T/x" ]; then echo "  $p: MISSING"; return; fi
  for pat in "$@"; do printf '  %-52s %-44s %s\n' "$p" "$pat" "$(grep -c -a -F "$pat" "$T/x" | sed 's/^0$/no/;s/^[1-9].*/yes/')"; done
  rm -f "$T/x"
}
echo "-- vendor HIDL HALs (manifest)"
debugfs -R 'cat /etc/vintf/manifest.xml' "$V" 2>/dev/null | tr -d ' \n' | grep -o '<hal format="hidl"[^/]*<name>[^<]*</name>' | sed 's/.*<name>//;s/<.*//' | sort -u | tr '\n' ' '; echo
debugfs -R 'cat /etc/vintf/manifest.xml' "$V" 2>/dev/null | grep -o -E 'android\.hardware\.[a-z.]+@[0-9.]+::I[A-Za-z]+/[a-z0-9_]+' | sort -u | tr '\n' ' '; echo
echo "-- framework clients in the GSI"
has "$S" /system/lib64/libkeymaster4support.so 'android.hardware.keymaster@3.0' 'android.hardware.keymaster@4.0'
has "$S" /system/lib64/libkm_compat.so 'android.hardware.keymaster@3.0' 'android.hardware.keymaster@4.1'
has "$S" /system/lib64/libui.so 'android.hardware.graphics.mapper@2.0' 'android.hardware.graphics.mapper@3.0' 'android.hardware.graphics.mapper@4.0' 'android.hardware.graphics.allocator@2.0'
has "$S" /system/lib64/libgatekeeper_aidl.so 'android.hardware.gatekeeper@1.0'
has "$S" /system/bin/gatekeeperd 'android.hardware.gatekeeper@1.0'
has "$S" /system/lib64/libsensorservice.so 'android.hardware.sensors@1.0' 'android.hardware.sensors@2.1'
has "$S" /system/lib64/libaudiohal@7.1.so 'android.hardware.audio@7.1'
has "$S" /system/lib64/libmedia_omx.so 'android.hardware.media.omx@1.0'
has "$S" /system/lib64/libstagefright_omx.so 'android.hardware.media.omx@1.0'
has "$S" /system/lib64/libbluetooth-binder.so 'android.hardware.bluetooth@1.0'
echo "-- audio HAL libs present in the GSI"
debugfs -R 'ls /system/lib64' "$S" 2>/dev/null | tr -s ' ' '\n' | grep -E '^libaudiohal' | sort -u | tr '\n' ' '; echo
echo "-- vendor mapper/allocator impls"
debugfs -R 'ls /lib64/hw' "$V" 2>/dev/null | tr -s ' ' '\n' | grep -E 'mapper|allocator|gralloc|keymaster|gatekeeper|audio' | sort -u | tr '\n' ' '; echo
