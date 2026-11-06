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
if [ -f "$MODPATH/product/etc/security/adb_keys" ]; then
    LOG "- Starbite: adb on from first boot (pre-authorized key)"
    ADD_TO_WORK_DIR "$MODPATH" "product" "etc/security/adb_keys" 0 0 644 "u:object_r:adb_keys_file:s0"
    SET_PROP "product" "persist.sys.usb.config" "mtp,adb"
fi

# 3. No setup wizard: the first boot goes to the home screen (Google's wizard honours this; Samsung's
#    wizard is completed by tools/android/oneui/first-boot.sh over adb until a native way is proven).
SET_PROP "product" "ro.setupwizard.mode" "DISABLED"
