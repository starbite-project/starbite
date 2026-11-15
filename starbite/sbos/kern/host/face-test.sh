#!/bin/sh
# Host test of Face Unlock (kern/facenet.c, kern/face.c): crypto, the unlock and enrolment rules, the liveness
# geometry; with the pack and a reference file also the runtime against onnxruntime / OpenCV and the LFW scores.
#   sh starbite/sbos/kern/host/face-test.sh [-v]
#   FACE_PACK=build/face/sbface.pak FACE_REF=/path/fref.bin sh starbite/sbos/kern/host/face-test.sh -v
# (FACE_REF: kern/host/face-ref.py's file; the pack: tools/face-pack.py. Without them the model checks are skipped.)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${TMPDIR:-/tmp}/sbos-face-test.$$
: "${FACE_PACK:=$root/build/face/sbface.pak}"
export FACE_PACK
${CC:-cc} -O2 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/.." \
    "$here/face-test.c" "$here/../face.c" "$here/../facenet.c" "$here/../passcode.c" -lm -o "$out"
set +e
"$out" "$@"
rc=$?
rm -f "$out"
rm -rf "$out.dSYM"
exit $rc
