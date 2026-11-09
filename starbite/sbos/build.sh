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
-fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror -ffile-prefix-map=$root=. -I$lib -I$here/kern -I$here/sdk -I$ut -I$root/starbite/wifi -I$root/starbite/guest/sbos"
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
for a in emergency light heat sos info clock battery settings wifi calc notes test-loop test-fault; do
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
wifi="$root/starbite/wifi"
for src in "$lib"/*.c "$lib"/*.S "$here"/kern/*.c "$here"/kern/*.S "$ut/tz.c" "$ut/exfat.c" \
           "$wifi/core.c" "$wifi/exynos_pcie.c" "$wifi/chip.c" "$wifi/fw.c" "$wifi/ring.c" "$wifi/msgbuf.c" "$wifi/link.c" \
           "$wifi/wpa.c" "$wifi/net.c" "$wifi/ext4.c" "$wifi/erofs.c" "$wifi/sb/bcmw_sb.c" \
           "$root/starbite/guest/sbos/guest.c" "$root/starbite/guest/host/sbghost.c"; do
    obj="$ob/k_$(basename "$src" | tr . _).o"
    flags=$CFLAGS
    case " smp.c heatk.c ocp.c " in
    *" $(basename "$src") "*)
        flags=$V80FLAGS
        v80objs="$v80objs $obj" ;;
    esac
    "${CROSS}gcc" $flags -DSBOS_KERNEL -DSB_ASSETS="\"$ob/sbassets.sb7\"" -DSBOS_APPS="\"$ob/apps.pack\"" "$src" -o "$obj"
    objs="$objs $obj"
done
# the self-test variant (docs/STARBITE-OS.md 12.6): the same objects with uitest.c built to run
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON "$here/kern/uitest.c" -o "$ob/k_uitest_on.o"
stobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_on.o#")
# 1.0-rc3: the LED-probe variant (only fled_probe, then a restart: the S2MPB02's first look on the phone)
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_LED "$here/kern/uitest.c" -o "$ob/k_uitest_led.o"
ledobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_led.o#")
# 1.2-alpha1: the Linux variant (P3a: the guest started, shown, tapped, stopped twice; then TWRP)
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_GUEST "$here/kern/uitest.c" -o "$ob/k_uitest_guest.o"
gstobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_guest.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_PROBE "$here/kern/uitest.c" -o "$ob/k_uitest_probe.o"
prbobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_probe.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_VIB "$here/kern/uitest.c" -o "$ob/k_uitest_vib.o"
vibobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_vib.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_GPU "$here/kern/uitest.c" -o "$ob/k_uitest_gpu.o"
gpuobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_gpu.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_GPUALU "$here/kern/uitest.c" -o "$ob/k_uitest_gpualu.o"
gpualuobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_gpualu.o#")
if [ "$stobjs" = "$objs" ]; then
    echo "sbos: the self-test variant did not get k_uitest_on.o" >&2
    exit 1
fi
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
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $stobjs -o "$out/starbite-os-selftest.elf"
relocs_only_relative "$out/starbite-os-selftest.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-selftest.elf" "$out/starbite-os-selftest.bin"
"${CROSS}nm" "$out/starbite-os-selftest.elf" | sort > "$out/starbite-os-selftest.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-selftest.bin" "$out/starbite-os-selftest.img" "$out/starbite-os-selftest.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $ledobjs -o "$out/starbite-os-ledprobe.elf"
relocs_only_relative "$out/starbite-os-ledprobe.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-ledprobe.elf" "$out/starbite-os-ledprobe.bin"
"${CROSS}nm" "$out/starbite-os-ledprobe.elf" | sort > "$out/starbite-os-ledprobe.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-ledprobe.bin" "$out/starbite-os-ledprobe.img" "$out/starbite-os-ledprobe.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $gstobjs -o "$out/starbite-os-guesttest.elf"
relocs_only_relative "$out/starbite-os-guesttest.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-guesttest.elf" "$out/starbite-os-guesttest.bin"
"${CROSS}nm" "$out/starbite-os-guesttest.elf" | sort > "$out/starbite-os-guesttest.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-guesttest.bin" "$out/starbite-os-guesttest.img" "$out/starbite-os-guesttest.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $prbobjs -o "$out/starbite-os-heatprobe.elf"
relocs_only_relative "$out/starbite-os-heatprobe.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-heatprobe.elf" "$out/starbite-os-heatprobe.bin"
"${CROSS}nm" "$out/starbite-os-heatprobe.elf" | sort > "$out/starbite-os-heatprobe.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-heatprobe.bin" "$out/starbite-os-heatprobe.img" "$out/starbite-os-heatprobe.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $vibobjs -o "$out/starbite-os-vibtest.elf"
relocs_only_relative "$out/starbite-os-vibtest.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-vibtest.elf" "$out/starbite-os-vibtest.bin"
"${CROSS}nm" "$out/starbite-os-vibtest.elf" | sort > "$out/starbite-os-vibtest.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-vibtest.bin" "$out/starbite-os-vibtest.img" "$out/starbite-os-vibtest.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $gpuobjs -o "$out/starbite-os-gputest.elf"
relocs_only_relative "$out/starbite-os-gputest.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-gputest.elf" "$out/starbite-os-gputest.bin"
"${CROSS}nm" "$out/starbite-os-gputest.elf" | sort > "$out/starbite-os-gputest.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-gputest.bin" "$out/starbite-os-gputest.img" "$out/starbite-os-gputest.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $gpualuobjs -o "$out/starbite-os-gpualu.elf"
relocs_only_relative "$out/starbite-os-gpualu.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-gpualu.elf" "$out/starbite-os-gpualu.bin"
"${CROSS}nm" "$out/starbite-os-gpualu.elf" | sort > "$out/starbite-os-gpualu.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-gpualu.bin" "$out/starbite-os-gpualu.img" "$out/starbite-os-gpualu.map"
if ! "${CROSS}nm" "$out/starbite-os.elf" | grep -q ' sbos_selftest_enabled$'; then
    echo "sbos: no sbos_selftest_enabled in the image" >&2
    exit 1
fi

leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp)/[ -~]{0,40}' "$out/starbite-os.img" "$out/starbite-os-selftest.img" "$out/starbite-os-ledprobe.img" "$out/starbite-os-guesttest.img" "$out/starbite-os-heatprobe.img" "$out/starbite-os-vibtest.img" "$out/starbite-os-gputest.img" "$out/starbite-os-gpualu.img" | head -3 || true)
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
sha256sum "$out/starbite-os-selftest.img" | sed "s#  .*#  starbite-os-selftest.img#" > "$out/starbite-os-selftest.img.sha256"
echo "sbos: $(cat "$out/starbite-os-selftest.img.sha256") (self-test variant)"
sha256sum "$out/starbite-os-ledprobe.img" | sed "s#  .*#  starbite-os-ledprobe.img#" > "$out/starbite-os-ledprobe.img.sha256"
echo "sbos: $(cat "$out/starbite-os-ledprobe.img.sha256") (LED-probe variant)"
sha256sum "$out/starbite-os-guesttest.img" | sed "s#  .*#  starbite-os-guesttest.img#" > "$out/starbite-os-guesttest.img.sha256"
echo "sbos: $(cat "$out/starbite-os-guesttest.img.sha256") (Linux self-test variant)"
sha256sum "$out/starbite-os-heatprobe.img" | sed "s#  .*#  starbite-os-heatprobe.img#" > "$out/starbite-os-heatprobe.img.sha256"
echo "sbos: $(cat "$out/starbite-os-heatprobe.img.sha256") (heat probe H1 variant)"
sha256sum "$out/starbite-os-vibtest.img" | sed "s#  .*#  starbite-os-vibtest.img#" > "$out/starbite-os-vibtest.img.sha256"
echo "sbos: $(cat "$out/starbite-os-vibtest.img.sha256") (vibration test variant)"
sha256sum "$out/starbite-os-gputest.img" | sed "s#  .*#  starbite-os-gputest.img#" > "$out/starbite-os-gputest.img.sha256"
echo "sbos: $(cat "$out/starbite-os-gputest.img.sha256") (GPU ladder H2 variant)"
sha256sum "$out/starbite-os-gpualu.img" | sed "s#  .*#  starbite-os-gpualu.img#" > "$out/starbite-os-gpualu.img.sha256"
echo "sbos: $(cat "$out/starbite-os-gpualu.img.sha256") (GPU compute H2d variant)"
echo "sbos: $(cat "$out/starbite-os.img.sha256") ($(wc -c < "$out/starbite-os.img") bytes)"
