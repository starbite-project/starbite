#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Builds sblinkd (the Starbite Link sender) and its host tests. Output: build/link/sender/
#   1. host tests (Mac clang): test_host under ASan+UBSan, then -O2; runs both unless --no-run
#   2. sblinkd-host (Mac): the real sender with a synthetic desktop over TCP, for the viewer app
#   3. sblinkd.exe (Windows 11 ARM64, tools/win/build.sh exe), signed with tools/win/sign.sh, and a
#      PE check: ARM64, WINDOWS subsystem, imports only system DLLs + the UCRT
# Usage: starbite/link/win/sender/build.sh [--no-run] [--host-only] [--win-only]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
proto="$here/../../proto"
out="$root/build/link/sender"
mkdir -p "$out"
run=1 host=1 win=1
for a in "$@"; do
    case $a in
    --no-run) run=0 ;;
    --host-only) win=0 ;;
    --win-only) host=0 ;;
    *) sed -n '3,9p' "$0"; exit 2 ;;
    esac
done

portable="session.c pipeline.c input_map.c tcp.c plat.c"
if [ $host = 1 ]; then
    CC=${CC:-clang}
    warn="-std=c11 -Wall -Wextra -Wshadow -Wno-unused-parameter -Werror -D_DARWIN_C_SOURCE"
    id="$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo nogit)+host"
    san="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
    srcs=""
    for f in $portable capture_synth.c inject_rec.c; do srcs="$srcs $here/$f"; done
    # lz4.c is third party: its own flags
    $CC -std=c99 $san -c "$proto/lz4/lz4.c" -o "$out/lz4-san.o"
    $CC -std=c99 -O2 -c "$proto/lz4/lz4.c" -o "$out/lz4.o"
    $CC $warn $san -c "$proto/sblink.c" -o "$out/sblink-san.o"
    $CC $warn -O2 -c "$proto/sblink.c" -o "$out/sblink.o"
    # shellcheck disable=SC2086
    $CC $warn $san -o "$out/test_host-san" "$here/test_host.c" $srcs "$out/sblink-san.o" "$out/lz4-san.o" -lpthread
    # shellcheck disable=SC2086
    $CC $warn -O2 -o "$out/test_host" "$here/test_host.c" $srcs "$out/sblink.o" "$out/lz4.o" -lpthread
    # shellcheck disable=SC2086
    $CC $warn -O2 "-DSB_BUILD_ID=\"$id\"" -o "$out/sblinkd-host" "$here/main.c" $srcs "$out/sblink.o" "$out/lz4.o" -lpthread
    echo "host: $out/test_host-san $out/test_host $out/sblinkd-host"
fi

if [ $win = 1 ]; then
    kit=${SBWIN:-$HOME/sbwin}
    if [ ! -f "$kit/PINS" ]; then
        echo "windows: skipped (no kit at $kit; tools/win/setup.sh)"
    else
        wsrc=""
        for f in main.c $portable capture_win.c inject_win.c usbfn_win.c service_win.c; do wsrc="$wsrc $here/$f"; done
        # windows subsystem: no console window when started at logon (--console attaches one)
        # shellcheck disable=SC2086
        LDFLAGS="/subsystem:windows,10.00 /entry:wmainCRTStartup" \
        LIBS="user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib ws2_32.lib cfgmgr32.lib advapi32.lib powrprof.lib wtsapi32.lib" \
        CFLAGS="-Wno-unused-function -D_CRT_SECURE_NO_WARNINGS" \
            "$root/tools/win/build.sh" exe "$out/sblinkd.exe" $wsrc "$proto/sblink.c" "$proto/lz4/lz4.c"
        "$root/tools/win/sign.sh" "$out/sblinkd.exe"
        . "$root/tools/win/env.sh"
        hdr=$("$SB_READOBJ" --file-headers "$out/sblinkd.exe")
        echo "$hdr" | grep -q 'Machine: IMAGE_FILE_MACHINE_ARM64 ' || { echo "PE: not ARM64"; exit 1; }
        echo "$hdr" | grep -q 'Subsystem: IMAGE_SUBSYSTEM_WINDOWS_GUI' || { echo "PE: not the WINDOWS subsystem"; exit 1; }
        dlls=$("$SB_READOBJ" --coff-imports "$out/sblinkd.exe" | sed -n 's/^ *Name: //p' | sort -u)
        bad=$(echo "$dlls" | grep -v -i -E '^(kernel32|user32|gdi32|d3d11|dxgi|ws2_32|cfgmgr32|advapi32|powrprof|wtsapi32|ntdll|api-ms-win-crt-[a-z0-9-]+)\.dll$' || true)
        echo "PE: ARM64, WINDOWS_GUI, imports: $(echo $dlls | tr '\n' ' ')"
        [ -z "$bad" ] || { echo "PE: unexpected imports: $bad"; exit 1; }
        (cd "$out" && shasum -a 256 sblinkd.exe > sblinkd.exe.sha256)
    fi
fi

if [ $host = 1 ] && [ $run = 1 ]; then
    echo "== test_host (ASan + UBSan) =="
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "$out/test_host-san"
    echo "== test_host (-O2) =="
    "$out/test_host"
fi
