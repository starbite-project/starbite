#!/bin/sh
# Host test of kern/abox_media.c (media through Calliope) with kern/abox.c against fake hardware and a fake Calliope
# (the Mac's cc). -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-calliope-test.$$
${CC:-cc} -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/calliope-test.c" "$here/../abox_media.c" "$here/../abox.c" "$here/../snd.c" "$here/../abox_pcm.c" \
    -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
