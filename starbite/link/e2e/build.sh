#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Builds the Starbite Link end-to-end measuring tools (Mac only). Output: build/link/e2e/
#   sblinkd-drag   the real sender (win/sender: main, session, encoder, flow control, TCP) with capture_drag.c:
#                  a mostly static desktop with one window dragged around, compositor-style dirty rects
#   encbench       CPU per frame of the sender's encoder pipeline on that desktop, decoded back exactly
#   viewbench      headless viewer (the Mac app's sbv.c session) that measures frames/s, Mbit/s, image match
# Usage: starbite/link/e2e/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
snd="$here/../win/sender"
proto="$here/../proto"
out="$root/build/link/e2e"
mkdir -p "$out"
CC=${CC:-clang}
warn="-std=c11 -Wall -Wextra -Wshadow -Wno-unused-parameter -Werror -D_DARWIN_C_SOURCE"
id="$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo nogit)+drag"

$CC -std=c99 -O2 -c "$proto/lz4/lz4.c" -o "$out/lz4.o"
$CC $warn -O2 -c "$proto/sblink.c" -o "$out/sblink.o"
$CC $warn -O2 -c "$here/scene.c" -o "$out/scene.o"

common="$snd/session.c $snd/pipeline.c $snd/input_map.c $snd/tcp.c $snd/plat.c"
# shellcheck disable=SC2086
$CC $warn -O2 "-DSB_BUILD_ID=\"$id\"" -o "$out/sblinkd-drag" "$snd/main.c" $common "$snd/inject_rec.c" \
    "$here/capture_drag.c" "$out/scene.o" "$out/sblink.o" "$out/lz4.o" -lpthread
# shellcheck disable=SC2086
$CC $warn -O2 -o "$out/encbench" "$here/encbench.c" "$snd/pipeline.c" "$snd/plat.c" \
    "$out/scene.o" "$out/sblink.o" "$out/lz4.o" -lpthread
$CC $warn -O2 -o "$out/viewbench" "$here/viewbench.c" "$here/../mac/sbv.c" "$out/sblink.o" "$out/lz4.o"
echo "built: $out/sblinkd-drag $out/encbench $out/viewbench"

rm -f "$out/sblinkd-drag-fix" "$out/session-fixed.c"   # the dead-link fix is in session.c now (2026-11-07)
