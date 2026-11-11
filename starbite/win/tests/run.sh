#!/bin/sh
# Host tests for the stage 2 Windows drivers' portable cores. No WDK needed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbwin-host-test
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -o "$out" "$here/host_test.c" "$here/../sbthermal/sbth_core.c" "$here/../sbdisplay/sbdc_core.c"
"$out"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -o "$out-mali" "$here/mali_test.c" "$here/../sbmali/sbm_core.c" "$here/../sbmali/sbg_core.c" "$here/../common/sbw_acpm.c"
"$out-mali"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -o "$out-sbx" "$here/sbx_test.c" "$here/../sbwddm/sbx_cmd.c"
"$out-sbx"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -o "$out-keys" "$here/keys_test.c" "$here/../sbkeys/sbk_core.c"
"$out-keys"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -DSB_HOST \
    -o "$out-crash" "$here/crash_test.c" "$here/../sbwdog/sbcl_core.c"
"$out-crash"
