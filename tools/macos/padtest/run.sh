#!/bin/sh
# Host test of the XNU touch driver's trackpad gestures (pending patch 0460) and the keyboard summon swipe.
#   tools/macos/padtest/run.sh XNU_TREE [BUILD_DIR]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$1
OUT=${2:-${TMPDIR:-/tmp}/sb-padtest}
mkdir -p "$OUT"
python3 "$HERE/extract.py" "$TREE/iokit/Kernel/arm/hid/SweetHID.cpp" "$OUT/pad_cut.inc"
clang++ -std=c++17 -O1 -g -fsanitize=${SAN:-undefined} -Wall -Wno-unused-function -Wno-unused-variable -I "$TREE/iokit/Kernel/arm" -I "$OUT" "$HERE/padtest.cpp" -o "$OUT/padtest"
for mode in 0 1 2 3; do "$OUT/padtest" $mode; done
