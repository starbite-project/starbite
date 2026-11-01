#!/usr/bin/env bash
# Amend the port tree's uncommitted change into the build-fix patch (HEAD,
# patch: 0318-port-12377-build-fixes.patch), add NOTE to its message, apply the
# same diff to the darwin-xnu-build tree (incremental rebuild) and re-export
# xnu/series-12377.
#   tools/fix-12377.sh "file.c (0xxx): what changed upstream"
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
PORT=$P/build/xnu-12377 DXB=$P/build/dxb-12377 OUT=$P/xnu/series-12377
g() { git -C "$PORT" -c user.name=starbite -c user.email=starbite@local "$@"; }
[ "$(g log -1 --format=%s)" = "patch: 0318-port-12377-build-fixes.patch" ] || { echo "HEAD is not 0318"; exit 1; }
diff=$(mktemp)
g add -A; g diff --cached > "$diff"
[ -s "$diff" ] || { echo "nothing to fix"; exit 1; }
git -C "$DXB/xnu" apply --check "$diff"
msg=$(g log -1 --format=%B)
printf '%s\n- %s\n' "$msg" "$1" | g commit -q --amend -F -
git -C "$DXB/xnu" apply "$diff"; rm "$diff"
g format-patch -q --stdout -1 --no-signature HEAD > "$OUT/0318-port-12377-build-fixes.patch"
cat "$OUT"/*.patch "$OUT"/*.sh | shasum -a 256 | cut -c1-16 > "$DXB/.series-12377"
echo "amended 0318; build tree updated; series re-exported"
