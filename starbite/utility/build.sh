#!/bin/sh
# Build the Starbite Utility OS (docs/UTILITY-OS.md): starbite/out/utility.img, a "bare" arm64 Image on
# libsb, for the OS store entry "utility.img" (catalog key "utility"). No packaging or flashing.
#   sh starbite/utility/build.sh            (in the Lima VM: native aarch64 gcc)
#   CROSS=aarch64-linux-gnu- sh starbite/utility/build.sh
# Also builds utility-at40.elf (the same objects linked at 0x40000000) for the relocation test,
# and checks: no undefined symbols, only R_AARCH64_RELATIVE relocations, the Image header.
# usb_dwc3.c, usb_phy.c, max77705_muic.c are the loader's drivers (shim/starlte) with the DMA window
# moved to 0xa7000000; usb_desc.c is the loader's with interface 2 as USB mass storage.
# SB7=path picks the menu v7 asset container (default build/brand/menu7.sb7 of this checkout or of
# the main checkout); without one the build is skipped (exit 0) so the loader build is unaffected.
#   sh starbite/utility/build.sh --repro     build twice (the second into out/repro) and require
#                                            byte-identical images
# On a native aarch64 host the build first runs the host unit tests (starbite/tools/unit-test.c)
# and stops if one fails. UT_OUT=dir changes the output directory.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
lib="$root/starbite/libsb"
out=${UT_OUT:-"$root/starbite/out"}
: "${CROSS:=}"
mkdir -p "$out"
native=0
if [ -z "$CROSS" ] && [ "$(uname -m)" = aarch64 ]; then native=1; fi

if [ "${1:-}" = --repro ]; then
    sh "$0"
    UT_OUT="$out/repro" sh "$0"
    a=$(sha256sum "$out/utility.img" | cut -d' ' -f1)
    b=$(sha256sum "$out/repro/utility.img" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then
        echo "utility: NOT reproducible: $a vs $b" >&2
        exit 1
    fi
    rm -rf "$out/repro"
    echo "utility: reproducible: two builds give $a"
    exit 0
fi

SB7=${SB7:-}
if [ -z "$SB7" ]; then
    for c in "$root/build/brand/menu7.sb7" "$root/../../../build/brand/menu7.sb7"; do
        if [ -f "$c" ]; then SB7=$c; break; fi
    done
fi
if [ -z "$SB7" ] || [ ! -f "$SB7" ]; then
    echo "utility: no build/brand/menu7.sb7 (run tools/brand/menu7-assets.py): Utility OS skipped" >&2
    exit 0
fi
python3 "$root/starbite/tools/mkassets.py" "$SB7" "$out/sbassets.sb7"
echo "utility: assets from $(basename "$SB7"), sha256 $(sha256sum "$SB7" | cut -d' ' -f1)"

# Host test of the USB mass storage and the FAT32 formatter (starbite/tools/ut-host-test.c): a
# failure stops the build.
if [ "$native" = 1 ]; then
    gcc -O1 -Wall -Wextra -Werror -I"$here" -I"$lib" -o "$out/ut-host-test" "$root/starbite/tools/ut-host-test.c" \
        "$here/msc.c" "$here/fat.c" "$here/usb_desc.c" "$here/sha256.c" "$here/calcmath.c" "$here/exfat.c" "$lib/str.c" -lm
    gzip -dc "$root/starbite/tools/fixtures/exfat-card.img.gz" > "$out/exfat-card.img"
    UT_EXFAT="$out/exfat-card.img" "$out/ut-host-test"
fi

CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror -ffile-prefix-map=$root=. -I$lib -I$here"
# Code that runs on the worker cores (the Mongoose M3s are ARMv8.0: no LSE, RDM, DotProd, FP16,
# LRCPC) is built for plain armv8-a and checked below: libsb/smp.c.
V80=" smp.c "
objs=""
v80objs=""
for src in "$lib"/*.c "$lib"/*.S "$here"/*.c; do
    obj="$out/ut_$(basename "$src" | tr . _).o"
    flags=$CFLAGS
    case "$V80" in
    *" $(basename "$src") "*)
        flags=$(echo "$CFLAGS" | sed 's/-mcpu=cortex-a55/-march=armv8-a+simd -mtune=cortex-a55/')
        v80objs="$v80objs $obj" ;;
    esac
    "${CROSS}gcc" $flags -DSB_ASSETS="\"$out/sbassets.sb7\"" "$src" -o "$obj"
    objs="$objs $obj"
done
# The worker code must not contain an instruction the M3 cores lack (ARMv8.1+: LSE atomics, RDM,
# DotProd, FP16 arithmetic, LRCPC, DC CVAP). An empty listing would pass silently: refuse it too.
for o in $v80objs; do
    d=$("${CROSS}objdump" -d --no-show-raw-insn "$o")
    if ! echo "$d" | grep -E -q '\bret\b'; then
        echo "utility: could not disassemble $(basename "$o") for the ARMv8.0 check" >&2
        exit 1
    fi
    bad=$(echo "$d" | grep -E -i '\s(ld(add|clr|eor|set|smax|smin|umax|umin)a?l?[bh]?|st(add|clr|eor|set|smax|smin|umax|umin)l?[bh]?|swpa?l?[bh]?|casa?l?[bh]?|caspa?l?|sqrdml[as]h|[su]dot|ldapr[bh]?|ldapur[bh]?|stlur[bh]?|dc\s+cvap)\s' || true)
    if [ -n "$bad" ]; then
        echo "utility: $(basename "$o") (runs on the M3 cores) has ARMv8.1+ instructions:" >&2
        echo "$bad" | head -5 >&2
        exit 1
    fi
done
LDFLAGS="-pie --no-dynamic-linker -z text -z norelro -z noexecstack --build-id=none --no-warn-rwx-segments \
--hash-style=gnu -T $lib/image.ld"
"${CROSS}ld" $LDFLAGS --defsym=SB_LINK_BASE=0 $objs -o "$out/utility.elf"
"${CROSS}ld" $LDFLAGS --defsym=SB_LINK_BASE=0x40000000 $objs -o "$out/utility-at40.elf"

und=$("${CROSS}nm" -u "$out/utility.elf" || true)
if [ -n "$und" ]; then
    echo "utility: undefined symbols:" >&2
    echo "$und" >&2
    exit 1
fi
bad=$("${CROSS}readelf" -rW "$out/utility.elf" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
if [ -n "$bad" ]; then
    echo "utility: relocations other than R_AARCH64_RELATIVE:" >&2
    echo "$bad" >&2
    exit 1
fi
# What runs before relocation (crt0 -> sb_mmu_enable, sb_reloc_check) must not load an absolute
# address from data: no `blr`/`br` through a register and no GOT loads. One such call (sb_dcache_all
# through the GOT) made the first phone boot reset silently.
for f in sb_mmu_enable sb_reloc_check sb_dcache_all; do
    d=$("${CROSS}objdump" -d --no-show-raw-insn --disassemble="$f" "$out/utility.elf" 2>/dev/null ||
        "${CROSS}objdump" -d --no-show-raw-insn "$out/utility.elf" | awk -v f="<$f>:" '$0 ~ f {p=1; next} /^$/ {p=0} p')
    if ! echo "$d" | grep -E -q '\bret\b'; then
        echo "utility: could not disassemble $f (the pre-relocation check would be empty)" >&2
        exit 1
    fi
    if echo "$d" | grep -E -q '\b(blr|br)\b|_GLOBAL_OFFSET_TABLE_|:got'; then
        echo "utility: $f (runs before relocation) has an indirect branch or GOT access:" >&2
        echo "$d" | grep -E '\b(blr|br)\b|_GLOBAL_OFFSET_TABLE_|:got' >&2
        exit 1
    fi
done
"${CROSS}objcopy" -O binary "$out/utility.elf" "$out/utility.bin"
"${CROSS}objcopy" -O binary "$out/utility-at40.elf" "$out/utility-at40.bin"
"${CROSS}nm" "$out/utility.elf" | sort > "$out/utility.map"
python3 "$root/starbite/tools/check-image.py" "$out/utility.bin" "$out/utility.img" "$out/utility.map"

# No build-host paths or names in the image (it is shared as a store entry).
leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp)/[ -~]{0,40}' "$out/utility.img" | head -3 || true)
if [ -n "$leak" ]; then
    echo "utility: the image contains build-host paths:" >&2
    echo "$leak" >&2
    exit 1
fi

# The relocation test runs the image's own relocation code on the host (aarch64 only).
if [ "$native" = 1 ]; then
    gcc -O2 -Wall -Wextra -Werror -o "$out/reloc-test" "$root/starbite/tools/reloc-test.c" "$lib/reloc.c"
    "$out/reloc-test" "$out/utility.img" "$out/utility.elf" "$out/utility-at40.bin" "$out/utility-at40.elf"
fi
sha256sum "$out/utility.img" | sed "s#  .*#  utility.img#" > "$out/utility.img.sha256"
echo "utility: $(cat "$out/utility.img.sha256")"
