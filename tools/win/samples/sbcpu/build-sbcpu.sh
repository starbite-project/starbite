#!/usr/bin/env bash
# Build sbcpu.exe for ARM64, x64 and x86 (tools/win/samples/sbcpu/sbcpu.c): freestanding, kernel32 + advapi32 only.
#   tools/win/samples/sbcpu/build-sbcpu.sh OUTDIR
# Import libraries come from small .def files through llvm-dlltool, so no x64/x86 SDK libraries are needed.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../env.sh"
out=${1:?usage: build-sbcpu.sh OUTDIR}
mkdir -p "$out"
llvm=$(dirname "$SB_CLANG_CL")
id="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo nogit)$(git -C "$here" diff --quiet HEAD -- . 2>/dev/null || echo +)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
CloseHandle
CreateFileA
ExitProcess
GetActiveProcessorCount
GetCommandLineA
GetCurrentProcess
GetCurrentProcessorNumber
GetCurrentThread
GetLastError
GetLogicalProcessorInformationEx
GetMaximumProcessorCount
GetNativeSystemInfo
GetProcessAffinityMask
GetStdHandle
GetSystemInfo
IsProcessorFeaturePresent
QueryPerformanceCounter
QueryPerformanceFrequency
SetThreadAffinityMask
Sleep
WriteFile
EOF
cat > "$tmp/advapi32.def" <<'EOF'
LIBRARY advapi32.dll
EXPORTS
RegCloseKey
RegOpenKeyExA
RegQueryValueExA
EOF
for arch in arm64 x64 x86; do
    case $arch in
    arm64) tgt=aarch64-pc-windows-msvc; mach=arm64; dt=arm64; ent=start ;;
    x64)   tgt=x86_64-pc-windows-msvc;  mach=x64;   dt=i386:x86-64; ent=start ;;
    x86)   tgt=i686-pc-windows-msvc;    mach=x86;   dt=i386; ent=start ;;
    esac
    for d in kernel32 advapi32; do
        if [ $arch = x86 ]; then
            # stdcall decoration for x86: name@argbytes; llvm-dlltool -k strips it for the DLL import name
            python3 - "$tmp/$d.def" "$tmp/$d-x86.def" <<'PY'
import sys
args = {'CloseHandle':4,'CreateFileA':28,'ExitProcess':4,'GetActiveProcessorCount':4,'GetCommandLineA':0,
 'GetCurrentProcess':0,'GetCurrentProcessorNumber':0,'GetCurrentThread':0,'GetLastError':0,
 'GetLogicalProcessorInformationEx':12,'GetMaximumProcessorCount':4,'GetNativeSystemInfo':4,
 'GetProcessAffinityMask':12,'GetStdHandle':4,'GetSystemInfo':4,'IsProcessorFeaturePresent':4,
 'QueryPerformanceCounter':4,'QueryPerformanceFrequency':4,'SetThreadAffinityMask':8,'Sleep':4,'WriteFile':20,
 'RegCloseKey':4,'RegOpenKeyExA':20,'RegQueryValueExA':24}
o = []
for l in open(sys.argv[1]):
    w = l.strip()
    o.append(f'{w}@{args[w]}\n' if w in args else l)
open(sys.argv[2], 'w').writelines(o)
PY
            "$llvm/llvm-dlltool" -m i386 -k -d "$tmp/$d-x86.def" -l "$tmp/$d-$arch.lib"
        else
            "$llvm/llvm-dlltool" -m "$dt" -d "$tmp/$d.def" -l "$tmp/$d-$arch.lib"
        fi
    done
    "$SB_CLANG_CL" --target=$tgt /nologo /O2 /GS- /Zl /W3 -Wno-unused-parameter /utf-8 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
        "-DSB_BUILD_ID=\"$id\"" -imsvc "$SBWIN_MSVC/include" -imsvc "$SBWIN_INC/shared" -imsvc "$SBWIN_INC/um" -imsvc "$SBWIN_INC/ucrt" \
        /c -Fo"$tmp/sbcpu-$arch.obj" -- "$here/sbcpu.c"
    "$SB_LLD_LINK" /nologo /machine:$mach /subsystem:console /entry:$ent /nodefaultlib /Brepro /out:"$out/sbcpu-$arch.exe" \
        "$tmp/sbcpu-$arch.obj" "$tmp/kernel32-$arch.lib" "$tmp/advapi32-$arch.lib"
done
( cd "$out" && shasum -a 256 sbcpu-*.exe )
