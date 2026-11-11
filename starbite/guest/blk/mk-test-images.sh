#!/bin/bash
# The stand-in images for sbblk's QEMU tests (run-tests.sh): a tiny base image (busybox, blk/test-sbinit as
# /sbin/sbinit, a known file tree with its md5 list), an empty ext4 overlay and a small repo image.
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/blk/mk-test-images.sh
# OUT (/mnt/build/sbguest2/blk/img): base.erofs (~260 MiB: the throughput test reads 200 MiB of it),
# overlay-empty.img (512 MiB, sparse), repo.erofs. The real images come from starbite/guest/rootfs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/mnt/build/sbguest2/blk/img}
BUSYBOX=${BUSYBOX:-/mnt/build/out/busybox-arm64}
rm -rf "$OUT/base" "$OUT/repo"
mkdir -p "$OUT/base"/{bin,sbin,dev,proc,sys,run,tmp,var,root,tree,big} "$OUT/repo"
cp "$BUSYBOX" "$OUT/base/bin/busybox"
for a in $("$BUSYBOX" --list); do
    [ "$a" = busybox ] || ln -s busybox "$OUT/base/bin/$a"
done
cp "$here/test-sbinit" "$OUT/base/sbin/sbinit"
chmod 755 "$OUT/base/sbin/sbinit" "$OUT/base/bin/busybox"
python3 - "$OUT/base" "$OUT/repo" <<'PY'
import os, random, sys
base, repo = sys.argv[1:]
r = random.Random(20261111)
# the known tree: 240 files of 0 .. 300 KiB in nested directories (odd sizes: tails, holes of zeros)
for i in range(240):
    d = os.path.join(base, 'tree', f'd{i % 12}', f's{i % 5}')
    os.makedirs(d, exist_ok=True)
    n = r.choice([0, 1, 511, 512, 513, 4095, 4096, 4097, 65535, 65536, 65537]) if i % 3 == 0 else r.randrange(300 * 1024)
    data = bytearray(r.randbytes(n))
    if i % 7 == 0 and n > 8192:
        data[4096:8192] = bytes(4096)
    open(os.path.join(d, f'f{i:03d}.bin'), 'wb').write(data)
# eight 28 MiB parts: the big reads (and the image passes 200 MiB)
for i in range(8):
    open(os.path.join(base, 'big', f'part{i}.bin'), 'wb').write(r.randbytes(28 << 20))
open(os.path.join(repo, 'REPO-OK'), 'wb').write(r.randbytes(3 << 20))
PY
(cd "$OUT/base" && find tree big -type f | sort | xargs md5sum > tree.md5)
(cd "$OUT/repo" && md5sum REPO-OK > repo.md5)
rm -f "$OUT/base.erofs" "$OUT/repo.erofs" "$OUT/overlay-empty.img"
mkfs.erofs -T 0 --all-root "$OUT/base.erofs" "$OUT/base" > "$OUT/mkfs.log" 2>&1
mkfs.erofs -T 0 --all-root "$OUT/repo.erofs" "$OUT/repo" >> "$OUT/mkfs.log" 2>&1
truncate -s 512M "$OUT/overlay-empty.img"
mkfs.ext4 -q -F -b 4096 -L sb-overlay -E lazy_itable_init=0,lazy_journal_init=0 "$OUT/overlay-empty.img"
md5sum < "$OUT/base/tree.md5" | cut -d' ' -f1 > "$OUT/tree.md5.md5"
wc -l < "$OUT/base/tree.md5" > "$OUT/tree.count"
rm -rf "$OUT/base" "$OUT/repo"
ls -la "$OUT"
echo "mk-test-images: $(cat "$OUT/tree.count") files, md5 of the md5 list $(cat "$OUT/tree.md5.md5")"
