#!/bin/sh
# Host tests for the stage 2 Windows drivers' portable cores. No WDK needed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbwin-host-test
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -o "$out" "$here/host_test.c" "$here/../sbthermal/sbth_core.c" "$here/../sbdisplay/sbdc_core.c"
"$out"
