#!/usr/bin/env bash
# Open the encrypted (.dmg.aea) disk images tools/fetch-ios-parts.sh fetched
# for iOS 18 and later. The key comes from Apple's public key service
# (wkms-public.apple.com, URL in each file's AEA header) through blacktop's
# ipsw tool (build/tools/ipsw, release binary); macOS's aea does the rest.
# The .aea is removed once its .dmg is written (re-fetchable).
#   OUT=build/ipsw-18 tools/decrypt-ios-parts.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=${OUT:-build/ipsw-18}
IPSW_TOOL=${IPSW_TOOL:-build/tools/ipsw/ipsw}
[ -x "$IPSW_TOOL" ] || { echo "missing $IPSW_TOOL (blacktop/ipsw macOS_arm64 release)"; exit 1; }

shopt -s nullglob
for f in "$out"/*.dmg.aea; do
    dmg=${f%.aea}
    [ -f "$dmg" ] && { echo "have $dmg"; continue; }
    key=$("$IPSW_TOOL" fw aea --key "$f" | grep -o 'base64:[A-Za-z0-9+/=]*')
    [ -n "$key" ] || { echo "no key for $f"; exit 1; }
    echo "decrypting $f"
    aea decrypt -i "$f" -o "$dmg.part" -key-value "$key"
    # iOS 18 images decrypt to a raw APFS volume, not UDIF (attach with
    # -imagekey diskimage-class=CRawDiskImage).
    file "$dmg.part" | grep -q 'Apple File System' || { echo "$dmg.part: not APFS"; exit 1; }
    mv "$dmg.part" "$dmg"
    rm "$f"
done
ls -la "$out"
