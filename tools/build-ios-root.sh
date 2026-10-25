#!/usr/bin/env bash
# Build the iOS 16 root volume for the S9's USERDATA from the pieces
# tools/fetch-ios-parts.sh downloaded, the same way the herolte ios-root
# workflow does on macOS: case-sensitive journaled HFS+ (HFSX), staged by
# a5xelte-stage-ios-root.sh, tuned by herolte-tune-ios-root.sh.
# Must run as root (iOS file ownership is part of the boot contract):
#   sudo tools/build-ios-root.sh
# Output: build/root.img (raw HFSX, flashed to USERDATA with Heimdall) and
# ref/ios-herolte/research/ipsw-x/d22ap.adt (for the ADT /product node).
# Apple software: keep every output local, never in a repository.
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root: sudo $0" >&2; exit 2; }
cd "$(dirname "$0")/.."
ROOT=$PWD
IPSW=$ROOT/build/ipsw
TOOLS=$ROOT/ref/ios-herolte/tools
SIZE_MB=${SIZE_MB:-16384}
owner=$(stat -f %u "$ROOT")

for f in os.dmg systemos.dmg appos.dmg devicetree.im4p BuildManifest.plist; do
    [[ -f $IPSW/$f ]] || { echo "missing $IPSW/$f (run tools/fetch-ios-parts.sh)" >&2; exit 1; }
done

# D22AP device tree for mkadt's /product node (MobileGestalt identity).
adt_dir=$ROOT/ref/ios-herolte/research/ipsw-x
if [[ ! -f $adt_dir/d22ap.adt ]]; then
    [[ -x $ROOT/build/venv/bin/pyimg4 ]] || {
        python3 -m venv "$ROOT/build/venv"
        "$ROOT/build/venv/bin/python" -m pip install --quiet pyimg4
    }
    mkdir -p "$adt_dir"
    "$ROOT/build/venv/bin/pyimg4" im4p extract -i "$IPSW/devicetree.im4p" -o "$adt_dir/d22ap.adt"
    chown -R "$owner" "$adt_dir" "$ROOT/build/venv"
fi

attach() { hdiutil attach -nobrowse "$@" | awk -F'\t' '/\/Volumes\// {print $NF}' | tail -1; }
os=$(attach -readonly -owners on "$IPSW/os.dmg")
sys=$(attach -readonly -owners on "$IPSW/systemos.dmg")
app=$(attach -readonly -owners on "$IPSW/appos.dmg")
echo "OS=$os SystemOS=$sys AppOS=$app"
rm -f "$ROOT/build/root.sparseimage"
hdiutil create -size "${SIZE_MB}m" -type SPARSE -layout NONE \
    -fs "Case-sensitive Journaled HFS+" -volname IOSROOT "$ROOT/build/root"
root=$(attach -owners on "$ROOT/build/root.sparseimage")
echo "root=$root"
STAGE_ALL_APPS=1 bash "$TOOLS/a5xelte-stage-ios-root.sh" "$os" "$sys" "$app" "$root"
build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$IPSW/BuildManifest.plist")
bash "$TOOLS/herolte-tune-ios-root.sh" "$root" "$build"
df -h "$root"
hdiutil detach "$root"; hdiutil detach "$os"; hdiutil detach "$sys"; hdiutil detach "$app"

# Raw volume image (no partition map: the HFSX volume starts at byte 0 of
# USERDATA, as ufsroot0 expects).
dev=$(hdiutil attach -nomount -nobrowse "$ROOT/build/root.sparseimage" | awk 'NR==1 {print $1}')
dd if="/dev/r${dev#/dev/}" of="$ROOT/build/root.img" bs=4m
hdiutil detach "$dev"
rm -f "$ROOT/build/root.sparseimage"
chown "$owner" "$ROOT/build/root.img"
ls -l "$ROOT/build/root.img"
echo "iOS root for build $build ready: build/root.img"
