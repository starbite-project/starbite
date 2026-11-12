#!/bin/sh
# Track J: host stills of Settings > Startup (shell_startup.inc) through the real Settings row, on host/startup_fake.inc's
# made-up platform: each still opens Settings, scrolls to the Startup row and taps it, then taps through the page as a
# finger would (host/main.c's "tap=X,Y" state: the gestures, then settled). One PNG per view in build/startup-preview/,
# plus Settings > General and Cellular (Track B's pages) to compare the cards and rows. Builds build/sbtk/sbtk-host
# first (host/build.sh); needs build/sbtk/sbtk.pak (tools/run.sh assets) or PAK=...
#   sh starbite/sbos/ui/tools/startup-preview.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../../.." && pwd)
out="$root/build/startup-preview"
mkdir -p "$out"
PAK=${PAK:-$root/build/sbtk/sbtk.pak}
py=${PY:-$root/build/venv/bin/python}
[ -x "$py" ] || py=/Users/dev/project-starbite/build/venv/bin/python
sh "$here/host/build.sh" > /dev/null
host="$root/build/sbtk/sbtk-host"
# Settings' root scrolled so the Startup row (its subtitle row) sits at y 521; the page's rows (pt): its first card at
# PUSHED_TOP - 8 = 96, 52 per row (Operating Systems 122, Firmware 174, Recovery 226; the n-th OS 122 + 52 n; Firmware's
# Diagnostics card from 287: Debug Level 313)
open="unlock=:1 app=settings:1 sescroll=:452 tap=195,521"   # (452: Bluetooth's root row moved Startup 52 pt down)
while read -r name states; do
    # shellcheck disable=SC2086
    "$host" "$PAK" still "$out/$name.ppm" $states 2> "$out/$name.log" || { cat "$out/$name.log"; exit 1; }
    "$py" -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$out/$name.ppm" "$out/$name.png"
    rm -f "$out/$name.ppm"
    echo "$out/$name.png"
done <<VIEWS
startup-root $open
startup-os $open tap=195,122
startup-os-starbite $open tap=195,122 tap=195,122
startup-os-ios $open tap=195,122 tap=195,174
startup-os-macos $open tap=195,122 tap=195,226
startup-os-windows $open tap=195,122 tap=195,278
startup-os-linux $open tap=195,122 tap=195,382
startup-firmware $open tap=195,174
startup-pick-default $open tap=195,174 tap=195,174
startup-pick-timeout $open tap=195,174 tap=195,226
startup-pick-debug $open tap=195,174 tap=195,313
startup-recovery $open tap=195,226
startup-health-linux $open tap=195,122 tap=195,382 tap=195,456
startup-health-windows $open tap=195,122 tap=195,278 tap=195,508
startup-health-store $open tap=195,226 tap=195,506
startup-os-windows-plan $open tap=195,122 tap=195,278 tap=195,313
startup-switch-off $open tap=195,313
startup-back $open tap=195,174 tap=50,66
settings-root-back $open tap=50,66
settings-general unlock=:1 app=settings:1 sub=general
settings-cell unlock=:1 app=settings:1 sub=cell
VIEWS
