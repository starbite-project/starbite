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
    CC=gcc sh "$here/kern/host/passcode-test.sh"          # Track A: the passcode (kern/passcode.c)
    CC=gcc sh "$here/kern/host/prefs-test.sh"             # Track A: prefs and blobs (kern/prefs.c)
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I "$here/kern" "$here/kern/host/lens-test.c" -lm -o "$ob/lens-test" &&
        "$ob/lens-test"                                   # Track A: Liquid Glass V3's model (kern/lens.h)
fi

# The sensor hub's firmware patch (docs/SENSOR-HUB.md 2.2): never in the repository. Only the staged test image
# starbite-os-shub.img embeds it, from a file outside the checkout (starbite/sbos/tools/shub-patch.py makes it from
# a Samsung kernel tree); without the file that image is skipped.
SHUB_PATCH=${SHUB_PATCH:-"$HOME/sbos-shub-blob/bcm4775-star-q.bin"}
shub_sha=b1a3e138ab808f2be247ad195d71d6aef2f2d3ecc7bd3e13e141b1443da408d0
shub_blob=""
if [ -f "$SHUB_PATCH" ]; then
    case "$(cd "$(dirname "$SHUB_PATCH")" && pwd)/" in
    "$root"/*)
        echo "sbos: $SHUB_PATCH is inside the checkout: the patch blob stays outside it" >&2
        exit 1 ;;
    esac
    if [ "$(sha256sum "$SHUB_PATCH" | cut -d' ' -f1)" != "$shub_sha" ]; then
        echo "sbos: $SHUB_PATCH is not the known sensor hub patch (sha256)" >&2
        exit 1
    fi
    shub_blob=$SHUB_PATCH
fi
if [ "$native" = 1 ]; then
    # the hub's wire formats against the GPL sample frames (and the blob's records when it is there)
    gcc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I"$here/kern" \
        -o "$ob/shub-host-test" "$root/starbite/tools/shub-host-test.c" "$here/kern/shub_proto.c" -lm
    "$ob/shub-host-test" $shub_blob
    if [ -n "$shub_blob" ]; then
        # the driver itself (kern/shub.c) against a simulated phone and chip: both self tests, the faults, the restore
        gcc -O1 -g -Wall -Wextra -Werror -DSB_HOST -fsanitize=address,undefined -fno-sanitize-recover=all -I"$lib" -I"$here/kern" \
            -o "$ob/shub-sim-test" "$root/starbite/tools/shub-sim-test.c" "$here/kern/shub.c" "$here/kern/shub_proto.c" "$lib/str.c" -lm
        "$ob/shub-sim-test" "$shub_blob"
    fi
fi

CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror -ffile-prefix-map=$root=. -I$lib -I$here/kern -I$here/sdk -I$ut -I$root/starbite/wifi -I$root/starbite/wifi/tls/bearssl/inc -I$root/starbite/guest/sbos"
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
for a in emergency light heat sos info clock battery settings wifi modem calc notes files guide test-loop test-fault; do
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
# 1.2-alpha21: the kernel loads at most APP_MAX apps from the pack (alpha20 had 13 for 12: test-fault was dropped)
appmax=$(sed -n 's/^#define APP_MAX \([0-9]*\).*/\1/p' "$here/kern/kern.h")
napps=$(echo $apps | wc -w)
if [ -z "$appmax" ] || [ "$napps" -gt "$appmax" ]; then
    echo "sbos: $napps apps in the pack but the kernel loads at most ${appmax:-?} (APP_MAX in kern/kern.h)" >&2
    exit 1
fi
python3 "$here/tools/mk-sba.py" pack "$ob/apps.pack" $apps

# ---- kernel ----------------------------------------------------------------------------------------
objs=""
v80objs=""
wifi="$root/starbite/wifi"
for src in "$lib"/*.c "$lib"/*.S "$here"/kern/*.c "$here"/kern/*.S "$ut/tz.c" "$ut/exfat.c" \
           "$wifi/core.c" "$wifi/exynos_pcie.c" "$wifi/chip.c" "$wifi/fw.c" "$wifi/ring.c" "$wifi/msgbuf.c" "$wifi/link.c" \
           "$wifi/wpa.c" "$wifi/net.c" "$wifi/ext4.c" "$wifi/erofs.c" "$wifi/sb/bcmw_sb.c" \
           "$wifi/tcp.c" "$wifi/netio.c" "$wifi/http.c" "$wifi/tls/ta.c" "$wifi/tls/tls_chains.c" "$wifi/tls/tls_check.c" \
           "$root/starbite/guest/sbos/guest.c" "$root/starbite/guest/host/sbghost.c"; do
    obj="$ob/k_$(basename "$src" | tr . _).o"
    flags=$CFLAGS
    case "$src" in                          # BearSSL's headers include <string.h>: the shim's, not the C library's
    */kern/wifi.c|*/wifi/http.c|*/wifi/netio.c|*/wifi/tls/*) flags="$flags -I$wifi/tls/shim" ;;
    */kern/modem.c) flags="$flags -DSBM_ATTENDED_TX" ;;   # mobile data: the user's Cellular switch (docs/MODEM.md 7.10)
    esac
    case " smp.c heatk.c ocp.c " in
    *" $(basename "$src") "*)
        flags=$V80FLAGS
        v80objs="$v80objs $obj" ;;
    esac
    "${CROSS}gcc" $flags -DSBOS_KERNEL -DSB_ASSETS="\"$ob/sbassets.sb7\"" -DSBOS_APPS="\"$ob/apps.pack\"" "$src" -o "$obj"
    objs="$objs $obj"
done
# Next (wifi-sbos-next): BearSSL for HTTPS (starbite/wifi/tls/bearssl: the files a TLS 1.2 client links, MIT,
# unmodified, so built without our warnings), with no OS randomness or clock of its own, byte-wise loads (the
# kernel is -mstrict-align), and <string.h> from the shim (libsb has the five functions it uses)
brs="$wifi/tls/bearssl"
BRFLAGS="$(echo "$CFLAGS" | sed 's/ -Wall -Wextra -Werror//') -w -I$wifi/tls/shim -I$brs/src -DBR_USE_URANDOM=0 \
-DBR_USE_UNIX_TIME=0 -DBR_USE_GETENTROPY=0 -DBR_RDRAND=0 -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0"
for f in $(cat "$brs/FILES"); do
    obj="$ob/br_$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    "${CROSS}gcc" $BRFLAGS "$brs/src/$f" -o "$obj"
    objs="$objs $obj"
done
# 1.2-alpha21: modem-core (docs/MODEM.md 7.6) for the Modem app, receive only. Its ext4 reader (a superset of
# Wi-Fi's: EFS has the encryption flag) is built with its names prefixed, so Wi-Fi keeps its own tested reader.
modem="$root/starbite/modem"
MREN="-Dext4_open=m_ext4_open -Dext4_inode=m_ext4_inode -Dext4_size=m_ext4_size -Dext4_read=m_ext4_read \
-Dext4_lookup=m_ext4_lookup -Dext4_bmap=m_ext4_bmap -Dext4_list=m_ext4_list"
for src in "$modem"/sbm_*.c "$modem/sb/sbm_sb.c" "$modem/sb/ext4.c" "$modem/sb/jbd2.c"; do
    obj="$ob/km_$(basename "$src" | tr . _).o"
    "${CROSS}gcc" $CFLAGS $MREN -DSBM_ATTENDED_TX -I"$modem" "$src" -o "$obj"
    objs="$objs $obj"
done
# the rule of the modem images: no partition write from the modem code (libsb's storage hold is on while it runs)
bad=""
for o in "$ob"/km_*.o "$ob/k_modem_c.o"; do
    w=$("${CROSS}nm" -u "$o" | grep -E ' (sb_blk_write|ufs_write[a-z_]*|sd_write[a-z0-9_]*|sb_write|sb_write_store|store_write[a-z_]*)$' || true)
    [ -n "$w" ] && bad="$bad $(basename "$o"):$w"
done
if [ -n "$bad" ]; then
    echo "sbos: the modem objects reference a block write:$bad" >&2
    exit 1
fi
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
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_GPUCOMP "$here/kern/uitest.c" -o "$ob/k_uitest_gpc.o"
gpcobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_gpc.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_SURVIVAL "$here/kern/uitest.c" -o "$ob/k_uitest_surv.o"
survobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_surv.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_UIBENCH "$here/kern/uitest.c" -o "$ob/k_uitest_uib.o"
uibobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_uib.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_GPU "$here/kern/uitest.c" -o "$ob/k_uitest_gpu.o"
gpuobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_gpu.o#")
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_GPUALU "$here/kern/uitest.c" -o "$ob/k_uitest_gpualu.o"
gpualuobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_gpualu.o#")
# Starbite OS Next (P2 of the UI port plan, docs/SBTK-P1.md): the sbtk shell as the UI, its own images
# starbite-next.img and starbite-next-selftest.img (catalog key starbite-next). Needs the phone's pack
# (starbite/sbos/ui/tools/run.sh assets, then sbtk-pack.py BUILD/sbtk --phone, on the Mac: Chrome draws the icons).
ui="$here/ui"
PAK=""
for c in "$root/build/sbtk/sbtk-phone.pak" "$root/../../../build/sbtk/sbtk-phone.pak"; do
    if [ -f "$c" ]; then PAK=$c; break; fi
done
nobjs=""
if [ -n "$PAK" ]; then
    NFLAGS="$CFLAGS -I$ui -DSBOS_KERNEL"
    nui=""
    for f in tk_math tk_raster tk_text tk_glass tk_comp tk_motion tk_pack tk_heap shell_lock kern_next; do
        "${CROSS}gcc" $NFLAGS "$ui/$f.c" -o "$ob/n_$f.o"
        nui="$nui $ob/n_$f.o"
    done
    # L2 + L3 (docs/STARBITE-LINUX-APPS.md): Linux apps and the App Store (the platform side of shell.h's sh_linux_*)
    "${CROSS}gcc" $NFLAGS "$root/starbite/guest/next/gwin.c" -o "$ob/n_gwin.o"
    nui="$nui $ob/n_gwin.o"
    "${CROSS}gcc" $NFLAGS -DSBOS_SELFTEST_NEXT "$ui/kern_next.c" -o "$ob/n_kern_next_st.o"
    "${CROSS}gcc" $CFLAGS -DSBTK_PACK="\"$PAK\"" "$ui/next_pack.S" -o "$ob/n_pack.o"
    "${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_NEXT "$here/kern/main.c" -o "$ob/k_main_next.o"
    "${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_NEXT "$here/kern/uitest.c" -o "$ob/k_uitest_next.o"
    nobjs="$(echo "$objs" | sed "s#$ob/k_main_c.o#$ob/k_main_next.o#") $nui $ob/n_pack.o"
    nstobjs=$(echo "$nobjs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_next.o#; s#$ob/n_kern_next.o#$ob/n_kern_next_st.o#")
    # wifi-sbos-next: Next with the silent Wi-Fi self test (the TLS self-check, one scan, no join; then recovery)
    "${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_NEXT_WIFI "$here/kern/uitest.c" -o "$ob/k_uitest_nwifi.o"
    nwobjs=$(echo "$nobjs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_nwifi.o#")
    # L2: Next with the Linux self test (the Terminal, foot on the M3 cores, a typed command; then recovery)
    "${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_NEXT_LINUX "$here/kern/uitest.c" -o "$ob/k_uitest_nlinux.o"
    nlobjs=$(echo "$nobjs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_nlinux.o#")
else
    echo "sbos: no build/sbtk/sbtk-phone.pak: the starbite-next images are not built" >&2
fi
"${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_SHUBPROBE "$here/kern/uitest.c" -o "$ob/k_uitest_shubprobe.o"
shubprobeobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_shubprobe.o#")
shubobjs=""
if [ -n "$shub_blob" ]; then
    "${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSBOS_SELFTEST_ON -DSBOS_SELFTEST_SHUB "$here/kern/uitest.c" -o "$ob/k_uitest_shub.o"
    "${CROSS}gcc" $CFLAGS -DSBOS_KERNEL -DSHUB_PATCH_FILE="\"$shub_blob\"" "$here/kern/shub_blob.S" -o "$ob/k_shub_blob_on.o"
    shubobjs=$(echo "$objs" | sed "s#$ob/k_uitest_c.o#$ob/k_uitest_shub.o#; s#$ob/k_shub_blob_S.o#$ob/k_shub_blob_on.o#")
    case "$shubobjs" in
    *k_shub_blob_on.o*) ;;
    *)
        echo "sbos: the sensor hub test image did not get the patch object" >&2
        exit 1 ;;
    esac
fi
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
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $gpcobjs -o "$out/starbite-os-gpucomp.elf"
relocs_only_relative "$out/starbite-os-gpucomp.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-gpucomp.elf" "$out/starbite-os-gpucomp.bin"
"${CROSS}nm" "$out/starbite-os-gpucomp.elf" | sort > "$out/starbite-os-gpucomp.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-gpucomp.bin" "$out/starbite-os-gpucomp.img" "$out/starbite-os-gpucomp.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $survobjs -o "$out/starbite-os-survival.elf"
relocs_only_relative "$out/starbite-os-survival.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-survival.elf" "$out/starbite-os-survival.bin"
"${CROSS}nm" "$out/starbite-os-survival.elf" | sort > "$out/starbite-os-survival.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-survival.bin" "$out/starbite-os-survival.img" "$out/starbite-os-survival.map"
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $uibobjs -o "$out/starbite-os-uibench.elf"
relocs_only_relative "$out/starbite-os-uibench.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-uibench.elf" "$out/starbite-os-uibench.bin"
"${CROSS}nm" "$out/starbite-os-uibench.elf" | sort > "$out/starbite-os-uibench.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-uibench.bin" "$out/starbite-os-uibench.img" "$out/starbite-os-uibench.map"
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
if [ -n "$nobjs" ]; then
    for v in next next-selftest next-wifitest next-linuxtest; do
        case $v in next) vo=$nobjs ;; next-selftest) vo=$nstobjs ;; next-linuxtest) vo=$nlobjs ;; *) vo=$nwobjs ;; esac
        "${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $vo -o "$out/starbite-$v.elf"
        und=$("${CROSS}nm" -u "$out/starbite-$v.elf" || true)
        if [ -n "$und" ]; then
            echo "sbos: starbite-$v: undefined symbols:" >&2
            echo "$und" >&2
            exit 1
        fi
        relocs_only_relative "$out/starbite-$v.elf"
        "${CROSS}objcopy" -O binary "$out/starbite-$v.elf" "$out/starbite-$v.bin"
        "${CROSS}nm" "$out/starbite-$v.elf" | sort > "$out/starbite-$v.map"
        python3 "$root/starbite/tools/check-image.py" "$out/starbite-$v.bin" "$out/starbite-$v.img" "$out/starbite-$v.map"
        leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp)/[ -~]{0,40}' "$out/starbite-$v.img" | head -3 || true)
        if [ -n "$leak" ]; then
            echo "sbos: starbite-$v.img contains build-host paths:" >&2
            echo "$leak" >&2
            exit 1
        fi
        sha256sum "$out/starbite-$v.img" | sed "s#  .*#  starbite-$v.img#" > "$out/starbite-$v.img.sha256"
        echo "sbos: $(cat "$out/starbite-$v.img.sha256") (Starbite OS Next, $(wc -c < "$out/starbite-$v.img") bytes)"
    done
fi
"${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $shubprobeobjs -o "$out/starbite-os-shubprobe.elf"
relocs_only_relative "$out/starbite-os-shubprobe.elf"
"${CROSS}objcopy" -O binary "$out/starbite-os-shubprobe.elf" "$out/starbite-os-shubprobe.bin"
"${CROSS}nm" "$out/starbite-os-shubprobe.elf" | sort > "$out/starbite-os-shubprobe.map"
python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-shubprobe.bin" "$out/starbite-os-shubprobe.img" "$out/starbite-os-shubprobe.map"
shubimg=""
rm -f "$out/starbite-os-shub.img" "$out/starbite-os-shub.img.sha256"
if [ -n "$shubobjs" ]; then
    "${CROSS}ld" $KLD --defsym=SB_LINK_BASE=0 $shubobjs -o "$out/starbite-os-shub.elf"
    relocs_only_relative "$out/starbite-os-shub.elf"
    "${CROSS}objcopy" -O binary "$out/starbite-os-shub.elf" "$out/starbite-os-shub.bin"
    "${CROSS}nm" "$out/starbite-os-shub.elf" | sort > "$out/starbite-os-shub.map"
    python3 "$root/starbite/tools/check-image.py" "$out/starbite-os-shub.bin" "$out/starbite-os-shub.img" "$out/starbite-os-shub.map"
    shubimg="$out/starbite-os-shub.img"
fi
# the patch is in the test image only: never in the image people install or in any other variant
for f in "$out/starbite-os.elf" "$out/starbite-os-selftest.elf" "$out/starbite-os-shubprobe.elf"; do
    sz=$("${CROSS}nm" "$f" | awk '$3 == "shub_patch_start" { a = $1 } $3 == "shub_patch_end" { b = $1 } END { print (a == b && a != "") ? "empty" : "not empty" }')
    if [ "$sz" != empty ]; then
        echo "sbos: $(basename "$f") carries the sensor hub patch (or has no shub_patch symbols)" >&2
        exit 1
    fi
done
if ! "${CROSS}nm" "$out/starbite-os.elf" | grep -q ' sbos_selftest_enabled$'; then
    echo "sbos: no sbos_selftest_enabled in the image" >&2
    exit 1
fi

leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp)/[ -~]{0,40}' "$out/starbite-os.img" "$out/starbite-os-selftest.img" "$out/starbite-os-ledprobe.img" "$out/starbite-os-guesttest.img" "$out/starbite-os-heatprobe.img" "$out/starbite-os-vibtest.img" "$out/starbite-os-uibench.img" "$out/starbite-os-survival.img" "$out/starbite-os-gputest.img" "$out/starbite-os-gpualu.img" "$out/starbite-os-gpucomp.img" "$out/starbite-os-shubprobe.img" $shubimg | head -3 || true)
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
sha256sum "$out/starbite-os-gpucomp.img" | sed "s#  .*#  starbite-os-gpucomp.img#" > "$out/starbite-os-gpucomp.img.sha256"
echo "sbos: $(cat "$out/starbite-os-gpucomp.img.sha256") (GPU compositor test P4)"
sha256sum "$out/starbite-os-survival.img" | sed "s#  .*#  starbite-os-survival.img#" > "$out/starbite-os-survival.img.sha256"
echo "sbos: $(cat "$out/starbite-os-survival.img.sha256") (survival tools self test)"
sha256sum "$out/starbite-os-uibench.img" | sed "s#  .*#  starbite-os-uibench.img#" > "$out/starbite-os-uibench.img.sha256"
echo "sbos: $(cat "$out/starbite-os-uibench.img.sha256") (UI compositor benchmark P0)"
sha256sum "$out/starbite-os-vibtest.img" | sed "s#  .*#  starbite-os-vibtest.img#" > "$out/starbite-os-vibtest.img.sha256"
echo "sbos: $(cat "$out/starbite-os-vibtest.img.sha256") (vibration test variant)"
sha256sum "$out/starbite-os-gputest.img" | sed "s#  .*#  starbite-os-gputest.img#" > "$out/starbite-os-gputest.img.sha256"
echo "sbos: $(cat "$out/starbite-os-gputest.img.sha256") (GPU ladder H2 variant)"
sha256sum "$out/starbite-os-gpualu.img" | sed "s#  .*#  starbite-os-gpualu.img#" > "$out/starbite-os-gpualu.img.sha256"
echo "sbos: $(cat "$out/starbite-os-gpualu.img.sha256") (GPU compute H2d variant)"
sha256sum "$out/starbite-os-shubprobe.img" | sed "s#  .*#  starbite-os-shubprobe.img#" > "$out/starbite-os-shubprobe.img.sha256"
echo "sbos: $(cat "$out/starbite-os-shubprobe.img.sha256") (sensor hub read-only probe)"
if [ -n "$shubimg" ]; then
    sha256sum "$shubimg" | sed "s#  .*#  starbite-os-shub.img#" > "$out/starbite-os-shub.img.sha256"
    echo "sbos: $(cat "$out/starbite-os-shub.img.sha256") (sensor hub full start; carries the patch blob: a test image, not for release)"
else
    echo "sbos: starbite-os-shub.img skipped: no patch blob at $SHUB_PATCH (starbite/sbos/tools/shub-patch.py)"
fi
echo "sbos: $(cat "$out/starbite-os.img.sha256") ($(wc -c < "$out/starbite-os.img") bytes)"
