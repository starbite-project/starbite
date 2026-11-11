#!/bin/sh
# gwin-host (gwin-host.c): L2's kernel glue and its self test with the real Next shell and a fake guest, on the Mac
# (arm64: the toolkit's NEON paths) or in the Lima VM.
#   starbite/guest/next/host/run.sh UI_TREE PAK OUT
# UI_TREE: a tree with the toolkit (starbite/sbos/ui) and starbite/guest/sbos, the L2 patch applied
# (starbite/guest/next/l2-next.patch); PAK: build/sbtk/sbtk-phone.pak; OUT: where the binary and the still go.
# GWIN_PPM (a 720 x 916 P6 PPM, foot's frame from run-term.sh MODE=wayland SURF=720x916) is the guest's frame.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
T=$1
PAK=$2
OUT=$3
ui="$T/starbite/sbos/ui"
mkdir -p "$OUT/o"
CC=${CC:-cc}
W="-Wall -Wextra -Werror -Wno-unused-parameter"
for f in tk_math tk_raster tk_text tk_glass tk_comp tk_motion tk_pack shell_lock; do
    $CC -c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-math-errno -ffp-contract=fast $W -I"$ui" "$ui/$f.c" -o "$OUT/o/$f.o"
done
$CC -c -O2 -std=gnu11 $W -I"$ui" "$ui/host/plat.c" -o "$OUT/o/plat.o"
$CC -c -O2 -std=gnu11 $W -DSBTK_NO_LINUX_FAKE -Dmain=sbtk_host_main -I"$ui" "$ui/host/main.c" -o "$OUT/o/main.o"
$CC -c -O2 -std=gnu11 $W -I"$here/fake" -I"$T/starbite/guest/sbos" -I"$ui" "$here/../gwin.c" -o "$OUT/o/gwin.o"
$CC -c -O2 -std=gnu11 $W -I"$here/fake" -I"$T/starbite/guest/sbos" -I"$ui" "$here/gwin-host.c" -o "$OUT/o/gwin-host.o"
$CC -o "$OUT/gwin-host" "$OUT"/o/*.o
"$OUT/gwin-host" "$PAK" "$OUT"
