#!/bin/sh
# sbm's host test: builds starbite/modem's core with host/fakecp.c and host/test.c and runs it.
#   sh starbite/modem/host/run.sh [RADIO.img]   the phone's RADIO image (read only: its TOC, memory map and
#                                               size); default: backup/*/RADIO.img in this checkout or the
#                                               main one, if present. Never in git.
#   SBM_LNK=<dmesg>...                          also decode the "LNK-RX/TX" frames of captured kernel logs
#                                               (default: build/test-boots/*/dmesg*.txt, last_kmsg*.txt)
#   CC=cc, OUT=dir                              compiler, output directory (default: $here/out)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
root=$(cd "$here/../../.." && pwd)
: "${CC:=cc}"
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
radio=${1:-}
if [ -z "$radio" ]; then
    for c in "$root"/backup/*/RADIO.img "$root"/../../../backup/*/RADIO.img; do
        if [ -f "$c" ]; then radio=$c; break; fi
    done
fi
src="$core/sbm_util.c $core/sbm_toc.c $core/sbm_sipc5.c $core/sbm_sbd.c $core/sbm_ipc.c $core/sbm_cell.c $core/sbm_probe.c $core/sbm_probe2.c $core/sbm_boot.c"
# the core alone must build freestanding, without a C library (libsb images, an XNU kext)
for f in $src; do
    $CC -std=c99 -O2 -Wall -Wextra -Werror -Wshadow -Wcast-align -ffreestanding -fno-builtin -c -I"$core" "$f" \
        -o "$OUT/$(basename "$f" .c).o"
done
# and for the phone: bare-metal aarch64 as libsb builds it (strict alignment, no FP/SIMD registers)
if $CC --target=aarch64-none-elf -E -x c /dev/null >/dev/null 2>&1; then
    for f in $src; do
        $CC --target=aarch64-none-elf -std=c99 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -mstrict-align \
            -mgeneral-regs-only -nostdinc -isystem "$($CC -print-resource-dir)/include" -c -I"$core" "$f" -o /dev/null
    done
    echo "freestanding aarch64 build: ok"
fi
# rule: the modem code never writes a partition. No block-write call anywhere under starbite/modem (the core has
# no block I/O at all; the glue reads RADIO and EFS only).
if grep -rnE '\b(sb_blk_write|ufs_write[a-z_]*|sd_write[a-z0-9_]*|sb_write|sb_write_store|store_write[a-z_]*|ext4_write[a-z_]*)\s*\(' \
        "$core" --include='*.c' --include='*.h' | grep -v '^.*://'; then
    echo "sbm: a block-write call in starbite/modem: refused" >&2
    exit 1
fi
echo "no block-write path in starbite/modem: ok"
flags="-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize=address,undefined -fno-sanitize-recover=all -I$core"
# shellcheck disable=SC2086
$CC $flags -o "$OUT/sbm-test" $src "$here/fakecp.c" "$here/fakesoc.c" "$here/test.c"
# shellcheck disable=SC2086
$CC $flags -o "$OUT/sbm-lnk" $src "$here/lnk.c"
if [ -n "$radio" ]; then "$OUT/sbm-test" "$radio"; else "$OUT/sbm-test"; fi
# EFS: the glue's ext4 reader + in-memory journal replay, on images from mk-efs-fixture.sh (made in the VM)
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o "$OUT/efs-test" "$core/sb/ext4.c" "$core/sb/jbd2.c" "$here/efs-test.c"
fx=${SBM_EFS_FIXTURES:-"$here/out"}
"$OUT/efs-test" "$fx/efs-nv-expected.bin" "$fx"/efs-*-dirty.img "$fx"/efs-*-clean.img
lnk=${SBM_LNK:-}
if [ -z "$lnk" ]; then
    lnk=$(ls "$root"/build/test-boots/*/dmesg*.txt "$root"/build/test-boots/*/last_kmsg*.txt 2>/dev/null || true)
fi
if [ -n "$lnk" ]; then
    # shellcheck disable=SC2086
    "$OUT/sbm-lnk" $lnk
fi
