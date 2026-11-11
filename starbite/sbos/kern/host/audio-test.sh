#!/bin/sh
# Host test of kern/amp.c and kern/codec.c against a fake bus (the Mac's cc). Run from anywhere; -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-audio-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/audio-test.c" "$here/../amp.c" "$here/../codec.c" -o "$out"
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
