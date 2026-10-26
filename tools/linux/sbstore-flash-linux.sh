#!/usr/bin/env bash
# Write the OS store from the phone's running Linux (bring-up init, USB
# network), same checks as sbstore-flash.sh: HFS+ root ends by 16 GiB, Kiosk
# ext4 intact at +17 GiB, target empty or an older store. USERDATA is made
# writable only for this one dd and read-only again; the image travels by nc.
#   tools/linux/sbstore-flash-linux.sh build/linux/sbstore.img
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-build/linux/sbstore.img}
P=tools/linux/phone.py
want=$(shasum -a 256 "$img" | cut -c1-64)
size=$(stat -f %z "$img")
blocks=$(( (size + 4095) / 4096 ))

part=$($P 'for d in /sys/class/block/sda*; do grep -q "^PARTNAME=USERDATA$" $d/uevent 2>/dev/null && echo ${d##*/}; done; true' | tr -d '\r\n')
[ -n "$part" ] || { echo "no USERDATA partition"; exit 1; }
dev=/dev/$part
echo "USERDATA = $dev"

# The phone fetches the image from a one-shot HTTP server on the Mac's USB
# address (busybox wget); nc over the ECM link stalled after ~140 KB.
python3 -m http.server 8642 --bind 172.16.42.2 --directory "$(dirname "$img")" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null' EXIT
sleep 1
$P "rm -f /tmp/sbstore.img; wget -q -O /tmp/sbstore.img http://172.16.42.2:8642/$(basename "$img"); ls -l /tmp/sbstore.img"
got=$($P "sha256sum /tmp/sbstore.img | cut -c1-64" | tr -d '\r\n')
[ "$got" = "$want" ] || { echo "transfer mismatch: $got"; exit 1; }

$P "
hfs=\$(dd if=$dev bs=512 skip=2 count=1 2>/dev/null | hexdump -v -e '1/1 \"%02x\"')
sig=\${hfs:0:4}; [ \"\$sig\" = 482b ] || [ \"\$sig\" = 4858 ] || { echo 'no HFS+ volume'; exit 1; }
bs=\$((0x\${hfs:80:8})); tot=\$((0x\${hfs:88:8}))
[ \$((bs * tot)) -le \$((16 << 30)) ] || { echo 'HFS+ volume past 16 GiB'; exit 1; }
cas=\$(dd if=$dev bs=4096 skip=4456448 count=1 2>/dev/null | dd bs=1 skip=1080 count=2 2>/dev/null | hexdump -v -e '1/1 \"%02x\"')
[ \"\$cas\" = 53ef ] || { echo 'Kiosk superblock missing'; exit 1; }
head=\$(dd if=$dev bs=4096 skip=4194304 count=1 2>/dev/null | dd bs=8 count=1 2>/dev/null | hexdump -v -e '1/1 \"%02x\"')
case \$head in 534253544f524531|0000000000000000) ;; *) echo \"unknown data at the store: \$head\"; exit 1 ;; esac
echo 'checks passed'"

# A partition cannot be writable under a read-only disk: unlock sda and the
# one partition, write, lock both again (no set -e: it would end the shell).
disk=${dev%%[0-9]*}
$P "blockdev --setrw $disk && blockdev --setrw $dev && dd if=/tmp/sbstore.img of=$dev bs=4096 seek=4194304 conv=notrunc,fsync 2>/dev/null; r=\$?; sync; blockdev --setro $dev; blockdev --setro $disk; echo dd \$r ro \$(blockdev --getro $disk)\$(blockdev --getro $dev)"
back=$($P "dd if=$dev bs=4096 skip=4194304 count=$blocks 2>/dev/null | head -c $size | sha256sum | cut -c1-64" | tr -d '\r\n')
[ "$back" = "$want" ] || { echo "readback mismatch"; exit 1; }
$P "dd if=$dev bs=4096 skip=4456448 count=1 2>/dev/null | dd bs=1 skip=1080 count=2 2>/dev/null | hexdump -v -e '1/1 \"%02x\"'; echo ' kiosk magic'"
echo "OS store written from Linux (${want:0:16}), $dev read-only again"
