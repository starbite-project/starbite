#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Builds the Starbite Link Mac viewer and its test source:
#   build/link/StarbiteLink.app        the app (Swift + AppKit, ad hoc signed)
#   build/link/mac/fakesender          fake SBLINK source (TCP 47474, test desktop, input log)
#   build/link/mac/sbv-test            unit + fuzz test of the viewer session (sbv.c)
#   build/link/mac/obj/                objects, libsblinkmac.a (sblink.c + lz4.c + sbv.c + link.m)
# Usage: starbite/link/mac/build.sh [--san]   (--san: ASan + UBSan build of everything, for tests)
# Needs only the Command Line Tools (clang, swiftc). No downloads. The Swift module cache (~200 MB) is
# deleted after the build to spare the disk; SBLINK_KEEP_CACHE=1 keeps it (rebuilds then take ~10 s, not ~70 s).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
proto="$here/../proto"
out="$root/build/link"
obj="$out/mac/obj"
app="$out/StarbiteLink.app"
mkdir -p "$obj" "$out/mac"

opt="-O2"
san=""
swsan=""
if [ "${1:-}" = "--san" ]; then
    opt="-O1 -g"
    san="-fsanitize=address,undefined -fno-sanitize-recover=all"
    swsan="-sanitize=address"
fi
min="-mmacosx-version-min=13.0"
warn="-Wall -Wextra -Wshadow -Wno-unused-parameter -Werror"

# the portable core (lz4.c is third party: its own quieter flags)
clang $min -std=c99 $opt $san -c "$proto/lz4/lz4.c" -o "$obj/lz4.o"
clang $min -std=c99 $opt $san $warn -Wconversion -Wno-sign-conversion -c "$proto/sblink.c" -o "$obj/sblink.o"
# the viewer session and the link (transports)
clang $min -std=c99 $opt $san $warn -Wconversion -Wno-sign-conversion -c "$here/sbv.c" -o "$obj/sbv.o"
clang $min -fobjc-arc $opt $san $warn -c "$here/link.m" -o "$obj/link.o"
rm -f "$obj/libsblinkmac.a"
ar rcs "$obj/libsblinkmac.a" "$obj/lz4.o" "$obj/sblink.o" "$obj/sbv.o" "$obj/link.o"

# the app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$here/Info.plist" "$app/Contents/Info.plist"
printf 'APPL????' > "$app/Contents/PkgInfo"
swiftc $swsan -O -swift-version 5 -target arm64-apple-macos13.0 \
    -module-cache-path "$out/mac/swift-module-cache" \
    -I "$here/cbridge" \
    "$here/main.swift" "$here/DisplayView.swift" "$here/KeyMap.swift" \
    -L "$obj" -lsblinkmac \
    -framework AppKit -framework IOSurface -framework QuartzCore -framework IOUSBHost -framework IOKit \
    -framework Carbon -framework SystemConfiguration -framework ImageIO \
    $( [ -n "$san" ] && echo "-Xlinker -lclang_rt.ubsan_osx_dynamic -Xlinker -L$(clang -print-resource-dir)/lib/darwin" ) \
    -o "$app/Contents/MacOS/StarbiteLink"
codesign --force --sign - "$app" >/dev/null 2>&1 && echo "signed ad hoc: $app"

# the fake source
clang $min -std=gnu11 $opt $san $warn -c "$here/fakesender.c" -o "$obj/fakesender.o"
clang $min $san "$obj/fakesender.o" "$obj/sblink.o" "$obj/lz4.o" "$obj/sbv.o" \
    -framework CoreGraphics -framework CoreText -framework CoreFoundation -o "$out/mac/fakesender"

# the viewer session's unit + fuzz test
clang $min -std=c11 $opt $san $warn -c "$here/sbv_test.c" -o "$obj/sbv_test.o"
clang $min $san "$obj/sbv_test.o" "$obj/sbv.o" "$obj/sblink.o" "$obj/lz4.o" -o "$out/mac/sbv-test"

[ "${SBLINK_KEEP_CACHE:-0}" = 1 ] || rm -rf "$out/mac/swift-module-cache"
echo "built: $app"
echo "built: $out/mac/fakesender"
