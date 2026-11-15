#!/bin/sh
# Host test of the Mali driver's buffer cache (pending patch 0459).   tools/macos/sbmcachetest/run.sh XNU_TREE [BUILD_DIR]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$1
OUT=${2:-${TMPDIR:-/tmp}/sb-sbmcachetest}
mkdir -p "$OUT"
python3 "$HERE/extract.py" "$TREE/iokit/Kernel/arm/StarbiteMali.cpp" "$OUT/cache_cut.inc"
clang++ -std=c++17 -O1 -g -fsanitize=${SAN:-undefined} -Wall -Wno-unused-function -I "$OUT" "$HERE/cachetest.cpp" -o "$OUT/cachetest"
"$OUT/cachetest" 4 3
"$OUT/cachetest" 3 1
