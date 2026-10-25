#!/usr/bin/env bash
# Regenerate 0001-starbite-mmu-breadcrumbs.patch from a clean edk2 checkout in
# the lima VM by running gen-crumbs.py, gen-vectors.py, gen-cntfrq.py and gen-ufs.py.
#   tools/woa/patches-edk2/regen.sh
set -euo pipefail
cd "$(dirname "$0")"
here=$PWD
limactl shell starbite -- bash -c "
    set -e
    cd ~/work/edk2
    git checkout -q -- .
    python3 '$here/gen-crumbs.py'
    python3 '$here/gen-vectors.py'
    python3 '$here/gen-cntfrq.py'
    python3 '$here/gen-ufs.py'
    git diff > '$here/0001-starbite-mmu-breadcrumbs.patch'
    git checkout -q -- .
"
grep -c '^+' 0001-starbite-mmu-breadcrumbs.patch
