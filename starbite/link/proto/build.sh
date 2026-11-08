#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Builds and runs the SBLINK v1 core host tests (test.c) on the Mac:
#   1. ASan + UBSan build (-O1 -g), run: correctness + fuzz, any sanitizer report aborts
#   2. -O2 build, run: the same tests with realistic timings
#   3. compile-only check of sblink.c + lz4.c for Windows ARM64 (aarch64-pc-windows-msvc) when the
#      shared kit's headers are there (tools/win/README.md), so the core stays portable
# Output: build/link/proto/. Usage: starbite/link/proto/build.sh [--no-run]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out="$root/build/link/proto"
mkdir -p "$out"
CC=${CC:-clang}
warn="-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror"

# lz4.c is third-party: built with its own (quieter) flags
$CC -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -c "$here/lz4/lz4.c" -o "$out/lz4-san.o"
$CC $warn -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -c "$here/sblink.c" -o "$out/sblink-san.o"
$CC -std=c99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -c "$here/test.c" -o "$out/test-san.o"
$CC -fsanitize=address,undefined "$out/test-san.o" "$out/sblink-san.o" "$out/lz4-san.o" -o "$out/sblink-test-san"

$CC -std=c99 -O2 -c "$here/lz4/lz4.c" -o "$out/lz4.o"
$CC $warn -O2 -c "$here/sblink.c" -o "$out/sblink.o"
$CC -std=c99 -Wall -Wextra -Werror -O2 -c "$here/test.c" -o "$out/test.o"
$CC "$out/test.o" "$out/sblink.o" "$out/lz4.o" -o "$out/sblink-test"

kit=${SBWIN:-$HOME/sbwin}/kit/Include/10.0.26100.0
msvc=${SBWIN:-$HOME/sbwin}/msvc/include
if [ -d "$kit/ucrt" ] && [ -d "$msvc" ]; then
    for f in sblink.c lz4/lz4.c; do
        o="$out/win-$(basename "$f" .c).obj"
        $CC --target=aarch64-pc-windows-msvc -std=c99 -O2 -ffreestanding -fms-extensions -Wall -Werror \
            -Wno-unknown-pragmas -isystem "$kit/ucrt" -isystem "$msvc" -isystem "$kit/shared" -isystem "$kit/um" \
            -c "$here/$f" -o "$o"
    done
    echo "windows arm64 compile check: ok ($out/win-sblink.obj, $out/win-lz4.obj)"
else
    echo "windows arm64 compile check: skipped (no kit at ${SBWIN:-$HOME/sbwin})"
fi

[ "${1:-}" = "--no-run" ] && exit 0
echo "== ASan + UBSan =="
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "$out/sblink-test-san"
echo "== -O2 =="
"$out/sblink-test"
