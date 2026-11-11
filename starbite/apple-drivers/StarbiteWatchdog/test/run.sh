#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteWatchdog host tests: the dead-man contract; the maths cross-checked against today's kernel's
# starlte_health.h when build/xnu-12377 is present (read only).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sbwdt.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
flags="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -O1 -g -fsanitize=address,undefined"
health="$root/build/xnu-12377/bsd/dev/starlte_health.h"
extra=""
if [ -f "$health" ]; then
	extra="-DHAVE_STARLTE_HEALTH -DSTARLTE_HEALTH=\"$health\""
fi
$cc $flags $extra -o "$out/test_wdt" "$here/test_wdt.c" "$here/../hw/sbwdt.c"
"$out/test_wdt" "$@"
