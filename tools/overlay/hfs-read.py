#!/usr/bin/env python3
"""Read one file from the phone's iOS root (case-sensitive journaled HFS+,
"HX", at the start of USERDATA) from TWRP over adb, read only. Walks the
catalog B-tree from its root node (HFSX keys compare binary: parent ID,
then the UTF-16 name) and copies the data fork's extents.
    hfs-read.py /System/Library/xpc/launchd.plist OUT
    hfs-read.py --ls /Library/Wallpaper          (names, types, sizes of one folder)
    HFS_CARD=1 hfs-read.py ...                   iOS 26's root on the microSD card instead
                                                 (/dev/block/mmcblk0, sector 182888448)
    HFS_IMAGE=FILE hfs-read.py ...               a local image file instead of the phone (tests)
Forks with more than 8 extents continue in the extents-overflow B-tree
(looked up by file ID and start block). HFS-compressed (decmpfs) files are
reported, not guessed. Reads go through the adb shell's dd and are cached
per block.
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
    if missing and os.environ.get('HFS_IMAGE'):
        with open(os.environ['HFS_IMAGE'], 'rb') as f:
            f.seek(first * 4096)
            data = f.read((last - first) * 4096)
        if len(data) != (last - first) * 4096:
            sys.exit(f'short read at block {first}')
        for i in range(last - first):
            cache[first + i] = data[i * 4096:(i + 1) * 4096]
        missing = []
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
        self.overflow = None
        self.overflow = self.fork(vh, 192, 3)                # the extents file never overflows itself
        ohdr = self.fork_read(self.overflow, 0, 512)
        self.ov_root = struct.unpack_from('>I', ohdr, 16)[0]
        self.ov_node_size = struct.unpack_from('>H', ohdr, 32)[0]
        self.catalog = self.fork(vh, 272, 4)
        self.attributes = self.fork(vh, 352, 8) if struct.unpack_from('>Q', vh, 352)[0] else None
        hdr = self.fork_read(self.catalog, 0, 512)
        (self.depth, self.root, _, _, _, self.node_size, _, _, _) = struct.unpack_from('>HIIIIHHII', hdr, 14)
        self.compare = hdr[14 + 37]
        if sig == b'HX' and self.compare != 0xBC:
            sys.exit(f'unexpected key compare type {self.compare:#x}')

    def fork(self, buf, off, file_id, fork_type=0):
        """(logical size, [(start block, block count)...]) of a data fork (HFSPlusForkData at buf[off:]); extents
        past the first 8 come from the extents-overflow B-tree, keyed (file ID, data fork, first file block)."""
        size, _, total = struct.unpack_from('>QII', buf, off)
        ext = [e for e in (struct.unpack_from('>II', buf, off + 16 + 8 * i) for i in range(8)) if e[1]]
        have = sum(e[1] for e in ext)
        while have < total:
            if self.overflow is None or len(ext) % 8:
                sys.exit(f'file {file_id}: fork has {have} of {total} blocks in its extents and no overflow record')
            rec = self.overflow_lookup(file_id, have, fork_type)
            if rec is None:
                sys.exit(f'file {file_id}: no extents-overflow record for block {have} (of {total})')
            more = [e for e in (struct.unpack_from('>II', rec, 8 * i) for i in range(8)) if e[1]]
            if not more:
                sys.exit(f'file {file_id}: empty extents-overflow record at block {have}')
            ext += more
            have += sum(e[1] for e in more)
        if have != total:
            sys.exit(f'file {file_id}: extents cover {have} blocks, fork says {total}')
        return size, ext

    def overflow_lookup(self, file_id, start, fork_type=0):
        """The 8-extent record keyed (file_id, fork_type (0 data, 0xff resource), start) in the extents-overflow
        B-tree, or None. Key: keyLength 10, forkType u8, pad u8, fileID u32, startBlock u32; ordered by fileID,
        forkType, start."""
        want = (file_id, fork_type, start)
        n, size = self.ov_root, self.ov_node_size
        for _ in range(16):
            if not n:
                return None
            node = self.fork_read(self.overflow, n * size, size)
            kind = struct.unpack_from('>b', node, 8)[0]
            best = None
            for r in self.records(node, size):
                klen, fork_type, fid, sblock = struct.unpack_from('>HBxII', r, 0)
                k = (fid, fork_type, sblock)
                if kind == -1 and k == want:
                    return r[2 + klen:2 + klen + 64]
                if kind == 0:
                    if k > want:
                        break
                    best = struct.unpack_from('>I', r, 2 + klen)[0]
            if kind != 0:
                return None
            n = best
        sys.exit('extents-overflow B-tree deeper than 16 levels')

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
        self.perms = {}
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
                    uid, gid, _, _, mode = struct.unpack_from('>IIBBH', data, 32)   # HFSPlusBSDInfo
                    self.perms[name.decode('utf-16-be', 'replace')] = (uid, gid, mode)
                    out.append((name.decode('utf-16-be', 'replace'), 'dir' if rtype == 1 else 'file', size))
            n = struct.unpack_from('>I', node, 0)[0]
        return out

    def getxattr(self, file_id, name):
        """An inline (record type 0x10) extended attribute's bytes, or None. Attributes B-tree key: keyLength u16,
        pad u16, fileID u32, startBlock u32, nameLength u16, name UTF-16BE; ordered by fileID, name, startBlock."""
        if not self.attributes:
            return None
        hdr = self.fork_read(self.attributes, 0, 512)
        n, size = struct.unpack_from('>I', hdr, 16)[0], struct.unpack_from('>H', hdr, 32)[0]
        want = (file_id, name.encode('utf-16-be'))
        for _ in range(16):
            if not n:
                return None
            node = self.fork_read(self.attributes, n * size, size)
            kind = struct.unpack_from('>b', node, 8)[0]
            best = None
            for r in self.records(node, size):
                klen, fid, _, nlen = struct.unpack_from('>HxxIIH', r, 0)
                k = (fid, r[14:14 + 2 * nlen])
                if kind == -1 and k == want:
                    d = r[2 + klen:]
                    rtype, = struct.unpack_from('>I', d, 0)
                    if rtype != 0x10:
                        sys.exit(f'file {file_id}: xattr {name} is not inline (record type {rtype:#x})')
                    asize, = struct.unpack_from('>I', d, 12)
                    return bytes(d[16:16 + asize])
                if kind == 0:
                    if k > want:
                        break
                    best = struct.unpack_from('>I', r, 2 + klen)[0]
            if kind != 0:
                return None
            n = best
        sys.exit('attributes B-tree deeper than 16 levels')

    def file_record(self, path):
        """The catalog file record of a path (hard links resolved to their inode's record)."""
        parent = 2
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
        if rtype != 2:
            sys.exit(f'{path}: not a file')
        if rec[48:56] == b'hlnkhfs+':
            inode = struct.unpack_from('>I', rec, 44)[0]
            priv = self.lookup(2, '\0\0\0\0HFS+ Private Data')
            if priv is None:
                sys.exit(f'{path}: hard link to inode {inode}, no private data folder')
            rec = self.lookup(struct.unpack_from('>I', priv, 8)[0], f'iNode{inode}')
            if rec is None:
                sys.exit(f'{path}: hard link to a missing iNode{inode}')
        return rec

    def compressed_parts(self, path):
        """(record, com.apple.decmpfs xattr, resource fork bytes) of an HFS-compressed file."""
        rec = self.file_record(path)
        fid = struct.unpack_from('>I', rec, 8)[0]
        if not rec[41] & 0x20:
            sys.exit(f'{path}: not compressed')
        x = self.getxattr(fid, 'com.apple.decmpfs')
        if x is None:
            sys.exit(f'{path}: compressed flag but no com.apple.decmpfs xattr')
        rf = self.fork(rec, 168, fid, 0xff)
        return rec, x, self.fork_read(rf, 0, rf[0]) if rf[0] else b''

    def read_path(self, path):
        rec = self.file_record(path)
        if rec[41] & 0x20:
            sys.exit(f'{path}: HFS-compressed (decmpfs): tools/ios26-decmpfs-fix.py reads those')
        fork = self.fork(rec, 88, struct.unpack_from('>I', rec, 8)[0])
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
