#!/bin/sh
# Track J: host stills of Settings > Startup (shell_startup.inc) through the temporary preview hook in shell_lock.c
# (SBTK_STARTUP_PREVIEW: Settings' sub-page 9 and the "startup=VIEW:N" state), on host/startup_fake.inc's made-up
# platform. Builds its own sbtk-host (the regular build/sbtk/sbtk-host stays without the hook), then one PNG per
# view into build/startup-preview/. Needs build/sbtk/sbtk.pak (tools/run.sh assets) or PAK=...
#   sh starbite/sbos/ui/tools/startup-preview.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../../.." && pwd)
out="$root/build/startup-preview"
ob="$out/obj"
mkdir -p "$ob"
CC=${CC:-cc}
PAK=${PAK:-$root/build/sbtk/sbtk.pak}
py=${PY:-$root/build/venv/bin/python}
[ -x "$py" ] || py=/Users/dev/project-starbite/build/venv/bin/python
WARN="-Wall -Wextra -Werror -Wno-unused-parameter"
for f in tk_math tk_raster tk_text tk_glass tk_comp tk_motion tk_pack shell_lock; do
    $CC -c -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-math-errno -ffp-contract=fast $WARN -DSBTK_STARTUP_PREVIEW \
        -I"$here" "$here/$f.c" -o "$ob/$f.o"
done
$CC -c -O2 -std=gnu11 $WARN -I"$here" "$here/host/plat.c" -o "$ob/plat.o"
$CC -c -O2 -std=gnu11 $WARN -I"$here" "$here/host/main.c" -o "$ob/main.o"
$CC -c -O2 -std=gnu11 $WARN "$here/../guide/survival_host.c" -o "$ob/survival_host.o"
$CC -o "$ob/sbtk-host" "$ob"/tk_*.o "$ob/shell_lock.o" "$ob/plat.o" "$ob/main.o" "$ob/survival_host.o"
# name, then the view: root, os, osd:N (the N-th catalog row), fw, pick:N (0 default .. 6 after a crash), rec
while read -r name view; do
    "$ob/sbtk-host" "$PAK" still "$out/$name.ppm" unlock=:1 app=settings:1 "startup=$view" 2> "$out/$name.log" || {
        cat "$out/$name.log"; exit 1; }
    "$py" -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$out/$name.ppm" "$out/$name.png"
    rm -f "$out/$name.ppm"
    echo "$out/$name.png"
done <<'VIEWS'
startup-root root
startup-os os
startup-os-ios osd:1
startup-os-macos osd:2
startup-os-windows osd:3
startup-os-linux osd:5
startup-os-starbite osd:0
startup-firmware fw
startup-pick-default pick:0
startup-pick-timeout pick:1
startup-pick-debug pick:2
startup-pick-watchdog pick:4
startup-pick-after pick:6
startup-recovery rec
VIEWS
