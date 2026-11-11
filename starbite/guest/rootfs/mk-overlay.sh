#!/bin/bash
# A blank overlay image for the guest's root (docs/STARBITE-LINUX-APPS.md L4, sbblk dev 1): ext4 (small journal,
# label sb-overlay) with the overlayfs dirs prepared: /upper (the root's changes: installed apps, /home, /etc, /var)
# and /work (overlayfs' work dir), both root-owned 0755. Sparse: it takes no space until written.
#   mk-overlay.sh OUT.img SIZE          e.g. mk-overlay.sh /mnt/build/sbguest2/rootfs/overlay.img 4G
# The initramfs mounts it with: mount -t overlay overlay -o lowerdir=/base,upperdir=/ov/upper,workdir=/ov/work /root
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 OUT.img SIZE (e.g. 4G)" >&2; exit 2; }
img=$1 size=$2
rm -f "$img"
truncate -s "$size" "$img"
mkfs.ext4 -q -F -L sb-overlay -U 5b5b5b5b-0000-4000-8000-000000000002 -J size=16 -m 0 \
    -E lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0 -O ^metadata_csum_seed "$img"
mnt=$(mktemp -d)
sudo mount -o loop "$img" "$mnt"
sudo mkdir -m 0755 "$mnt/upper" "$mnt/work"
echo "starbite overlay v1, made $(date -u +%F)" | sudo tee "$mnt/sb-overlay" >/dev/null
sudo umount "$mnt"
rmdir "$mnt"
echo "mk-overlay: $img ($size, ext4 sb-overlay: /upper /work)"
