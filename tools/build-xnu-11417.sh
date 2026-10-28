#!/usr/bin/env bash
# Build the STARLTE kernel on xnu-11417.140.69 (iPadOS 18.7 / macOS 15.6)
# with the ported series in xnu/series-11417: blacktop/darwin-xnu-build at the
# same pinned commit as tools/build-xnu-local.sh, Xcode 16.4 and KDK 15.6
# (24G84, unpacked into build/kdk, not installed). The iOS 16 kernel keeps
# its own tree (build/dxb, Xcode 15.4) and is not touched.
#   tools/build-xnu-11417.sh           -> out/kernel.development.starlte-11417
#   FRESH=1 tools/build-xnu-11417.sh   start from a clean source tree
# darwin-xnu-build runs patches/15.0/*.sh, then applies patches/15.0/*.patch in
# name order and skips the ones that do not apply: our series (0xxx) goes
# first; its own link_kdk, link_kdk146 and remove_tightbeam are expected skips.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
SERIES=$ROOT/xnu/series-11417
DXB=$ROOT/build/dxb-11417
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
export MACOS_VERSION=15.6 KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT} MACHINE_CONFIG=${MACHINE_CONFIG:-STARLTE}
export KDKROOT=$ROOT/build/kdk/KDK_15.6_24G84.kdk
[ -d "$DEVELOPER_DIR" ] || { echo "Xcode 16.4 missing at $DEVELOPER_DIR (unxip Xcode_16.4.xip to /Applications/Xcode_16.4.app)"; exit 1; }
[ -d "$KDKROOT" ] || { echo "KDK missing at $KDKROOT (see NOTES.md, iPadOS 18 plan)"; exit 1; }

for tool in jq gum xcodes cmake ninja; do
    command -v "$tool" >/dev/null || brew install "$tool"
done

series_id=$(cat "$SERIES"/*.patch "$SERIES"/*.sh | shasum -a 256 | cut -c1-16)
if [ ! -d "$DXB" ] || [ -n "${FRESH:-}" ]; then
    rm -rf "$DXB"
    git init -q "$DXB"
    git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
    git -C "$DXB" checkout -q FETCH_HEAD
    # Same firehose flags as the iOS 16 build: plain arm64, no ARMv8.1 LSE atomics.
    python3 - "$DXB/build.sh" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
old = 'ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"'
new = ('ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS="-mkernel -nostdinc '
       '-Wno-packed -mcpu=cortex-a53 -march=armv8-a+crypto"')
text = path.read_text()
assert text.count(old) == 1, 'darwin-xnu-build firehose line changed'
path.write_text(text.replace(old, new))
PY
    cp "$SERIES"/*.patch "$SERIES"/*.sh "$DXB"/patches/15.0/
    echo "$series_id" > "$DXB/.series-11417"
else
    # The series is applied once, when the source tree is cloned. make is
    # incremental afterwards; a changed series needs a fresh tree.
    [ "$(cat "$DXB/.series-11417" 2>/dev/null)" = "$series_id" ] || {
        echo "xnu/series-11417 changed since $DXB was set up: run with FRESH=1"; exit 1; }
    [ -d "$DXB/xnu" ] && export SKIP_PATCHES=1
fi
grep -q 'SKIP_PATCHES' "$DXB/build.sh" ||
    sed -i '' 's/^    patches$/    [ -n "${SKIP_PATCHES:-}" ] || patches/' "$DXB/build.sh"

cd "$DXB"
cfg=$(echo "$KERNEL_CONFIG" | tr A-Z a-z).$(echo "$MACHINE_CONFIG" | tr A-Z a-z)
# build.sh skips the build when this file exists; make itself is incremental.
rm -f "$DXB/build/xnu.obj/kernel.$cfg" "$DXB/fakeroot/System/Library/Kernels/kernel.$cfg"
start=$(date +%s)
touch "$ROOT/build/xnu-11417.log.start"
MAKEFLAGS=${MAKEFLAGS:--k} bash build.sh 2>&1 | tee "$ROOT/build/xnu-11417.log" |
    grep -E --line-buffered "error:|BUILD|Installing|Applying|Skipping|Running script|xnu" | tail -60 || true
kernel=$DXB/fakeroot/System/Library/Kernels/kernel.$cfg
[ -f "$kernel" ] || { echo "no kernel; see build/xnu-11417.log"; exit 1; }
[ "$kernel" -nt "$ROOT/build/xnu-11417.log.start" ] || { echo "kernel was not relinked; see build/xnu-11417.log"; exit 1; }
cp "$kernel" "$ROOT/out/kernel.$cfg-11417"
echo "kernel built in $(( $(date +%s) - start )) s -> out/kernel.$cfg-11417"
