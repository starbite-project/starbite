#!/bin/sh
# Starbite OS host test (docs/STARBITE-OS.md 12.2), run by starbite/sbos/build.sh on a native aarch64 host and
# by hand in the Lima VM:  sh starbite/tools/sbos-host-test.sh [APP.sba ...]
# Builds sbos-host-test (and sbos-apptest, the same binary) with gcc, ASan and UBSan, and runs it; with app
# files it also checks each one and broken copies of it.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${SBOS_OUT:-"$root/starbite/out"}/sbos
mkdir -p "$out"
k="$root/starbite/sbos/kern"
gcc -O1 -g -Wall -Wextra -Werror -DSB_HOST -DSBOS_KERNEL -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$root/starbite/libsb" -I"$k" -I"$root/starbite/sbos/sdk" -o "$out/sbos-host-test" \
    "$root/starbite/tools/sbos-host-test.c" "$k/timer.c" "$k/appfmt.c" "$k/state.c" "$k/batmodel.c" "$k/heatctl.c" "$k/heatk.c" \
    "$root/starbite/libsb/dvfs_levels.c"
cp "$out/sbos-host-test" "$out/sbos-apptest"
apps=${*:-}
if [ -z "$apps" ] && ls "$out"/*.sba >/dev/null 2>&1; then apps=$(ls "$out"/*.sba); fi
"$out/sbos-host-test" $apps
