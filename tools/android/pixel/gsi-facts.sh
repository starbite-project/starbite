#!/bin/bash
# gsi-facts.sh SYSTEM.img : read-only facts about a GSI system image (debugfs, lima). Writes nothing.
set -euo pipefail
S=${1:?system.img}
ls_() { debugfs -R "ls $1" "$S" 2>/dev/null | tr -s ' ' '\n' | grep -v -E '^\.{1,2}$|^$' | sort -u; }
echo "== $S"
dumpe2fs -h "$S" 2>/dev/null | grep -E 'features|Block count|Block size|Inode count|Free blocks|volume name|mount options'
echo "-- top level"; ls_ / | tr '\n' ' '; echo
echo "-- 32-bit userspace: /system/lib entries: $(ls_ /system/lib | wc -l), /system/bin/linker: $(debugfs -R 'stat /system/bin/linker' "$S" 2>/dev/null | grep -c Inode || true)"
echo "-- zygote rcs"; ls_ /system/etc/init | grep -i zygote || true
for i in /system/etc/init/hw/init.zygote64_32.rc /system/etc/init/hw/init.zygote64.rc; do
  printf '%s: ' "$i"; debugfs -R "stat $i" "$S" 2>/dev/null | grep -q Inode && echo present || echo missing
done
echo "-- selinux mapping"; ls_ /system/etc/selinux/mapping | tr '\n' ' '; echo
ls_ /system/etc/selinux | tr '\n' ' '; echo
echo "-- system_ext / product selinux"
ls_ /system/system_ext/etc/selinux | tr '\n' ' '; echo; ls_ /system/system_ext/etc/selinux/mapping | tr '\n' ' '; echo
ls_ /system/product/etc/selinux | tr '\n' ' '; echo; ls_ /system/product/etc/selinux/mapping | tr '\n' ' '; echo
echo "-- vintf compatibility matrices"; ls_ /system/etc/vintf | tr '\n' ' '; echo
echo "-- apexes"; ls_ /system/apex | tr '\n' ' '; echo
echo "-- launcher / setup / google apps"
for d in /system/priv-app /system/app /system/product/priv-app /system/product/app /system/system_ext/priv-app /system/system_ext/app; do
  echo "$d: $(ls_ $d | tr '\n' ' ')"
done
echo "-- product overlays"; ls_ /system/product/overlay | tr '\n' ' '; echo
