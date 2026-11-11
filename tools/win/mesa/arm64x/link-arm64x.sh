#!/bin/bash
# Links the ARM64X sbumd from build-umd-a64 (ARM64 objects, clang 18) and build-umd-ec (ARM64EC objects, clang 20)
# with lld-link 20.1.2. Run in ~/sbmesa/build-umd-x. See BUILD.md for why each workaround object exists.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/sbmesa/native:$HOME/sbmesa/venv/bin:$PATH
SB=$HOME/sbmesa; X=$PWD
T=src/gallium/targets/d3d10umd/libgallium_d3d10.dll
LIBCMT=/Users/dev/sbwin/msvc/lib/arm64/libcmt.lib

# workaround objects
clang-20 --target=arm64ec-pc-windows-msvc -c altnames_ec.S -o altnames_ec.obj
$SB/tc/clang-cl-ec /nologo /O2 /GS- /c tlsindex_ec.c /Fotlsindex_ec.obj
$SB/tc/clang-cl /nologo /c exports_a64.c /Foexports_a64.obj
rm -rf x && mkdir x && (cd x && llvm-ar-20 x $LIBCMT "$(llvm-ar-20 t $LIBCMT | grep "arm64.tlssup\.obj\$")" && mv ./*tlssup.obj ../tlssup_a64.obj) && rmdir x

# the objects and archives of each meson link, in meson order, without the wholearchive one
inputs() { ninja -C $SB/$1 -t commands $T | tail -1 | tr " " "\n" | grep "\.obj\$\|\.a\$" | grep -v "^/WHOLEARCHIVE\|asan_noop" | awk "!s[\$0]++" | sed "s|^|$SB/$1/|"; }
inputs build-umd-a64 > objs-a64.txt
inputs build-umd-ec > objs-ec.txt
{
echo /machine:arm64x /nologo /dll /opt:ref /fixed:no /dynamicbase /nxcompat /incremental:no
echo /out:sbumd-arm64x.dll /implib:sbumd-arm64x.lib /map:sbumd-arm64x.map
echo /def:$SB/build-umd-ec/src/gallium/targets/d3d10umd/d3d10.def
echo /libpath:/Users/dev/sbwin/msvc/lib/arm64 /libpath:/Users/dev/sbwin/kit/Lib/10.0.26100.0/ucrt/arm64 /libpath:/Users/dev/sbwin/kit/Lib/10.0.26100.0/um/arm64
echo /nodefaultlib:libucrt.lib /defaultlib:ucrt.lib
# ARM64 half first, in the plain ARM64 link order (d3d10_gdi.c.obj, whole libd3d10umd.a, the rest)
head -1 objs-a64.txt
echo $X/exports_a64.obj
echo /wholearchive:$SB/build-umd-a64/src/gallium/frontends/d3d10umd/libd3d10umd.a
tail -n +2 objs-a64.txt
# the ARM64 tlssup.obj before any ARM64EC input: its _tls_start / _tls_end / _tls_used must come first
echo $X/tlssup_a64.obj
echo /wholearchive:$SB/build-umd-ec/src/gallium/frontends/d3d10umd/libd3d10umd.a
cat objs-ec.txt
echo $SB/tc/ec/asan_noop_ec.obj $X/altnames_ec.obj $X/tlsindex_ec.obj
echo ws2_32.lib synchronization.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
} > link-x.rsp
lld-link-20 @link-x.rsp 2>&1 | tee link-x.log
python3 verify_arm64x.py sbumd-arm64x.dll $SB/build-umd-a64/$T --write-ec-view ecview.dll | tee verify.txt
