#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteRTC host tests: hw/ alone, and through StarbiteACPM's hw/ (IPC + write policy) on its mock APM.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
acpm="$here/../../StarbiteACPM"
out=$(mktemp -d "${TMPDIR:-/tmp}/sbrtc.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
flags="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g -fsanitize=address,undefined"
$cc $flags -o "$out/test_rtc" "$here/test_rtc.c" "$here/../hw/s2mps18_rtc.c" "$acpm/hw/acpm_ipc.c" \
    "$acpm/hw/s2mps18_policy.c" "$acpm/test/mock_apm.c"
"$out/test_rtc" "$@"
