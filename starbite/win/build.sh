#!/bin/sh
# Build the Starbite Windows drivers (ARM64 kernel mode) on the Mac with clang-cl + lld-link.
#
#   starbite/win/build.sh [out-dir]          default out-dir: build/win-gpu (repo root, ignored by git)
#
# Needs the WDK/SDK headers and kernel libraries, version 10.0.26100.0, laid out as Microsoft's NuGet
# packages unpack them (Microsoft.Windows.WDK.ARM64 c/Include/*/km, c/Lib/*/km/ARM64 and
# Microsoft.Windows.SDK.CPP c/Include/*/shared). WDK_KIT points at that tree; the default is this
# track's copy on iCloud Drive (Starbite-overflow/win-gpu/kit). When tools/win/ (track L0) has its
# own kit, point WDK_KIT there.
#
# Signing is separate (tools/win/, the "Starbite Test Signing" certificate, osslsigncode).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/win-gpu}
kit=${WDK_KIT:-"$HOME/Library/Mobile Documents/com~apple~CloudDocs/Starbite-overflow/win-gpu/kit"}
ver=10.0.26100.0
llvm=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
cc="$llvm/clang-cl"
ld=${LLD_LINK:-$(command -v lld-link || echo "$llvm/lld-link")}
[ -f "$kit/c/Include/$ver/km/wdm.h" ] || { echo "no WDK headers in $kit (set WDK_KIT)" >&2; exit 1; }
mkdir -p "$out"
# The flags below are split on spaces: reach the kit through a link without spaces in its path.
ln -sfn "$kit" "$out/.kit"
inc="$out/.kit/c/Include/$ver"
lib="$out/.kit/c/Lib/$ver/km/ARM64"

# Kernel mode, ARM64, Windows 10/11 headers at the 22H2/23H2 level (NTDDI_WIN10_NI).
cflags="--target=aarch64-pc-windows-msvc /c /nologo /kernel /GS- /Gy /Zl /O2 /W4 /Z7
 -D_ARM64_ -DARM64 -D_KERNEL_MODE -D_WIN64 -DNTDDI_VERSION=0x0A00000C -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
 -DPOOL_NX_OPTIN=1 -DPOOL_ZERO_DOWN_LEVEL_SUPPORT
 -Wno-unknown-pragmas -Wno-ignored-pragma-intrinsic -Wno-microsoft-anon-tag -Wno-nonportable-include-path
 -Wno-unused-command-line-argument -Wno-pragma-pack -Wno-ignored-attributes -Wno-microsoft-enum-forward-reference
 -Wno-language-extension-token -Wno-extra-tokens -Wno-unused-parameter -Wno-c11-extensions
 /imsvc$inc/km/crt /imsvc$inc/km /imsvc$inc/shared"
ldflags="/nologo /machine:arm64 /driver /subsystem:native,10.00 /release /nodefaultlib /incremental:no
 /opt:ref /opt:icf /debug /guard:no /libpath:$lib"

build() {   # name entry libs sources...
    name=$1; entry=$2; libs=$3; shift 3
    objs=""
    for src in "$@"; do
        obj="$out/$(basename "$src" .c).$name.obj"
        # shellcheck disable=SC2086
        "$cc" $cflags /Fo"$obj" -- "$src"
        objs="$objs $obj"
    done
    # shellcheck disable=SC2086
    "$ld" $ldflags /entry:$entry /out:"$out/$name.sys" /pdb:"$out/$name.pdb" $objs $libs
    echo "built $out/$name.sys"
}

build sbthermal GsDriverEntry "ntoskrnl.lib hal.lib bufferoverflowfastfailk.lib" \
    "$here/sbthermal/sbthermal.c" "$here/sbthermal/sbth_core.c"
build sbdisplay GsDriverEntry "ntoskrnl.lib hal.lib displib.lib ntstrsafe.lib bufferoverflowfastfailk.lib libcntpr.lib" \
    "$here/sbdisplay/sbdisplay.c" "$here/sbdisplay/sbdc_core.c"
cp "$here/sbdisplay/sbdisplay.inf" "$out/"
