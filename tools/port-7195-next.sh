#!/usr/bin/env bash
# iOS 14 kernel: replay the iOS 16 STARLTE series (xnu-8796.141.3 + the fork's
# patches + xnu/patches/03xx, one commit each on branch base-8796 of
# build/xnu-7195) back onto xnu-7195.141.2 (branch port-7195), one cherry-pick
# each, in order, skipping ones already picked (matched by subject) and the
# ones listed in xnu/series-7195/SKIPPED.txt. Stops at the first conflict:
# resolve, `git add`, `git -c core.editor=true cherry-pick --continue` (or
# `cherry-pick --skip` and add the subject to SKIPPED.txt), and run again.
#   tools/port-7195-next.sh            set up build/xnu-7195 if missing, then replay
#   tools/export-7195.sh               write the port commits to xnu/series-7195
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$P/build/xnu-7195 S=$P/xnu/series-7195
g() { git -C "$T" -c user.name=starbite -c user.email=starbite@local "$@"; }
if [ ! -d "$T/.git" ]; then
    git init -q "$T"
    g remote add origin https://github.com/apple-oss-distributions/xnu.git
    g fetch -q --depth 1 origin tag xnu-7195.141.2 tag xnu-8796.141.3
    # base-8796: the iOS 16 tree exactly as tools/build-xnu-local.sh builds it.
    F=$P/build/ios-a5xelte/xnu/patches
    [ -d "$F" ] || { echo "the fork checkout (build/ios-a5xelte) is missing"; exit 1; }
    g checkout -q -b base-8796 xnu-8796.141.3
    g commit -q --allow-empty -m "base: xnu-8796.141.3"
    (cd "$T" && for s in "$F"/*.sh; do MACHINE_CONFIG=STARLTE MACOS_VERSION=13.5 bash "$s"; done)
    g add -A; g commit -q -m "stage: libpthread, BearSSL, hfs (8796)"
    for p in "$F"/*.patch "$P"/xnu/patches/03*.patch; do
        n=$(basename "$p")
        g log --format=%s | grep -qxF "patch: $n" && continue
        g apply --index "$p"; g commit -q -m "patch: $n"
    done
    g checkout -q -b port-7195 xnu-7195.141.2
    g commit -q --allow-empty -m "base: xnu-7195.141.2"
    (cd "$T" && for s in "$S"/*-stage-*.sh; do MACHINE_CONFIG=STARLTE MACOS_VERSION=11.5 bash "$s"; done)
    g add -A; g commit -q -m "stage: libpthread-454.120.2, BearSSL, hfs-556.100.11"
fi
[ "$(g rev-parse --abbrev-ref HEAD)" = port-7195 ] || { echo "build/xnu-7195 is not on port-7195"; exit 1; }
base7=$(g log --format=%H --grep='^base: xnu-7195' -1)
stage8=$(g log base-8796 --format=%H --grep='^stage: ' -1)
done_list=$(g log --format=%s "$base7"..HEAD)
g log base-8796 --reverse --format='%H %s' "$stage8"..base-8796 | while read -r sha subj; do
    grep -qxF "$subj" <<<"$done_list" && continue
    [ -f "$S/SKIPPED.txt" ] && grep -q "^${subj#patch: }" "$S/SKIPPED.txt" && continue
    if g cherry-pick --allow-empty "$sha" >/dev/null 2>&1; then
        echo "ok   $subj"
    else
        echo "STOP $subj ($sha)"
        g status --short | grep -E '^(UU|AA|DU|UD|AU|UA|DD) ' || true
        exit 2
    fi
done
