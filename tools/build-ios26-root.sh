#!/usr/bin/env bash
# iOS 26.6.2 (iPhone 11, N104AP) root for the S9, from build/ipsw-26-n104
# (tools/fetch-ios-parts.sh + decrypt-ios-parts.sh): trimmed to fit 16 GiB,
# a few stock apps, the arm64e shared cache translated for the S9's cores.
# Run as root:  sudo tools/build-ios26-root.sh   -> build/root-ios26.img
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root: sudo $0" >&2; exit 2; }
cd "$(dirname "$0")/.."
# Mounts left behind by an earlier, interrupted build.
for v in IOSROOT LuckG23G90.N104OS LuckG23G90.N104SystemCryptex AppCryptex; do
    [[ -d "/Volumes/$v" ]] && hdiutil detach -force "/Volumes/$v" >/dev/null 2>&1 || true
done
export IPSW=build/ipsw-26-n104 OUT=build/root-ios26.img
export TRIM_IOS26=1 TRANSLATE_A64E=1 DELETE_SOURCES=1
export INSTALL_STAGED_APPS="MobileSafari Calculator MobileNotes Photos Camera Files MobileTimer Weather MobileCal Contacts Reminders"
exec tools/build-ios-root.sh
