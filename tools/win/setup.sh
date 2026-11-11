#!/usr/bin/env bash
# Windows 11 ARM64 build kit on the Mac (docs/PLAN-2026-11-07.md step L0; tools/win/README.md).
#
#   tools/win/setup.sh            download, verify (sha256), extract into ${SBWIN:-$HOME/sbwin}; idempotent
#   SBWIN_ARCHES="x64 x86" tools/win/setup.sh   also the x64 and/or x86 user-mode libraries (SDK um/ucrt + the
#                                 MSVC CRT): builds of DLLs that load into emulated x64/x86 processes (the OpenGL
#                                 ICD, docs/WIN11-GPU-CAPS.md 5a). ARM64EC needs nothing extra: the arm64 libraries
#                                 are ARM64X (they carry ARM64EC objects too).
#   KEEP_DL=1 tools/win/setup.sh  keep the downloaded archives in $SBWIN/dl (default: deleted after extraction)
#
# Everything comes from Microsoft's own servers, pinned by version and sha256 below:
#   - NuGet  Microsoft.Windows.SDK.CPP        10.0.26100.6584  headers (shared, um, ucrt, winrt)
#   - NuGet  Microsoft.Windows.SDK.CPP.arm64  10.0.26100.6584  import/static libs (um, ucrt) for arm64
#   - NuGet  Microsoft.Windows.WDK.ARM64      10.0.26100.6584  km headers/libs, KMDF 1.15-1.35, UMDF 2.15-2.35,
#                                                             IddCx 1.0-1.10, UFX, NetCx, ...
#   - VS 17.14.44 release channel (VisualStudio.vsman sha256 963760b0...): MSVC 14.44.35207 CRT headers and the
#     ARM64 CRT libraries (Desktop.base: libcmt, libvcruntime, libcpmt...; Store.base: msvcrt, vcruntime,
#     oldnames, delayimp... without its uwp/ and store/ variants)
#   - the same SDK/VS packages for x64 and x86 (Microsoft.Windows.SDK.CPP.x64/.x86, CRT.x64/x86.Desktop/Store.base
#     of VS 17.14, MSVC 14.44.17.14: the release the arm64 pins come from), only when SBWIN_ARCHES asks
# Licence: the user accepted the WDK/SDK/VS licence terms for this on 2026-11-07. Nothing here is committed;
# $SBWIN lives outside the repository. Only ARM64 libraries are extracted; no Windows .exe is ever run.
#
# Layout (other sessions build against this; keep it stable):
#   $SBWIN/kit/Include/10.0.26100.0/{shared,um,ucrt,winrt,km}   $SBWIN/kit/Include/wdf/{kmdf,umdf}/<ver>
#   $SBWIN/kit/Lib/10.0.26100.0/{um,ucrt,km}/arm64               $SBWIN/kit/Lib/wdf/{kmdf,umdf}/arm64/<ver>
#   $SBWIN/msvc/include                                         $SBWIN/msvc/lib/arm64
#   $SBWIN/PINS (what is installed), $SBWIN/.stamp-* (idempotency)
set -euo pipefail
SBWIN=${SBWIN:-$HOME/sbwin}
DL=$SBWIN/dl
mkdir -p "$DL"

NUGET=https://api.nuget.org/v3-flatcontainer
SDKV=10.0.26100.6584
KITV=10.0.26100.0
VCV=14.44.17.14
# name|url|sha256|bytes
PINS=(
"sdk-cpp|$NUGET/microsoft.windows.sdk.cpp/$SDKV/microsoft.windows.sdk.cpp.$SDKV.nupkg|5d31b38205bdd9ac761b4cb39fbbc6b7209b01c11194324afc674d7d119483a0|160036542"
"sdk-cpp-arm64|$NUGET/microsoft.windows.sdk.cpp.arm64/$SDKV/microsoft.windows.sdk.cpp.arm64.$SDKV.nupkg|ac06922186fdd75e4ff1033a304ea629d7917bd0ca85e483762f8109584ccfc4|103843219"
"wdk-arm64|$NUGET/microsoft.windows.wdk.arm64/$SDKV/microsoft.windows.wdk.arm64.$SDKV.nupkg|e705b2a63eab891def8f98087666f93e8f21da8e3b5def81a624b83fef5bdae9|45983131"
"vc-crt-headers|https://download.visualstudio.microsoft.com/download/pr/c610cd8c-801b-44b8-a80a-82cc382aeb43/852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5/Microsoft.VC.$VCV.CRT.Headers.base.vsix|852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5|2128977"
"vc-crt-arm64|https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/ba1aeca6d6470d2b3b318ec7bffb3e61f9a736cfb76d8a7d18e004a8e7c26651/Microsoft.VC.$VCV.CRT.ARM64.Desktop.base.vsix|ba1aeca6d6470d2b3b318ec7bffb3e61f9a736cfb76d8a7d18e004a8e7c26651|49166761"
"vc-crt-arm64-store|https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/57ece91747be72fdd9ed0c39b83225011c8afba6b6f616a23ace54d0522f79e9/Microsoft.VC.$VCV.CRT.ARM64.Store.base.vsix|57ece91747be72fdd9ed0c39b83225011c8afba6b6f616a23ace54d0522f79e9|61763419"
)

for a in ${SBWIN_ARCHES:-}; do
    case $a in
    x64) PINS+=(
"sdk-cpp-x64|$NUGET/microsoft.windows.sdk.cpp.x64/$SDKV/microsoft.windows.sdk.cpp.x64.$SDKV.nupkg|c29ce7a4641cb37ee32ebb8078cc65cfbabc7025076bcfba869039204b1e960d|52245405"
"vc-crt-x64|https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/f01f701a7bcd9587a340898c851424f6a52bb913a70c185ff0d5bf0288c5831a/Microsoft.VC.$VCV.CRT.x64.Desktop.base.vsix|f01f701a7bcd9587a340898c851424f6a52bb913a70c185ff0d5bf0288c5831a|50776097"
"vc-crt-x64-store|https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/9135b03c0df53c7a0aa9bef7230a1c2ff4263a0ee7baa7e419d034f484f6bb56/Microsoft.VC.$VCV.CRT.x64.Store.base.vsix|9135b03c0df53c7a0aa9bef7230a1c2ff4263a0ee7baa7e419d034f484f6bb56|27596908") ;;
    x86) PINS+=(
"sdk-cpp-x86|$NUGET/microsoft.windows.sdk.cpp.x86/$SDKV/microsoft.windows.sdk.cpp.x86.$SDKV.nupkg|47bfc24ae5d594b62154acc062e04f7a0e54ff1534a50659f71cb40e878ee9cf|49788719"
"vc-crt-x86|https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/e4dc8ef4551d621501295d2175c890788368d8f8a218401c7c96405d31cb8e41/Microsoft.VC.$VCV.CRT.x86.Desktop.base.vsix|e4dc8ef4551d621501295d2175c890788368d8f8a218401c7c96405d31cb8e41|87554282"
"vc-crt-x86-store|https://download.visualstudio.microsoft.com/download/pr/67cf767c-5e71-47c2-a54a-cd5631e28942/d29d5bd117b1b3a4485a9172d57180417adcb47e004d492b47e031d4c2933dd5/Microsoft.VC.$VCV.CRT.x86.Store.base.vsix|d29d5bd117b1b3a4485a9172d57180417adcb47e004d492b47e031d4c2933dd5|27059639") ;;
    *) echo "SBWIN_ARCHES: unknown $a (x64, x86)"; exit 1 ;;
    esac
done
free_mib() { df -m "$SBWIN" | awk 'NR==2 {print $4}'; }

fetch() {  # name url sha bytes -> $DL/name.zip, verified
    local f=$DL/$1.zip
    if [ -f "$f" ] && [ "$(shasum -a 256 "$f" | cut -c1-64)" = "$3" ]; then return; fi
    local need=$(( $4 / 1048576 * 5 + 200 ))   # archive + extracted (~4x) + margin
    [ "$(free_mib)" -gt "$need" ] || { echo "not enough space for $1: $(free_mib) MiB free, want $need"; exit 1; }
    echo "  download $1 ($(( $4 / 1048576 )) MiB)"
    curl -fsSL --retry 3 -o "$f.part" "$2"
    [ "$(shasum -a 256 "$f.part" | cut -c1-64)" = "$3" ] || { echo "sha256 mismatch: $1"; rm -f "$f.part"; exit 1; }
    mv "$f.part" "$f"
}

# extract name dest prefix [extra unzip args...]: unzip entries under PREFIX into DEST (prefix stripped)
extract() {
    local name=$1 dest=$2 prefix=$3; shift 3
    local tmp=$SBWIN/.x-$name
    rm -rf "$tmp"; mkdir -p "$tmp" "$dest"
    unzip -q -o "$DL/$name.zip" "$prefix/*" "$@" -d "$tmp"
    # NuGet/VSIX names are URL-escaped in a few places (%20 etc.); none of the kept paths need it.
    rsync -a "$tmp/$prefix/" "$dest/"
    rm -rf "$tmp"
}

lower_dirs() {  # rename every ARM64/Arm64 directory below $1 to arm64 (case-sensitive volumes)
    find "$1" -depth -type d -iname arm64 ! -name arm64 | while read -r d; do
        local t; t=$(dirname "$d")/arm64
        if [ -d "$t" ] && [ "$(stat -f %i "$t")" != "$(stat -f %i "$d")" ]; then
            rsync -a "$d/" "$t/"; rm -rf "$d"
        else
            mv "$d" "$d.tmp"; mv "$d.tmp" "$t"
        fi
    done
}

vsix_dir() {  # name subdir -> Contents/VC/Tools/MSVC/<ver>/<subdir> as spelled in the archive
    { unzip -Z1 "$DL/$1.zip" || true; } | grep -i -o "^Contents/VC/Tools/MSVC/[^/]*/$2/" | head -1 | sed 's#/$##'
}

step() {  # name url sha bytes
    local stamp=$SBWIN/.stamp-$1
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$3" ]; then echo "  $1: installed"; return; fi
    fetch "$@"
    echo "  extract $1"
    case $1 in
    sdk-cpp)
        extract sdk-cpp "$SBWIN/kit/Include/$KITV" "c/Include/$KITV" -x '*.idl' "c/Include/$KITV/cppwinrt/*"
        mkdir -p "$SBWIN/licences"; unzip -q -o -j "$DL/sdk-cpp.zip" LICENSE.txt -d "$SBWIN/licences" 2>/dev/null &&
            mv "$SBWIN/licences/LICENSE.txt" "$SBWIN/licences/windows-sdk.txt" || true ;;
    sdk-cpp-arm64)
        extract sdk-cpp-arm64 "$SBWIN/kit/Lib/$KITV" c -x 'c/ucrt_enclave/*' ;;
    wdk-arm64)
        extract wdk-arm64 "$SBWIN/kit/Include" c/Include -x '*.idl'
        extract wdk-arm64 "$SBWIN/kit/Lib" c/Lib
        unzip -q -o -j "$DL/wdk-arm64.zip" LICENSE.txt -d "$SBWIN/licences" && mv "$SBWIN/licences/LICENSE.txt" "$SBWIN/licences/wdk.txt"
        lower_dirs "$SBWIN/kit/Lib" ;;
    vc-crt-headers)
        extract vc-crt-headers "$SBWIN/msvc/include" "$(vsix_dir vc-crt-headers include)" ;;
    sdk-cpp-x64|sdk-cpp-x86)
        extract "$1" "$SBWIN/kit/Lib/$KITV" c -x 'c/ucrt_enclave/*' ;;
    vc-crt-x64|vc-crt-x64-store|vc-crt-x86|vc-crt-x86-store)
        local a=${1#vc-crt-}; a=${a%-store}; local p; p=$(vsix_dir "$1" "lib/$a")
        extract "$1" "$SBWIN/msvc/lib/$a" "$p" -x '*.pdb' "$p/uwp/*" "$p/store/*" "$p/enclave/*" "$p/onecore/*" ;;
    vc-crt-arm64|vc-crt-arm64-store)
        # the "Store" package is where msvcrt.lib, vcruntime.lib, oldnames.lib, delayimp.lib live;
        # its uwp/ and store/ variants are not needed
        local p; p=$(vsix_dir "$1" lib/arm64)
        extract "$1" "$SBWIN/msvc/lib/arm64" "$p" -x '*.pdb' "$p/uwp/*" "$p/store/*" ;;
    esac
    echo "$3" > "$stamp"
    [ "${KEEP_DL:-0}" = 1 ] || rm -f "$DL/$1.zip"
}

echo "SBWIN=$SBWIN ($(free_mib) MiB free)"
for p in "${PINS[@]}"; do IFS='|' read -r n u s b <<<"$p"; step "$n" "$u" "$s" "$b"; done

# clang fixups (MSVC accepts these, clang does not). Idempotent; each one is a few bytes in our copy only.
# 1. WDF C++: wdfdevice.h forward-declares "enum _WDF_REQUEST_TYPE : int;" but wdfrequest.h defines it without
#    the ": int" -> clang: "enumeration previously declared with fixed underlying type". Give the definition
#    the same underlying type in C++.
for f in "$SBWIN"/kit/Include/wdf/kmdf/*/wdfrequest.h "$SBWIN"/kit/Include/wdf/umdf/*/wdfrequest.h; do
    grep -q 'SBWIN fixup' "$f" && continue
    grep -q '^typedef enum _WDF_REQUEST_TYPE {' "$f" || continue
    sed -i '' 's/^typedef enum _WDF_REQUEST_TYPE {/typedef enum _WDF_REQUEST_TYPE \/* SBWIN fixup *\/ WDF_SB_ENUM_INT {/' "$f"
    sed -i '' '1i\
#if defined(__cplusplus) \&\& !defined(WDF_SB_ENUM_INT)\
#define WDF_SB_ENUM_INT : int\
#elif !defined(WDF_SB_ENUM_INT)\
#define WDF_SB_ENUM_INT\
#endif
' "$f"
done

# Files the sessions depend on (Starbite Link, USB, Wi-Fi, GPU); fail loudly if a pin ever drops one.
I=$SBWIN/kit/Include/$KITV L=$SBWIN/kit/Lib/$KITV
missing=0
for f in \
    "$I/km/wdm.h" "$I/km/ntddk.h" "$I/km/ntifs.h" "$I/km/dispmprt.h" "$I/shared/d3dkmddi.h" "$I/shared/d3dkmdt.h" \
    "$I/shared/d3dukmdt.h" "$I/km/usbfnioctl.h" "$I/km/ufx/1.1/ufxclient.h" "$I/um/iddcx/1.4/IddCx.h" \
    "$I/um/windows.h" "$I/ucrt/stdio.h" "$I/shared/sal.h" "$SBWIN/msvc/include/vcruntime.h" \
    "$SBWIN/kit/Include/wdf/kmdf/1.33/wdf.h" "$SBWIN/kit/Include/wdf/umdf/2.33/wdf.h" \
    "$L/km/arm64/ntoskrnl.lib" "$L/km/arm64/hal.lib" "$L/km/arm64/displib.lib" "$L/km/arm64/wdmsec.lib" \
    "$L/km/arm64/bufferoverflowfastfailk.lib" "$L/km/arm64/ufx/1.1/ufxstub.lib" \
    "$L/um/arm64/kernel32.lib" "$L/um/arm64/iddcx/1.4/iddcxstub.lib" "$L/ucrt/arm64/ucrt.lib" \
    "$SBWIN/kit/Lib/wdf/kmdf/arm64/1.33/wdfdriverentry.lib" "$SBWIN/kit/Lib/wdf/kmdf/arm64/1.33/wdfldr.lib" \
    "$SBWIN/kit/Lib/wdf/umdf/arm64/2.33/WdfDriverStubUm.lib" \
    "$SBWIN/msvc/lib/arm64/libcmt.lib" "$SBWIN/msvc/lib/arm64/libvcruntime.lib" "$SBWIN/msvc/lib/arm64/msvcrt.lib"
do [ -f "$f" ] || { echo "MISSING ${f#$SBWIN/}"; missing=1; }; done
[ "$missing" = 0 ] || exit 1

{ echo "# installed by tools/win/setup.sh"; printf '%s\n' "${PINS[@]}" | cut -d'|' -f1,3; } > "$SBWIN/PINS"
rmdir "$DL" 2>/dev/null || true
echo "sizes:"; du -sh "$SBWIN/kit/Include" "$SBWIN/kit/Lib" "$SBWIN/msvc" "$SBWIN" 2>/dev/null | sed 's/^/  /'
echo "free: $(free_mib) MiB"
