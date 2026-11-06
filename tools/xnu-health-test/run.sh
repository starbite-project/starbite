#!/bin/sh
# Host test of XNU's side of the boot health (docs/STARBITE-HEALTH.md 5.4): bsd/dev/starlte_health.h
# is taken out of xnu/series-12377/0399-starlte-health-wdt-crash-return.patch, so what is tested is
# what the kernel build applies, and checked against the loader's own code (shim/starlte: sbfmt.c,
# crash.c's needles, hw.h, wdt.h, recovery.c). Nothing here touches the phone.
#   sh tools/xnu-health-test/run.sh            (CC=gcc in the Lima VM works too)
set -eu
cd "$(dirname "$0")/../.."
: "${CC:=cc}"
P=xnu/series-12377/0399-starlte-health-wdt-crash-return.patch
L=shim/starlte
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# the new file's hunk: the "+" lines after its +++ header
awk '/^\+\+\+ b\/bsd\/dev\/starlte_health.h$/ { on = 1; next }
     on && /^@@/ { next }
     on && /^\+/ { print substr($0, 2); next }
     on { exit }' "$P" > "$T/starlte_health.h"
[ -s "$T/starlte_health.h" ] || { echo "no starlte_health.h in $P"; exit 1; }
# the console separators the test assumes are the ones herolte_ufs.c writes
grep -q '^+.*"\\n--- console before the panic:\\n" : "\\n--- console before the reset:\\n";$' "$P" ||
    { echo "$P: starlte_pmsg_save's separators changed; update test.c MID_*"; exit 1; }

def() { sed -n "s/^#define $1[[:space:]]*\(0x[0-9A-Fa-f]*\)[UL]*.*/\1/p" "$2" | head -1; }
D="-DLDR_INFORM_NORMAL=$(def INFORM_NORMAL $L/hw.h) -DLDR_INFORM_RECOVERY=$(def INFORM_RECOVERY $L/hw.h)"
D="$D -DLDR_PMU_BASE=$(def PMU_BASE $L/hw.h) -DLDR_WDT1_BASE=$(def WDT1_BASE $L/wdt.h)"
D="$D -DLDR_PMU_WDT_DISABLE=$(def PMU_WDT_DISABLE $L/wdt.h) -DLDR_PMU_WDT_MASK=$(def PMU_WDT_MASK $L/wdt.h)"
D="$D -DLDR_WDT1_PMU_BIT=$(sed -n 's/^#define WDT1_PMU_BIT[[:space:]]*\([0-9]*\)U.*/\1/p' $L/wdt.h)"
D="$D -DLDR_NEXT_MAILBOX=$(def NEXT_MAILBOX $L/recovery.c)"
case "$D" in *"=-"* | *"= "* | *"=") echo "a loader constant was not found: $D"; exit 1 ;; esac

# shellcheck disable=SC2086
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -I"$T" -I$L -I shim $D \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    tools/xnu-health-test/test.c $L/sbfmt.c -o "$T/test"
# the live payloads' boot-args, when the main checkout has them (out/ is not in git)
main=$(cd "$(git rev-parse --git-common-dir)/.." 2>/dev/null && pwd || pwd)
o=$main/out
if [ -f "$o/xnu-mac13-v33.xnu" ] && [ -f "$o/xnu-ios26-sd.xnu" ] && [ -f "$o/xnu-ios26-sdt.xnu" ]; then
    "$T/test" $L/crash.c "$o/xnu-mac13-v33.xnu" "$o/xnu-ios26-sd.xnu" "$o/xnu-ios26-sdt.xnu"
else
    echo "(payload boot-args checks skipped: no $o/xnu-*.xnu)"
    "$T/test" $L/crash.c
fi
echo "xnu-health-test: all passed"
