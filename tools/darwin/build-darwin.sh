#!/usr/bin/env bash
# Darwin console (Lab): our XNU 12377 kernel booted into a tiny Darwin userland, no iOS root.
#
#   tools/darwin/build-darwin.sh            -> out/darwin.xnu   (OS store entry, type xnu)
#   tools/darwin/build-darwin.sh --host     -> build/darwin/dsh-host (the same shell, run on the Mac)
#
# What it builds:
#   build/darwin/dsh      tools/darwin/{start.s,dsh.c}: one static arm64 Mach-O (no dyld, no
#                         libSystem, raw syscalls), ad-hoc signed. It is init (PID 1) and the shell.
#   out/darwin.xnu        SWEETXNU payload (ref/ios-herolte/tools/mkpayload.py --ramdisk):
#                         the 12377 kernel (strip -x copy) + the iPhone 11 ADT (out/starlte-iphone11.adt,
#                         as tools/mkimage-ios26.sh) + dsh as the RAM disk + tools/darwin/darwin-cmdline.txt.
# How it boots: mkpayload puts the RAM disk right after the kernel and adds /chosen/memory-map
# "RAMDisk" = <phys, size> to the ADT; rd=md0 makes IOFindBSDRoot create md0 from it; mockfs
# (series-12377 0021/0024, Mach-O check) mounts md0 as the root and serves the whole device as
# /sbin/launchd (plus devfs at /dev), so dsh runs as PID 1. A DEVELOPMENT kernel accepts static
# executables; the built-in SWEET AMFI makes signed binaries on the root mount platform binaries.
# herolte proved this path on hardware with xnu-8796 (ref/ios-herolte/NOTES.md, 0024/0025 boots); on
# the 12377 starlte kernel it is untested. dsh carries LC_BUILD_VERSION platform=iOS and the kernel is
# built XNU_TARGET_OS_OSX, so exec refuses it (EXEC_EXIT_REASON_WRONG_PLATFORM) unless the cmdline has
# unentitled_ios_sim_launch=1 (darwin-cmdline.txt does).
#
# The loader stages a store payload at 0x8c000000..0x91000000 (80 MiB, xnu_boot.c STAGE): kernel +
# ADT + RAM disk must stay below that; the script checks it.
#   KERNEL=... ADT=... CMDLINE=... override the inputs.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
B=build/darwin
mkdir -p "$B" out
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CFLAGS=(-O2 -std=c11 -D_FORTIFY_SOURCE=0 -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-stack-protector
	-fno-stack-check -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables)

if [ "${1:-}" = --host ]; then
	xcrun --sdk macosx clang "${CFLAGS[@]}" -DDSH_HOST -o "$B/dsh-host" tools/darwin/dsh.c
	shasum -a 256 "$B/dsh-host"
	exit 0
fi

TARGET=(-target arm64-apple-ios14.0 -isysroot "$SDK")
xcrun clang "${TARGET[@]}" -c tools/darwin/start.s -o "$B/start.o"
xcrun clang "${TARGET[@]}" "${CFLAGS[@]}" -c tools/darwin/dsh.c -o "$B/dsh.o"
xcrun ld -arch arm64 -platform_version ios 14.0 18.5 -static -e _start -headerpad 0x100 -o "$B/dsh" "$B/start.o" "$B/dsh.o"
# ld -static leaves out LC_BUILD_VERSION; add it (iOS 14.0, SDK 18.5) so the process has a platform
# and codesign writes a current CodeDirectory (exec segment, main binary), like iOS's own binaries.
python3 - "$B/dsh" <<'PY'
import struct, sys
path = sys.argv[1]
b = bytearray(open(path, 'rb').read())
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', b, 0)
assert magic == 0xfeedfacf and ftype == 2
off, first_sect = 32, len(b)
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', b, off)
    assert cmd != 0x32, 'already has LC_BUILD_VERSION'
    if cmd == 0x19:
        for i in range(struct.unpack_from('<I', b, off + 64)[0]):
            s_off = struct.unpack_from('<I', b, off + 72 + 80 * i + 48)[0]
            if s_off:
                first_sect = min(first_sect, s_off)
    off += size
lc = struct.pack('<6I', 0x32, 24, 2, 14 << 16, (18 << 16) | (5 << 8), 0)   # PLATFORM_IOS
assert off + len(lc) <= first_sect and not any(b[off:off + len(lc)]), 'no header room'
b[off:off + len(lc)] = lc
struct.pack_into('<II', b, 16, ncmds + 1, sizeofcmds + len(lc))
open(path, 'wb').write(b)
PY
undef=$(xcrun nm -u "$B/dsh" || true)
[ -z "$undef" ] || { echo "dsh has undefined symbols: $undef"; exit 1; }
codesign -f -s - -i org.starbite.dsh "$B/dsh" 2>&1 | grep -v "replacing existing signature" || true
codesign -v "$B/dsh"

KERNEL=${KERNEL:-out/kernel.development.starlte-12377}
ADT=${ADT:-out/starlte-iphone11.adt}
CMDLINE=${CMDLINE:-tools/darwin/darwin-cmdline.txt}
cp "$KERNEL" "$B/kernel.x"
xcrun strip -x "$B/kernel.x"
python3 ref/ios-herolte/tools/mkpayload.py "$B/kernel.x" "$ADT" out/darwin.xnu \
	--ramdisk "$B/dsh" --cmdline "$(tr -s ' \n' ' ' < "$CMDLINE" | sed 's/ $//')" | grep -E "^(size|ramdisk|entry|kernel_end)"
rm -f "$B/kernel.x" "$B/start.o" "$B/dsh.o"
size=$(stat -f %z out/darwin.xnu)
[ "$size" -lt $((0x91000000 - 0x8c000000)) ] || { echo "out/darwin.xnu ($size bytes) exceeds the loader's 80 MiB stage"; exit 1; }
shasum -a 256 "$B/dsh" out/darwin.xnu
echo "store: tools/store/sbstore2.py build ... darwin.xnu=out/darwin.xnu   (type xnu from the suffix)"
