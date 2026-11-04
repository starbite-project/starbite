#!/usr/bin/env bash
# Ventura test payload into the store under catalog key "mac13" (never boot by file name):
#   tools/macos/mac13-payload.sh N CMDLINE_FILE   -> out/xnu-mac13-vN.xnu, catalog files = [mac13-vN.xnu], store flashed
set -euo pipefail
cd "$(dirname "$0")/../.."
n=$1 cmd=$2
cat=build/store/catalog-live.toml
old=$(sed -n '/^key = "mac13"/,/^files/p' "$cat" | sed -n 's/^files = \["\(.*\)"\]/\1/p')
[ -n "$old" ] || { echo "no mac13 entry in $cat"; exit 1; }
KERNEL=out/kernel.development.starlte-12377-ventura ADT=out/starlte-mac-j313.adt tools/mkpayload-ios26.sh "mac13-v$n" "$cmd" | grep cmdline
sed -i '' "s/files = \[\"$old\"\]/files = [\"mac13-v$n.xnu\"]/" "$cat"
prev=$(ls -t build/store/ufs-store-mac13*.img | head -1)
new=build/store/ufs-store-mac13-v$n.img
python3 tools/store/sbstore2.py build "$new" --medium ufs --catalog "$cat" --import "$prev" --omit "$old" "mac13-v$n.xnu=out/xnu-mac13-v$n.xnu:xnu" | tail -1
python3 tools/store/sbstore2.py list "$new" | grep mac13
tools/store/store-flash.sh "$new" | tail -1
for f in $(ls -t build/store/ufs-store-mac13*.img | tail -n +3); do rm -f "$f"; done
