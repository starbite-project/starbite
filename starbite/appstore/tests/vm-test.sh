#!/bin/bash
# sbstore against the real world in the VM's trixie arm64 root (tools/vm-chroot.sh make first):
#   limactl shell starbite -- $PWD/starbite/appstore/tests/vm-test.sh [CATALOG_DIR] [TESTS...]
# A throwaway overlay of the root per run; real GitHub releases, real source builds, real apt. PASS/FAIL lines.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$here/.." && pwd)
CAT=${1:-$TOP/catalog}; shift || true
T=${TESTS:-${*:-neovim-release btop-release btop-source gnome-mines-distro}}
CH=$TOP/tools/vm-chroot.sh
NAME=vmtest-$$
M=$("$CH" fresh "$NAME" | tail -1)
trap 'R=$M "$CH" umount; "$CH" drop "$NAME" >/dev/null 2>&1' EXIT
# a GitHub token (optional, lifts the 60 requests / hour limit) from a file, never on a command line
[ -n "${GH_TOKEN_FILE:-}" ] && sudo install -m 600 "$GH_TOKEN_FILE" "$M/root/.ghtok"
run() { R=$M "$CH" run env SBSTORE_CATALOG="$CAT" SBSTORE_MIRROR=/nonexistent SBSTORE_REPO=/nonexistent \
        sh -c '[ -f /root/.ghtok ] && export SBSTORE_GITHUB_TOKEN=$(cat /root/.ghtok); exec "$@"' sh "$@"; }
sb() { run python3 "$TOP/sbstore.py" "$@"; }
pass=0; fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
no() { echo "FAIL $*"; fail=$((fail + 1)); }
for t in $T; do
    id=${t%-*}; m=${t##*-}
    t0=$(date +%s)
    if sb install "$id" --method "$m" > "/tmp/$NAME-$t.log" 2>&1; then
        dt=$(( $(date +%s) - t0 ))
        ver=$(sb status --json | python3 -c "import json,sys; print([r['installed'] for r in json.load(sys.stdin) if r['id']=='$id'][0])")
        ok "$t installed $ver in ${dt}s"
        bin=$(grep -o '^exec = "[^" ]*' "$CAT/$id.toml" | head -1 | sed 's/exec = "//')
        if run sh -c "command -v $bin >/dev/null && ($bin --version || $bin -v || true) 2>&1 | head -2"; then ok "$t runs"; else no "$t does not run"; fi
        [ -f "$M/usr/local/share/applications/sbstore-$id.desktop" ] && ok "$t launcher" || [ "$m" = distro ] || no "$t no launcher"
    else
        no "$t install: $(tail -3 "/tmp/$NAME-$t.log" | tr '\n' ' ')"
    fi
done
for t in $T; do id=${t%-*}; sb remove "$id" >/dev/null 2>&1; done
left=$(ls "$M/opt/sbstore" 2>/dev/null; ls "$M/usr/local/bin" 2>/dev/null)
[ -z "$left" ] && ok "removed everything" || no "left behind: $left"
echo "RESULT pass=$pass fail=$fail"
