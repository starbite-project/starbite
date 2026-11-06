#!/bin/bash
# build-lpix.sh GMS_DIR OUT.img : "Pixel-style LineageOS" (docs/ANDROID-PIXEL.md section 10). Runs in the
# lima VM. Takes LineageOS 24's system.img (~/android/z/system.img.gz, sha256 of the raw image 318f76ad...)
# and Google's own Android 17 GMS apps taken from Google's GSI (GMS_DIR: the GSI's system/ subtree with only
# the paths in GMS_PATHS below, e.g. pulled with
#   adb exec-out 'cd /tmp/gsi/system && tar -cf - <GMS_PATHS>' | tar -xf - -C GMS_DIR
# from a ro mount of the GSI's SYSTEM in TWRP), and writes OUT.img (same size as LineageOS's):
#   - removes LineageOS's duplicate apps (browser, calendar, SMS, dialer, contacts, gallery, music,
#     recorder) and LineageSetupWizard (Google's setup wizard runs instead);
#   - removes LineageOS's boot animation (AOSP's "android" animation is shown);
#   - adds the GMS apps and their permission/sysconfig files, root:root 0755/0644, system_file;
#   - NOT GoogleConfigOverlay (names Google's mainline modules, e.g. com.google.android.ext.services,
#     which LineageOS does not have: system_server dies in PackageManagerService), NOT Google WebView +
#     its overlay (149 on the GSI, not a valid provider here; LineageOS's com.android.webview 156 is used);
#   - DEV=1 (default): USB debugging on from first boot and ONLY this Mac's adb key pre-authorized
#     (product/etc/security/adb_keys, adb_keys_file). DEV=0 leaves adb as LineageOS ships it.
# Result of 2026-11-05 (DEV=1): sha256 74fc930ee6f95a486a557ed81af7c8bec5b14a9e5a7554ebc1e0adaec993d4ba,
# booted to Google's setup wizard in ~30 s, no system_server crash.
set -euo pipefail
GMS=${1:?GMS_DIR}; OUT=${2:?OUT.img}; DEV=${DEV:-1}
SRC=${SRC:-$HOME/android/z/system.img.gz}
ADBKEY=${ADBKEY:-/Users/$USER/.android/adbkey.pub}
[ ! -e "$OUT" ] || { echo "REFUSED: $OUT exists"; exit 1; }
GMS_PATHS="product/priv-app/PrebuiltGmsCore product/priv-app/Phonesky product/priv-app/SetupWizardPrebuilt
 product/priv-app/GoogleOneTimeInitializer product/priv-app/PartnerSetupPrebuilt product/priv-app/ConfigUpdater
 product/priv-app/Velvet product/priv-app/GoogleDialer product/priv-app/PrebuiltBugle product/priv-app/WellbeingPrebuilt
 product/app/GoogleCalendarSyncAdapter product/app/GoogleContacts product/app/Chrome product/app/Chrome-Stub
 product/app/TrichromeLibrary product/app/TrichromeLibrary-Stub product/app/Photos product/app/GoogleTTS
 product/app/LocationHistoryPrebuilt system_ext/priv-app/GoogleServicesFramework system_ext/priv-app/GoogleFeedback
 product/etc/permissions/privapp-permissions-google-p.xml product/etc/permissions/privapp-permissions-google-pearl.xml
 product/etc/permissions/privapp-permissions-google-spoon.xml product/etc/permissions/split-permissions-google.xml
 product/etc/permissions/velvet_app-privapp-permissions.xml product/etc/sysconfig/google.xml
 product/etc/sysconfig/google-hiddenapi-package-whitelist.xml
 product/etc/sysconfig/preinstalled-packages-gms-mandatory-product.xml product/etc/preferred-apps
 system_ext/etc/permissions/privapp-permissions-google-se.xml"
REMOVE="product/app/Jelly product/app/Etar product/app/messaging product/app/Glimpse product/app/Twelve
 product/app/Recorder product/priv-app/Contacts product/priv-app/Dialer system_ext/priv-app/LineageSetupWizard
 product/media/bootanimation.zip product/media/bootanimation-dark.zip"
for p in $GMS_PATHS; do [ -e "$GMS/$p" ] || { echo "missing in GMS_DIR: $p"; exit 1; }; done
gzip -dc "$SRC" > "$OUT"
M=$(mktemp -d); trap 'mountpoint -q "$M" && sudo umount "$M"; rmdir "$M"' EXIT
sudo mount -o loop "$OUT" "$M"; S=$M/system
for p in $REMOVE; do sudo rm -r "$S/$p"; done
for p in $GMS_PATHS; do sudo mkdir -p "$S/$(dirname "$p")"; sudo cp -a --no-preserve=ownership "$GMS/$p" "$S/$(dirname "$p")/"; done
for p in $GMS_PATHS; do
  sudo find "$S/$p" -type d -exec chown root:root {} + -exec chmod 755 {} + -exec setfattr -n security.selinux -v u:object_r:system_file:s0 {} \;
  sudo find "$S/$p" -type f -exec chown root:root {} + -exec chmod 644 {} + -exec setfattr -n security.selinux -v u:object_r:system_file:s0 {} \;
done
if [ "$DEV" = 1 ]; then
  sudo install -d -m 755 "$S/product/etc/security"
  sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$S/product/etc/security"
  sudo install -m 644 "$ADBKEY" "$S/product/etc/security/adb_keys"
  sudo setfattr -n security.selinux -v u:object_r:adb_keys_file:s0 "$S/product/etc/security/adb_keys"
  printf '\n# Starbite (dev): USB debugging on from first boot; only the Starbite Mac key is pre-authorized\npersist.sys.usb.config=adb\n' | sudo tee -a "$S/product/etc/build.prop" >/dev/null
fi
# No setup wizard: the first boot lands on the home screen (verified 2026-11-06: device_provisioned=1,
# user_setup_complete=1); gesture navigation on from the first boot (navigation_mode=2).
printf '\n# Starbite: no setup wizard, first boot goes straight to the home screen\nro.setupwizard.mode=DISABLED\n' | sudo tee -a "$S/product/etc/build.prop" >/dev/null
sudo install -d -m 755 "$S/product/overlay/config"
sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$S/product/overlay/config"
printf '<?xml version="1.0" encoding="utf-8"?>\n<config>\n    <overlay package="com.android.internal.systemui.navbar.gestural" enabled="true" />\n</config>\n' | sudo tee "$S/product/overlay/config/config.xml" >/dev/null
sudo chmod 644 "$S/product/overlay/config/config.xml"
sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$S/product/overlay/config/config.xml"
sync; sudo umount "$M"
/sbin/e2fsck -fn "$OUT" >/dev/null
sha256sum "$OUT"
