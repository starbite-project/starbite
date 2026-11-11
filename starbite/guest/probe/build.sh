#!/bin/sh
# guest-probe P1 and P2 (docs/STARBITE-OS-LINUX-M3.md 10): bare libsb payloads for the OS store / catalog.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/probe/build.sh [p1|p2|p4|p5|all|bundle]
# bundle: guest-linux.img / .dtb / -rd.img for the OS store (P3, Starbite OS kern/guest.c), in $OUT/bundle.
# libsb comes from git at LIBSB_REF (default aed0ce1, Starbite OS 1.0-rc2's libsb, proven on the phone), never
# from a working tree other sessions edit. Outputs in $OUT: guest-probe.img (P1), guest-p2.img (P2), + .sha256.
# P1's doorbell candidates skip the S9 DT's SPIs (probe.c dt_used, from ~/distros/_kernel/exynos9810-starlte.dtb).
# P2 carries the guest: P2_IMAGE (default the GIC-patched S9 Image from linux/build-patched.sh), the initramfs
# (run-qemu.sh BUILD_ONLY=1: guestd, sbguest.ko, init) and the DTB (mk-guest-dts.py --profile s9). Doorbells:
# P2_DB_GUEST / P2_DB_HOST (INTIDs from P1; default 0 = the guest polls).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
what=${1:-all}
REF=${LIBSB_REF:-aed0ce1}
OUT=${OUT:-/mnt/build/sbguest/probe}
WORK=${WORK:-/mnt/build/sbguest}
P2_IMAGE=${P2_IMAGE:-$WORK/Image-patched}
P2_DB_GUEST=${P2_DB_GUEST:-0}
P2_DB_HOST=${P2_DB_HOST:-0}
CROSS=${CROSS:-}
snap="$OUT/snap"
rm -rf "$snap" "$OUT/o"
mkdir -p "$snap" "$OUT/o"
git -c safe.directory='*' -C "$root" archive "$REF" starbite/libsb starbite/tools/check-image.py | tar -x -C "$snap"
lib="$snap/starbite/libsb"
CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -ffp-contract=fast -Wall -Wextra -Werror -ffile-prefix-map=$root=. -ffile-prefix-map=$snap=. \
-I$lib -I$here/.."
LDFLAGS="-pie --no-dynamic-linker -z text -z norelro -z noexecstack --build-id=none --no-warn-rwx-segments --hash-style=gnu"

cc() {  # cc SRC [extra flags]: object path on stdout
    src=$1
    shift
    obj="$OUT/o/$(basename "$src" | tr . _).o"
    flags=$CFLAGS
    [ "$(basename "$src")" = smp.c ] && flags=$(echo "$CFLAGS" | sed 's/-mcpu=cortex-a55/-march=armv8-a+simd -mtune=cortex-a55/')
    "${CROSS}gcc" $flags "$@" "$src" -o "$obj"
    echo "$obj"
}
libobjs=""
for src in "$lib"/*.c "$lib"/*.S; do
    case "$(basename "$src")" in
    sbui*.c|sb7.c|assets.S) continue ;;      # no UI: pmsg (and P2's raw framebuffer) only
    esac
    libobjs="$libobjs $(cc "$src")"
done

link() {  # link NAME OBJS...: $OUT/NAME.img
    name=$1
    shift
    "${CROSS}ld" $LDFLAGS -T "$lib/image.ld" --defsym=SB_LINK_BASE=0 $libobjs "$@" -o "$OUT/$name.elf"
    und=$("${CROSS}nm" -u "$OUT/$name.elf" || true)
    [ -z "$und" ] || { echo "$name: undefined symbols: $und" >&2; exit 1; }
    bad=$("${CROSS}readelf" -rW "$OUT/$name.elf" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
    [ -z "$bad" ] || { echo "$name: relocations other than R_AARCH64_RELATIVE: $bad" >&2; exit 1; }
    "${CROSS}objcopy" -O binary "$OUT/$name.elf" "$OUT/$name.bin"
    "${CROSS}nm" "$OUT/$name.elf" | sort > "$OUT/$name.map"
    python3 "$snap/starbite/tools/check-image.py" "$OUT/$name.bin" "$OUT/$name.img" "$OUT/$name.map"
    leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp|mnt)/[ -~]{0,40}' "$OUT/$name.img" | head -3 || true)
    [ -z "$leak" ] || { echo "$name: build-host paths in the image: $leak" >&2; exit 1; }
    sha256sum "$OUT/$name.img" | sed "s#  .*#  $name.img#" > "$OUT/$name.img.sha256"
    rm -f "$OUT/$name.bin"
    echo "$name: $(cat "$OUT/$name.img.sha256") ($(wc -c < "$OUT/$name.img") bytes, libsb $REF)"
}

if [ "$what" = p1 ] || [ "$what" = all ]; then
    link guest-probe "$(cc "$here/probe.c")" "$(cc "$here/m3stub.S")"
fi
if [ "$what" = bundle ]; then          # P3: the three store files Starbite OS's kern/guest.c loads
    [ -f "$P2_IMAGE" ] || { echo "bundle: no $P2_IMAGE (starbite/guest/linux/build-patched.sh)" >&2; exit 1; }
    BUILD_ONLY=1 WORK="$WORK" "$here/../run-qemu.sh" >/dev/null
    rdf="$WORK/out/initrd.cpio.gz"
    B="$OUT/bundle"
    mkdir -p "$B"
    python3 "$here/../mk-guest-dts.py" --profile s9 --rd-size "$(stat -c %s "$rdf")" --db-guest 511 "$B/guest-linux.dts" >/dev/null
    dtc -q -I dts -O dtb -o "$B/guest-linux.dtb" "$B/guest-linux.dts"
    cp "$P2_IMAGE" "$B/guest-linux.img"
    cp "$rdf" "$B/guest-linux-rd.img"
    (cd "$B" && sha256sum guest-linux.img guest-linux.dtb guest-linux-rd.img > SHA256SUMS && cat SHA256SUMS)
    exit 0
fi
p2_name=guest-p2
p2_kout=""
p2_dts=""
p2_defs=""
p2_rd_make="BUILD_ONLY=1 WORK=$WORK"
p2_rd=$WORK/out/initrd.cpio.gz
if [ "$what" = p5 ]; then               # P5 = L1: sbwl + foot (docs/STARBITE-LINUX-APPS.md 11)
    P2_IMAGE=${P5_IMAGE:-$WORK/Image-guest}
    p2_name=guest-p5
    p2_dts="--bootargs-extra sbguest.mode=wayland"
    p2_defs="-DP2_TERM=1 -DP2_CYCLES=2U"
    "$here/../sbwl/mk-initrd.sh" >/dev/null
    p2_rd=$WORK/out/initrd-wl.cpio.gz
    p2_rd_make=""
    what=p2
fi
if [ "$what" = p4 ]; then               # P4: the terminal on the guest kernel of its own (docs 12 steps 2 + 3)
    P2_IMAGE=${P4_IMAGE:-$WORK/Image-guest}
    p2_name=guest-p4
    p2_kout="KOUT=$WORK/gout"
    p2_dts="--fb 720x1440 --term"
    p2_defs="-DP2_TERM=1 -DP2_CYCLES=2U"
    what=p2
fi
if [ "$what" = p2 ] || [ "$what" = all ]; then
    [ -f "$P2_IMAGE" ] || { echo "$p2_name: no $P2_IMAGE (starbite/guest/linux/build-patched.sh)" >&2; exit 1; }
    [ -z "$p2_rd_make" ] || env $p2_rd_make $p2_kout "$here/../run-qemu.sh" >/dev/null
    rdf="$p2_rd"
    python3 "$here/../mk-guest-dts.py" --profile s9 --rd-size "$(stat -c %s "$rdf")" --db-guest "$P2_DB_GUEST" \
        $p2_dts "$OUT/s9-guest.dts" >/dev/null
    dtc -q -I dts -O dtb -o "$OUT/s9-guest.dtb" "$OUT/s9-guest.dts"
    cp "$P2_IMAGE" "$OUT/p2-Image"
    cp "$rdf" "$OUT/p2-initrd.cpio.gz"
    blob=$(cd "$OUT" && cc "$here/p2blob.S" -DP2_IMAGE='"p2-Image"' -DP2_DTB='"s9-guest.dtb"' -DP2_RD='"p2-initrd.cpio.gz"' -Wa,-I"$OUT")
    link $p2_name "$(cc "$here/p2.c" -DP2_DB_GUEST="$P2_DB_GUEST" -DP2_DB_HOST="$P2_DB_HOST" $p2_defs)" \
        "$(cc "$here/../host/sbghost.c")" "$blob"
    echo "$p2_name: Image $(sha256sum "$P2_IMAGE" | cut -c1-16)..., doorbells $P2_DB_GUEST/$P2_DB_HOST"
fi
