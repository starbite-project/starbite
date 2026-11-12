#!/bin/bash
# sbsnd's QEMU test: the guest kernel with CONFIG_SND (the dummy card off), sbguest.ko with sbsnd, sbsim with the v3
# services (sim_snd.c: a 48 kHz mixer that pulls the playback and pushes a 1 kHz mic tone). A tiny base image
# (busybox, snd-test, snd-sbinit as /sbin/sbinit) is the root.
#   limactl shell starbite flock /var/tmp/sbvm-heavy.lock /Users/dev/project-starbite/starbite/guest/v3test/snd-run.sh
# PASS when the guest logs "v3-test: snd: PASS" (it played 440 Hz and recorded sbsim's 1 kHz) and sbsim's own line
# says "440 Hz in N of M 100-ms blocks (PASS)".
# Environment: WORK (/mnt/build/sbguest3/snd), IMAGE (/mnt/build/sbguest3/Image-guest), KOUT (/mnt/build/sbguest3/gout)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export WORK=${WORK:-/mnt/build/sbguest3/snd}
export IMAGE=${IMAGE:-/mnt/build/sbguest3/Image-guest}
export KOUT=${KOUT:-/mnt/build/sbguest3/gout}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
img=$WORK/img
rm -rf "$img/base"
mkdir -p "$img/base"/{bin,sbin,dev,proc,sys,run,tmp,var,root} "$WORK/out"
cp "$BUSYBOX" "$img/base/bin/busybox"
for a in $("$BUSYBOX" --list); do
    [ "$a" = busybox ] || ln -s busybox "$img/base/bin/$a"
done
gcc -O2 -static -Wall -Wextra "$here/snd-test.c" -o "$img/base/bin/snd-test" -lm
cp "$here/snd-sbinit" "$img/base/sbin/sbinit"
chmod 755 "$img/base/sbin/sbinit" "$img/base/bin/snd-test" "$img/base/bin/busybox"
rm -f "$img/base.erofs"
mkfs.erofs -T 0 --all-root "$img/base.erofs" "$img/base" > "$img/mkfs.log" 2>&1
log=out/qemu-snd.log
set +e
BASE="$img/base.erofs" FLAGS=$((66 | 128)) EXTRA="snd_dummy.enable=0 ${EXTRA:-}" LOG=$log TAIL=0 TIMEOUT=${TIMEOUT:-900} \
    "$here/../run-desktop.sh" > "$WORK/out/snd-run.out" 2>&1
rc=$?
set -e
grep -a 'sbsnd\|snd: \|sbinit: \|v3-test' "$WORK/$log" | cut -c1-400
guest=$(grep -a 'v3-test: snd: ' "$WORK/$log" | tail -1)
host=$(grep -a 'sbsim: snd: playback' "$WORK/$log" | tail -1)
if [[ "$guest" == *"v3-test: snd: PASS"* ]] && [[ "$host" == *"blocks (PASS)"* ]]; then
    echo "== snd: PASS (run-desktop rc $rc)"
else
    echo "== snd: FAIL (run-desktop rc $rc)"
    exit 1
fi
