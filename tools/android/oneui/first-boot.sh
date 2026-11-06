#!/bin/bash
# first-boot.sh [WAIT_S] : finish One UI's first boot without any setup screens (docs/ANDROID-ONEUI.md).
# Runs from the Mac while One UI boots for the first time (adb is on from the first boot and accepts
# only this Mac's key, mod-starbite). It marks the device provisioned and setup complete, disables
# Samsung's and Google's setup wizards for user 0, and brings up the home screen. Safe to re-run.
set -euo pipefail
WAIT=${1:-900}
SW_PKGS="com.sec.android.app.SecSetupWizard com.google.android.setupwizard com.samsung.android.app.omcagent"
echo "waiting for adb (up to ${WAIT}s)..."
timeout "$WAIT" adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 3; done
echo "boot completed; skipping setup"
adb shell settings put global device_provisioned 1
adb shell settings put secure user_setup_complete 1
adb shell settings put secure user_setup_personalization_state 10 2>/dev/null || true
for p in $SW_PKGS; do
    if adb shell pm list packages "$p" | tr -d '\r' | grep -qx "package:$p"; then
        adb shell pm disable-user --user 0 "$p" >/dev/null && echo "  disabled $p"
    fi
done
adb shell am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null
sleep 3
echo "focus: $(adb shell dumpsys window | grep -m1 mCurrentFocus | tr -d '\r')"
echo "provisioned=$(adb shell settings get global device_provisioned | tr -d '\r') setup_complete=$(adb shell settings get secure user_setup_complete | tr -d '\r')"
