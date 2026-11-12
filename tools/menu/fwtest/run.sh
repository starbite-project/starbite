#!/bin/sh
# Host test of shim/starlte/sbfwfs.c: build with ASan/UBSan, run the synthetic selftest, then the
# sniff + health check on any local images given as arguments (reads stay in their first 8 MiB).
#   sh tools/menu/fwtest/run.sh [IMAGE...]
set -e
cd "$(dirname "$0")/../../.."
out=${TMPDIR:-/tmp}/fwtest.$$
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Ishim/starlte -o "$out" tools/menu/fwtest/fwtest.c shim/starlte/sbfwfs.c
"$out" selftest
for f in "$@"; do
    echo "== $f"
    "$out" fs "$f" 8 || true
done
rm -f "$out"
