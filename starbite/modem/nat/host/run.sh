#!/bin/sh
# sbnat's host test: builds starbite/modem/nat (the guest NAT) with host/test.c under ASan/UBSan and runs it.
#   sh starbite/modem/nat/host/run.sh
#   CC=cc, OUT=dir      compiler, output directory (default: $here/out)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
: "${CC:=cc}"
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
# the library must build freestanding, without a C library
$CC -std=c99 -O2 -Wall -Wextra -Werror -Wshadow -Wcast-align -ffreestanding -fno-builtin -c -I"$core" \
    "$core/sbnat.c" -o "$OUT/sbnat.o"
echo "freestanding build: ok"
# and for the phone: bare-metal aarch64 as libsb builds it (strict alignment, no FP/SIMD registers)
if $CC --target=aarch64-none-elf -E -x c /dev/null >/dev/null 2>&1; then
    $CC --target=aarch64-none-elf -std=c99 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mstrict-align \
        -mgeneral-regs-only -nostdinc -isystem "$($CC -print-resource-dir)/include" -c -I"$core" "$core/sbnat.c" \
        -o "$OUT/sbnat-aarch64.o"
    echo "freestanding aarch64 build: ok"
fi
$CC -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I"$core" \
    -o "$OUT/sbnat-test" "$core/sbnat.c" "$here/test.c"
"$OUT/sbnat-test"
