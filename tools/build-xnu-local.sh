#!/usr/bin/env bash
# Build the STARLTE kernel on this Mac, the same way the fork's xnu workflow
# does on GitHub: blacktop/darwin-xnu-build at the pinned commit, the herolte
# patch series + our 0300 patch, Xcode 15.4 and KDK 13.5 (22G74).
#   tools/build-xnu-local.sh           -> out/kernel.development.starlte
#   FRESH=1 tools/build-xnu-local.sh   start from a clean source tree
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
FORK=$ROOT/build/ios-a5xelte                      # starlte branch of the fork
DXB=$ROOT/build/dxb
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
export DEVELOPER_DIR=/Applications/Xcode_15.4.app/Contents/Developer
export MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=STARLTE
export KDKROOT=/Library/Developer/KDKs/KDK_13.5_22G74.kdk
[ -d "$DEVELOPER_DIR" ] || { echo "Xcode 15.4 missing at $DEVELOPER_DIR"; exit 1; }
[ -d "$KDKROOT" ] || { echo "KDK missing at $KDKROOT"; exit 1; }

for tool in jq gum xcodes cmake ninja; do
    command -v "$tool" >/dev/null || brew install "$tool"
done

if [ ! -d "$DXB" ] || [ -n "${FRESH:-}" ]; then
    rm -rf "$DXB"
    git init -q "$DXB"
    git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
    git -C "$DXB" checkout -q FETCH_HEAD
    # Same firehose flags as the workflow: plain arm64, no ARMv8.1 LSE atomics.
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
fi
# The patch set always comes from the fork checkout plus ours (xnu/patches/03xx:
# 0300 board target, 0301 Mali-G72, ...).
OURS=$(ls "$ROOT"/xnu/patches/03[0-9][0-9]-*.patch)
cp $OURS "$FORK"/xnu/patches/
if [ -f "$DXB/.applied-0300-starlte-board-target.patch" ] && [ -d "$DXB/xnu" ]; then
    # Incremental: the series is already in the tree. If any of our patches
    # changed (or is new), unwind ours newest first and re-apply them all in
    # order: neighbouring hunks (bsd/conf/files) depend on each other, so one
    # patch cannot be swapped alone. make then recompiles what changed.
    changed=
    for p in $OURS; do
        applied=$DXB/.applied-$(basename "$p")
        [ -f "$applied" ] && cmp -s "$applied" "$p" || changed=1
    done
    if [ -n "$changed" ]; then
        for applied in $(ls -r "$DXB"/.applied-03[0-9][0-9]-*.patch); do
            git -C "$DXB/xnu" apply -R "$applied"
            rm "$applied"
        done
        for p in $OURS; do
            git -C "$DXB/xnu" apply "$p"
            cp "$p" "$DXB/.applied-$(basename "$p")"
            echo "applied $(basename "$p") in the patched tree"
        done
    fi
    export SKIP_PATCHES=1
else
    rm -f "$DXB"/patches/0[0-9][0-9][0-9]-*.patch "$DXB"/patches/0[0-9][0-9][0-9]-*.sh
    cp "$FORK"/xnu/patches/*.patch "$FORK"/xnu/patches/*.sh "$DXB"/patches/
fi
grep -q 'SKIP_PATCHES' "$DXB/build.sh" ||
    sed -i '' 's/^    patches$/    [ -n "${SKIP_PATCHES:-}" ] || patches/' "$DXB/build.sh"

cd "$DXB"
# build.sh skips the build when this file exists; make itself is incremental.
rm -f "$DXB/build/xnu.obj/kernel.development.starlte" "$DXB/fakeroot/System/Library/Kernels/kernel.development.starlte"
start=$(date +%s)
touch "$ROOT/build/xnu-local.log.start"
MAKEFLAGS=${MAKEFLAGS:--k} bash build.sh 2>&1 | tee "$ROOT/build/xnu-local.log" | grep -E --line-buffered "error:|BUILD|Installing|Applying|Building|xnu" | tail -40 || true
kernel=$DXB/fakeroot/System/Library/Kernels/kernel.development.starlte
[ -f "$kernel" ] || { echo "no kernel; see build/xnu-local.log"; exit 1; }
for p in $OURS; do [ -f "$DXB/.applied-$(basename "$p")" ] || cp "$p" "$DXB/.applied-$(basename "$p")"; done
[ "$kernel" -nt "$ROOT/build/xnu-local.log.start" ] || { echo "kernel was not relinked; see build/xnu-local.log"; exit 1; }
cp "$kernel" "$ROOT/out/kernel.development.starlte"
echo "kernel built in $(( $(date +%s) - start )) s -> out/kernel.development.starlte"
