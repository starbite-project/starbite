#!/usr/bin/env bash
# Fold the uncommitted change in the port tree (build/xnu-11417) into the
# commit of patch NAME, re-export xnu/series-11417, and apply the same diff to
# the darwin-xnu-build source tree so the next build is incremental.
#   tools/fold-fix-11417.sh 0004-sweet-machine-config.patch "note for the port log"
#   tools/fold-fix-11417.sh --finish DIFF   (apply DIFF to the build tree and re-export only)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
PORT=$P/build/xnu-11417
DXB=$P/build/dxb-11417
OUT=$P/xnu/series-11417
name=$1 note=${2:-}
g() { git -C "$PORT" -c user.name=starbite -c user.email=starbite@local "$@"; }

if [ "$name" = --finish ]; then
    git -C "$DXB/xnu" apply "$2"
    note=
    finish=1
fi
if [ -z "${finish:-}" ]; then
diff=$(mktemp)
git -C "$PORT" add -A
git -C "$PORT" diff --cached > "$diff"
[ -s "$diff" ] || { echo "nothing to fold"; exit 1; }
git -C "$DXB/xnu" apply --check "$diff"
target=$(git -C "$PORT" log --format=%H -n 1 --fixed-strings --grep="patch: $name")
[ -n "$target" ] || { echo "no commit for $name"; exit 1; }
g commit -q -m "fixup! patch: $name"
GIT_SEQUENCE_EDITOR=: g rebase -q -i --autosquash "$target~1"
if [ -n "$note" ]; then
    # append the note to that commit's message
    target=$(git -C "$PORT" log --format=%H -n 1 --fixed-strings --grep="patch: $name")
    msg=$(git -C "$PORT" log -1 --format=%B "$target")
    GIT_SEQUENCE_EDITOR="sed -i '' '1s/^pick /edit /'" g rebase -q -i "$target~1"
    printf '%s\n\nbuild: %s\n' "$msg" "$note" | g commit -q --amend --allow-empty -F -
    g rebase --continue
fi
git -C "$DXB/xnu" apply "$diff"
rm "$diff"
fi

# re-export the series (same names, obsolete empty commits skipped)
for c in $(git -C "$PORT" log --reverse --format=%H); do
    subj=$(git -C "$PORT" log -1 --format=%s "$c")
    case "$subj" in
        "patch: "*) n=${subj#patch: } ;;
        "port: rebase board MASTER configs on xnu-11417") n=0307-port-board-master-configs-11417.patch ;;
        *) continue ;;
    esac
    [ -n "$(git -C "$PORT" diff-tree --no-commit-id -r "$c")" ] || continue
    git -C "$PORT" format-patch -q --stdout -1 --no-signature "$c" > "$OUT/$n"
done
cat "$OUT"/*.patch "$OUT"/*.sh | shasum -a 256 | cut -c1-16 > "$DXB/.series-11417"
echo "folded into $name; series re-exported; build tree updated"
