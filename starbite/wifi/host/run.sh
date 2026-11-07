#!/bin/sh
# bcmw's host test: builds starbite/wifi's core with host/model.c and host/test.c and runs it.
#   sh starbite/wifi/host/run.sh [dir]     dir: the phone's Wi-Fi files (bcmdhd_sta.bin_b2,
#                                          nvram.txt_*, bcmdhd_clm.blob), read only; default: the
#                                          first of build/refport/.../firmware/wifi, build/linux-bundle/firmware
#                                          found in this checkout or the main one. Never in git.
#   BCMW_LINUX=<brcmfmac dir>              also compare the NVRAM processing byte for byte with
#                                          Linux's brcmfmac parser (firmware.c), compiled from there
#   CC=gcc, OUT=dir                        compiler, output directory (default /tmp-free: $here/out)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
root=$(cd "$here/../../.." && pwd)
: "${CC:=cc}"
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
dir=${1:-}
if [ -z "$dir" ]; then
    for c in "$root/build/refport/refport-starlte/firmware/wifi" "$root/../../../build/refport/refport-starlte/firmware/wifi" \
             "$root/build/linux-bundle/firmware" "$root/../../../build/linux-bundle/firmware"; do
        if [ -f "$c/bcmdhd_sta.bin_b2" ]; then dir=$c; break; fi
    done
fi
flags="-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -I$core"
src="$core/core.c $core/exynos_pcie.c $core/chip.c $core/fw.c $core/ring.c $core/msgbuf.c $here/model.c $here/test.c"
extra=""
if [ -n "${BCMW_LINUX:-}" ] && [ -f "$BCMW_LINUX/firmware.c" ]; then
    # Linux's parser, from firmware.c's start to the end of brcmf_fw_nvram_strip(), on a small shim
    awk '/^static void \*brcmf_fw_nvram_strip/{f=1} {print} f&&/^}/{exit}' "$BCMW_LINUX/firmware.c" |
        grep -v '^#include' > "$OUT/linux-nvram.inc"
    # Linux's code is built as it is: its warnings are not ours
    $CC -std=gnu11 -O1 -g -w -fsanitize=address,undefined -I"$OUT" -c "$here/golden.c" -o "$OUT/golden.o"
    extra="-DBCMW_GOLDEN $OUT/golden.o"
fi
# the core alone must build freestanding, without a C library (as in libsb and a Windows driver)
$CC -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -nostdlib -c -I"$core" \
    "$core/core.c" "$core/exynos_pcie.c" "$core/chip.c" "$core/fw.c" "$core/ring.c" "$core/msgbuf.c" -o /dev/null 2>/dev/null ||
    for f in core exynos_pcie chip fw ring msgbuf; do
        $CC -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -c -I"$core" "$core/$f.c" -o "$OUT/$f.o"
    done
# shellcheck disable=SC2086
$CC $flags $extra -o "$OUT/bcmw-test" $src
if [ -n "$dir" ]; then
    echo "bcmw: the phone's files from $dir (read only)"
    "$OUT/bcmw-test" "$dir"
else
    echo "bcmw: no directory of the phone's files: synthetic files only"
    "$OUT/bcmw-test"
fi
