#!/usr/bin/env bash
# starbite/win/sbsel/build.sh [OUTDIR]: build sbsel.dll (ARM64X: one file for ARM64 and for x64-on-ARM processes)
# in the lima VM "starbite", with the toolchain and link helpers of the sbumd build (~/sbmesa/tc, build-int-x:
# docs/WIN11-GPU.md 6.60), check it (dual-architecture structure, no initializer tables: 6.106) and copy
# sbsel.dll + sbsel.map to OUTDIR (default build/win/sbsel in the main checkout). Nothing touches the phone.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
main=$(cd "$here" && cd "$(git rev-parse --git-common-dir)/.." && pwd)
out=${1:-$main/build/win/sbsel}
mkdir -p "$out"
want=$(shasum -a 256 "$here/sbsel.c" | cut -c1-64)
limactl shell starbite bash -s "$here" "$out" "$want" "$HOME" <<'VM'
set -euo pipefail
here=$1; out=$2; want=$3; mac=$4
export PATH=$HOME/sbmesa/native:$HOME/sbmesa/venv/bin:$PATH
SB=$HOME/sbmesa; X=$SB/build-int-x
mkdir -p $SB/build-sel && cd $SB/build-sel
for i in 1 2 3 4 5 6 7 8 9 10; do   # the VM can see a rewritten file late
    [ "$(sha256sum "$here/sbsel.c" | cut -c1-64)" = "$want" ] && break; sleep 1
done
[ "$(sha256sum "$here/sbsel.c" | cut -c1-64)" = "$want" ] || { echo "the VM does not see sbsel.c as written"; exit 1; }
cp "$here/sbsel.c" "$here/sbsel.def" .
cp $X/altnames_ec.obj $X/tlsindex_ec.obj $X/tlssup_a64.obj $X/verify_arm64x.py .
cp $SB/build-nir-x/check-init-tables.py .
printf '#pragma comment(linker, "/export:OpenAdapter10")\n#pragma comment(linker, "/export:OpenAdapter10_2")\n#pragma comment(linker, "/export:SbSelWhich")\n' > exports_sel_a64.c
$SB/tc/clang-cl /nologo /O2 /MT /GS- /W4 /WX /c sbsel.c /Fosbsel-a64.obj
$SB/tc/clang-cl /nologo /c exports_sel_a64.c /Foexports_sel_a64.obj
$SB/tc/clang-cl-ec /nologo /O2 /MT /GS- /W4 /WX /c sbsel.c /Fosbsel-ec.obj
L="/libpath:$mac/sbwin/msvc/lib/arm64 /libpath:$mac/sbwin/kit/Lib/10.0.26100.0/ucrt/arm64 /libpath:$mac/sbwin/kit/Lib/10.0.26100.0/um/arm64 /nodefaultlib:libucrt.lib /defaultlib:ucrt.lib"
lld-link-20 /machine:arm64x /nologo /dll /opt:ref /fixed:no /dynamicbase /nxcompat /incremental:no /Brepro \
    /out:sbsel.dll /implib:sbsel.lib /map:sbsel.map /def:sbsel.def $L \
    sbsel-a64.obj exports_sel_a64.obj tlssup_a64.obj sbsel-ec.obj $SB/tc/ec/asan_noop_ec.obj altnames_ec.obj tlsindex_ec.obj \
    kernel32.lib advapi32.lib
python3 verify_arm64x.py sbsel.dll > verify.log 2>&1 || { tail -20 verify.log; echo "ARM64X structure check FAILED"; exit 1; }
tail -1 verify.log
python3 check-init-tables.py sbsel.map
llvm-objdump -p sbsel.dll | sed -n '/Export Table/,/^$/p' | awk 'NR>4 && $3 {print "export " $3}'
llvm-objdump -p sbsel.dll | grep 'DLL Name' | sort -u | awk '{printf "%s ", $3} END {print ""}'
cp sbsel.dll sbsel.map "$out/"
sha256sum "$out/sbsel.dll" | cut -c1-16
VM
