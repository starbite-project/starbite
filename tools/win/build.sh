#!/usr/bin/env bash
# Build Windows 11 ARM64 binaries on the Mac with clang-cl + lld-link (tools/win/README.md).
#
#   tools/win/build.sh kmdf OUT.sys SRC.c...   KMDF 1.33 kernel driver (entry FxDriverEntry -> DriverEntry)
#   tools/win/build.sh wdm  OUT.sys SRC.c...   plain WDM kernel driver  (entry GsDriverEntry -> DriverEntry)
#   tools/win/build.sh umdf OUT.dll SRC.c...   UMDF 2.33 driver DLL (IDDCX=1.9 adds IddCx headers + stub lib)
#   tools/win/build.sh exe  OUT.exe SRC.c...   console program (static vcruntime, in-box ucrtbase.dll)
#   tools/win/build.sh dll  OUT.dll SRC.c...   plain user-mode DLL (same CRT choice as exe)
#
# Extra flags from the environment: CFLAGS (C and C++), CXXFLAGS (C++ only), LDFLAGS (linker), LIBS (more .lib names or paths),
# BUILD_ID (default: git short hash + UTC time; passed as -DSB_BUILD_ID="..."), DEBUG=1 (/Od /Z7 + /debug).
# Objects go next to OUT in OUT.obj/. (Sources are passed after "--": clang-cl reads /Users/... as /U.) A .rc file among the sources is compiled with llvm-rc.
# Nothing here signs: run tools/win/sign.sh on the result.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
[ $# -ge 3 ] || { sed -n '2,13p' "$0"; exit 2; }
kind=$1 out=$2; shift 2
objdir=$out.obj
mkdir -p "$objdir" "$(dirname "$out")"

if [ -z "${BUILD_ID:-}" ]; then
    BUILD_ID="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo nogit)$(git -C "$here" diff --quiet HEAD 2>/dev/null || echo +)-$(date -u +%Y%m%dT%H%M%SZ)"
fi

# Common: aarch64 MSVC ABI (x18 reserved for the TEB/KPCR by the target), C17, MS extensions, warnings as on WDK.
cf=(--target=aarch64-pc-windows-msvc -fms-compatibility-version=19.44 /nologo /W4 /Zc:wchar_t /Zc:inline
    /Gy /Gw /GF /Brepro /utf-8 -Wno-unused-parameter -Wno-microsoft-anon-tag -Wno-ignored-pragma-intrinsic
    -Wno-pragma-pack -Wno-unknown-pragmas -Wno-nonportable-include-path -Wno-sizeof-array-div
    -Wno-missing-field-initializers
    -D_ARM64_ -DARM64 -D_WIN64 -D_USE_DECLSPECS_FOR_SAL=1 -DSTD_CALL
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=$SB_NTDDI "-DSB_BUILD_ID=\"$BUILD_ID\"")
lf=(/nologo /machine:arm64 /Brepro /opt:ref /opt:icf /release /version:10.0 /osversion:10.0)
if [ "${DEBUG:-0}" = 1 ]; then cf+=(/Od /Z7); lf+=(/debug /pdbaltpath:%_PDB%); else cf+=(/O2 /Oi); fi
libs=()
inc_um=(-imsvc "$SBWIN_MSVC/include" -imsvc "$SBWIN_INC/ucrt" -imsvc "$SBWIN_INC/shared" -imsvc "$SBWIN_INC/um"
        -imsvc "$SBWIN_INC/winrt")
lib_um=("/libpath:$SBWIN_MSVC/lib/arm64" "/libpath:$SBWIN_LIB/ucrt/arm64" "/libpath:$SBWIN_LIB/um/arm64")
# User mode CRT: static vcruntime/startup code (libcmt + libvcruntime) but the UCRT from the in-box
# ucrtbase.dll ("hybrid CRT"): no vcruntime140.dll needed on the phone, small binaries.
crt_um=(/nodefaultlib:libucrt.lib ucrt.lib libcmt.lib libvcruntime.lib)

case $kind in
kmdf|wdm)
    # /kernel: no RTTI/EH, _KERNEL_MODE; drivers link nothing from the user-mode CRT.
    cf+=(/kernel /GS -D_KERNEL_MODE=1 -DWINNT=1 -DDEPRECATE_DDK_FUNCTIONS=1 -DPOOL_NX_OPTIN=1
         -DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1 -imsvc "$SBWIN_INC/km/crt" -imsvc "$SBWIN_INC/km" -imsvc "$SBWIN_INC/shared")
    # Section flags as link.exe /driver gives them (checked against the WDK-built Exynos9810Ufs.sys):
    # resident code/data NOT_PAGED, INIT executable + discardable. lld replaces all permission bits of a
    # section named here, so each is spelled out in full.
    lf+=(/driver /subsystem:native,10.00 /nodefaultlib /tsaware:no /section:.text,ERP /section:.rdata,RP
         /section:.data,RWP /section:.pdata,RP /section:INIT,ERD /section:PAGE,ER "/libpath:$SBWIN_LIB/km/arm64")
    # _load_config_used (from bufferoverflowfastfailk.lib) gives the kernel loader the address of
    # __security_cookie to seed; link.exe pulls it in by itself, lld-link only when asked. Without it the
    # cookie keeps its default and __security_init_cookie fast-fails (brk #0xf003) on load.
    lf+=(/include:_load_config_used)
    libs+=(bufferoverflowfastfailk.lib ntoskrnl.lib hal.lib wmilib.lib)
    if [ "$kind" = kmdf ]; then
        cf+=(-DKMDF_VERSION_MAJOR=1 -DKMDF_VERSION_MINOR=$SB_KMDF_MINOR -DKMDF_MINIMUM_VERSION_REQUIRED=$SB_KMDF_MINOR
             -imsvc "$SBWIN_WDF_INC/kmdf/1.$SB_KMDF_MINOR")
        lf+=(/entry:FxDriverEntry "/libpath:$SBWIN_WDF_LIB/kmdf/arm64/1.$SB_KMDF_MINOR")
        libs+=(wdfldr.lib wdfdriverentry.lib)
    else
        lf+=(/entry:GsDriverEntry)
    fi ;;
umdf)
    cf+=(/MT /GS -D_WINDLL -DUNICODE -D_UNICODE -DUMDF_USING_NTSTATUS -DUMDF_VERSION_MAJOR=2
         -DUMDF_VERSION_MINOR=$SB_UMDF_MINOR -DUMDF_MINIMUM_VERSION_REQUIRED=$SB_UMDF_MINOR
         -imsvc "$SBWIN_WDF_INC/umdf/2.$SB_UMDF_MINOR" "${inc_um[@]}")
    lf+=(/dll /subsystem:windows,10.00 "${lib_um[@]}" "/libpath:$SBWIN_WDF_LIB/umdf/arm64/2.$SB_UMDF_MINOR")
    libs+=(WdfDriverStubUm.lib ntdll.lib onecoreuap.lib "${crt_um[@]}")
    if [ -n "${IDDCX:-}" ]; then
        cf+=(-DIDDCX_VERSION_MAJOR=${IDDCX%%.*} -DIDDCX_VERSION_MINOR=${IDDCX#*.}
             -DIDDCX_MINIMUM_VERSION_REQUIRED=${IDDCX#*.} -imsvc "$SBWIN_INC/um/iddcx/$IDDCX")
        libs+=("$SBWIN_LIB/um/arm64/iddcx/$IDDCX/iddcxstub.lib")
    fi ;;
exe|dll)
    cf+=(/MT /GS /guard:cf -DUNICODE -D_UNICODE "${inc_um[@]}")
    lf+=("${lib_um[@]}" /guard:cf)
    [ "$kind" = exe ] && lf+=(/subsystem:console,10.00) || { cf+=(-D_WINDLL); lf+=(/dll /subsystem:windows,10.00); }
    libs+=(kernel32.lib advapi32.lib ntdll.lib "${crt_um[@]}") ;;
*) echo "unknown kind $kind"; exit 2 ;;
esac

objs=()
for src in "$@"; do
    b=$(basename "${src%.*}")
    case $src in
    *.rc)  "$SB_LLVM_RC" /nologo /I "$SBWIN_INC/um" /I "$SBWIN_INC/shared" /fo "$objdir/$b.res" "$src"
           objs+=("$objdir/$b.res") ;;
    *.c)   "$SB_CLANG_CL" "${cf[@]}" /std:c17 ${CFLAGS:-} /c "/Fo$objdir/$b.obj" -- "$src"
           objs+=("$objdir/$b.obj") ;;
    *.cpp|*.cc)   # C++17; exceptions only in user mode (/kernel turns them off)
           x=(/std:c++17); case $kind in kmdf|wdm) ;; *) x+=(/EHsc) ;; esac
           "$SB_CLANG_CL" "${cf[@]}" "${x[@]}" ${CXXFLAGS:-} ${CFLAGS:-} /c "/Fo$objdir/$b.obj" -- "$src"
           objs+=("$objdir/$b.obj") ;;
    *.obj|*.lib) objs+=("$src") ;;
    *) echo "unknown source $src"; exit 2 ;;
    esac
done
# shellcheck disable=SC2086
"$SB_LLD_LINK" "${lf[@]}" ${LDFLAGS:-} "/out:$out" "${objs[@]}" "${libs[@]}" ${LIBS:-}
echo "$out ($(wc -c < "$out" | tr -d ' ') bytes, build $BUILD_ID)"
