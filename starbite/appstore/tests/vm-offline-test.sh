#!/bin/bash
# The phone's case: no network. A throwaway trixie root whose apt sees ONLY the offline mirror's debs, sbstore with
# SBSTORE_OFFLINE=1 and the mirror installed as install-rootfs.sh would put it. Release + source (btop) installs.
#   limactl shell starbite -- $PWD/starbite/appstore/tests/vm-offline-test.sh MIRROR_DIR
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$here/.." && pwd)
MIR=$(cd "${1:?mirror dir}" && pwd)
CH=$TOP/tools/vm-chroot.sh
NAME=offline-$$
M=$("$CH" fresh "$NAME" | tail -1)
trap 'sudo umount -l "$M/srv/appstore" 2>/dev/null; R=$M "$CH" umount; "$CH" drop "$NAME" >/dev/null 2>&1' EXIT
sudo sh "$TOP/install-rootfs.sh" "$M" >/dev/null
# the mirror as the phone has it: the card file appstore.erofs mounted read-only at /srv/appstore
sudo mkdir -p "$M/srv/appstore" && sudo mount --bind -o ro "$MIR" "$M/srv/appstore"
sudo rm -f "$M/etc/apt/sources.list" "$M/etc/apt/sources.list.d/debian.sources" "$M/etc/apt/sources.list.d/security.list"
sudo rm -rf "$M/var/lib/apt/lists/"*
run() { R=$M "$CH" run env SBSTORE_OFFLINE=1 SBSTORE_REPO=/nonexistent "$@"; }
pass=0; fail=0
ck() { if [ "$1" = 0 ]; then echo "PASS $2"; pass=$((pass + 1)); else echo "FAIL $2"; fail=$((fail + 1)); fi; }
run apt-get -q update >/dev/null 2>&1
for t in ${OFFLINE_TESTS:-neovim:release ripgrep:release yt-dlp:release btop:source}; do
    id=${t%:*}; m=${t#*:}; t0=$(date +%s)
    run /usr/local/sbin/sbstore install "$id" --method "$m" > /tmp/$NAME-$id.log 2>&1
    rc=$?; ck $rc "$id by $m offline in $(( $(date +%s) - t0 )) s: $(tail -1 /tmp/$NAME-$id.log)"
done
run sh -c 'apt-cache policy gcc | grep -q "Candidate: [0-9]"'; ck $? "apt sees build-essential (only from the mirror, which sbstore registered)"
for b in nvim rg yt-dlp btop; do run sh -c "command -v $b >/dev/null" && { run sh -c "$b --version 2>&1 | head -1"; ck $? "$b runs"; }; done
run sh -c '/usr/local/sbin/sbstore status; cat /etc/apt/sources.list.d/sbstore-mirror.list'
run /usr/local/sbin/sbstore install firefox --method release > /tmp/$NAME-ff.log 2>&1
[ $? = 3 ] && grep -q "needs the network" /tmp/$NAME-ff.log; ck $? "an app not in the mirror says it needs the network"
echo "RESULT pass=$pass fail=$fail"
