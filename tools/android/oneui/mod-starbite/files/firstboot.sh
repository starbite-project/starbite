#!/system/bin/sh
# Starbite first boot (see /system/etc/init/starbite-firstboot.rc).
if [ "$(settings get global device_provisioned)" = 1 ] && [ "$(settings get secure user_setup_complete)" = 1 ]; then
    exit 0
fi
log -p i -t starbite "first boot: finishing setup without the wizards"
settings put global device_provisioned 1
settings put secure user_setup_complete 1
settings put secure user_setup_personalization_state 10
for p in com.sec.android.app.SecSetupWizard com.google.android.setupwizard com.samsung.android.app.omcagent; do
    pm list packages "$p" | grep -qx "package:$p" && pm disable-user --user 0 "$p" >/dev/null 2>&1
done
am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null 2>&1
echo "setup finished $(date)" > /data/local/tmp/starbite-firstboot 2>/dev/null
