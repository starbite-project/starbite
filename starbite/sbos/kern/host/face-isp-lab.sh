#!/bin/sh
# Face Unlock round 2's camera lab (kern/host/face-isp-lab.c): v1 and v2 pipelines against recognition.
#   FACE_REF=fref.bin sh starbite/sbos/kern/host/face-isp-lab.sh synth [N]
#   sh starbite/sbos/kern/host/face-isp-lab.sh raw OUTDIR build/face/facedbg/<time>/*-raw.bin
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${TMPDIR:-/tmp}/sbos-face-isp-lab.$$
: "${FACE_PACK:=$root/build/face/sbface.pak}"
export FACE_PACK
${CC:-cc} -O2 -std=gnu11 -Wall -Wextra -Werror -I"$here/.." "$here/face-isp-lab.c" "$here/../cam_isp.c" "$here/../face.c" \
    "$here/../facenet.c" "$here/../passcode.c" -lm -o "$out"
set +e
if [ "${1:-}" = synth ]; then "$out" synth "$FACE_REF" "${2:-30}"; else "$out" "$@"; fi
rc=$?
rm -f "$out"
exit $rc
