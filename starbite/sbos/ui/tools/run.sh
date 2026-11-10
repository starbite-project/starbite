#!/bin/sh
# sbtk on the host: assets, build, the concept's reference frames, the comparison, the films.
#   sh starbite/sbos/ui/tools/run.sh assets      fonts (fontTools + HarfBuzz), icons (headless Chrome), the pack
#   sh starbite/sbos/ui/tools/run.sh build       build/sbtk/sbtk-host
#   sh starbite/sbos/ui/tools/run.sh compare     tools/states.json: Chrome and sbtk-host -> build/sbtk/compare/
#   sh starbite/sbos/ui/tools/run.sh film NAME   a 60 fps film of tools/films.txt's NAME -> build/sbtk/films/NAME.mp4
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
    sh "$ui/host/build.sh" ;;
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
    "$out/sbtk-host" "$out/sbtk.pak" film "$out/films/$name.bgra" "$script" half > "$out/films/$name.frames.txt"
    swift "$here/encode.swift" "$out/films/$name.bgra" 720 1480 60 "$out/films/$name.mp4"
    [ -n "${KEEP:-}" ] || rm -f "$out/films/$name.bgra"
    "$py" "$here/frametimes.py" "$out/films/$name.frames.txt" ;;
*) echo "usage: run.sh assets|build|compare|film NAME" >&2; exit 2 ;;
esac
