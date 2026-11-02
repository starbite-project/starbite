#!/bin/sh
# mk-payload.sh DIR NAME: the files sb-integrate.sh installs into a distro root, gathered from the
# VM and the repo's git-ignored build/linux (docs/LINUX-REBUILD-PLAN.md 3.4). In the VM:
#   sh tools/linux/distros/mk-payload.sh /tmp/sb/payload ubuntu
# For a root built on the phone, copy DIR to the card next to sb-integrate.sh (card-install.sh).
#   busybox          static busybox for reboot-to's devmem ($BUSYBOX, fetch_busybox.py)
#   reboot-to        tools/linux/reboot-to
#   firmware/brcm/   the port's BCM4361 Wi-Fi + BT firmware, already under the names brcmfmac wants
#   authorized_keys  build/linux/ssh/starbite_ed25519.pub
#   login.txt        build/linux/NAME-login.txt: "user user" + "pin NNNNNN", made here once
set -eu
D=${1:?usage: mk-payload.sh DIR NAME}
NAME=${2:?usage: mk-payload.sh DIR NAME}
HERE=$(cd "$(dirname "$0")" && pwd)
L=$(cd "$HERE/.." && pwd)
P=${P:-/Users/dev/project-starbite}           # the main checkout: build/linux is git-ignored state
UH=${UH:-$HOME}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
FW=${FW:-$UH/linux/s9p/device/base/lib/firmware/brcm}
mkdir -p "$D/firmware/brcm"
install -m755 "$BUSYBOX" "$D/busybox"
install -m755 "$L/reboot-to" "$D/reboot-to"
for f in brcmfmac4361-pcie.bin brcmfmac4361-pcie.txt brcmfmac4361-pcie.clm_blob bcm4361B2_semco.hcd; do
	[ -s "$FW/$f" ] && install -m644 "$FW/$f" "$D/firmware/brcm/$f"
done
[ -s "$D/firmware/brcm/brcmfmac4361-pcie.bin" ] || { echo "mk-payload: no Wi-Fi firmware in $FW" >&2; exit 1; }
K=$P/build/linux/ssh/starbite_ed25519.pub
[ -s "$K" ] && install -m644 "$K" "$D/authorized_keys"
LOGIN=$P/build/linux/$NAME-login.txt
# an older file (one line, e.g. "user starbite PIN: NNNNNN"; older ones named the uid-1000 user
# starbite): the PIN is taken out first and both lines written back, so the PIN survives. One with
# no 6-digit PIN at all is kept as LOGIN.old and a new PIN is made below.
if [ -s "$LOGIN" ] && ! grep -q '^pin [0-9]\{6\}$' "$LOGIN"; then
	pin=$(grep -o '[0-9]\{6\}' "$LOGIN" | head -1 || true)
	if [ -n "$pin" ]; then printf 'user user\npin %s\n' "$pin" > "$LOGIN"
	else mv "$LOGIN" "$LOGIN.old"; echo "mk-payload: no PIN in $LOGIN (kept as $LOGIN.old), making a new one" >&2; fi
fi
if [ ! -s "$LOGIN" ]; then
	mkdir -p "$(dirname "$LOGIN")"
	printf 'user user\npin %06d\n' $(( $(od -An -N4 -tu4 /dev/urandom) % 1000000 )) > "$LOGIN"
fi
sed -i '/[0-9]\{6\}/!s/^user .*/user user/' "$LOGIN"   # never a line that holds the PIN
install -m600 "$LOGIN" "$D/login.txt"
echo "payload in $D (login: $LOGIN)"
