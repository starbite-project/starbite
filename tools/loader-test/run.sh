#!/bin/sh
# Host tests of the loader's boot-health code (docs/STARBITE-HEALTH.md section 8). Nothing here
# touches the phone. Any C compiler: the Mac's clang, or gcc in the Lima VM (CC=gcc).
#   sh tools/loader-test/run.sh
# Uses the committed fixtures (UN1CA's DS-ACK G960F DTBH, S-Boot's FDT with the phone's identity
# blanked by scrub-sboot-fdt.py), and when the main checkout has them: LineageOS's dt.img
# (build/android/lineage/dt.img) and the Linux DTB (build/linux/exynos9810-starlte.dtb).
set -eu
cd "$(dirname "$0")/../.."
root=$PWD
main=$(cd "$(git rev-parse --git-common-dir)/.." 2>/dev/null && pwd || echo "$root")
: "${CC:=cc}"
S=shim/starlte
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fx=tools/loader-test/fixtures
lineage=-
[ -f "$main/build/android/lineage/dt.img" ] && lineage=$main/build/android/lineage/dt.img
linux=-
[ -f "$main/build/linux/exynos9810-starlte.dtb" ] && { cp "$main/build/linux/exynos9810-starlte.dtb" "$T/linux.dtb"; linux=$T/linux.dtb; }

python3 -I -B tools/store/sbstore2.py catalog tools/store/catalog.toml "$T/cat.sbc"
python3 -I -B tools/store/sbstore2.py settings "$T/set-py.bin" --post quick --log 2 --crash save --crash-detail 3 \
    --crash-action retry --crash-timeout 120 --wdt 45 --default linux-pmos >/dev/null
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -I$S -I shim \
    -DCAT_BUF='((uintptr_t)host_cat_buf)' -include tools/loader-test/host_cat.h \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    tools/loader-test/host_test.c $S/sbfmt.c $S/dtmerge.c $S/catalog.c -o "$T/host_test"
"$T/host_test" "$T" $fx/sboot-fdt-scrubbed.dtb $fx/un1ca-G960F-dtb "$lineage" "$linux"
python3 -I -B tools/loader-test/check_dtmerge.py "$T" $fx/sboot-fdt-scrubbed.dtb
python3 -I -B tools/loader-test/test_sbstore2.py "$T"
echo "loader-test: all passed"
