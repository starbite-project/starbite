#!/usr/bin/env bash
# Fetch the exynos9810-woa (S9+, star2lte) UEFI release images, check them
# against the release's SHA256SUMS and firmware.json, and extract the BOOT
# kernel slot (bootshim + 7 MiB EDK2 FD) for tools/make-xnu-boot.sh UEFI=.
#   tools/woa/get-uefi.sh [22621.7582|22621.2428]
set -euo pipefail
cd "$(dirname "$0")/../.."
build=${1:-22621.7582}
rel=build/woa-release
mkdir -p "$rel" build/woa
img="star2lte-uefi-$build.img"
[ -f "$rel/$img" ] || gh release download v0.1.0 -R ntdevlabs/exynos9810-woa -D "$rel" --clobber \
    -p "$img" -p firmware.json -p SHA256SUMS
(cd "$rel" && shasum -a 256 -c SHA256SUMS --ignore-missing | grep -F "$img: OK")
want=$(python3 -c "import json,sys; print(next(i['sha256'] for i in json.load(open('$rel/firmware.json'))['images'] if i['file']=='$img'))")
[ "$(shasum -a 256 "$rel/$img" | cut -d' ' -f1)" = "$want" ] || { echo "$img: firmware.json hash mismatch"; exit 1; }
python3 tools/starlte-pack-boot.py kernel "$rel/$img" "build/woa/uefi-$build.bin"
