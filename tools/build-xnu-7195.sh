#!/usr/bin/env bash
# Build the STARLTE kernel on xnu-7195.141.2 (iOS 14 / macOS 11.5) with the
# back-ported series in xnu/series-7195: blacktop/darwin-xnu-build at the same
# pinned commit as the 8796/11417/12377 builds, taught one more version (11.5,
# Apple's macos-115 release.json: libdispatch-1271.120.2, dtrace-370.40.1,
# AvailabilityVersions-80.3, bootstrap_cmds-121.100.1, Libsystem-1292.120.1,
# libplatform-254.80.2). Xcode: DEVELOPER_DIR, default Xcode 15.4 (oldest
# installed). 7195 does not link any KDK library for our configs; KDKROOT just
# has to exist, so the 15.6 KDK in build/kdk is passed.
#   tools/build-xnu-7195.sh                       -> out/kernel.development.starlte-7195
#   MACHINE_CONFIG=BCM2837 tools/build-xnu-7195.sh   stock smoke test, no series
#   FRESH=1 tools/build-xnu-7195.sh               re-clone xnu and re-apply the series
#                                                 (the built dependencies are kept)
# darwin-xnu-build runs patches/11.5/*.sh, then applies patches/11.5/*.patch in
# name order and skips the ones that do not apply: the build log lists
# skips, which must be none (tools/export-7195.sh produces a series that
# applies in order on a clean xnu-7195.141.2).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
SERIES=$ROOT/xnu/series-7195
DXB=$ROOT/build/dxb-7195
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_15.4.app/Contents/Developer}
export MACOS_VERSION=11.5 KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT} MACHINE_CONFIG=${MACHINE_CONFIG:-STARLTE}
export KDKROOT=${KDKROOT:-$ROOT/build/kdk/KDK_15.6_24G84.kdk}
[ -d "$DEVELOPER_DIR" ] || { echo "Xcode missing at $DEVELOPER_DIR"; exit 1; }
[ -d "$KDKROOT" ] || { echo "KDKROOT missing at $KDKROOT"; exit 1; }
free_gb=$(df -g / | awk 'NR==2 {print $4}')
[ "$free_gb" -ge 6 ] || { echo "only ${free_gb} GB free on /: not building"; exit 1; }

for tool in jq gum xcodes cmake ninja; do
    command -v "$tool" >/dev/null || brew install "$tool"
done

if [ ! -d "$DXB" ]; then
    git init -q "$DXB"
    git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
    git -C "$DXB" checkout -q FETCH_HEAD
    python3 - "$DXB/build.sh" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
text = path.read_text()
def sub(old, new):
    global text
    assert text.count(old) == 1, f'darwin-xnu-build changed: {old!r}'
    text = text.replace(old, new)
# Same firehose flags as the other STARLTE builds: plain arm64, no ARMv8.1 LSE atomics.
sub('ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"',
    'ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS="-mkernel -nostdinc '
    '-Wno-packed -mcpu=cortex-a53 -march=armv8-a+crypto"')
# macOS 11.5 = xnu-7195.141.2 (iOS 14.7 generation).
sub("""    case ${MACOS_VERSION} in
    '12.5')""", """    case ${MACOS_VERSION} in
    '11.5')
        RELEASE_URL='https://raw.githubusercontent.com/apple-oss-distributions/distribution-macOS/macos-115/release.json'
        KDK_NAME='Kernel Debug Kit 11.5 build 20G71'
        KDKROOT='/Library/Developer/KDKs/KDK_11.5_20G71.kdk'
        RC_DARWIN_KERNEL_VERSION='20.6.0'
        ;;
    '12.5')""")
sub("""        case ${MACOS_VERSION} in
        '12.5' | '13.0'""", """        case ${MACOS_VERSION} in
        '11.5')
            PATCH_DIR="${WORK_DIR}/patches/11.5"
            ;;
        '12.5' | '13.0'""")
sub('\n    patches\n', '\n    [ -n "${SKIP_PATCHES:-}" ] || patches\n')
# dtrace-370 installs ctfconvert/ctfmerge/ctfdump under the toolchain path
# inside DSTROOT; the kernel build looks for them in fakeroot/usr/local/bin.
sub('''            xcodebuild install -sdk macosx -target ctfconvert -target ctfdump -target ctfmerge ARCHS="arm64 x86_64" CODE_SIGN_IDENTITY="-" OBJROOT="${OBJROOT}" SYMROOT="${SYMROOT}" DSTROOT="${DSTROOT}"
''', '''            xcodebuild install -sdk macosx -target ctfconvert -target ctfdump -target ctfmerge ARCHS="arm64 x86_64" CODE_SIGN_IDENTITY="-" OBJROOT="${OBJROOT}" SYMROOT="${SYMROOT}" DSTROOT="${DSTROOT}"
        mkdir -p "${FAKEROOT_DIR}/usr/local/bin"
        find "${DSTROOT}" -path '*/usr/local/bin/ctf*' -type f ! -path "${FAKEROOT_DIR}/usr/local/bin/*" -exec cp {} "${FAKEROOT_DIR}/usr/local/bin/" \\;
''')
path.write_text(text)
PY
fi
mkdir -p "$DXB/patches/11.5"
rm -f "$DXB"/patches/11.5/*
if [ "$MACHINE_CONFIG" = STARLTE ]; then
    cp "$SERIES"/*.patch "$SERIES"/*.sh "$DXB"/patches/11.5/
fi
if [ "$MACHINE_CONFIG" = STARLTE ]; then
    series_id=$( (echo STARLTE; cat "$SERIES"/*.patch "$SERIES"/*.sh) | shasum -a 256 | cut -c1-16)
else
    series_id=$(echo "$MACHINE_CONFIG" | shasum -a 256 | cut -c1-16)
fi
if [ -n "${FRESH:-}" ] || [ "$(cat "$DXB/.series-7195" 2>/dev/null)" != "$series_id" ]; then
    [ -z "${FRESH:-}" ] && [ -d "$DXB/xnu" ] && {
        echo "series or MACHINE_CONFIG changed since $DXB/xnu was set up: run with FRESH=1"; exit 1; }
    rm -rf "$DXB/xnu" "$DXB"/build/xnu*.obj "$DXB"/build/xnu*.sym "$DXB/fakeroot/.xnu_headers_installed"
    echo "$series_id" > "$DXB/.series-7195"
else
    [ -d "$DXB/xnu" ] && export SKIP_PATCHES=1
fi

cd "$DXB"
cfg=$(echo "$KERNEL_CONFIG" | tr A-Z a-z).$(echo "$MACHINE_CONFIG" | tr A-Z a-z)
rm -f "$DXB/build/xnu.obj/kernel.$cfg" "$DXB/fakeroot/System/Library/Kernels/kernel.$cfg"
start=$(date +%s)
touch "$ROOT/build/xnu-7195.log.start"
MAKEFLAGS=${MAKEFLAGS:--k} bash build.sh 2>&1 | tee "$ROOT/build/xnu-7195.log" |
    grep -E --line-buffered "error:|BUILD|Installing|Applying|Skipping|Running script|xnu" | tail -60 || true
kernel=$DXB/fakeroot/System/Library/Kernels/kernel.$cfg
[ -f "$kernel" ] || { echo "no kernel; see build/xnu-7195.log"; exit 1; }
[ "$kernel" -nt "$ROOT/build/xnu-7195.log.start" ] || { echo "kernel was not relinked; see build/xnu-7195.log"; exit 1; }
mkdir -p "$ROOT/out"
cp "$kernel" "$ROOT/out/kernel.$cfg-7195"
echo "kernel built in $(( $(date +%s) - start )) s -> out/kernel.$cfg-7195"
shasum -a 256 "$ROOT/out/kernel.$cfg-7195"
