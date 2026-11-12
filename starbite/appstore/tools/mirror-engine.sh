#!/bin/sh
# The engine and the catalog into the offline mirror (-> appstore.erofs's engine/): the base's `sbstore daemon` runs
# this copy when its VERSION is newer (sbstore.py card_engine), so store fixes reach the phone with the card file.
#   sh starbite/appstore/tools/mirror-engine.sh MIRROR_DIR
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
M=${1:?usage: mirror-engine.sh MIRROR_DIR}
PY=${PYTHON:-python3}
rm -rf "$M/engine"; mkdir -p "$M/engine"
cp "$here/sbstore.py" "$M/engine/sbstore.py"
"$PY" -I -B "$here/tools/catalog.py" build "$M/engine" >/dev/null
echo "mirror-engine: sbstore $("$PY" -I -B "$here/sbstore.py" version), $(grep -c '"id"' "$M/engine/catalog.json") catalog entries -> $M/engine"
