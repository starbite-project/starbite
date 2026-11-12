#!/bin/bash
# sbnet's QEMU test: the guest kernel + sbguest.ko's sbnet against sbghost's net service (host/sbg_net.c) and sbsim's
# NAT stand-in (sbsim/sim_net.c). A tiny base image (busybox, net-test, rootfs/sbnet-up, v3test/net-sbinit as
# /sbin/sbinit) is served as sbblk0; the guest runs the checks and logs "v3-test: net: PASS|FAIL ..." lines.
#   limactl shell starbite flock /var/tmp/sbvm-heavy.lock /Users/dev/project-starbite/starbite/guest/v3test/net-run.sh
# (a heavy VM job: the QEMU run; the shared lock keeps one heavy job at a time on the VM)
# Environment: WORK (/mnt/build/sbguest3/net), IMAGE (/mnt/build/sbguest3/Image-guest), KOUT (/mnt/build/sbguest3/gout),
# MB (the flood's MiB, 20), POLL=1 (no doorbells: both sides poll), EXTRA (more bootargs), TIMEOUT (1200 s).
# Exit 0 only when sbsim's RESULT is PASS, the summary line is a PASS and no check FAILed.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
g=$(cd "$here/.." && pwd)
export WORK=${WORK:-/mnt/build/sbguest3/net}
export IMAGE=${IMAGE:-/mnt/build/sbguest3/Image-guest}
export KOUT=${KOUT:-/mnt/build/sbguest3/gout}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
MB=${MB:-20}
img=$WORK/img
mkdir -p "$img" "$WORK/out"

echo "== net-test (static)"
gcc -O2 -static -Wall -Wextra "$here/net-test.c" -o "$img/net-test" || exit 1

echo "== base image"
rm -rf "$img/base"
mkdir -p "$img/base"/{bin,sbin,dev,proc,sys,run,tmp,var,root,etc}
cp "$BUSYBOX" "$img/base/bin/busybox"
for a in $("$BUSYBOX" --list); do
    [ "$a" = busybox ] || ln -s busybox "$img/base/bin/$a"
done
cp "$img/net-test" "$img/base/bin/net-test"
cp "$here/net-sbinit" "$img/base/sbin/sbinit"
cp "$g/rootfs/sbnet-up" "$img/base/sbin/sbnet-up"
chmod 755 "$img/base/sbin/sbinit" "$img/base/sbin/sbnet-up" "$img/base/bin/busybox" "$img/base/bin/net-test"
echo "127.0.0.1 localhost" > "$img/base/etc/hosts"
rm -f "$img/base.erofs"
mkfs.erofs -T 0 --all-root "$img/base.erofs" "$img/base" > "$img/mkfs.log" 2>&1 || { cat "$img/mkfs.log"; exit 1; }
rm -rf "$img/base"

name=net${POLL:+-poll}
dbs=()
[ "${POLL:-}" = 1 ] && dbs=(DB_GUEST=0 DB_HOST=0)
echo "== QEMU ($name): FLAGS=$((66 | 128)) (BLKTEST | STRICT | V3)"
env "${dbs[@]}" BASE="$img/base.erofs" FLAGS=$((66 | 128)) EXTRA="sbnet.mb=$MB ${EXTRA:-}" LOG="out/test-$name.log" \
    TIMEOUT="${TIMEOUT:-1200}" TAIL=0 "$g/run-desktop.sh" > "$WORK/out/test-$name.out" 2>&1
rc=$?
log=$WORK/out/test-$name.log
grep -a 'v3-test: net: \|sbsim: sim_net\|guest: sbnet\|sbnet' "$log" | grep -av 'linux| *sbinit\|dmesg:' | cut -c1-400 | tail -40
grep -a 'sbsim: RESULT' "$log" || tail -20 "$WORK/out/test-$name.out"
if [ $rc = 0 ] && grep -aq 'v3-test: net: PASS all' "$log" && ! grep -aq 'v3-test: net: FAIL' "$log"; then
    echo "== net-run ($name): PASS"
    exit 0
fi
echo "== net-run ($name): FAIL (sbsim rc $rc; logs $log, $WORK/out/test-$name.out)"
exit 1
