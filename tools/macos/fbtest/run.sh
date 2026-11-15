#!/bin/sh
# Host test of the XNU display driver's present path (pending patch 0457): frames, the composed pointer, the
# on-screen keyboard's rows. The code under test is cut from the tree's own SweetFramebuffer.cpp.
#   tools/macos/fbtest/run.sh XNU_TREE [BUILD_DIR]     (XNU_TREE: the xnu sources with the series applied)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$1
OUT=${2:-${TMPDIR:-/tmp}/sb-fbtest}
mkdir -p "$OUT"
python3 "$HERE/extract.py" "$TREE/iokit/Kernel/arm/SweetFramebuffer.cpp" "$OUT/fb_cut.inc"
clang++ -std=c++17 -O2 -g -fsanitize=address,undefined -Wno-everything -I "$TREE/iokit/Kernel/arm" -I "$OUT" \
	"$HERE/fbtest.cpp" -o "$OUT/fbtest"
"$OUT/fbtest" 360 740 360 740 3000 1 32          # 1:1 canvas (the 2x mode's path)
"$OUT/fbtest" 360 740 207 448 3000 2 32          # scaled canvas (the 828-wide mode's path)
"$OUT/fbtest" 300 620 300 620 3000 11 40
"$OUT/fbtest" 300 620 173 357 3000 12 24
