#!/bin/sh
# Host test of Face Unlock's service (ui/face_svc.c) with recorded frames: set-up, unlock, lockout, restart, passcode
# change and removal, Security's liveness. Needs the pack and kern/host/face-ref.py's file:
#   FACE_REF=/path/fref.bin sh starbite/sbos/kern/host/face-svc-test.sh [-v]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${TMPDIR:-/tmp}/sbos-face-svc-test.$$
: "${FACE_PACK:=$root/build/face/sbface.pak}"
export FACE_PACK
${CC:-cc} -O2 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." -I"$here/../../ui" \
    "$here/face-svc-test.c" "$here/../../ui/face_svc.c" "$here/../face.c" "$here/../facenet.c" "$here/../passcode.c" -lm -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
