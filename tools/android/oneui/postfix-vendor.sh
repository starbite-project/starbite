#!/bin/bash
# In the VM, after make_rom: UN1CA's exynos9810_boot_restore patch (build_order 20, after the mods)
# rewrites the vendor fstabs with /data on USERDATA (the iOS slot + Starbite OS store). Fix the finished
# work_dir and rebuild vendor.img with UN1CA's own builder:
#   fstab.samsungexynos9810, fstab.exynos9810: by-name/USERDATA -> by-name/A16DATA
#   vendor_file_contexts: the uppercase by-name label userdata_block_device moves from USERDATA to
#   A16DATA (USERDATA keeps the generic block_device label: no Android domain may open it)
# Output: /mnt/oneui/imgs/vendor.raw (raw EROFS) + checks. Never touches the source tree (the work hash).
set -euo pipefail
cd ~/oneui/un1ca
W=out/target/starlte/work_dir
V=$W/vendor/etc
for f in fstab.samsungexynos9810 fstab.exynos9810; do
    sed -i -E 's#(/dev/block/platform/11120000\.ufs/by-name/)USERDATA([[:space:]])#\1A16DATA\2#g' "$V/$f"
done
sed -i -E 's#^(/dev/block/platform/\.\+/by-name/\(userdata\|)USERDATA(\)\?[[:space:]])#\1A16DATA\2#' "$V/selinux/vendor_file_contexts"
echo "== vendor fstab /data lines"; grep -h -E "[[:space:]]/data[[:space:]]" "$V/fstab.samsungexynos9810" "$V/fstab.exynos9810"
echo "== contexts"; grep -n "by-name/(userdata" "$V/selinux/vendor_file_contexts"
if grep -rIl "by-name/USERDATA" "$W/vendor" "$W/odm" 2>/dev/null | grep -q .; then
    echo "STOP: USERDATA still named in:"; grep -rIl "by-name/USERDATA" "$W/vendor" "$W/odm"; exit 1
fi
grep -q "by-name/(userdata|A16DATA)?" "$V/selinux/vendor_file_contexts" || { echo "STOP: contexts not patched"; exit 1; }
[ "$(grep -c "by-name/A16DATA" "$V/fstab.samsungexynos9810")" -ge 1 ] || { echo "STOP: fstab not patched"; exit 1; }
source buildenv.sh starlte >/dev/null 2>&1
O=/mnt/oneui/imgs; mkdir -p "$O"; rm -f "$O/vendor.fixed.img"
scripts/build_fs_image.sh erofs -o "$O/vendor.fixed.img" -s "$TARGET_VENDOR_PARTITION_SIZE" \
    "$W/vendor" "$W/configs/file_context-vendor" "$W/configs/fs_config-vendor" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tail -4
f="$O/vendor.fixed.img"
if [ "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" = "3aff26ed" ]; then simg2img "$f" "$O/vendor.raw"; rm -f "$f"; else mv "$f" "$O/vendor.raw"; fi
[ "$(od -An -tx1 -j1024 -N4 "$O/vendor.raw" | tr -d ' \n')" = "e2e1f5e0" ] || { echo "STOP: vendor.raw not EROFS"; exit 1; }
rm -rf "$O/vx"; fsck.erofs --extract="$O/vx" "$O/vendor.raw" >/dev/null
grep -h -E "[[:space:]]/data[[:space:]]" "$O/vx/etc/fstab.samsungexynos9810" | cut -c1-60
if grep -rIl "by-name/USERDATA" "$O/vx" | grep -q .; then echo "STOP: image still names USERDATA"; exit 1; fi
rm -rf "$O/vx"
s=$(stat -c %s "$O/vendor.raw")
echo "vendor.raw $s B ($((s / 1048576)) MiB, $((s / 4096)) blocks) sha256 $(sha256sum "$O/vendor.raw" | cut -c1-64): /data on A16DATA, image checked"
