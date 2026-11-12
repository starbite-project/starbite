#!/bin/sh
# Host test of kern/sbfw_health.c (Track J FW4: the health checks, the store check, the logs bundle, the slots) with the
# loader's sbfwfs.c and libsb's real sbfw_api.c / sbfw.c / str.c, with the Mac's cc (or gcc in the VM), ASan and UBSan.
# SBSTORE2=<claude/track-j's tools/store/sbstore2.py> also reads the saved bundle back with its "logs" command.
# Run from anywhere; -v lists every check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
lib="$root/starbite/libsb"
out=${TMPDIR:-/tmp}/sbos-sbfw-health-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -D_FORTIFY_SOURCE=0 -D_GNU_SOURCE -DSB_HOST \
    -DSB_HOST_FAKE_MEM -DSBOS_KERNEL -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$here/.." -I"$lib" -I"$root/starbite/sbos/sdk" -I"$root/starbite/guest/sbos" \
    "$here/sbfw-health-test.c" "$here/../sbfw_health.c" "$root/shim/starlte/sbfwfs.c" \
    "$lib/sbfw.c" "$lib/sbfw_api.c" "$lib/str.c" -o "$out"
set +e
"$out" "$@" "$out.bundle"
rc=$?
if [ $rc = 0 ] && [ -n "${SBSTORE2:-}" ]; then
    python3 "$SBSTORE2" logs "$out.bundle" "$out.logs" && grep -q "Starbite OS's own log" "$out.logs/sbos-log" &&
        grep -q "starting starbite-os" "$out.logs/loader-lines" && [ "$(wc -c < "$out.logs/crashlog")" -eq 65536 ]
    r=$?
    if [ $r = 0 ]; then echo "sbstore2.py logs: the bundle reads back (sbos-log, loader-lines, crashlog)"
    else echo "FAIL: sbstore2.py logs could not read the bundle"; rc=1; fi
fi
rm -rf "$out" "$out.dSYM" "$out.bundle" "$out.logs"
exit $rc
