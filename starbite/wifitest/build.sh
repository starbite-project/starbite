#!/bin/sh
# Build the Wi-Fi test image (starbite/wifitest/main.c, docs/WIFI-BCM4361.md): starbite/out/wifitest.img,
# a "bare" arm64 Image on libsb with starbite/wifi's core, for an OS store entry (catalog key
# "wifitest"). No packaging or flashing (that is the main session's).
#   sh starbite/wifitest/build.sh              (in the Lima VM: native aarch64 gcc)
#   CROSS=aarch64-linux-gnu- sh starbite/wifitest/build.sh
# Needs build/brand/menu7.sb7 (fonts), as the Engineer and Utility OS builds. On a native aarch64 host
# it first runs the core's host test (starbite/wifi/host/run.sh) and stops if it fails.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
lib="$root/starbite/libsb"
wifi="$root/starbite/wifi"
out=${WT_OUT:-"$root/starbite/out"}
: "${CROSS:=}"
mkdir -p "$out"

SB7=${SB7:-}
if [ -z "$SB7" ]; then
    for c in "$root/build/brand/menu7.sb7" "$root/../../../build/brand/menu7.sb7"; do
        if [ -f "$c" ]; then SB7=$c; break; fi
    done
fi
if [ -z "$SB7" ] || [ ! -f "$SB7" ]; then
    echo "wifitest: no build/brand/menu7.sb7 (run tools/brand/menu7-assets.py)" >&2
    exit 1
fi
python3 "$root/starbite/tools/mkassets.py" "$SB7" "$out/wt-sbassets.sb7"

if [ -z "$CROSS" ] && [ "$(uname -m)" = aarch64 ]; then
    OUT="$out/wifi-host" CC=gcc sh "$wifi/host/run.sh" >"$out/wifi-host.log" 2>&1 || {
        tail -20 "$out/wifi-host.log" >&2
        echo "wifitest: the core's host test failed" >&2
        exit 1
    }
    tail -1 "$out/wifi-host.log"
fi

CFLAGS="-c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector -fpie -fvisibility=hidden \
-nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-plt -mstrict-align -mcpu=cortex-a55 \
-fno-math-errno -Wall -Wextra -Werror -ffile-prefix-map=$root=. -I$lib -I$wifi"
objs=""
for src in "$lib"/*.c "$lib"/*.S "$wifi/core.c" "$wifi/exynos_pcie.c" "$wifi/chip.c" "$wifi/fw.c" "$wifi/ring.c" \
           "$wifi/msgbuf.c" "$wifi/link.c" "$wifi/wpa.c" "$wifi/ext4.c" "$wifi/erofs.c" "$wifi/sb/bcmw_sb.c" "$here/main.c"; do
    obj="$out/wt_$(basename "$src" | tr . _).o"
    # shellcheck disable=SC2086
    "${CROSS}gcc" $CFLAGS -DSB_ASSETS="\"$out/wt-sbassets.sb7\"" "$src" -o "$obj"
    objs="$objs $obj"
done
LDFLAGS="-pie --no-dynamic-linker -z text -z norelro -z noexecstack --build-id=none --no-warn-rwx-segments \
--hash-style=gnu -T $lib/image.ld"
# shellcheck disable=SC2086
"${CROSS}ld" $LDFLAGS --defsym=SB_LINK_BASE=0 $objs -o "$out/wifitest.elf"
und=$("${CROSS}nm" -u "$out/wifitest.elf" || true)
if [ -n "$und" ]; then
    echo "wifitest: undefined symbols:" >&2
    echo "$und" >&2
    exit 1
fi
bad=$("${CROSS}readelf" -rW "$out/wifitest.elf" | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' || true)
if [ -n "$bad" ]; then
    echo "wifitest: relocations other than R_AARCH64_RELATIVE:" >&2
    echo "$bad" >&2
    exit 1
fi
"${CROSS}nm" "$out/wifitest.elf" > "$out/wifitest.map"
"${CROSS}objcopy" -O binary "$out/wifitest.elf" "$out/wifitest.bin"
python3 "$root/starbite/tools/check-image.py" "$out/wifitest.bin" "$out/wifitest.img" "$out/wifitest.map"
echo "wifitest: $out/wifitest.img, $(wc -c < "$out/wifitest.img") bytes, sha256 $(sha256sum "$out/wifitest.img" | cut -d' ' -f1)"
