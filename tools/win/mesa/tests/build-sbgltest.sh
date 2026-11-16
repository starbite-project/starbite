#!/usr/bin/env bash
# build/win/patchwork/sbgltest-build.sh [OUTDIR]: build sbgltest.c (the system-OpenGL test, plain C) three times in
# the lima VM "starbite": sbgltest-arm64.exe (native), sbgltest-arm64ec.exe (ARM64EC: an x64 process made of ARM64
# code) and sbgltest-x64.exe (real x64 code, emulated on the phone: what an x64 game is). Toolchain as
# tools/win/mesa/tests/build-test.sh (~/sbmesa/tc wrappers, the kit in ~/sbwin); the ARM64EC link is lld-link 20 with
# the flags of ~/sbmesa/tc/cross-ec.ini. OUTDIR defaults to build/win/gl-sys of this worktree. Nothing touches a phone.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$(cd "$here/.." && pwd)/gl-sys}
mkdir -p "$out"
want=$(shasum -a 256 "$here/sbgltest.c" | cut -c1-64)
limactl shell starbite bash -s "$here" "$out" "$want" "$HOME" <<'VM'
set -euo pipefail
here=$1; out=$2; want=$3; mac=$4
SB=$HOME/sbmesa; K=$mac/sbwin
mkdir -p $SB/build-glsys-x/test && cd $SB/build-glsys-x/test
for i in 1 2 3 4 5 6 7 8 9 10; do   # the VM can see a rewritten file late
    [ "$(sha256sum "$here/sbgltest.c" | cut -c1-64)" = "$want" ] && break; sleep 1
done
[ "$(sha256sum "$here/sbgltest.c" | cut -c1-64)" = "$want" ] || { echo "the VM does not see sbgltest.c as written"; exit 1; }
cp "$here/sbgltest.c" .
C="/nologo /O2 /MT /W4 -Wno-deprecated-declarations -D_CRT_SECURE_NO_WARNINGS /c sbgltest.c"
libs() { echo "/libpath:$K/msvc/lib/$1 /libpath:$K/kit/Lib/10.0.26100.0/um/$1 /libpath:$K/kit/Lib/10.0.26100.0/ucrt/$1"; }
L="/nologo /subsystem:console /opt:ref /dynamicbase /nxcompat /incremental:no /Brepro"
$SB/tc/clang-cl $C /Fot-arm64.obj
lld-link-20 /machine:arm64 $L $(libs arm64) /out:sbgltest-arm64.exe t-arm64.obj
$SB/tc/clang-cl-ec $C /Fot-arm64ec.obj
lld-link-20 /machine:arm64ec $L $(libs arm64) /out:sbgltest-arm64ec.exe t-arm64ec.obj
$SB/tc/x64/clang-cl $C /Fot-x64.obj
lld-link-20 /machine:x64 $L $(libs x64) /out:sbgltest-x64.exe t-x64.obj
for f in sbgltest-arm64.exe sbgltest-arm64ec.exe sbgltest-x64.exe; do
    m=$(python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(1024); print(hex(struct.unpack_from("<H",d,struct.unpack_from("<I",d,0x3c)[0]+4)[0]))' $f)
    echo "$f: PE machine $m, $(llvm-readobj --file-headers $f | awk '/Format:/ {print $2}'), imports $(llvm-objdump -p $f | awk '/DLL Name/ {printf "%s ", $3}')"
    cp $f "$out/"
done
(cd "$out" && sha256sum sbgltest-*.exe)
VM
