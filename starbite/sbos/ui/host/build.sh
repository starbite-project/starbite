#!/bin/sh
# Build sbtk-host (the toolkit + the lock shell on the host; arm64 for the NEON paths: an Apple-silicon Mac or the
# Lima VM). Output: build/sbtk/sbtk-host. Assets first: tools/run.sh assets.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../../.." && pwd)
out="$root/build/sbtk"
mkdir -p "$out"
CC=${CC:-cc}
WARN="-Wall -Wextra -Werror -Wno-unused-parameter"
# the toolkit and the shell as the kernel builds them: freestanding, no libc headers, own math
for f in tk_math tk_raster tk_text tk_glass tk_comp tk_motion tk_pack shell_lock; do
    $CC -c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-math-errno -ffp-contract=fast $WARN -I"$here" "$here/$f.c" -o "$out/$f.o"
done
$CC -c -O2 -std=gnu11 $WARN -I"$here" "$here/host/plat.c" -o "$out/plat.o"
$CC -c -O2 -std=gnu11 $WARN -I"$here" "$here/host/main.c" -o "$out/main.o"
$CC -o "$out/sbtk-host" "$out"/tk_*.o "$out/shell_lock.o" "$out/plat.o" "$out/main.o"
echo "$out/sbtk-host"
