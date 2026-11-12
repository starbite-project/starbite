#!/bin/bash
# sbpkg's test without the phone or QEMU (in the Lima VM, aarch64 like the guest): the guest's root as the
# initramfs puts it together (base.erofs + a fresh ext4 overlay image + repo.erofs on /repo), sbpkg in a chroot of
# it against a plain file standing in for the shared region, and fake-sbwl.py in sbwl's place on /run/sbwl.sock.
# fake-sbwl installs firefox-esr, checks the progress lines, the table's flags and the .desktop file, queues and
# cancels, sends a stale op, refuses to remove a system app, removes firefox-esr and checks the space returns.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/rootfs/test-sbpkg.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/mnt/build/sbguest2/rootfs}
BASE=${BASE:-$OUT/base.erofs}     # the base image to build / test against
REPO=${REPO:-$OUT/repo.erofs}    # the repository image to test against
SBPKG=${SBPKG:-}                  # test a freshly built sbpkg instead of the base's copy
W=$OUT/test
M=$W/root
cleanup() {
    [ -n "${spid:-}" ] && sudo kill "$spid" 2>/dev/null
    sudo pkill -f "^/usr/local/sbin/sbpkg" 2>/dev/null || true
    for m in "$M/repo" "$M/run" "$M/dev" "$M/proc" "$M" "$W/ov" "$W/repo" "$W/base"; do
        mountpoint -q "$m" 2>/dev/null && sudo umount -l "$m"
    done
    return 0
}
trap cleanup EXIT
cleanup
mkdir -p "$W"/{base,ov,repo,root}
"$here/mk-overlay.sh" "$W/overlay.img" 3G
sudo mount -o loop,ro -t erofs "$BASE" "$W/base"
sudo mount -o loop,ro -t erofs "$REPO" "$W/repo"
sudo mount -o loop "$W/overlay.img" "$W/ov"
sudo mount -t overlay overlay -o "lowerdir=$W/base,upperdir=$W/ov/upper,workdir=$W/ov/work" "$M"
sudo mount -t proc proc "$M/proc"
sudo mount --bind /dev "$M/dev"
sudo mount -t tmpfs -o mode=0755 run "$M/run"
sudo mount --bind "$W/repo" "$M/repo"
[ -n "$SBPKG" ] && sudo install -m 0755 "$SBPKG" "$M/usr/local/sbin/sbpkg"
# the shared region: a plain 32 MiB file (header .. icon pool), zeroed like the host leaves it
sudo truncate -s 32M "$M/run/sbshm.img"

sudo TEST_EXTRA="${TEST_EXTRA:-}" python3 -I "$here/fake-sbwl.py" "$M/run/sbwl.sock" "$M/run/sbshm.img" "$M" &
fpid=$!
sleep 1
# MemTotal as on the phone's 1.5 GiB guest (TOO_BIG above MemTotal - 600 MB); the overlay is the root
# FAKE_1970=1: the guest's clock as it may be on the phone (no RTC, no network time): libfaketime from the dev
# root, preloaded into sbpkg and everything it runs (apt, dpkg)
fake=()
if [ "${FAKE_1970:-0}" = 1 ]; then
    D=${DEVROOT:-$OUT/devroot}
    [ -f "$D/usr/lib/aarch64-linux-gnu/faketime/libfaketime.so.1" ] ||
        sudo chroot "$D" sh -c 'apt-get install -y -q --no-install-recommends faketime >/dev/null'
    sudo cp "$D/usr/lib/aarch64-linux-gnu/faketime/libfaketime.so.1" "$M/run/libfaketime.so.1"
    fake=(LD_PRELOAD=/run/libfaketime.so.1 "FAKETIME=@1970-01-01 00:01:00" FAKETIME_DONT_RESET=1)
    echo "test-sbpkg: sbpkg runs with the clock at $(sudo chroot "$M" env "${fake[@]}" date -u)"
fi
sudo chroot "$M" env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root SBPKG_SHM=/run/sbshm.img SBPKG_MEM_MB=1490 \
    "${fake[@]}" /usr/local/sbin/sbpkg 2> "$W/sbpkg.log" &
spid=$!
rc=0
wait "$fpid" || rc=$?
echo "--- sbpkg's log ($W/sbpkg.log) ---"
cat "$W/sbpkg.log"
echo "test-sbpkg: $([ $rc = 0 ] && echo PASS || echo FAIL) (overlay image $W/overlay.img: $(du -m "$W/overlay.img" | cut -f1) MB used)"
exit $rc
