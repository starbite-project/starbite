SKIPUNZIP=1
# Starbite integration for UN1CA 9810 (docs/ANDROID-ONEUI.md). Applied after UN1CA's own patches.

# 1. /data lives on A16DATA (Starbite's Android data partition). USERDATA holds the iOS 16.5 slot and
#    the Starbite OS store and must never be formatted or mounted by Android.
for f in "$WORK_DIR/vendor/etc/fstab.samsungexynos9810" "$WORK_DIR/vendor/etc/fstab.exynos9810"; do
    [ -f "$f" ] || continue
    LOG "- Starbite: /data on A16DATA in ${f//$WORK_DIR\//}"
    EVAL "sed -i 's|/by-name/USERDATA |/by-name/A16DATA |' \"$f\""
done
if grep -rIl "by-name/USERDATA" "$WORK_DIR/vendor/etc" "$WORK_DIR/odm" 2>/dev/null | grep -q .; then
    LOGE "Starbite: USERDATA is still referenced in vendor/odm:"
    grep -rIl "by-name/USERDATA" "$WORK_DIR/vendor/etc" "$WORK_DIR/odm" >&2
    return 1
fi

# 2. USB debugging from the first boot, accepted only from the Starbite build Mac (the key is copied
#    into product/etc/security/adb_keys by tools/android/oneui/prepare-un1ca.sh; never committed).
#    On the S9 build product lives inside system (system-as-root: /product = system/product, and
#    system/system/product is an absolute link to /product), so the key is added as a "system" file
#    at product/etc/security/adb_keys (the helper's system-as-root branch needs $MODPATH/system/system
#    to exist) and the props go into system's build.prop.
if [ -f "$MODPATH/system/product/etc/security/adb_keys" ]; then
    LOG "- Starbite: adb on from first boot (pre-authorized key)"
    #    (copied directly: ADD_TO_WORK_DIR's system branch records the label under /system/product/...,
    #    the wrong device path; the image builder labels by these config entries)
    EVAL "install -D -m 644 \"$MODPATH/system/product/etc/security/adb_keys\" \"$WORK_DIR/system/product/etc/security/adb_keys\"" || return 1
    grep -q "^/product/etc/security/adb_keys " "$WORK_DIR/configs/file_context-system" ||
        echo "/product/etc/security/adb_keys u:object_r:adb_keys_file:s0" >> "$WORK_DIR/configs/file_context-system"
    grep -q "^product/etc/security/adb_keys " "$WORK_DIR/configs/fs_config-system" ||
        echo "product/etc/security/adb_keys 0 0 644 capabilities=0x0" >> "$WORK_DIR/configs/fs_config-system"
    SET_PROP "system" "persist.sys.usb.config" "mtp,adb"
fi

# 3. No setup wizard: the first boot goes to the home screen (Google's wizard honours this; Samsung's
#    wizard is completed by tools/android/oneui/first-boot.sh over adb until a native way is proven).
SET_PROP "system" "ro.setupwizard.mode" "DISABLED"

# 4. Boot guard: a hang before sys.boot_completed restarts into recovery after ro.starbite.bootguard
#    seconds (files/starbite-bootguard.rc, files/bootguard.sh), so an unattended first boot always returns.
LOG "- Starbite: boot guard (recovery after 1200 s without boot_completed)"
EVAL "install -D -m 644 \"$MODPATH/files/starbite-bootguard.rc\" \"$WORK_DIR/system/system/etc/init/starbite-bootguard.rc\"" || return 1
EVAL "install -D -m 644 \"$MODPATH/files/bootguard.sh\" \"$WORK_DIR/system/system/etc/starbite/bootguard.sh\"" || return 1
for e in "/system/etc/init/starbite-bootguard\\.rc" "/system/etc/starbite" "/system/etc/starbite/bootguard\\.sh"; do
    grep -qF "$e " "$WORK_DIR/configs/file_context-system" ||
        echo "$e u:object_r:system_file:s0" >> "$WORK_DIR/configs/file_context-system"
done
grep -q "^system/etc/init/starbite-bootguard.rc " "$WORK_DIR/configs/fs_config-system" ||
    echo "system/etc/init/starbite-bootguard.rc 0 0 644 capabilities=0x0" >> "$WORK_DIR/configs/fs_config-system"
grep -q "^system/etc/starbite " "$WORK_DIR/configs/fs_config-system" ||
    echo "system/etc/starbite 0 0 755 capabilities=0x0" >> "$WORK_DIR/configs/fs_config-system"
grep -q "^system/etc/starbite/bootguard.sh " "$WORK_DIR/configs/fs_config-system" ||
    echo "system/etc/starbite/bootguard.sh 0 0 644 capabilities=0x0" >> "$WORK_DIR/configs/fs_config-system"
SET_PROP "system" "ro.starbite.bootguard" "1200"
