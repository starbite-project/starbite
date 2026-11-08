#!/bin/sh
# Build the modem probe image (starbite/modem/probe/main.c, docs/MODEM.md section 5): $out/modemprobe.img, a
# "bare" arm64 Image on libsb with starbite/modem's core, for an OS store entry (catalog keys "modemprobe",
# "modemprobe-shm"). No packaging or flashing (that is the main session's).
#   sh starbite/modem/probe/build.sh              (in the Lima VM: native aarch64 gcc)
#   CROSS=aarch64-linux-gnu- sh starbite/modem/probe/build.sh
# Needs build/brand/menu7.sb7 (fonts). On a native aarch64 host it first runs the core's host test
# (starbite/modem/host/run.sh) and stops if it fails.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
modem=$(cd "$here/.." && pwd)
root=$(cd "$here/../../.." && pwd)
lib="$root/starbite/libsb"
out=${MP_OUT:-"$modem/out"}
: "${CROSS:=}"
mkdir -p "$out"

SB7=${SB7:-}
if [ -z "$SB7" ]; then
    for c in "$root/build/brand/menu7.sb7" "$root/../../../build/brand/menu7.sb7"; do
        if [ -f "$c" ]; then SB7=$c; break; fi
    done
fi
if [ -z "$SB7" ] || [ ! -f "$SB7" ]; then
    echo "modemprobe: no build/brand/menu7.sb7 (run tools/brand/menu7-assets.py)" >&2
    exit 1
fi
python3 "$root/starbite/tools/mkassets.py" "$SB7" "$out/mp-sbassets.sb7"

if [ -z "$CROSS" ] && [ "$(uname -m)" = aarch64 ]; then
    OUT="$out/host" CC=gcc sh "$modem/host/run.sh" >"$out/host.log" 2>&1 || {
        tail -20 "$out/host.log" >&2
        echo "modemprobe: the core's host test failed" >&2
        exit 1
    }
    grep 'sbm host test' "$out/host.log"
fi

CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -Wall -Wextra -Werror -ffile-prefix-map=$root=. -I$lib -I$modem"
objs=""
for src in "$lib"/*.c "$lib"/*.S "$modem"/sbm_*.c "$modem/sb/sbm_sb.c" "$modem/sb/ext4.c" "$modem/sb/jbd2.c" "$here/main.c"; do
    obj="$out/mp_$(basename "$src" | tr . _).o"
    # shellcheck disable=SC2086
    "${CROSS}gcc" $CFLAGS -DSB_ASSETS="\"$out/mp-sbassets.sb7\"" "$src" -o "$obj"
    objs="$objs $obj"
done
# rule: no partition write from the modem code. Its objects must not even reference a block-write function
# (libsb's own code keeps them, behind sb_storage_hold, which the stage 3 keys set for the whole run).
bad=""
for o in "$out"/mp_sbm_*.o "$out"/mp_ext4_c.o "$out"/mp_jbd2_c.o "$out"/mp_main_c.o; do
    w=$("${CROSS}nm" -u "$o" | grep -E ' (sb_blk_write|ufs_write[a-z_]*|sd_write[a-z0-9_]*|sb_write|sb_write_store|store_write[a-z_]*)$' || true)
    [ -n "$w" ] && bad="$bad $(basename "$o"):$w"
done
if [ -n "$bad" ]; then
    echo "modemprobe: the modem objects reference a block write:$bad" >&2
    exit 1
fi
echo "modemprobe: no block-write reference in the modem objects"
LDFLAGS="-pie --no-dynamic-linker -z text -z norelro -z noexecstack --build-id=none --no-warn-rwx-segments \
--hash-style=gnu -T $lib/image.ld"
# shellcheck disable=SC2086
"${CROSS}ld" $LDFLAGS --defsym=SB_LINK_BASE=0 $objs -o "$out/modemprobe.elf"
und=$("${CROSS}nm" -u "$out/modemprobe.elf" || true)
if [ -n "$und" ]; then
    echo "modemprobe: undefined symbols:" >&2
    echo "$und" >&2
    exit 1
fi
bad=$("${CROSS}readelf" -rW "$out/modemprobe.elf" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
if [ -n "$bad" ]; then
    echo "modemprobe: relocations other than R_AARCH64_RELATIVE:" >&2
    echo "$bad" >&2
    exit 1
fi
# the probe must not store to MMIO: no str to the PMU / mailbox / carve-out constants is possible from the
# core (it has no write path); list any immediate of those blocks in the image's code for review
"${CROSS}nm" "$out/modemprobe.elf" > "$out/modemprobe.map"
"${CROSS}objcopy" -O binary "$out/modemprobe.elf" "$out/modemprobe.bin"
python3 "$root/starbite/tools/check-image.py" "$out/modemprobe.bin" "$out/modemprobe.img" "$out/modemprobe.map"
rm -rf "$out"/mp_*.o "$out/modemprobe.bin" "$out/mp-sbassets.sb7" "$out/host"   # keep the disk footprint small
echo "modemprobe: $out/modemprobe.img, $(wc -c < "$out/modemprobe.img") bytes, sha256 $(sha256sum "$out/modemprobe.img" | cut -d' ' -f1)"
