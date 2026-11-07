#!/usr/bin/env bash
# Host test of PpttBuild: build, run on the release PPTT, then let iasl (lima VM) disassemble the
# result as an independent parse.
set -euo pipefail
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/pptt-test.$$
mkdir -p "$out"; trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -o "$out/t" pptt-host-test.c ../PpttBuild.c
clang -target aarch64-none-elf -ffreestanding -std=c11 -Wall -Wextra -Werror -O2 -c ../PpttBuild.c -o "$out/p.o"
"$out/t" release-pptt.bin "$out/new.dat"
limactl shell starbite -- bash -c 'd=$(mktemp -d); cd $d; cat > pptt.dat; iasl -d pptt.dat >log 2>&1 || { cat log; exit 1; }; grep -c "Cache Type" pptt.dsl | sed "s/^/iasl: cache structures: /"; grep -i -E "error|warning" pptt.dsl log | head -5; rm -rf $d' < "$out/new.dat"
