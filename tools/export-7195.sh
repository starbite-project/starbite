#!/usr/bin/env bash
# Write the iOS 14 port tree's patch commits (build/xnu-7195, branch
# port-7195, after the "stage:" commit) to xnu/series-7195, one file per
# commit, named after its subject ("patch: 0004-sweet-machine-config.patch").
# The stage scripts (*-stage-*.sh) and SKIPPED.txt there are kept as they are.
# tools/build-xnu-7195.sh copies the series into darwin-xnu-build, which runs
# the stage scripts and applies the patches in name order on xnu-7195.141.2.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$P/build/xnu-7195 OUT=$P/xnu/series-7195
g() { git -C "$T" -c user.name=starbite -c user.email=starbite@local "$@"; }
[ "$(g rev-parse --abbrev-ref HEAD)" = port-7195 ] || { echo "build/xnu-7195 is not on port-7195"; exit 1; }
[ -z "$(g status --porcelain --untracked-files=no)" ] || { echo "build/xnu-7195 has uncommitted changes"; exit 1; }
stage=$(g log --format=%H --grep='^stage: ' -1)
rm -f "$OUT"/*.patch
n=0
g log --reverse --format='%H %s' "$stage"..HEAD | while read -r sha subj; do
    case "$subj" in "patch: "*) name=${subj#patch: } ;; *) echo "not a patch commit: $subj"; exit 1 ;; esac
    g format-patch -q --stdout -1 --no-signature "$sha" > "$OUT/$name"
    [ -s "$OUT/$name" ] || { rm -f "$OUT/$name"; echo "empty: $name (kept as a commit only)"; }
done
echo "exported $(ls "$OUT"/*.patch | wc -l | tr -d ' ') patches to xnu/series-7195"
