#!/usr/bin/env bash
# Cross-build Mesa 26.2.x's D3D10 user-mode driver (d3d10umd -> libgallium_d3d10.dll) for Windows 11 ARM64
# in the lima VM "starbite", MSVC ABI (clang-cl + lld-link against the read-only kit in ~/sbwin, which the
# VM sees at the same path). docs/WIN11-GPU.md section 6.7.
#
#   limactl shell starbite -- tools/win/mesa/build-mesa.sh softpipe     d3d10umd + softpipe
#   limactl shell starbite -- tools/win/mesa/build-mesa.sh panfrost     + Panfrost (needs libpan, see below)
#   limactl shell starbite -- tools/win/mesa/build-mesa.sh libpan       Panfrost's OpenCL C kernels -> SPIR-V
#                                                                       (native Linux build with LLVM 18)
#
# Work tree: ~/sbmesa in the VM (VM disk, not the Mac): mesa-$VER.tar.xz (checked against the SHA-256 in
# Mesa's release notes), src/ (pristine + tools/win/mesa/g52 patches, a git repo so the diff is visible),
# build-<cfg>/, out/. The DLL is copied to build/win/mesa/ in this tree.
# Patches: wddm-mali-g52 (github.com/gahingwoo/wddm-mali-g52 at e8bcd36, GPL-2.0), tools/win/mesa/g52/.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
VER=${MESA_VER:-26.2.6}
SHA=${MESA_SHA:-b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92}
SBWIN=${SBWIN:-/Users/dev/sbwin}
KIT=$SBWIN/kit
KV=10.0.26100.0
W=${SBMESA:-$HOME/sbmesa}
cfg=${1:-softpipe}
mkdir -p "$W" && cd "$W"
# meson >= 1.4 (Ubuntu 24.04 has 1.3.2): a venv in the work tree
if [ ! -x venv/bin/meson ]; then
    python3 -m venv venv
    venv/bin/pip -q install "meson==1.6.1" mako pyyaml packaging
fi
export PATH=$W/venv/bin:$PATH
fresh() { [ -d "$1" ] && echo --wipe; }   # FRESH=1: reconfigure an existing build dir from scratch

# ---- source --------------------------------------------------------------------------------------
if [ ! -d src ]; then
    [ -f mesa-$VER.tar.xz ] || curl -fsSLO https://archive.mesa3d.org/mesa-$VER.tar.xz
    echo "$SHA  mesa-$VER.tar.xz" | sha256sum -c -
    tar xf mesa-$VER.tar.xz
    mv mesa-$VER src
    git -C src init -q && git -C src add -A >/dev/null
    git -C src -c user.email=sb@local -c user.name=sb commit -qm "mesa $VER"
    for p in "$here"/g52/mesa-patches/*.patch; do
        git -C src apply --whitespace=nowarn "$p"
    done
    mkdir -p src/include/win32-compat
    cp -R "$here"/g52/win32-compat/. src/include/win32-compat/
    cp "$here"/g52/include/*.h src/include/win32-compat/
    git -C src add -A >/dev/null
    git -C src -c user.email=sb@local -c user.name=sb commit -qm "wddm-mali-g52 patches 0001-0020 + win32-compat"
    # ours, on top (they touch the win32-compat copy too): tools/win/mesa/sb-patches/
    for p in "$here"/sb-patches/*.patch; do
        [ -e "$p" ] || continue
        git -C src apply --whitespace=nowarn "$p"
        git -C src -c user.email=sb@local -c user.name=sb commit -qam "$(basename "$p")"
    done
fi

# ---- toolchain: clang 18 in cl mode, lld-link, llvm-lib, llvm-rc -------------------------------------
tc=$W/tc
mkdir -p "$tc"
inc=(-imsvc "$SBWIN/msvc/include" -imsvc "$KIT/Include/$KV/ucrt" -imsvc "$KIT/Include/$KV/shared"
     -imsvc "$KIT/Include/$KV/um" -imsvc "$KIT/Include/$KV/winrt")
# target and kit headers in the wrapper itself: meson also runs the compiler without c_args (its ninja
# dependency-prefix probe compiles #include <stdio.h>)
{
    echo '#!/bin/sh'
    printf 'exec clang --driver-mode=cl -fuse-ld=lld --target=aarch64-pc-windows-msvc -fms-compatibility-version=19.44'
    printf ' %s' "${inc[@]}"
    echo ' "$@"'
} > "$tc/clang-cl"
chmod +x "$tc/clang-cl"
cflags=(/utf-8
        -D_ARM64_ -DARM64 -D_WIN64 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
        -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -DHAVE_FUNC_ATTRIBUTE_PACKED
        -Wno-unused-command-line-argument -Wno-microsoft-anon-tag -Wno-ignored-pragma-intrinsic
        -Wno-pragma-pack -Wno-nonportable-include-path)
# hybrid CRT, as tools/win/build.sh: static vcruntime (b_vscrt=mt) but the in-box ucrtbase.dll, so the phone
# needs no VCRUNTIME140.dll
lflags=("/libpath:$SBWIN/msvc/lib/arm64" "/libpath:$KIT/Lib/$KV/ucrt/arm64" "/libpath:$KIT/Lib/$KV/um/arm64"
        /machine:arm64 /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib)
q() { local s="" a; for a in "$@"; do s+="'$a', "; done; printf '[%s]' "${s%, }"; }
cat > "$tc/cross.ini" <<EOF
[binaries]
c = ['$tc/clang-cl']
cpp = ['$tc/clang-cl']
ar = 'llvm-lib'
windres = 'llvm-rc'
strip = 'true'

[built-in options]
c_args = $(q "${cflags[@]}")
cpp_args = $(q "${cflags[@]}" /EHsc)
c_link_args = $(q "${lflags[@]}")
cpp_link_args = $(q "${lflags[@]}")

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

common=(-Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt -Dplatforms=windows -Dgallium-d3d10umd=true -Dvulkan-drivers=
        -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled
        -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dzstd=disabled
        -Dshader-cache=disabled -Dbuild-tests=false -Dexpat=disabled -Dzlib=disabled)

case $cfg in
softpipe)
    bd=build-softpipe
    [ -f $bd/build.ninja ] && [ "${FRESH:-0}" != 1 ] || meson setup $(fresh $bd) $bd src --cross-file "$tc/cross.ini" "${common[@]}" -Dgallium-drivers=softpipe
    ;;
panfrost)
    bd=build-panfrost
    [ -x "$W/native/panfrost_compile" ] || { echo "run '$0 libpan' first" >&2; exit 2; }
    # the build-time tools come from the native (Linux) build of the same patched tree: mesa_clc and
    # vtn_bindgen2 for libpan, panfrost_compile for its Mali binaries (a cross build cannot build them)
    export PATH=$W/native:$PATH
    [ -f $bd/build.ninja ] && [ "${FRESH:-0}" != 1 ] || meson setup $(fresh $bd) $bd src --cross-file "$tc/cross.ini" "${common[@]}" \
        -Dgallium-drivers=softpipe,panfrost -Dmesa-clc=system -Dprecomp-compiler=system
    ;;
libpan)
    bd=build-libpan
    [ -f $bd/build.ninja ] && [ "${FRESH:-0}" != 1 ] || meson setup $(fresh $bd) $bd src -Dbuildtype=release -Dgallium-drivers=panfrost -Dvulkan-drivers= \
        -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=enabled -Dmesa-clc=enabled \
        -Dvideo-codecs= -Dbuild-tests=false
    t=""
    for v in 4 5 6 7 9 10 12 13 14; do
        t="$t src/panfrost/libpan/libpan_v$v.spv src/panfrost/libpan/libpan_v$v.cpp src/panfrost/libpan/libpan_v$v.h"
    done
    ninja -C $bd $t src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile
    mkdir -p libpan native
    cp $bd/src/panfrost/libpan/libpan_v*.{spv,cpp,h} libpan/
    cp $bd/src/compiler/clc/mesa_clc $bd/src/compiler/spirv/vtn_bindgen2 $bd/src/panfrost/clc/panfrost_compile native/
    ls -la native
    exit 0
    ;;
*)
    echo "usage: $0 softpipe|panfrost|libpan" >&2; exit 2 ;;
esac

ninja -C $bd -k 0 2>&1 | tee $bd.log | grep -E "FAILED|error" | head -40 || true
dll=$(find $bd -name libgallium_d3d10.dll | head -1)
[ -n "$dll" ] || { echo "no libgallium_d3d10.dll; $(grep -c FAILED $bd.log) failed steps, log: $W/$bd.log" >&2; exit 1; }
mkdir -p "$repo/build/win/mesa"
cp "$dll" "$repo/build/win/mesa/libgallium_d3d10-$cfg.dll"
sha256sum "$repo/build/win/mesa/libgallium_d3d10-$cfg.dll"
du -sh "$W/$bd"
