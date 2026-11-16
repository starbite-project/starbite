#!/usr/bin/env bash
# build/win/patchwork/sbglicd/build.sh [OUTDIR]: build sbglicd.dll (ARM64X, C only: the front of the system-wide
# OpenGL ICD, see sbglicd.c) in the lima VM "starbite" with the toolchain and link helpers of the sbumd / sbsel
# builds (~/sbmesa/tc, build-int-x: docs/WIN11-GPU.md 6.60), check it (dual-architecture structure, no initializer
# tables: 6.106, the same exports in both halves) and copy sbglicd.dll + sbglicd.map to OUTDIR (default
# build/win/gl-sys of this worktree). The build tree is ~/sbmesa/build-glsys-x/icd in the VM. Nothing touches a phone.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$(cd "$here/../.." && pwd)/gl-sys}
mkdir -p "$out"
want=$(shasum -a 256 "$here/sbglicd.c" | cut -c1-64)
limactl shell starbite bash -s "$here" "$out" "$want" "$HOME" <<'VM'
set -euo pipefail
here=$1; out=$2; want=$3; mac=$4
export PATH=$HOME/sbmesa/native:$HOME/sbmesa/venv/bin:$PATH
SB=$HOME/sbmesa; X=$SB/build-int-x
mkdir -p $SB/build-glsys-x/icd && cd $SB/build-glsys-x/icd
for i in 1 2 3 4 5 6 7 8 9 10; do   # the VM can see a rewritten file late
    [ "$(sha256sum "$here/sbglicd.c" | cut -c1-64)" = "$want" ] && break; sleep 1
done
[ "$(sha256sum "$here/sbglicd.c" | cut -c1-64)" = "$want" ] || { echo "the VM does not see sbglicd.c as written"; exit 1; }
cp "$here/sbglicd.c" "$here/sbglicd.def" .
cp $X/altnames_ec.obj $X/tlsindex_ec.obj $X/tlssup_a64.obj $X/verify_arm64x.py .
cp $SB/build-nir-x/check-init-tables.py .
# the verifier names sbumd's two exports; this copy checks every export of ours by name instead (the generic checks,
# "all native exports are ARM64" and "every EC export is an x64 thunk redirected to ARM64EC code", stay as they are)
names=$(awk 'f && NF {printf "\"%s\", ", $1} /^EXPORTS/ {f=1}' sbglicd.def)
sed -i "s/for n in (\"OpenAdapter10\", \"OpenAdapter10_2\"):/for n in ($names):/" verify_arm64x.py
[ "$(grep -c 'for n in ("DrvCopyContext"' verify_arm64x.py)" = 2 ] || { echo "verify_arm64x.py: the export list was not replaced"; exit 1; }
# the native half's exports: one /export directive per name of the .def (the .def itself exports the ARM64EC half)
awk 'f && NF {printf "#pragma comment(linker, \"/export:%s\")\n", $1} /^EXPORTS/ {f=1}' sbglicd.def > exports_icd_a64.c
$SB/tc/clang-cl /nologo /O2 /MT /GS- /W4 /WX /c sbglicd.c /Fosbglicd-a64.obj
$SB/tc/clang-cl /nologo /c exports_icd_a64.c /Foexports_icd_a64.obj
$SB/tc/clang-cl-ec /nologo /O2 /MT /GS- /W4 /WX /c sbglicd.c /Fosbglicd-ec.obj
L="/libpath:$mac/sbwin/msvc/lib/arm64 /libpath:$mac/sbwin/kit/Lib/10.0.26100.0/ucrt/arm64 /libpath:$mac/sbwin/kit/Lib/10.0.26100.0/um/arm64 /nodefaultlib:libucrt.lib /defaultlib:ucrt.lib"
lld-link-20 /machine:arm64x /nologo /dll /opt:ref /fixed:no /dynamicbase /nxcompat /incremental:no /Brepro \
    /out:sbglicd.dll /implib:sbglicd.lib /map:sbglicd.map /def:sbglicd.def $L \
    sbglicd-a64.obj exports_icd_a64.obj tlssup_a64.obj sbglicd-ec.obj $SB/tc/ec/asan_noop_ec.obj altnames_ec.obj tlsindex_ec.obj \
    kernel32.lib
python3 verify_arm64x.py sbglicd.dll --write-ec-view ecview.dll > verify.log 2>&1 || { tail -20 verify.log; echo "ARM64X structure check FAILED"; exit 1; }
tail -1 verify.log
python3 check-init-tables.py sbglicd.map
# both halves export the same names: the native view is the file as it is, the x64 view is verify's ecview.dll
exp() { llvm-objdump -p "$1" | sed -n '/Export Table/,/^$/p' | awk 'NR>4 && $3 {print $3}' | sort; }
exp sbglicd.dll > exports-a64.txt; exp ecview.dll > exports-ec.txt
awk 'f && NF {print $1} /^EXPORTS/ {f=1}' sbglicd.def | sort > exports-def.txt
cmp exports-def.txt exports-a64.txt && cmp exports-def.txt exports-ec.txt || { echo "the halves do not export the .def's names"; exit 1; }
echo "exports: $(wc -l < exports-def.txt) names, the same in the ARM64 and the x64 view"
llvm-objdump -p sbglicd.dll | grep 'DLL Name' | sort -u | awk '{printf "%s ", $3} END {print ""}'
cp sbglicd.dll sbglicd.map "$out/"
sha256sum "$out/sbglicd.dll" | cut -c1-16
VM
