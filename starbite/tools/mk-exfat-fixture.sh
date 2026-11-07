#!/bin/sh
# Makes starbite/tools/fixtures/exfat-card.img.gz for the Utility OS host test (exfat.c): a 16 MiB "card"
# (exfat-fuse) with an MBR, an exFAT partition (type 0x07) at sector 2048 with 4 KiB clusters, and known files, among
# them a fragmented one. Needs the VM's root (loop mount). Run once; the result is committed:
#   limactl shell starbite -- sudo sh /Users/dev/project-starbite/starbite/tools/mk-exfat-fixture.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
img=/tmp/exfat-card.img
mnt=/tmp/exfat-card.mnt
rm -f "$img"
truncate -s 16M "$img"
printf 'label: dos\nstart=2048, type=7\n' | sfdisk -q "$img"
dev=$(losetup --show -f -o $((2048 * 512)) "$img")
mkfs.exfat -q -c 4096 -L STARBITE "$dev"
mkdir -p "$mnt"
mount.exfat-fuse "$dev" "$mnt"
printf 'hello starbite\n' > "$mnt/hello.txt"
mkdir -p "$mnt/distros" "$mnt/deep/a/b"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*7+3)&255 for i in range(300000)))" > "$mnt/distros/small.bin"
printf 'deep\n' > "$mnt/deep/a/b/c.txt"
printf 'long\n' > "$mnt/A very long file name with spaces.txt"
head -c 204800 /dev/zero > "$mnt/f1"
head -c 204800 /dev/zero > "$mnt/f2"
sync
rm "$mnt/f1"
sync
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*13+5)&255 for i in range(500000)))" > "$mnt/frag.bin"
sync
umount "$mnt"
losetup -d "$dev"
fsck.exfat -n "$img" >/dev/null 2>&1 || true
gzip -9 -n -c "$img" > "$here/fixtures/exfat-card.img.gz"
ls -l "$here/fixtures/exfat-card.img.gz"
