#!/bin/bash
# apex-versions.sh SYSTEM.img : name + versionCode of every APEX/CAPEX in a system image (lima, read-only).
# Used to compare the GSI's Google-signed Mainline modules with LineageOS 24's (the 4.9 kernel is proven
# only with Lineage's): a much newer Tethering/Connectivity module may need BPF the 4.9 backport lacks.
set -u
S=${1:?system.img}
T=$(mktemp -d /tmp/apexv.XXXXXX); trap 'rm -rf "$T"' EXIT
for a in $(debugfs -R 'ls /system/apex' "$S" 2>/dev/null | tr -s ' ' '\n' | grep -E '\.c?apex$' | sort -u); do
  debugfs -R "dump /system/apex/$a $T/a" "$S" >/dev/null 2>&1
  # apex_manifest.pb: protobuf, field 1 name (string), field 2 version (varint)
  v=$(unzip -p "$T/a" apex_manifest.pb 2>/dev/null | python3 -I -c '
import sys
d = sys.stdin.buffer.read(); i = 0; name = ver = None
while i < len(d):
    k = d[i]; i += 1; f, t = k >> 3, k & 7
    if t == 2:
        n = 0; s = 0
        while True:
            b = d[i]; i += 1; n |= (b & 0x7f) << s; s += 7
            if b < 0x80: break
        if f == 1: name = d[i:i + n].decode()
        i += n
    elif t == 0:
        n = 0; s = 0
        while True:
            b = d[i]; i += 1; n |= (b & 0x7f) << s; s += 7
            if b < 0x80: break
        if f == 2: ver = n
    else: break
print(name, ver)')
  printf '%-52s %s\n' "$a" "$v"
  rm -f "$T/a"
done
