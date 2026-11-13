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
$CC -c -O2 -std=gnu11 $WARN -I"$here" -I"$here/../kern" -I"$here/../../utility" "$here/host/main.c" -o "$out/main.o"
# Track A: the time zones as the kernel has them (kern/tzdb.c on the Utility OS's civil dates)
$CC -c -O2 -std=gnu11 $WARN -I"$here/../kern" -I"$here/../../utility" "$here/../kern/tzdb.c" -o "$out/tzdb.o"
$CC -c -O2 -std=gnu11 $WARN -I"$here/../kern" "$here/../kern/tzcities.c" -o "$out/tzcities.o"   # 8a: the cities (GeoNames)
$CC -c -O2 -std=gnu11 $WARN -I"$here/../kern" "$here/../kern/tzcity.c" -o "$out/tzcity.o"
$CC -c -O2 -std=gnu11 $WARN -I"$here/../../utility" "$here/../../utility/tz.c" -o "$out/tz.o"
# survival-next: the survival API with simulated state (Emergency mode's tools; nothing touches hardware)
$CC -c -O2 -std=gnu11 $WARN "$here/../guide/survival_host.c" -o "$out/survival_host.o"
# Track A: the passcode (kern/passcode.c, pure) behind the host's sh_pass_* fakes
$CC -c -O2 -std=gnu11 $WARN "$here/../kern/passcode.c" -o "$out/passcode.o"
$CC -o "$out/sbtk-host" "$out"/tk_*.o "$out/shell_lock.o" "$out/plat.o" "$out/main.o" "$out/survival_host.o" "$out/passcode.o" "$out/tzdb.o" "$out/tzcities.o" "$out/tzcity.o" "$out/tz.o"
echo "$out/sbtk-host"
