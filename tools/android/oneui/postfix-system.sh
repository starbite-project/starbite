#!/bin/bash
# In the VM: apply mod-starbite's system changes to a FINISHED UN1CA work_dir (the mod is idempotent: it only
# adds what is missing, e.g. a section added after the build) and rebuild system.img with UN1CA's builder:
# /mnt/oneui/imgs/system.raw (raw EROFS) + SHA-256. Never edits the source tree (make_rom's work hash).
#   limactl shell starbite -- bash /Users/dev/project-starbite/tools/android/oneui/postfix-system.sh
set -euo pipefail
cd ~/oneui/un1ca
W=$PWD/out/target/starlte/work_dir
M=$(cd "$(dirname "$0")" && pwd)/mod-starbite
[ -f "$M/system/product/etc/security/adb_keys" ] || M=$PWD/unica/mods/starbite      # the copy with the adb key
LOG() { echo "  $*"; }
LOGE() { echo "ERROR $*" >&2; }
EVAL() { eval "$1"; }
SET_PROP() { :; }                                     # props are already in the built images
( WORK_DIR=$W MODPATH=$M; source "$M/customize.sh" ) || { echo "STOP: the mod failed"; exit 1; }
grep -q "starbite-firstboot" "$W/configs/fs_config-system" || { echo "STOP: firstboot not in the configs"; exit 1; }
set +eu; source buildenv.sh starlte >/dev/null 2>&1; set -eu
O=/mnt/oneui/imgs; mkdir -p "$O"; rm -f "$O/system.fixed.img"
scripts/build_fs_image.sh erofs -o "$O/system.fixed.img" -s "$TARGET_SYSTEM_PARTITION_SIZE" \
    "$W/system" "$W/configs/file_context-system" "$W/configs/fs_config-system" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tail -3
f="$O/system.fixed.img"
if [ "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" = "3aff26ed" ]; then simg2img "$f" "$O/system.raw"; rm -f "$f"; else mv "$f" "$O/system.raw"; fi
[ "$(od -An -tx1 -j1024 -N4 "$O/system.raw" | tr -d ' \n')" = "e2e1f5e0" ] || { echo "STOP: system.raw not EROFS"; exit 1; }
dump.erofs --path=/system/etc/init/starbite-firstboot.rc "$O/system.raw" >/dev/null 2>&1 ||
    { echo "STOP: the firstboot service is not in system.raw"; exit 1; }
s=$(stat -c %s "$O/system.raw")
echo "system.raw $s B ($((s / 1048576)) MiB) sha256 $(sha256sum "$O/system.raw" | cut -c1-64)"
