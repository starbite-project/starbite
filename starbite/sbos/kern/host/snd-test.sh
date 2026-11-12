#!/bin/sh
# Host test of kern/snd.c and kern/abox_pcm.c (media sound) against fake hardware (the Mac's cc). -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-snd-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/snd-test.c" "$here/../snd.c" "$here/../abox_pcm.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
