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
trap 'R=$M "$CH" umount; "$CH" drop "$NAME" >/dev/null 2>&1' EXIT
sudo env SBSTORE_MIRROR_SRC="$MIR" sh "$TOP/install-rootfs.sh" "$M" >/dev/null
sudo rm -f "$M/etc/apt/sources.list" "$M/etc/apt/sources.list.d/debian.sources" "$M/etc/apt/sources.list.d/security.list"
sudo rm -rf "$M/var/lib/apt/lists/"*
run() { R=$M "$CH" run env SBSTORE_OFFLINE=1 SBSTORE_REPO=/nonexistent "$@"; }
pass=0; fail=0
ck() { if [ "$1" = 0 ]; then echo "PASS $2"; pass=$((pass + 1)); else echo "FAIL $2"; fail=$((fail + 1)); fi; }
run apt-get -q update >/dev/null 2>&1
run sh -c 'apt-cache policy gcc | grep -q "Candidate: [0-9]"'; ck $? "apt sees build-essential in the mirror only"
for t in neovim:release ripgrep:release yt-dlp:release btop:source; do
    id=${t%:*}; m=${t#*:}; t0=$(date +%s)
    run /usr/local/sbin/sbstore install "$id" --method "$m" > /tmp/$NAME-$id.log 2>&1
    rc=$?; ck $rc "$id by $m offline in $(( $(date +%s) - t0 )) s: $(tail -1 /tmp/$NAME-$id.log)"
done
run sh -c 'nvim --version | head -1 && rg --version | head -1 && yt-dlp --version && btop --version'; ck $? "the four run"
run sh -c '/usr/local/sbin/sbstore status'
run /usr/local/sbin/sbstore install firefox --method release > /tmp/$NAME-ff.log 2>&1
[ $? = 3 ] && grep -q "needs the network" /tmp/$NAME-ff.log; ck $? "an app not in the mirror says it needs the network"
echo "RESULT pass=$pass fail=$fail"
