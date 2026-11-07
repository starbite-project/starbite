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

# sblinkd-drag-fix: the same with a one-line fix to a COPY of session.c (the source is not changed here):
# the capture loop reads `now` before it may wait for s->lk while the receive thread handles input, so
# last_rx can be later than now, now - last_rx wraps to ~2^64 and the session is declared dead at once.
fixed="$out/session-fixed.c"
sed 's/if (owner \&\& now - last_rx > (uint64_t)s->cfg.dead_ms \* 1000u) {/if (owner \&\& now > last_rx \&\& now - last_rx > (uint64_t)s->cfg.dead_ms * 1000u) {/' \
    "$snd/session.c" > "$fixed"
if grep -q 'now > last_rx && now - last_rx' "$fixed"; then
    common_fixed="$fixed $snd/pipeline.c $snd/input_map.c $snd/tcp.c $snd/plat.c"
    # shellcheck disable=SC2086
    $CC $warn -O2 -I"$snd" "-DSB_BUILD_ID=\"$id-fix\"" -o "$out/sblinkd-drag-fix" "$snd/main.c" $common_fixed \
        "$snd/inject_rec.c" "$here/capture_drag.c" "$out/scene.o" "$out/sblink.o" "$out/lz4.o" -lpthread
    echo "built: $out/sblinkd-drag-fix (session.c with the dead-link fix)"
else
    rm -f "$out/sblinkd-drag-fix"
    echo "sblinkd-drag-fix: not built (session.c no longer has the line; probably fixed upstream)"
fi
