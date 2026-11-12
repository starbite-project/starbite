#!/bin/sh
# sbtk on the host: assets, build, the concept's reference frames, the comparison, the films.
#   sh starbite/sbos/ui/tools/run.sh assets      fonts (fontTools + HarfBuzz), icons (headless Chrome), the pack
#   sh starbite/sbos/ui/tools/run.sh build       build/sbtk/sbtk-host (and, once, the previews' made-up card)
#   sh starbite/sbos/ui/tools/run.sh card        build/sbtk/card again (tools/sbtk-fakecard.py)
#   sh starbite/sbos/ui/tools/run.sh imgtest     Photos' decoders (shell_media_img.inc) against PIL
#   sh starbite/sbos/ui/tools/run.sh compare     tools/states.json: Chrome and sbtk-host -> build/sbtk/compare/
#   sh starbite/sbos/ui/tools/run.sh film NAME   a 60 fps film of tools/films.txt's NAME -> build/sbtk/films/NAME.mp4
#                                                (UIBENCH=file with the phone's uibench: lines adds the A55 estimate)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ui=$(cd "$here/.." && pwd)
root=$(cd "$ui/../../.." && pwd)
py=${PY:-$root/build/venv/bin/python}
[ -x "$py" ] || py=/Users/dev/project-starbite/build/venv/bin/python
out="$root/build/sbtk"
cd "$root"
case "${1:-}" in
assets)
    "$py" "$here/sbtk-fonts.py" "$out/fonts"
    node "$here/sbtk-icons.mjs" "$out/icons"
    "$py" "$here/sbtk-pack.py" "$out" ;;
build)
    sh "$ui/host/build.sh"
    # the made-up microSD card of the Photos / Files / Music previews (host/main.c), once
    [ -d "$out/card" ] || "$py" "$here/sbtk-fakecard.py" "$out/card" ;;
imgtest)
    # Photos' JPEG / PNG decoders against PIL (after build: it links the host's objects)
    mkdir -p "$out/imgtest"
    ${CC:-cc} -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I"$ui" "$here/imgtest.c" "$out/tk_math.o" "$out/plat.o" "$out/tk_raster.o" -o "$out/imgtest/imgtest"
    "$py" "$here/imgtest.py" "$out/imgtest"
    rm -rf "$out/imgtest" ;;
card)
    rm -rf "$out/card"
    "$py" "$here/sbtk-fakecard.py" "$out/card" ;;
compare)
    [ -n "${NOREF:-}" ] || node "$here/concept-ref.mjs" "$here/states.json" "$out/ref"
    mkdir -p "$out/host"
    "$py" - "$here/states.json" "$out" <<'PY'
import json, subprocess, sys
st = json.load(open(sys.argv[1])); out = sys.argv[2]
for s in st:
    r = subprocess.run([out + '/sbtk-host', out + '/sbtk.pak', 'still', out + '/host/' + s['name'] + '.ppm'] + s['host'], capture_output=True, text=True)
    print(s['name'], r.stderr.strip().replace('\n', ', '))
PY
    "$py" "$here/compare.py" "$out/ref" "$out/host" "$out/compare" $("$py" -c "import json,sys; print(' '.join(s['name'] for s in json.load(open(sys.argv[1]))))" "$here/states.json") ;;
film)
    name=$2
    script=$(grep "^$name:" "$here/films.txt" | cut -d: -f2-)
    mkdir -p "$out/films"
    # frames go straight into the encoder (half resolution): nothing raw is written to disk
    swiftc -O -o "$out/sbtk-encode" "$here/encode.swift" 2>/dev/null || true
    "$out/sbtk-host" "$out/sbtk.pak" film - "$script" half 2> "$out/films/$name.frames.txt" | "$out/sbtk-encode" - 720 1480 60 "$out/films/$name.mp4"
    "$out/sbtk-host" "$out/sbtk.pak" bench > "$out/hostbench.txt"
    if [ -n "${UIBENCH:-}" ]; then "$py" "$here/frametimes.py" "$out/films/$name.frames.txt" "$out/hostbench.txt" "$UIBENCH"
    else "$py" "$here/frametimes.py" "$out/films/$name.frames.txt"; fi ;;
*) echo "usage: run.sh assets|build|card|imgtest|compare|film NAME" >&2; exit 2 ;;
esac
