#!/bin/sh
# Build Starbite OS (docs/STARBITE-OS.md): starbite/out/starbite-os.img, a "bare" arm64 Image on libsb for the
# OS store entry "starbite-os.img" (catalog key "starbite-os"), with the built-in apps (.sba) packed inside.
# No packaging or flashing.
#   sh starbite/sbos/build.sh              (in the Lima VM: native aarch64 gcc)
#   CROSS=aarch64-linux-gnu- sh starbite/sbos/build.sh
#   sh starbite/sbos/build.sh --repro      build twice, require byte-identical images
# Needs build/brand/menu7.sb7 (tools/brand/menu7-assets.py) and build/brand/sbos-extra.sb7
# (starbite/sbos/tools/sbos-assets.py, run on the Mac) in this checkout or the main one.
# Checks (as the Utility OS's build): no undefined symbols, only R_AARCH64_RELATIVE relocations, nothing
# indirect before relocation, the worker code (smp.c, heatk.c) ARMv8.0 only, no build-host paths; each app:
# only R_AARCH64_RELATIVE, its header filled and accepted. On a native aarch64 host the host test runs first.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
lib="$root/starbite/libsb"
ut="$root/starbite/utility"
out=${SBOS_OUT:-"$root/starbite/out"}
ob="$out/sbos"
: "${CROSS:=}"
mkdir -p "$out" "$ob"
native=0
if [ -z "$CROSS" ] && [ "$(uname -m)" = aarch64 ]; then native=1; fi

if [ "${1:-}" = --repro ]; then
    sh "$0"
    SBOS_OUT="$out/repro" sh "$0"
    a=$(sha256sum "$out/starbite-os.img" | cut -d' ' -f1)
    b=$(sha256sum "$out/repro/starbite-os.img" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then
        echo "sbos: NOT reproducible: $a vs $b" >&2
        exit 1
    fi
    rm -rf "$out/repro"
    echo "sbos: reproducible: two builds give $a"
    exit 0
fi

find_brand() {
    for c in "$root/build/brand/$1" "$root/../../../build/brand/$1"; do
        if [ -f "$c" ]; then echo "$c"; return 0; fi
    done
    return 1
}
SB7=${SB7:-$(find_brand menu7.sb7 || true)}
EXTRA=${SBOS_EXTRA:-$(find_brand sbos-extra.sb7 || true)}
if [ -z "$SB7" ] || [ -z "$EXTRA" ]; then
    echo "sbos: needs build/brand/menu7.sb7 and build/brand/sbos-extra.sb7 (starbite/sbos/tools/sbos-assets.py)" >&2
    exit 1
fi
python3 "$root/starbite/tools/mkassets.py" "$SB7" "$ob/libsb-assets.sb7" >/dev/null
python3 "$here/tools/mergeassets.py" "$ob/sbassets.sb7" "$ob/libsb-assets.sb7" "$EXTRA"

if [ "$native" = 1 ]; then
    sh "$here/../tools/sbos-host-test.sh"
fi

CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror -ffile-prefix-map=$root=. -I$lib -I$here/kern -I$here/sdk -I$ut"
V80FLAGS=$(echo "$CFLAGS" | sed 's/-mcpu=cortex-a55/-march=armv8-a+simd -mtune=cortex-a55/')
LDFLAGS="-pie --no-dynamic-linker -z text -z norelro -z noexecstack --build-id=none --no-warn-rwx-segments --hash-style=gnu"

relocs_only_relative() {
    bad=$("${CROSS}readelf" -rW "$1" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
    if [ -n "$bad" ]; then
        echo "sbos: $(basename "$1"): relocations other than R_AARCH64_RELATIVE:" >&2
        echo "$bad" >&2
        exit 1
    fi
}

# ---- apps ------------------------------------------------------------------------------------------
"${CROSS}gcc" $CFLAGS "$here/sdk/sbapp_crt.S" -o "$ob/app_crt.o"
"${CROSS}gcc" $CFLAGS "$here/sdk/sbapp_lib.c" -o "$ob/app_lib.o"
"${CROSS}gcc" $CFLAGS "$ut/tz.c" -o "$ob/app_tz.o"
apps=""
for a in light heat sos info clock battery settings test-loop test-fault; do
    "${CROSS}gcc" $CFLAGS "$here/apps/$a.c" -o "$ob/app_$a.o"
    extra=""
    [ "$a" = clock ] && extra="$ob/app_tz.o"
    "${CROSS}ld" $LDFLAGS -T "$here/sdk/app.ld" "$ob/app_crt.o" "$ob/app_$a.o" $extra "$ob/app_lib.o" -o "$ob/$a.elf"
    und=$("${CROSS}nm" -u "$ob/$a.elf" || true)
    if [ -n "$und" ]; then
        echo "sbos: app $a: undefined symbols: $und" >&2
        exit 1
    fi
    relocs_only_relative "$ob/$a.elf"
    "${CROSS}objcopy" -O binary "$ob/$a.elf" "$ob/$a.bin"
    NM="${CROSS}nm" python3 "$here/tools/mk-sba.py" app "$ob/$a.elf" "$ob/$a.bin" "$ob/$a.sba"
    apps="$apps $ob/$a.sba"
done
python3 "$here/tools/mk-sba.py" pack "$ob/apps.pack" $apps

# ---- kernel ----------------------------------------------------------------------------------------
objs=""
v80objs=""
for src in "$lib"/*.c "$lib"/*.S "$here"/kern/*.c "$here"/kern/*.S "$ut/tz.c" "$ut/exfat.c"; do
    obj="$ob/k_$(basename "$src" | tr . _).o"
    flags=$CFLAGS
    case " smp.c heatk.c " in
    *" $(basename "$src") "*)
        flags=$V80FLAGS
        v80objs="$v80objs $obj" ;;
    esac
    "${CROSS}gcc" $flags -DSBOS_KERNEL -DSB_ASSETS="\"$ob/sbassets.sb7\"" -DSBOS_APPS="\"$ob/apps.pack\"" "$src" -o "$obj"
    objs="$objs $obj"
done
for o in $v80objs; do
    d=$("${CROSS}objdump" -d --no-show-raw-insn "$o")
    if ! echo "$d" | grep -E -q '\bret\b'; then
        echo "sbos: could not disassemble $(basename "$o") for the ARMv8.0 check" >&2
        exit 1
    fi
    bad=$(echo "$d" | grep -E -i '\s(ld(add|clr|eor|set|smax|smin|umax|umin)a?l?[bh]?|st(add|clr|eor|set|smax|smin|umax|umin)l?[bh]?|swpa?l?[bh]?|casa?l?[bh]?|caspa?l?|sqrdml[as]h|[su]dot|ldapr[bh]?|ldapur[bh]?|stlur[bh]?|dc\s+cvap)\s' || true)
    if [ -n "$bad" ]; then
        echo "sbos: $(basename "$o") (runs on the M3 cores) has ARMv8.1+ instructions:" >&2
        echo "$bad" | head -5 >&2
        exit 1
    fi
done
KLD="$LDFLAGS -T $lib/image.ld"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $objs -o "$out/starbite-os.elf"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0x40000000 $objs -o "$out/starbite-os-at40.elf"
und=$("${CROSS}nm" -u "$out/starbite-os.elf" || true)
if [ -n "$und" ]; then
    echo "sbos: undefined symbols:" >&2
    echo "$und" >&2
    exit 1
fi
relocs_only_relative "$out/starbite-os.elf"
for f in sb_mmu_enable sb_reloc_check sb_dcache_all; do
    d=$("${CROSS}objdump" -d --no-show-raw-insn --disassemble="$f" "$out/starbite-os.elf" 2>/dev/null ||
        "${CROSS}objdump" -d --no-show-raw-insn "$out/starbite-os.elf" | awk -v f="<$f>:" '$0 ~ f {p=1; next} /^$/ {p=0} p')
    if ! echo "$d" | grep -E -q '\bret\b'; then
        echo "sbos: could not disassemble $f (the pre-relocation check would be empty)" >&2
        exit 1
    fi
    if echo "$d" | grep -E -q '\b(blr|br)\b|_GLOBAL_OFFSET_TABLE_|:got'; then
        echo "sbos: $f (runs before relocation) has an indirect branch or GOT access" >&2
        exit 1
    fi
done
"${CROSS}objcopy" -O binary "$out/starbite-os.elf" "$out/starbite-os.bin"
"${CROSS}objcopy" -O binary "$out/starbite-os-at40.elf" "$out/starbite-os-at40.bin"
"${CROSS}nm" "$out/starbite-os.elf" | sort > "$out/starbite-os.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os.bin" "$out/starbite-os.img" "$out/starbite-os.map"

leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp)/[ -~]{0,40}' "$out/starbite-os.img" | head -3 || true)
if [ -n "$leak" ]; then
    echo "sbos: the image contains build-host paths:" >&2
    echo "$leak" >&2
    exit 1
fi
if [ "$native" = 1 ]; then
    gcc -O2 -Wall -Wextra -Werror -o "$ob/reloc-test" "$root/starbite/tools/reloc-test.c" "$lib/reloc.c"
    "$ob/reloc-test" "$out/starbite-os.img" "$out/starbite-os.elf" "$out/starbite-os-at40.bin" "$out/starbite-os-at40.elf"
    # every packed app passes the kernel's own check (the host test's sbos-apptest)
    "$ob/sbos-apptest" apps $apps
    sh "$root/starbite/tools/sbos-host-test.sh" $apps
fi
sha256sum "$out/starbite-os.img" | sed "s#  .*#  starbite-os.img#" > "$out/starbite-os.img.sha256"
echo "sbos: $(cat "$out/starbite-os.img.sha256") ($(wc -c < "$out/starbite-os.img") bytes)"
