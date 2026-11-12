#!/bin/bash
# sbp9's QEMU test (contract v3, sbshm-p9.h): the guest kernel + sbguest.ko's 9p transport "sbshm" against sbghost's
# 9P2000.L server (host/sbg_p9.c) in sbsim, over sbsim's read-write tree (sbsim/sim_p9.c). A tiny base image
# (busybox, v3test/p9-sbinit as /sbin/sbinit, rootfs/sbp9-mount, the tree's md5 list from p9-expect.py) on sbblk0,
# no overlay (tmpfs upper); sbsim flags F_BLKTEST | F_STRICT | F_V3. The verdict is the guest's line
# "v3-test: p9: PASS|FAIL ..." (and sbsim's RESULT).
#   limactl shell starbite flock /var/tmp/sbvm-heavy.lock /Users/dev/project-starbite/starbite/guest/v3test/p9-run.sh
# (a heavy job: the initramfs build and a QEMU run; the project's rule is one heavy VM job at a time, under that lock)
# WORK (/mnt/build/sbguest3/p9), IMAGE (/mnt/build/sbguest3/Image-guest), KOUT (/mnt/build/sbguest3/gout),
# DB_GUEST=0 DB_HOST=0: no doorbells (both sides poll). Log: $WORK/out/$P9_LOG (qemu-p9.log).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export WORK=${WORK:-/mnt/build/sbguest3/p9}
export IMAGE=${IMAGE:-/mnt/build/sbguest3/Image-guest}
export KOUT=${KOUT:-/mnt/build/sbguest3/gout}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
img=$WORK/img
root=$img/root
rm -rf "$root"
mkdir -p "$root"/{bin,sbin,usr/sbin,dev,proc,sys,run,tmp,root,etc,home/user} "$WORK/out"
cp "$BUSYBOX" "$root/bin/busybox"
for a in $("$BUSYBOX" --list); do
    [ "$a" = busybox ] || ln -s busybox "$root/bin/$a"
done
install -m 0755 "$here/p9-sbinit" "$root/sbin/sbinit"
install -m 0755 "$here/../rootfs/sbp9-mount" "$root/usr/sbin/sbp9-mount"
python3 "$here/p9-expect.py" > "$root/p9.md5"
rm -f "$img/base.erofs"
mkfs.erofs -T 0 --all-root "$img/base.erofs" "$root" > "$img/mkfs.log" 2>&1
set +e
FLAGS=$((64 | 2 | 128)) BASE="$img/base.erofs" OVERLAY= REPO= LOG=out/${P9_LOG:-qemu-p9.log} TAIL=0 TIMEOUT=${TIMEOUT:-2400} \
    "$here/../run-desktop.sh" > "$WORK/out/p9-run.out" 2>&1
rc=$?
set -e
log=$WORK/out/${P9_LOG:-qemu-p9.log}
grep -a -e 'sbp9' -e 'p9-test: ' -e 'v3-test: p9' -e '9pnet' "$log" | cut -c1-400 | tail -n 40
echo "== $(grep -a 'sbsim: RESULT' "$log" | cut -c1-200 || echo 'sbsim: no RESULT line')"
v=$(grep -a 'v3-test: p9: ' "$log" | tail -1 | sed 's/.*v3-test: p9: //')
echo "== p9: ${v:-no verdict line (see $log)}"
[ $rc = 0 ] && [ "${v#PASS}" != "$v" ]
