#!/bin/sh
# guest-probe P1 (docs/STARBITE-OS-LINUX-M3.md 10): a bare libsb payload for the OS store / catalog.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/probe/build.sh
# libsb comes from git at LIBSB_REF (default aed0ce1, Starbite OS 1.0-rc2's libsb, proven on the phone), never
# from a working tree other sessions edit. Output: $OUT/guest-probe.img (+ .sha256).
# The doorbell candidates skip the S9 DT's SPIs (probe.c dt_used, from ~/distros/_kernel/exynos9810-starlte.dtb).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
REF=${LIBSB_REF:-aed0ce1}
OUT=${OUT:-/mnt/build/sbguest/probe}
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
objs=""
for src in "$lib"/*.c "$lib"/*.S "$here/probe.c" "$here/m3stub.S"; do
    case "$(basename "$src")" in
    sbui*.c|sb7.c|assets.S) continue ;;      # no UI: pmsg only
    esac
    obj="$OUT/o/$(basename "$src" | tr . _).o"
    flags=$CFLAGS
    [ "$(basename "$src")" = smp.c ] && flags=$(echo "$CFLAGS" | sed 's/-mcpu=cortex-a55/-march=armv8-a+simd -mtune=cortex-a55/')
    "${CROSS}gcc" $flags "$src" -o "$obj"
    objs="$objs $obj"
done
"${CROSS}ld" $LDFLAGS -T "$lib/image.ld" --defsym=SB_LINK_BASE=0 $objs -o "$OUT/guest-probe.elf"
und=$("${CROSS}nm" -u "$OUT/guest-probe.elf" || true)
if [ -n "$und" ]; then
    echo "guest-probe: undefined symbols:" >&2
    echo "$und" >&2
    exit 1
fi
bad=$("${CROSS}readelf" -rW "$OUT/guest-probe.elf" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
[ -z "$bad" ] || { echo "guest-probe: relocations other than R_AARCH64_RELATIVE: $bad" >&2; exit 1; }
"${CROSS}objcopy" -O binary "$OUT/guest-probe.elf" "$OUT/guest-probe.bin"
"${CROSS}nm" "$OUT/guest-probe.elf" | sort > "$OUT/guest-probe.map"
python3 "$snap/starbite/tools/check-image.py" "$OUT/guest-probe.bin" "$OUT/guest-probe.img" "$OUT/guest-probe.map"
# the M3 stub: ARMv8.0 only (the M3 has no LSE etc.), no literal pool outside it
"${CROSS}objdump" -d "$OUT/o/m3stub_S.o" -j .rodata.m3stub >/dev/null 2>&1 || true
leak=$(LC_ALL=C grep -a -o -E '/(Users|home|private|tmp|mnt)/[ -~]{0,40}' "$OUT/guest-probe.img" | head -3 || true)
[ -z "$leak" ] || { echo "guest-probe: build-host paths in the image: $leak" >&2; exit 1; }
sha256sum "$OUT/guest-probe.img" | sed "s#  .*#  guest-probe.img#" > "$OUT/guest-probe.img.sha256"
echo "guest-probe: $(cat "$OUT/guest-probe.img.sha256") ($(wc -c < "$OUT/guest-probe.img") bytes, libsb $REF)"
