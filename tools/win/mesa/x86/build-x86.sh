#!/bin/bash
# build-x86.sh: sbumd for 32-bit x86 programs (PE32, i686-pc-windows-msvc), the Mesa tree ~/sbmesa/src-x86
# (branch sb-x86 = 0167 b8730f0 + the sb-x86 commits). Run INSIDE the lima VM:
#   limactl shell starbite bash /Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/patchwork/build-x86.sh
# Same meson options as build-int-a64 / build-int-ec (d3d10umd target, softpipe + panfrost, no LLVM, /MT with the
# UCRT as a DLL), cross file ~/sbmesa/tc/cross-x86.ini (clang-cl 18 + lld-link 18, as the ARM64 half).
# Steps: meson setup (first time) -> ninja -j4 -> the link again as sbumd.dll with a map (build-x86-link) ->
# verify-x86.py. Touches only build-x86-umd and build-x86-link. Compiled only: nothing here has run on Windows.
set -euo pipefail
export PATH=$HOME/sbmesa/native:$HOME/sbmesa/venv/bin:$PATH
here=$(cd "$(dirname "$0")" && pwd)
SB=$HOME/sbmesa
B=$SB/build-x86-umd
L=$SB/build-x86-link
T=src/gallium/targets/d3d10umd/libgallium_d3d10.dll
OPTS="-Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt -Dplatforms=windows -Dgallium-d3d10umd=true -Dvulkan-drivers= -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dzstd=disabled -Dshader-cache=disabled -Dbuild-tests=false -Dexpat=disabled -Dzlib=disabled -Dgallium-drivers=softpipe,panfrost -Dmesa-clc=system -Dprecomp-compiler=system"
cd "$SB"
if [ ! -f "$B/build.ninja" ]; then
  meson setup "$B" src-x86 $OPTS --cross-file="$SB/tc/cross-x86.ini" > build-x86-umd.setup.log 2>&1 || { echo "setup failed: $SB/build-x86-umd.setup.log"; exit 1; }
fi
ninja -j4 -C "$B" $T > build-x86-umd.build.log 2>&1 || { echo "build failed: $SB/build-x86-umd.build.log"; exit 1; }

# The same link once more under the driver's name, with a map (the umdlog's abort and exception stacks are
# offsets in this file) and its own import library. Inputs and options are meson's, in meson's order.
mkdir -p "$L"
cd "$B"
ninja -t commands $T | tail -1 | tr ' ' '\n' | grep -v '^lld-link$\|^$' |
  sed -e "s|^/OUT:.*|/OUT:$L/sbumd.dll|" -e "s|^/IMPLIB:.*|/IMPLIB:$L/sbumd.lib|" > "$L/link-x86.rsp"
echo "/MAP:$L/sbumd.map" >> "$L/link-x86.rsp"
lld-link @"$L/link-x86.rsp" 2>&1 | tee "$L/link-x86.log"
python3 "$here/verify-x86.py" "$L/sbumd.dll" "$L/sbumd.map" | tee "$L/verify.txt"
(cd "$SB/src-x86" && echo "source: $(git rev-parse --short HEAD) $(git status --short | wc -l) uncommitted") | tee -a "$L/verify.txt"
sha256sum "$L/sbumd.dll" "$L/sbumd.map" | tee -a "$L/verify.txt"
