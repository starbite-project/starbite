#!/bin/sh
# Host test of kern/codec_media.c (and kern/codec.c under it) against a fake CS47L93 and jack (the Mac's cc).
# Run from anywhere; -v traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/sbos-codec-media-test.$$
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/codec-media-test.c" "$here/../codec_media.c" "$here/../codec.c" -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
