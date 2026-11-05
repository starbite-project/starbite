#!/bin/sh
# Build Engineer OS (docs/ENGINEER-OS.md): starbite/out/engineer.img, a "bare" arm64 Image on
# libsb, for the OS store entry "engineer.img" (catalog key "engineer"). No packaging or flashing.
#   sh starbite/engineer/build.sh            (in the Lima VM: native aarch64 gcc)
#   CROSS=aarch64-linux-gnu- sh starbite/engineer/build.sh
# Also builds engineer-at40.elf (the same objects linked at 0x40000000) for the relocation test,
# and checks: no undefined symbols, only R_AARCH64_RELATIVE relocations, the Image header.
# SB7=path picks the menu v7 asset container (default build/brand/menu7.sb7 of this checkout or of
# the main checkout); without one the build is skipped (exit 0) so the loader build is unaffected.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
lib="$root/starbite/libsb"
out="$root/starbite/out"
: "${CROSS:=}"
mkdir -p "$out"

SB7=${SB7:-}
if [ -z "$SB7" ]; then
    for c in "$root/build/brand/menu7.sb7" "$root/../../../build/brand/menu7.sb7"; do
        if [ -f "$c" ]; then SB7=$c; break; fi
    done
fi
if [ -z "$SB7" ] || [ ! -f "$SB7" ]; then
    echo "engineer: no build/brand/menu7.sb7 (run tools/brand/menu7-assets.py): Engineer OS skipped" >&2
    exit 0
fi
python3 "$root/starbite/tools/mkassets.py" "$SB7" "$out/sbassets.sb7"

CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror -I$lib -I$here"
objs=""
for src in "$lib"/*.c "$lib"/*.S "$here"/*.c; do
    obj="$out/eng_$(basename "$src" | tr . _).o"
    "${CROSS}gcc" $CFLAGS -DSB_ASSETS="\"$out/sbassets.sb7\"" "$src" -o "$obj"
    objs="$objs $obj"
done
LDFLAGS="-pie --no-dynamic-linker -z text -z norelro -z noexecstack --build-id=none --no-warn-rwx-segments \
--hash-style=gnu -T $lib/image.ld"
"${CROSS}ld" $LDFLAGS --defsym=SB_LINK_BASE=0 $objs -o "$out/engineer.elf"
"${CROSS}ld" $LDFLAGS --defsym=SB_LINK_BASE=0x40000000 $objs -o "$out/engineer-at40.elf"

und=$("${CROSS}nm" -u "$out/engineer.elf" || true)
if [ -n "$und" ]; then
    echo "engineer: undefined symbols:" >&2
    echo "$und" >&2
    exit 1
fi
bad=$("${CROSS}readelf" -rW "$out/engineer.elf" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
if [ -n "$bad" ]; then
    echo "engineer: relocations other than R_AARCH64_RELATIVE:" >&2
    echo "$bad" >&2
    exit 1
fi
# What runs before relocation (crt0 -> sb_mmu_enable, sb_reloc_check) must not load an absolute
# address from data: no `blr`/`br` through a register and no GOT loads. One such call (sb_dcache_all
# through the GOT) made the first phone boot reset silently.
for f in sb_mmu_enable sb_reloc_check; do
    d=$("${CROSS}objdump" -d --no-show-raw-insn --disassemble="$f" "$out/engineer.elf" 2>/dev/null ||
        "${CROSS}objdump" -d --no-show-raw-insn "$out/engineer.elf" | awk -v f="<$f>:" '$0 ~ f {p=1; next} /^$/ {p=0} p')
    if echo "$d" | grep -E -q '\b(blr|br)\b|_GLOBAL_OFFSET_TABLE_|:got'; then
        echo "engineer: $f (runs before relocation) has an indirect branch or GOT access:" >&2
        echo "$d" | grep -E '\b(blr|br)\b|_GLOBAL_OFFSET_TABLE_|:got' >&2
        exit 1
    fi
done
"${CROSS}objcopy" -O binary "$out/engineer.elf" "$out/engineer.bin"
"${CROSS}objcopy" -O binary "$out/engineer-at40.elf" "$out/engineer-at40.bin"
"${CROSS}nm" "$out/engineer.elf" | sort > "$out/engineer.map"
python3 "$root/starbite/tools/check-image.py" "$out/engineer.bin" "$out/engineer.img" "$out/engineer.map"

# The relocation test runs the image's own relocation code on the host (aarch64 only).
if [ -z "$CROSS" ] && [ "$(uname -m)" = aarch64 ]; then
    gcc -O2 -Wall -Wextra -Werror -o "$out/reloc-test" "$root/starbite/tools/reloc-test.c" "$lib/reloc.c"
    "$out/reloc-test" "$out/engineer.img" "$out/engineer.elf" "$out/engineer-at40.bin" "$out/engineer-at40.elf"
fi
