#!/bin/sh
# T6 (docs/WIN11-M3-X86.md section 3): build FEX's libarm64ecfex.dll for Windows 11 23H2 on the Exynos M3, in the
# lima VM 'starbite' (aarch64). Host only; nothing here touches the phone or any Windows file.
#
#   build-fex-arm64ec.sh            run on the Mac: re-executes itself inside the VM
#
# Toolchain: llvm-mingw 20261104 ucrt ubuntu-22.04 aarch64 (prebuilt, 78 MB .tar.xz; its default TOOLCHAIN_ARCHS include
# arm64ec, so arm64ec-w64-mingw32-clang is there). FEX source e3b30dd + 0001-starbite-23h2-m3.patch (patches A/B/C).
# Tuning: TUNE_ARCH=generic gives -march=armv8-a+crc (the M3's ISA: no LSE/RCPC in FEX itself); TUNE_CPU=none.
# Output: $OUT/libarm64ecfex.dll (+ .pdb) with sha256, and an LSE/RCPC word count of the DLL (must be 0 outside
# FEX's own runtime-dispatched paths; check with m3-scan.py on the Mac).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "$(uname -s)" = Darwin ]; then
    df -g /Users | awk 'NR==2 && $4 < 6 {print "Mac has " $4 " GiB free: need >= 6 (VM disk grows); free space first"; exit 1}'
    exec limactl shell starbite -- sh "$HERE/build-fex-arm64ec.sh"
fi
W=${FEX_WORK:-$HOME/fex-arm64ec}
OUT=${FEX_OUT:-$HERE/out}
LM=llvm-mingw-20261104-ucrt-ubuntu-22.04-aarch64
mkdir -p "$W" "$OUT"
cd "$W"
if [ ! -x "$LM/bin/arm64ec-w64-mingw32-clang" ]; then
    curl -fL -o "$LM.tar.xz" "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/$LM.tar.xz"
    tar xf "$LM.tar.xz"
fi
PATH="$W/$LM/bin:$PATH"
if [ ! -d fex/.git ]; then
    git clone -q https://github.com/FEX-Emu/FEX.git fex
fi
cd fex
git checkout -q -f e3b30dd
git submodule update --init --depth 1 External/fmt External/range-v3 External/rpmalloc External/unordered_dense \
    External/vixl External/xxhash External/drm-headers External/Vulkan-Headers Source/Common/cpp-optparse
git apply --check "$HERE/0001-starbite-23h2-m3.patch" && git apply "$HERE/0001-starbite-23h2-m3.patch"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/Data/CMake/toolchain_mingw.cmake" -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
    -DTUNE_ARCH=generic -DTUNE_CPU=none -DUSE_PDB_DEBUGINFO=True -DENABLE_LTO=False -DENABLE_ASSERTIONS=True \
    -DBUILD_TESTING=False -DBUILD_FEXCONFIG=False -DENABLE_CCACHE=False
ninja -C build arm64ecfex
D=$(find build -name libarm64ecfex.dll | head -1)
cp "$D" "$OUT/"
P=${D%.dll}.pdb; [ -f "$P" ] && cp "$P" "$OUT/"
ls -l "$OUT"
sha256sum "$OUT/libarm64ecfex.dll"
