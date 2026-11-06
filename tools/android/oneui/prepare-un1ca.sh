#!/bin/bash
# prepare-un1ca.sh UN1CA_DIR : install the Starbite mod into a UN1CA 9810 checkout (in the VM).
set -euo pipefail
U=${1:?UN1CA checkout}; M=$(dirname "$0")/mod-starbite
rm -rf "$U/unica/mods/starbite"; cp -a "$M" "$U/unica/mods/starbite"
K=${ADBKEY:-/Users/$USER/.android/adbkey.pub}
if [ -f "$K" ]; then
    mkdir -p "$U/unica/mods/starbite/product/etc/security"
    cut -d' ' -f1 "$K" > "$U/unica/mods/starbite/product/etc/security/adb_keys"   # key only, no user@host
fi
ls -la "$U/unica/mods/starbite" "$U/unica/mods/starbite/product/etc/security" 2>/dev/null
