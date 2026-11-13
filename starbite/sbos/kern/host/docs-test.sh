#!/bin/sh
# Host test of kern/docs.c through kern/state.c (Track A, 8a: Notes and Contacts documents), with the Mac's cc (or gcc in the VM),
# ASan and UBSan. Run from anywhere; -v lists every check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${TMPDIR:-/tmp}/sbos-docs-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -DSB_HOST -DSBOS_KERNEL -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$here/.." -I"$root/starbite/libsb" -I"$root/starbite/sbos/sdk" -I"$root/starbite/guest/sbos" \
    "$here/docs-test.c" "$here/../docs.c" "$here/../state.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
