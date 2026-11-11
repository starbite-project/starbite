#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteACPM host tests: hw/ against a mock APM (test/mock_apm.c). Builds into a temp dir, writes nothing here.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sbacpm.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
flags="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g -fsanitize=address,undefined"
$cc $flags -o "$out/test_acpm" "$here/test_acpm.c" "$here/mock_apm.c" "$here/../hw/acpm_ipc.c" \
    "$here/../hw/s2mps18_policy.c"
"$out/test_acpm" "$@"
