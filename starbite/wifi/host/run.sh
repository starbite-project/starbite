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
$CC $flags -o "$OUT/ext4-test" "$core/ext4.c" "$here/ext4-test.c"
$CC $flags -o "$OUT/erofs-test" "$core/erofs.c" "$here/erofs-test.c"

# ext4: images made by mke2fs -d from a tree like VENDOR's: /firmware (300 files, a hashed directory)
# and /etc/wifi, with the phone's files when given (else stand-ins), a sparse file, a 3 MiB file;
# 4 KiB blocks with extents and 64-bit descriptors (VENDOR's layout), 1 KiB blocks, no extents
if command -v mke2fs >/dev/null 2>&1; then
    t="$OUT/ext4-tree"
    rm -rf "$t" && mkdir -p "$t/firmware" "$t/etc/wifi"
    i=0
    while [ $i -lt 300 ]; do echo "file $i" > "$t/firmware/f$i.txt"; i=$((i + 1)); done
    files=""
    for f in bcmdhd_sta.bin_b2 nvram.txt_r02a_b2 bcmdhd_clm.blob; do
        if [ -n "$dir" ] && [ -f "$dir/$f" ]; then cp "$dir/$f" "$t/firmware/$f"; else head -c 70000 /dev/urandom > "$t/firmware/$f"; fi
        cp "$t/firmware/$f" "$t/etc/wifi/$f"
        files="$files firmware/$f etc/wifi/$f"
    done
    head -c 3145728 /dev/urandom > "$t/firmware/big.bin"
    dd if=/dev/urandom of="$t/firmware/sparse.bin" bs=4096 count=1 seek=300 2>/dev/null
    for opts in "-b 4096 -O extent,64bit" "-b 1024 -O ^extent,^64bit,^flex_bg" "-b 4096 -O ^extent -I 128"; do
        rm -f "$OUT/v.img"
        # shellcheck disable=SC2086
        mke2fs -q -F -t ext4 $opts -d "$t" "$OUT/v.img" 64M >/dev/null 2>&1 ||
            mke2fs -q -F $opts -d "$t" "$OUT/v.img" 64M
        # shellcheck disable=SC2086
        "$OUT/ext4-test" "$OUT/v.img" "$t" $files firmware/f0.txt firmware/f299.txt firmware/big.bin firmware/sparse.bin
    done
    rm -rf "$t" "$OUT/v.img"
else
    echo "bcmw: no mke2fs: the ext4 reader's test is skipped"
fi

# EROFS: images made by mkfs.erofs from the same kind of tree: lz4hc with compact indexes (One UI's
# VENDOR), lz4 with full indexes, uncompressed; then, with BCMW_VENDOR_IMG=<a copy of the phone's
# VENDOR image> and BCMW_VENDOR_REF=<what fsck.erofs --extract made of it>, the radio's files in it
if command -v mkfs.erofs >/dev/null 2>&1; then
    t="$OUT/erofs-tree"
    rm -rf "$t" && mkdir -p "$t/firmware" "$t/etc/wifi"
    i=0
    while [ $i -lt 300 ]; do echo "file $i" > "$t/firmware/f$i.txt"; i=$((i + 1)); done
    files=""
    for f in bcmdhd_sta.bin_b2 nvram.txt_r02a_b2 bcmdhd_clm.blob; do
        if [ -n "$dir" ] && [ -f "$dir/$f" ]; then cp "$dir/$f" "$t/firmware/$f"; else head -c 70000 /dev/urandom > "$t/firmware/$f"; fi
        cp "$t/firmware/$f" "$t/etc/wifi/$f"
        files="$files firmware/$f etc/wifi/$f"
    done
    head -c 3145728 /dev/urandom > "$t/firmware/random.bin"
    yes "the same line, compressible" | head -c 200000 > "$t/firmware/text.txt"
    for opts in "-z lz4hc,9 -b 4096" "-z lz4 -E legacy-compress -b 4096" "-b 4096"; do
        rm -f "$OUT/v.erofs"
        # shellcheck disable=SC2086
        mkfs.erofs --quiet $opts "$OUT/v.erofs" "$t" >/dev/null
        # shellcheck disable=SC2086
        "$OUT/erofs-test" "$OUT/v.erofs" "$t" $files firmware/f0.txt firmware/f299.txt firmware/random.bin firmware/text.txt
    done
    rm -rf "$t" "$OUT/v.erofs"
else
    echo "bcmw: no mkfs.erofs: the EROFS reader's test is skipped (run it in the VM)"
fi
if [ -n "${BCMW_VENDOR_IMG:-}" ] && [ -n "${BCMW_VENDOR_REF:-}" ]; then
    vf=""
    for f in "$BCMW_VENDOR_REF"/firmware/bcmdhd* "$BCMW_VENDOR_REF"/firmware/nvram.txt* "$BCMW_VENDOR_REF"/etc/wifi/*; do
        [ -f "$f" ] && vf="$vf ${f#"$BCMW_VENDOR_REF"/}"
    done
    # shellcheck disable=SC2086
    "$OUT/erofs-test" "$BCMW_VENDOR_IMG" "$BCMW_VENDOR_REF" $vf
fi
if [ -n "$dir" ]; then
    echo "bcmw: the phone's files from $dir (read only)"
    "$OUT/bcmw-test" "$dir"
else
    echo "bcmw: no directory of the phone's files: synthetic files only"
    "$OUT/bcmw-test"
fi
