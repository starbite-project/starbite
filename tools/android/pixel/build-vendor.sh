#!/bin/bash
# build-vendor.sh IN_VENDOR.img OUT_VENDOR.img [--dry-run]
# Make the VENDOR image for the Google GSI from the phone's current VENDOR (LineageOS 24's, already
# patched for Starbite: /data = A16DATA). Lima (debugfs), works on a copy; IN is never written.
# Changes, each idempotent (running it on its own output changes nothing more):
#   1. /etc/fstab.samsungexynos9810: the microSD line (11500000.dwmmc2 voldmanaged) is commented out,
#      so the GSI's vold never offers to "format" the Starbite card (it holds the other OSes).
#   2. /etc/init/starbite-log.rc + /bin/starbite-log.sh: the boot logger (GSI variant, no su domain).
#   3. /etc/selinux/vendor_sepolicy.cil += tools/android/pixel/starbite-log.cil (between markers).
#   4. /etc/selinux/vendor_file_contexts += the logger script's label.
# Each touched file keeps (or gets) owner, mode and SELinux label explicitly: a file without its
# security.selinux xattr breaks second-stage init under enforcing (NOTES.md 2026-10-27).
# --dry-run: shows the diffs of 1, 3 and 4 and the free space, writes nothing.
set -euo pipefail
IN=${1:?in vendor.img}; OUT=${2:?out vendor.img}; DRY=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /tmp/bvendor.XXXXXX); trap 'rm -rf "$T"' EXIT
FSTAB=/etc/fstab.samsungexynos9810
CIL=/etc/selinux/vendor_sepolicy.cil
FC=/etc/selinux/vendor_file_contexts
dump() { debugfs -R "dump $2 $3" "$1" >/dev/null 2>&1; [ -s "$3" ] || { echo "missing $2 in $1"; exit 1; }; }
grep -q 'by-name/A16DATA /data f2fs' <(debugfs -R "cat $FSTAB" "$IN" 2>/dev/null) ||
  { echo "REFUSED: $IN has no /data = A16DATA fstab line (not Starbite's VENDOR)"; exit 1; }
[ "$(debugfs -R 'cat /etc/selinux/plat_sepolicy_vers.txt' "$IN" 2>/dev/null | tr -d '[:space:]')" = 202604 ] ||
  { echo "REFUSED: vendor sepolicy version is not 202604"; exit 1; }
dump "$IN" $FSTAB "$T/fstab.old"; dump "$IN" $CIL "$T/cil.old"; dump "$IN" $FC "$T/fc.old"
debugfs -R "ea_get -f $T/ctx.cfg $FSTAB security.selinux" "$IN" >/dev/null 2>&1
debugfs -R "ea_get -f $T/ctx.cil $CIL security.selinux" "$IN" >/dev/null 2>&1
debugfs -R "ea_get -f $T/ctx.fc $FC security.selinux" "$IN" >/dev/null 2>&1
for c in cfg cil fc; do [ -s "$T/ctx.$c" ] || { echo "REFUSED: no SELinux label on a source file ($c)"; exit 1; }; done
printf 'u:object_r:vendor_configs_file:s0\0' > "$T/ctx.rc"
printf 'u:object_r:starbite_log_exec:s0\0' > "$T/ctx.sh"
# 1
sed -E 's|^(/devices/platform/11500000\.dwmmc2/.*voldmanaged=sdcard.*)$|#\1|' "$T/fstab.old" > "$T/fstab.new"
# 3
if grep -q '^;; starbite-log.cil begin' "$T/cil.old"; then
  sed '/^;; starbite-log.cil begin/,/^;; starbite-log.cil end/d' "$T/cil.old" > "$T/cil.base"
else cp "$T/cil.old" "$T/cil.base"; fi
{ cat "$T/cil.base"; echo ';; starbite-log.cil begin'; grep -v '^;' "$HERE/starbite-log.cil"; echo ';; starbite-log.cil end'; } > "$T/cil.new"
# 4
grep -v 'starbite-log' "$T/fc.old" > "$T/fc.new" || true
echo '/(vendor|system/vendor)/bin/starbite-log\.sh u:object_r:starbite_log_exec:s0' >> "$T/fc.new"
echo "== $FSTAB"; diff -u "$T/fstab.old" "$T/fstab.new" || true
echo "== $CIL (tail)"; diff -u "$T/cil.old" "$T/cil.new" | tail -25 || true
echo "== $FC"; diff -u "$T/fc.old" "$T/fc.new" || true
dumpe2fs -h "$IN" 2>/dev/null | grep -E 'Free blocks|Block count'
if [ "$DRY" = --dry-run ]; then echo "dry run: $OUT not written"; exit 0; fi
[ "$OUT" != "$IN" ] || { echo "REFUSED: OUT = IN"; exit 1; }
cp --sparse=always "$IN" "$OUT"
cp "$HERE/starbite-log-gsi.rc" "$T/rc.new"; cp "$HERE/../starbite-log.sh" "$T/sh.new"
put() {  # put LOCAL VENDOR_PATH MODE UID GID CTXFILE
  echo "rm $2"
  echo "write $1 $2"
  echo "sif $2 mode 0$3"
  echo "sif $2 uid $4"
  echo "sif $2 gid $5"
  echo "ea_set -f $6 $2 security.selinux"
}
{
  put "$T/fstab.new" $FSTAB 100644 0 0 "$T/ctx.cfg"
  put "$T/cil.new" $CIL 100644 0 0 "$T/ctx.cil"
  put "$T/fc.new" $FC 100644 0 0 "$T/ctx.fc"
  put "$T/rc.new" /etc/init/starbite-log.rc 100644 0 0 "$T/ctx.rc"
  put "$T/sh.new" /bin/starbite-log.sh 100755 0 2000 "$T/ctx.sh"
} > "$T/cmds"
debugfs -w -f "$T/cmds" "$OUT" 2>&1 | grep -v -E '^debugfs|^rm: File not found|^$' || true
# verify: contents, modes, labels, filesystem
for pair in "fstab.new:$FSTAB" "cil.new:$CIL" "fc.new:$FC" "rc.new:/etc/init/starbite-log.rc" "sh.new:/bin/starbite-log.sh"; do
  f=${pair%%:*}; p=${pair#*:}
  debugfs -R "dump $p $T/back" "$OUT" >/dev/null 2>&1
  cmp -s "$T/$f" "$T/back" || { echo "VERIFY FAILED: $p content"; exit 1; }
  printf '%-34s %s  %s\n' "$p" "$(debugfs -R "stat $p" "$OUT" 2>/dev/null | sed -n 's/.*Mode: *\([0-7]*\).*/\1/p;s/^User: *\([0-9]*\) *Group: *\([0-9]*\).*/uid \1 gid \2/p' | tr '\n' ' ')" \
    "$(debugfs -R "ea_get $p security.selinux" "$OUT" 2>/dev/null | tail -1)"
done
e2fsck -fn "$OUT" >/dev/null 2>&1 && echo "e2fsck -fn: clean" || { echo "VERIFY FAILED: e2fsck"; exit 1; }
echo "$OUT sha256 $(sha256sum "$OUT" | cut -d' ' -f1)"
