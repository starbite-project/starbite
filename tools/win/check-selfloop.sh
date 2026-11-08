#!/bin/sh
# Fails (exit 1) if any ARM64 PE image given has a branch to its own address (b ., cbz xN, . ...): the shape a
# function calling itself unconditionally on some path compiles to (sbg_core's barrier helper in T12's first
# build, caught before it reached the phone). Uses the Mac's llvm-objdump (tools/win/env.sh).
. "$(dirname "$0")/env.sh"
bad=0
for f in "$@"; do
    hits=$("$SB_OBJDUMP" -d --no-show-raw-insn "$f" | perl -ne '
        print "    $1: $2\n" if /^\s*([0-9a-f]+):\s+(b|bl|b\.\w+|cbz|cbnz|tbz|tbnz)\s+(?:[wx]\d+,\s*)?(?:#\d+,\s*)?0x([0-9a-f]+)/ && hex($1) == hex($3)')
    if [ -n "$hits" ]; then
        echo "check-selfloop: $f branches to itself:"; echo "$hits"; bad=1
    fi
done
exit $bad
