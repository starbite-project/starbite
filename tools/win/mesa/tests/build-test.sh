#!/usr/bin/env bash
# build-test.sh NAME [OUTDIR]: build tools/win/mesa/tests/NAME.cpp as an ARM64 Windows program (NAME.exe) with the
# VM's clang-cl (~/sbmesa/tc/clang-cl) against the kit in ~/sbwin (tools/win/env.sh), as sbcomp/sbdrag/sbshow were.
# OUTDIR defaults to build/win/tests in the main checkout.
set -euo pipefail
name=${1:?usage: build-test.sh NAME [OUTDIR]}
src=$(cd "$(dirname "$0")" && pwd)/$name.cpp
out=${2:-/Users/dev/project-starbite/build/win/tests}
[ -f "$src" ] || { echo "no $src"; exit 1; }
mkdir -p "$out"
cp "$src" "$out/$name.cpp"
K=$HOME/sbwin
limactl shell starbite -- bash -c "cd '$out' && ~/sbmesa/tc/clang-cl /nologo /O2 /EHsc /MT -Wno-deprecated-declarations -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /Fe:$name.exe $name.cpp /link /LIBPATH:$K/msvc/lib/arm64 /LIBPATH:$K/kit/Lib/10.0.26100.0/um/arm64 /LIBPATH:$K/kit/Lib/10.0.26100.0/ucrt/arm64 2>&1 | grep -E 'error|warning' | head -30; rm -f $name.obj $name.cpp"
file "$out/$name.exe"; shasum -a 256 "$out/$name.exe"
