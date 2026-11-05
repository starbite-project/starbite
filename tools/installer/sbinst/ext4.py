"""Read one small file out of an ext4 image (read only, stdlib only), for the no-password check
of the Linux roots (etc/shadow, etc/starbite/login) on macOS and Linux without root, loop mounts or
e2fsprogs. Supports what mke2fs makes by default: extents (any depth), 64bit, metadata_csum,
hashed directories (read linearly). Refuses inline_data and block-mapped files."""
import struct

INCOMPAT_64BIT, INCOMPAT_EXTENTS, INCOMPAT_INLINE = 0x80, 0x40, 0x8000
EXT4_EXTENTS_FL = 0x80000


class Ext4Error(Exception):
    pass


class Ext4:
    def __init__(self, path, offset=0):
        self.f = open(path, 'rb')
        self.base = offset
        sb = self._read(1024, 1024)
        if len(sb) < 1024 or struct.unpack_from('<H', sb, 56)[0] != 0xEF53:
            self.f.close()
            raise Ext4Error('not an ext2/3/4 file system')
        self.bs = 1024 << struct.unpack_from('<I', sb, 24)[0]
        self.ipg = struct.unpack_from('<I', sb, 40)[0]
        self.first_data = struct.unpack_from('<I', sb, 20)[0]
        self.rev = struct.unpack_from('<I', sb, 76)[0]
        self.isz = struct.unpack_from('<H', sb, 88)[0] if self.rev else 128
        self.incompat = struct.unpack_from('<I', sb, 96)[0]
        self.dsz = struct.unpack_from('<H', sb, 254)[0] if self.incompat & INCOMPAT_64BIT else 32
        self.dsz = self.dsz or 32

    def close(self):
        self.f.close()

    def _read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def _block(self, b, count=1):
        return self._read(b * self.bs, self.bs * count)

    def inode(self, ino):
        g, idx = divmod(ino - 1, self.ipg)
        gd = self._read((self.first_data + 1) * self.bs + g * self.dsz, self.dsz)
        table = struct.unpack_from('<I', gd, 8)[0]
        if self.dsz >= 64:
            table |= struct.unpack_from('<I', gd, 0x28)[0] << 32
        return self._read(table * self.bs + idx * self.isz, self.isz)

    def _extents(self, node):
        magic, entries, _max, depth = struct.unpack_from('<HHHH', node, 0)
        if magic != 0xF30A:
            raise Ext4Error('bad extent header')
        for i in range(entries):
            e = node[12 + 12 * i: 24 + 12 * i]
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack('<IHHI', e)
                if ln > 32768:                      # uninitialized extent: reads as zeros
                    yield lblk, ln - 32768, None
                else:
                    yield lblk, ln, (hi << 32) | lo
            else:
                lblk, lo, hi, _ = struct.unpack('<IIHH', e)
                yield from self._extents(self._block((hi << 32) | lo))

    def data(self, ino, limit=1 << 20):
        raw = self.inode(ino)
        size = struct.unpack_from('<I', raw, 4)[0] | (struct.unpack_from('<I', raw, 108)[0] << 32)
        flags = struct.unpack_from('<I', raw, 32)[0]
        if flags & 0x10000000:
            raise Ext4Error('inline data is not supported')
        if not flags & EXT4_EXTENTS_FL:
            raise Ext4Error('block-mapped files are not supported')
        size = min(size, limit)
        out = bytearray(size)
        for lblk, ln, pblk in self._extents(raw[40:100]):
            start = lblk * self.bs
            if start >= size:
                continue
            chunk = bytes(ln * self.bs) if pblk is None else self._block(pblk, ln)
            out[start:start + len(chunk)] = chunk[:max(0, size - start)]
        return bytes(out[:size])

    def lookup(self, path):
        ino = 2
        for name in [p for p in path.split('/') if p]:
            d = self.data(ino, limit=64 << 20)
            found = None
            for blk in range(0, len(d), self.bs):
                b, off = d[blk:blk + self.bs], 0
                while off + 8 <= len(b):
                    i, rec, nl = struct.unpack_from('<IHB', b, off)
                    if rec < 8:
                        break
                    if i and b[off + 8:off + 8 + nl].decode(errors='replace') == name:
                        found = i
                        break
                    off += rec
                if found:
                    break
            if not found:
                return None
            ino = found
        return ino

    def read_file(self, path):
        ino = self.lookup(path)
        return None if ino is None else self.data(ino)


def read_file(image, path):
    fs = Ext4(image)
    try:
        return fs.read_file(path)
    finally:
        fs.close()
