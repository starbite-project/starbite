#!/usr/bin/env bash
# Build the iOS 16 root volume for the S9's USERDATA from the pieces
# tools/fetch-ios-parts.sh downloaded, the same way the herolte ios-root
# workflow does on macOS: case-sensitive journaled HFS+ (HFSX), staged by
# a5xelte-stage-ios-root.sh, tuned by herolte-tune-ios-root.sh.
# Must run as root (iOS file ownership is part of the boot contract):
#   sudo tools/build-ios-root.sh
#   sudo IPSW=build/ipsw-18 OUT=build/root-ipados18.img INSTALL_STAGED_APPS=1 tools/build-ios-root.sh
#     iPadOS 18.7.11 (J171AP) from tools/fetch-ios-parts.sh + decrypt-ios-parts.sh;
#     staged with tools/stage-ios-root.sh, stock apps installed into /Applications.
#   sudo IPSW=build/ipsw-26-n104 OUT=build/root-ios26.img TRIM_IOS26=1 INSTALL_STAGED_APPS="MobileSafari ..." TRANSLATE_A64E=1 tools/build-ios-root.sh
#     iOS 26.6.2 (iPhone 11, N104AP); arm64e cache translated for the S9 (needs numpy in build/venv).
# Output: build/root.img (raw HFSX, flashed to USERDATA with Heimdall) and
# ref/ios-herolte/research/ipsw-x/d22ap.adt (for the ADT /product node; the
# iPad identity uses build/ipsw-18/j171ap.adt, mkadt_starlte.py --ipad7).
# Apple software: keep every output local, never in a repository.
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root: sudo $0" >&2; exit 2; }
cd "$(dirname "$0")/.."
ROOT=$PWD
IPSW=$ROOT/${IPSW:-build/ipsw}
OUT=$ROOT/${OUT:-build/root.img}
TOOLS=$ROOT/ref/ios-herolte/tools
SIZE_MB=${SIZE_MB:-16384}
owner=$(stat -f %u "$ROOT")

for f in os.dmg systemos.dmg appos.dmg devicetree.im4p BuildManifest.plist; do
    [[ -f $IPSW/$f ]] || { echo "missing $IPSW/$f (run tools/fetch-ios-parts.sh)" >&2; exit 1; }
done

# D22AP device tree for mkadt's /product node (MobileGestalt identity).
adt_dir=$ROOT/ref/ios-herolte/research/ipsw-x
if [[ $IPSW == $ROOT/build/ipsw && ! -f $adt_dir/d22ap.adt ]]; then
    [[ -x $ROOT/build/venv/bin/pyimg4 ]] || {
        python3 -m venv "$ROOT/build/venv"
        "$ROOT/build/venv/bin/python" -m pip install --quiet pyimg4
    }
    mkdir -p "$adt_dir"
    "$ROOT/build/venv/bin/pyimg4" im4p extract -i "$IPSW/devicetree.im4p" -o "$adt_dir/d22ap.adt"
    chown -R "$owner" "$adt_dir" "$ROOT/build/venv"
fi

attach() { hdiutil attach -nobrowse "$@" | awk -F'\t' '/\/Volumes\// {print $NF}' | tail -1; }
# iOS 18's decrypted AEA images are raw APFS volumes, not UDIF.
raw() { file "$1" | grep -q 'Apple File System' && echo "-imagekey diskimage-class=CRawDiskImage" || true; }
os=$(attach -readonly -owners on $(raw "$IPSW/os.dmg") "$IPSW/os.dmg")
sys=$(attach -readonly -owners on $(raw "$IPSW/systemos.dmg") "$IPSW/systemos.dmg")
app=$(attach -readonly -owners on $(raw "$IPSW/appos.dmg") "$IPSW/appos.dmg")
echo "OS=$os SystemOS=$sys AppOS=$app"
rm -f "$ROOT/build/root.sparseimage"
hdiutil create -size "${SIZE_MB}m" -type SPARSE -layout NONE \
    -fs "Case-sensitive Journaled HFS+" -volname IOSROOT "$ROOT/build/root"
root=$(attach -owners on "$ROOT/build/root.sparseimage")
echo "root=$root"
STAGE_ALL_APPS=1 INSTALL_STAGED_APPS=${INSTALL_STAGED_APPS:-0} \
    bash "$ROOT/tools/stage-ios-root.sh" "$os" "$sys" "$app" "$root"
build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$IPSW/BuildManifest.plist")
bash "$TOOLS/herolte-tune-ios-root.sh" "$root" "$build"
# iOS 26 (arm64e only): rewrite the PAC instructions of the shared cache in place
# (tools/a64e/a64e-rewrite.py; the /System/Library/Caches copy is hard-linked to the
# cryptex's, one rewrite covers both). The kernel emulates what is left.
if [[ ${TRANSLATE_A64E:-0} == 1 ]]; then
    "$ROOT/build/venv/bin/python" -I "$ROOT/tools/a64e/a64e-rewrite.py" cache \
        "$root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e" \
        --skip libcorecrypto    # cc_fips_test MACs corecrypto's own code
fi
df -h "$root"
hdiutil detach "$root"; hdiutil detach "$os"; hdiutil detach "$sys"; hdiutil detach "$app"
# DELETE_SOURCES=1: drop the decrypted OS/SystemOS images before the raw copy when the
# disk cannot hold both (they come back with fetch-ios-parts.sh + decrypt-ios-parts.sh).
if [[ ${DELETE_SOURCES:-0} == 1 ]]; then
    rm -f "$IPSW/os.dmg" "$IPSW/systemos.dmg"
fi

# Raw volume image (no partition map: the HFSX volume starts at byte 0 of
# USERDATA, as ufsroot0 expects).
dev=$(hdiutil attach -nomount -nobrowse "$ROOT/build/root.sparseimage" | awk 'NR==1 {print $1}')
dd if="/dev/r${dev#/dev/}" of="$OUT" bs=4m
hdiutil detach "$dev"
rm -f "$ROOT/build/root.sparseimage"
chown "$owner" "$OUT"
ls -l "$OUT"
echo "iOS root for build $build ready: $OUT"
