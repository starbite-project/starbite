#!/bin/sh
# gwin-host (gwin-host.c): the L2 + L3 kernel glue (gwin.c), its self test, the L3 paths and the contract v3 paths
# (Track E: Linux apps' own switcher cards, the key bar, notifications, the clipboard, the zero-copy window) with the
# real Next shell and a fake guest (contract v2 + v3), on the Mac (arm64: the toolkit's NEON paths) or in the Lima VM.
#   starbite/guest/next/host/run.sh UI_TREE PAK OUT
# UI_TREE: a tree with the toolkit (starbite/sbos/ui) and starbite/guest/sbos; PAK: build/sbtk/sbtk-phone.pak; OUT:
# where the binary and the stills go (GWIN_STILLS: the v3 stills' folder, default OUT).
# The UI is compiled from a scratch copy (OUT/sbos/ui) with two test-only patches, never the real build's files:
#   zc-compat.patch      Track A's tk_layer_external / sh_linux_zero_copy (16925a8), when the tree has none yet
#   sw-ext-compat.patch  a stand-in for the toolkit's (Track B) external switcher cards, keyboard accessory bar and
#                        Centre withdrawal, with SH_EXT_API and TK_HAS_LAYER_EXTERNAL on (skipped when the tree's
#                        shell_switcher.inc has sw_ext_add already: then the toolkit's real API is tested)
# GWIN_PPM (a 720 x 916 P6 PPM, foot's frame from run-term.sh MODE=wayland SURF=720x916) is the guest's frame.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
T=$1
PAK=$2
OUT=$3
rm -rf "$OUT/o"
mkdir -p "$OUT/o"
sb=$(cd "$T/starbite/sbos" && pwd)
rm -rf "$OUT/sbos"
mkdir -p "$OUT/sbos"
cp -R "$sb/ui" "$OUT/sbos/ui"
ln -s "$sb/kern" "$OUT/sbos/kern"                      # (the UI's ../kern and ../guide includes)
ln -s "$sb/guide" "$OUT/sbos/guide"
ui="$OUT/sbos/ui"
zc_ok=1
if ! grep -q tk_layer_external "$ui/sbtk.h"; then
    # the stand-in is a context patch: on a toolkit that moved on it may not apply; then the zero-copy checks are
    # skipped (TK_HAS_LAYER_EXTERNAL stays off), the rest still runs
    if (cd "$OUT/sbos" && sed '/^#/d' "$here/zc-compat.patch" | patch -s -p3 --dry-run >/dev/null 2>&1); then
        (cd "$OUT/sbos" && sed '/^#/d' "$here/zc-compat.patch" | patch -s -p3)
    else
        zc_ok=0
        echo "run.sh: zc-compat.patch does not apply to this UI tree: zero-copy checks skipped"
    fi
fi
ext_ok=1
if ! grep -q sw_ext_add "$ui/shell_switcher.inc"; then
  if (cd "$OUT/sbos" && sed '/^#/d' "$here/sw-ext-compat.patch" | patch -s -p1 --dry-run >/dev/null 2>&1); then
    (cd "$OUT/sbos" && sed '/^#/d' "$here/sw-ext-compat.patch" | patch -s -p1)
    [ $zc_ok = 1 ] || sed -i.bak "/^+*#define TK_HAS_LAYER_EXTERNAL 1/d" "$ui"/*.c "$ui"/*.h "$ui"/*.inc 2>/dev/null
  else
    ext_ok=0
    echo "run.sh: sw-ext-compat.patch does not apply to this UI tree: the cards / key bar / NC checks skipped"
  fi
fi
rm -f "$ui"/*.bak
find "$ui" -name '*.orig' -delete
CC=${CC:-cc}
W="-Wall -Wextra -Werror -Wno-unused-parameter"
for f in tk_math tk_raster tk_text tk_glass tk_comp tk_motion tk_pack shell_lock; do
    $CC -c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-math-errno -ffp-contract=fast $W -I"$ui" "$ui/$f.c" -o "$OUT/o/$f.o"
done
$CC -c -O2 -std=gnu11 $W -I"$ui" "$ui/host/plat.c" -o "$OUT/o/plat.o"
$CC -c -O2 -std=gnu11 $W -DSBTK_NO_LINUX_FAKE -Dmain=sbtk_host_main -I"$ui" -I"$ui/../kern" -I"$T/starbite/utility" "$ui/host/main.c" -o "$OUT/o/main.o"
# Track A: host/main.c's time zones are kern/tzdb.c's
$CC -c -O2 -std=gnu11 $W -I"$ui/../kern" -I"$T/starbite/utility" "$ui/../kern/tzdb.c" -o "$OUT/o/tzdb.o"
$CC -c -O2 -std=gnu11 $W -I"$T/starbite/utility" "$T/starbite/utility/tz.c" -o "$OUT/o/tz.o"
$CC -c -O2 -std=gnu11 $W "$T/starbite/sbos/kern/passcode.c" -o "$OUT/o/passcode.o"     # Track A: the host's sh_pass_*
$CC -c -O2 -std=gnu11 $W "$sb/guide/survival_host.c" -o "$OUT/o/survival_host.o"   # (main.c's survival API)
$CC -c -O2 -std=gnu11 $W -I"$here/fake" -I"$T/starbite/guest/sbos" -I"$ui" "$here/../gwin.c" -o "$OUT/o/gwin.o"
zcdef=; [ $zc_ok = 1 ] || zcdef=-DGWIN_NO_ZC; [ $ext_ok = 1 ] || zcdef="$zcdef -DGWIN_NO_EXT -Wno-unused-function"
$CC -c -O2 -std=gnu11 $W $zcdef -I"$here/fake" -I"$T/starbite/guest/sbos" -I"$ui" "$here/gwin-host.c" -o "$OUT/o/gwin-host.o"
$CC -o "$OUT/gwin-host" "$OUT"/o/*.o
mkdir -p "${GWIN_STILLS:-$OUT}"
"$OUT/gwin-host" "$PAK" "$OUT"
