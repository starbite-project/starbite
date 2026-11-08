#!/usr/bin/env python3
"""Read one file from the phone's iOS root (case-sensitive journaled HFS+,
"HX", at the start of USERDATA) from TWRP over adb, read only. Walks the
catalog B-tree from its root node (HFSX keys compare binary: parent ID,
then the UTF-16 name) and copies the data fork's extents.
    hfs-read.py /System/Library/xpc/launchd.plist OUT
    hfs-read.py --ls /Library/Wallpaper          (names, types, sizes of one folder)
    HFS_CARD=1 hfs-read.py ...                   iOS 26's root on the microSD card instead
                                                 (/dev/block/mmcblk0, sector 182888448)
Limits: no extents-overflow file (at most 8 extents per fork), no
HFS-compressed (decmpfs) files; both are reported, not guessed. Reads go
through the adb shell's dd and are cached per block.
"""
import struct
import subprocess
import sys
from pathlib import Path

import os
if os.environ.get('HFS_CARD'):
    DEV, BASE = '/dev/block/mmcblk0', 182888448 * 512      # tools/store/cardlayout.py: the ios26 area
else:
    DEV, BASE = '/dev/block/by-name/USERDATA', 0
cache = {}


def dev_read(off, length):
    """Bytes [off, off + length) of USERDATA, in 4 KiB blocks."""
    first, last = off // 4096, (off + length + 4095) // 4096
    missing = [b for b in range(first, last) if b not in cache]
    while missing:
        start = missing[0]
        run = 1
        while run < len(missing) and missing[run] == start + run and run < 4096:
            run += 1
        data = subprocess.run(['adb', 'exec-out', f'dd if={DEV} bs=4096 skip={start + BASE // 4096} count={run} 2>/dev/null'],
                              capture_output=True, check=True).stdout
        if len(data) != run * 4096:
            sys.exit(f'short read at block {start}')
        for i in range(run):
            cache[start + i] = data[i * 4096:(i + 1) * 4096]
        missing = missing[run:]
    buf = b''.join(cache[b] for b in range(first, last))
    return buf[off - first * 4096:off - first * 4096 + length]


class Volume:
    def __init__(self):
        vh = dev_read(1024, 512)
        sig = vh[:2]
        if sig not in (b'H+', b'HX'):
            sys.exit(f'no HFS+ volume header ({sig!r})')
        self.block = struct.unpack_from('>I', vh, 40)[0]
        self.catalog = self.fork(vh, 272)
        hdr = self.fork_read(self.catalog, 0, 512)
        (self.depth, self.root, _, _, _, self.node_size, _, _, _) = struct.unpack_from('>HIIIIHHII', hdr, 14)
        self.compare = hdr[14 + 37]
        if sig == b'HX' and self.compare != 0xBC:
            sys.exit(f'unexpected key compare type {self.compare:#x}')

    @staticmethod
    def fork(buf, off):
        size, _, total = struct.unpack_from('>QII', buf, off)
        ext = [struct.unpack_from('>II', buf, off + 16 + 8 * i) for i in range(8)]
        ext = [e for e in ext if e[1]]
        if sum(e[1] for e in ext) < total:
            sys.exit('fork has more than 8 extents (extents overflow file not supported)')
        return size, ext

    def fork_read(self, fork, off, length):
        out, pos = bytearray(), 0
        for start, count in fork[1]:
            span = count * self.block
            if off < pos + span and len(out) < length:
                a = max(off, pos) - pos
                n = min(span - a, length - len(out))
                out += dev_read(start * self.block + a, n)
            pos += span
        return bytes(out)

    def node(self, n):
        return self.fork_read(self.catalog, n * self.node_size, self.node_size)

    @staticmethod
    def records(node, size):
        count = struct.unpack_from('>H', node, 10)[0]
        offs = [struct.unpack_from('>H', node, size - 2 * (i + 1))[0] for i in range(count + 1)]
        return [node[offs[i]:offs[i + 1]] for i in range(count)]

    @staticmethod
    def key(rec):
        klen, parent, nlen = struct.unpack_from('>HIH', rec, 0)
        name = rec[8:8 + 2 * nlen]
        return klen, (parent, name)

    def lookup(self, parent, name):
        want = (parent, name.encode('utf-16-be'))
        n = self.root
        while True:
            node = self.node(n)
            kind = struct.unpack_from('>b', node, 8)[0]
            recs = self.records(node, self.node_size)
            if kind == 0:                       # index node: last key <= want
                child = None
                for r in recs:
                    klen, k = self.key(r)
                    if k > want:
                        break
                    child = struct.unpack_from('>I', r, 2 + klen)[0]
                if child is None:
                    return None
                n = child
            elif kind == -1:                    # leaf
                for r in recs:
                    klen, k = self.key(r)
                    if k == want:
                        data = r[2 + klen + ((2 + klen) & 1):]
                        return data
                return None
            else:
                sys.exit(f'unexpected node kind {kind}')

    def folder_id(self, path):
        parent = 2
        for part in [p for p in path.split('/') if p]:
            rec = self.lookup(parent, part)
            if rec is None or struct.unpack_from('>h', rec, 0)[0] != 1:
                sys.exit(f'{path}: no such folder')
            parent = struct.unpack_from('>I', rec, 8)[0]
        return parent

    def listdir(self, path):
        """(name, type, size) of every entry of one folder: from the leaf holding (id, "") along fLinks."""
        fid = self.folder_id(path)
        want = (fid, b'')
        n = self.root
        while True:
            node = self.node(n)
            if struct.unpack_from('>b', node, 8)[0] != 0:
                break
            child = None
            for r in self.records(node, self.node_size):
                klen, k = self.key(r)
                if k > want and child is not None:
                    break
                child = struct.unpack_from('>I', r, 2 + klen)[0]
            n = child
        out = []
        while n:
            node = self.node(n)
            for r in self.records(node, self.node_size):
                klen, (parent, name) = self.key(r)
                if parent < fid:
                    continue
                if parent > fid:
                    return out
                data = r[2 + klen + ((2 + klen) & 1):]
                rtype = struct.unpack_from('>h', data, 0)[0]
                if rtype in (1, 2):
                    size = struct.unpack_from('>Q', data, 88)[0] if rtype == 2 else 0
                    out.append((name.decode('utf-16-be', 'replace'), 'dir' if rtype == 1 else 'file', size))
            n = struct.unpack_from('>I', node, 0)[0]
        return out

    def read_path(self, path):
        parent = 2                              # the root folder's ID
        parts = [p for p in path.split('/') if p]
        for i, part in enumerate(parts):
            rec = self.lookup(parent, part)
            if rec is None:
                sys.exit(f'{"/" + "/".join(parts[:i + 1])}: not found')
            rtype = struct.unpack_from('>h', rec, 0)[0]
            if i < len(parts) - 1:
                if rtype != 1:
                    sys.exit(f'{part}: not a folder')
                parent = struct.unpack_from('>I', rec, 8)[0]
            else:
                if rtype != 2:
                    sys.exit(f'{part}: not a file')
                owner_flags = rec[32 + 9]
                if owner_flags & 0x20:
                    sys.exit(f'{path}: HFS-compressed (decmpfs), not supported')
                fork = self.fork(rec, 88)
                return self.fork_read(fork, 0, fork[0])


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--ls':
        for name, kind, size in Volume().listdir(sys.argv[2]):
            print(f'{kind:4} {size:>12} {name}')
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = Volume().read_path(sys.argv[1])
    Path(sys.argv[2]).write_bytes(data)
    print(f'{sys.argv[1]}: {len(data)} bytes -> {sys.argv[2]}')


if __name__ == '__main__':
    main()
