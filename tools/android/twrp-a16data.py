#!/usr/bin/env python3
"""Make TWRP's /data the Android data partition A16DATA instead of USERDATA
(which holds the iOS root and other OSes at raw offsets), so a wipe or
"format data" that Android sends to recovery can never reach USERDATA.

    twrp-a16data.py TWRP.img OUTDIR
    -> OUTDIR/kernel, OUTDIR/ramdisk-a16.lzma; then
    tools/starlte-pack-boot.py pack TWRP.img OUTDIR/kernel OUT.img --ramdisk OUTDIR/ramdisk-a16.lzma
"""
import lzma
import struct
import sys
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
img = src.read_bytes()
ksz, _, rsz, _, _, _, _, page = struct.unpack_from('<8I', img, 8)
P = lambda n: (n + page - 1) // page * page
(out / 'kernel').write_bytes(img[page:page + ksz])
raw = lzma.decompress(img[page + P(ksz):page + P(ksz) + rsz], format=lzma.FORMAT_ALONE)
pad = lambda b: b + bytes((-len(b)) % 4)
o, res = 0, bytearray()
while o < len(raw):
    f = [int(raw[o + 6 + 8 * i:o + 14 + 8 * i], 16) for i in range(13)]
    nsz, fsz = f[11], f[6]
    name = raw[o + 110:o + 110 + nsz - 1].decode()
    d0 = (o + 110 + nsz + 3) & ~3
    data = raw[d0:d0 + fsz]
    if name in ('etc/recovery.fstab', 'fstab.samsungexynos9810'):
        t = data.decode()
        t = t.replace('/data           ext4   /dev/block/platform/11120000.ufs/by-name/USERDATA      flags=encryptable=footer;length=-20480',
                      '/data           f2fs   /dev/block/platform/11120000.ufs/by-name/A16DATA       flags=display="Data (Android, A16DATA)"')
        t = t.replace('by-name/USERDATA', 'by-name/A16DATA')
        data = t.encode()
        f[6] = len(data)
    res += pad(b'070701' + b''.join(b'%08X' % v for v in f) + raw[o + 110:o + 110 + nsz]) + pad(data)
    if name == 'TRAILER!!!':
        break
    o = (d0 + fsz + 3) & ~3
res += bytes((-len(res)) % 512)
assert b'by-name/USERDATA' not in res
(out / 'ramdisk-a16.lzma').write_bytes(lzma.compress(bytes(res), format=lzma.FORMAT_ALONE, preset=9))
print(f'{out}: kernel + ramdisk-a16.lzma (no USERDATA left)')
