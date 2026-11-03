#!/usr/bin/env python3
"""Read single files out of an IPSW's APFS disk images over HTTP range requests,
without downloading the IPSW (or even the whole disk image).

  ipsw-apfs.py URL members
      The zip members with their sizes and storage (stored / deflated).
  ipsw-apfs.py URL ls MEMBER [PATH]
      List a directory of the APFS volume in MEMBER (default /).
  ipsw-apfs.py URL get MEMBER OUTDIR PATH ... [--list FILE] [--index FILE]
      Extract files, symlinks and directories (recursively) to OUTDIR/PATH.
      Mode, mtime and (as root) owner are kept; APFS compression (decmpfs zlib,
      LZVN, LZFSE) is undone, so OUTDIR gets plain files. --list adds paths from a
      file (one per line, '#' comments). A trailing '/' on a path means "the
      directory only, not its contents".
  ipsw-apfs.py URL du MEMBER PATH ... [--index FILE]
      Logical sizes of the given trees (no data is fetched).

How it reads, per member storage:
  stored (zip method 0): the member is a UDIF disk image (koly trailer, blkx
      chunk table, LZFSE/zlib/raw/zero chunks). Only the chunks under the APFS
      metadata and the wanted files are fetched, one ranged GET each.
  deflated (method 8): the member is a raw APFS image compressed by zip, so there
      is no random access. Pass 1 streams the member once and keeps only blocks that
      carry a valid APFS object checksum (superblocks, B-tree nodes, object maps):
      the volume's metadata, a few MB, saved to --index for reuse. Pass 2 streams it
      again and copies the wanted files' blocks as they go by. Nothing but the output
      files is written to disk.

Apple software stays local: write OUTDIR under build/ (gitignored), never into a repo.
Only https://updates.cdn-apple.com/ and https://appldnld.apple.com/ URLs are accepted.
"""
import argparse, bisect, ctypes, os, pickle, plistlib, stat, struct, subprocess, sys, zlib
from collections import OrderedDict

HOSTS = ('https://updates.cdn-apple.com/', 'https://appldnld.apple.com/')


# ---------------------------------------------------------------- HTTP / zip
def curl(*args):
    # curl, not urllib: urllib hangs on this host's broken IPv6 route to Apple's CDN.
    return subprocess.run(['curl', '-sfL', '--retry', '5', '--max-time', '600', *args],
                          check=True, capture_output=True).stdout


class Remote:
    def __init__(self, url):
        if not url.startswith(HOSTS):
            sys.exit(f'refusing {url}: not an Apple CDN URL')
        self.url = url
        self.fetched = 0

    def get(self, start, n):
        data = curl('-r', f'{start}-{start + n - 1}', self.url)
        if len(data) != n:   # a server ignoring Range would send the whole file
            raise IOError(f'bad range reply: {len(data)} of {n} bytes at {start}')
        self.fetched += n
        return data

    def stream(self, start, n, chunk=1 << 20):
        p = subprocess.Popen(['curl', '-sfL', '--max-time', '7200', '-r', f'{start}-{start + n - 1}',
                              self.url], stdout=subprocess.PIPE)
        left = n
        while left:
            b = p.stdout.read(min(chunk, left))
            if not b:
                break
            left -= len(b)
            self.fetched += len(b)
            yield b
        p.stdout.close()
        if p.wait() or left:
            raise IOError(f'stream ended early ({left} bytes left)')

    def size(self):
        for line in curl('-I', self.url).decode().splitlines():
            if line.lower().startswith('content-length:'):
                return int(line.split(':')[1])
        raise IOError('no Content-Length')

    def members(self):
        total = self.size()
        tail = self.get(max(0, total - 65558), min(total, 65558))
        e = tail.rfind(b'PK\x05\x06')
        cd_size, cd_off = struct.unpack_from('<II', tail, e + 12)
        z = tail.rfind(b'PK\x06\x06')
        if z >= 0:
            cd_size, cd_off = struct.unpack_from('<QQ', tail, z + 40)
        cd = self.get(cd_off, cd_size)
        out, i = {}, 0
        while cd[i:i + 4] == b'PK\x01\x02':
            method, = struct.unpack_from('<H', cd, i + 10)
            crc, csize, usize = struct.unpack_from('<III', cd, i + 16)
            nlen, xlen, clen = struct.unpack_from('<HHH', cd, i + 28)
            lho, = struct.unpack_from('<I', cd, i + 42)
            name = cd[i + 46:i + 46 + nlen].decode()
            ex, j = cd[i + 46 + nlen:i + 46 + nlen + xlen], 0
            while j + 4 <= len(ex):
                hid, hl = struct.unpack_from('<HH', ex, j)
                if hid == 1:
                    v = list(struct.unpack_from('<%dQ' % (hl // 8), ex, j + 4))
                    if usize == 0xffffffff: usize = v.pop(0)
                    if csize == 0xffffffff: csize = v.pop(0)
                    if lho == 0xffffffff: lho = v.pop(0)
                j += 4 + hl
            out[name] = dict(method=method, csize=csize, usize=usize, lho=lho, crc=crc)
            i += 46 + nlen + xlen + clen
        return out

    def data_start(self, m):
        h = self.get(m['lho'], 30)
        nlen, xlen = struct.unpack_from('<HH', h, 26)
        return m['lho'] + 30 + nlen + xlen


# ---------------------------------------------------------------- decompression
_lc = ctypes.CDLL('libcompression.dylib')
_lc.compression_decode_buffer.restype = ctypes.c_size_t
_lc.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                         ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
LZFSE, LZBITMAP, LZVN = 0x801, 0x702, 0x900   # 0x900: libcompression's raw LZVN


def lc_decode(src, size, algo=LZFSE):
    dst = ctypes.create_string_buffer(size + 64)
    n = _lc.compression_decode_buffer(dst, size + 64, src, len(src), None, algo)
    if n != size:
        raise IOError(f'libcompression 0x{algo:x}: {n} bytes, wanted {size}')
    return dst.raw[:size]


def lzvn(src, size):
    return lc_decode(src, size, LZVN)


def decmpfs_chunk(kind, data, size):
    """One compressed chunk of decmpfs type family `kind` (zlib / lzvn / lzfse / lzbitmap / raw).
    A chunk stored uncompressed is a marker byte (0xff zlib/lzfse, 0x06 lzvn, 0xcc raw) + the data."""
    if kind == 'raw':
        return data[1:1 + size] if len(data) == size + 1 else data[:size]
    if len(data) == size + 1 and data[0] in (0xff, 0x06, 0xcc, 0x0f):
        return data[1:]
    if kind == 'zlib':
        return data[1:1 + size] if (data[0] & 0x0f) == 0x0f else zlib.decompress(data)
    if kind == 'lzvn':
        return data[1:1 + size] if data[0] == 0x06 else lzvn(data, size)
    if kind == 'lzfse':
        return data[1:1 + size] if data[0] == 0xff else lc_decode(data, size)
    if kind == 'lzbitmap':
        return data[1:1 + size] if data[0] == 0xff else lc_decode(data, size, LZBITMAP)
    raise IOError(kind)


# decmpfs compression_type -> (family, data in the xattr (True) or the resource fork)
# (apfs-fuse's Decmpfs.h numbering: 9/10 = stored uncompressed, 5 = sparse.)
DECMPFS = {1: ('none', True), 3: ('zlib', True), 4: ('zlib', False), 7: ('lzvn', True),
           8: ('lzvn', False), 9: ('raw', True), 10: ('raw', False), 11: ('lzfse', True),
           12: ('lzfse', False), 13: ('lzbitmap', True), 14: ('lzbitmap', False)}


# ---------------------------------------------------------------- UDIF
class Udif:
    """Random access to the Apple_APFS partition of a stored UDIF member."""
    def __init__(self, rem, start, size, cache=96):
        self.rem, self.start = rem, start
        koly = rem.get(start + size - 512, 512)
        assert koly[:4] == b'koly', 'not a UDIF image'
        dfo, = struct.unpack_from('>Q', koly, 0x18)
        xo, xl = struct.unpack_from('>QQ', koly, 0xd8)
        blkx = plistlib.loads(rem.get(start + xo, xl))['resource-fork']['blkx']
        part = [b for b in blkx if 'Apple_APFS' in b.get('Name', '')]
        assert part, 'no Apple_APFS partition: ' + ', '.join(b.get('Name', '?') for b in blkx)
        d = part[0]['Data']
        assert d[:4] == b'mish'
        self.base_sector, = struct.unpack_from('>Q', d, 8)
        doff, = struct.unpack_from('>Q', d, 24)
        n, = struct.unpack_from('>I', d, 0xc8)
        self.chunks = []    # (byte offset in partition, length, type, file offset, comp len)
        for i in range(n):
            t, _, ss, sc, co, cl = struct.unpack_from('>IIQQQQ', d, 0xcc + 40 * i)
            if t in (0x7ffffffe, 0xffffffff):
                continue
            self.chunks.append((ss * 512, sc * 512, t, start + dfo + doff + co, cl))
        self.offs = [c[0] for c in self.chunks]
        self.cache, self.cmax = OrderedDict(), cache

    def _chunk(self, i):
        if i in self.cache:
            self.cache.move_to_end(i)
            return self.cache[i]
        off, ln, t, fo, cl = self.chunks[i]
        if t in (0, 2):
            data = bytes(ln)
        else:
            raw = self.rem.get(fo, cl)
            if t == 1: data = raw
            elif t == 0x80000007: data = lc_decode(raw, ln)
            elif t == 0x80000005: data = zlib.decompress(raw)
            else: raise IOError(f'UDIF chunk type 0x{t:x} not handled')
        self.cache[i] = data
        if len(self.cache) > self.cmax:
            self.cache.popitem(last=False)
        return data

    def read(self, off, n):
        out = bytearray()
        while n:
            i = bisect.bisect_right(self.offs, off) - 1
            c = self._chunk(i)
            o = off - self.chunks[i][0]
            b = c[o:o + n]
            if not b:
                raise IOError(f'read past the image at {off}')
            out += b; off += len(b); n -= len(b)
        return bytes(out)


# ---------------------------------------------------------------- APFS
def fletcher64_ok(blk):
    w = struct.unpack_from('<%dI' % ((len(blk) - 8) // 4), blk, 8)
    s1 = s2 = 0
    for x in w:
        s1 = (s1 + x) % 0xffffffff
        s2 = (s2 + s1) % 0xffffffff
    c1 = 0xffffffff - ((s1 + s2) % 0xffffffff)
    c2 = 0xffffffff - ((s1 + c1) % 0xffffffff)
    return struct.unpack_from('<Q', blk)[0] == (c2 << 32) | c1


OBJ_PHYSICAL, OBJ_EPHEMERAL = 0x40000000, 0x80000000
T_INODE, T_XATTR, T_EXTENT, T_DREC = 3, 4, 8, 9
S_IFMT = 0o170000


class Apfs:
    """Read-only APFS: container superblock, object maps, the first volume's fs
    tree (and the fext tree of sealed volumes), files with decmpfs undone."""
    def __init__(self, read_blocks, bs=4096):
        self.rb, self.bs = read_blocks, bs
        self.ncache = OrderedDict()
        nx = self.block(0)
        assert nx[32:36] == b'NXSB', 'no APFS container superblock'
        self.bs, = struct.unpack_from('<I', nx, 36)
        # newest valid superblock in the checkpoint descriptor area
        dblocks, = struct.unpack_from('<I', nx, 0x68)
        dbase, = struct.unpack_from('<Q', nx, 0x70)
        best = nx
        if not dblocks & 0x80000000:
            for i in range(dblocks):
                try:
                    b = self.block(dbase + i)
                except KeyError:
                    continue
                if b[32:36] == b'NXSB' and fletcher64_ok(b) and b[16:24] > best[16:24]:
                    best = b
        nx = best
        self.xid, = struct.unpack_from('<Q', nx, 16)
        omap_oid, = struct.unpack_from('<Q', nx, 0xa0)
        self.nx_omap = self.omap_root(omap_oid)
        fs_oid, = struct.unpack_from('<Q', nx, 0xb8)
        vs = self.block(self.omap_lookup(self.nx_omap, fs_oid))
        assert vs[32:36] == b'APSB', 'no APFS volume superblock'
        self.vol_name = vs[0x2c0:0x3c0].split(b'\0')[0].decode()
        self.incompat, = struct.unpack_from('<Q', vs, 0x38)
        self.sealed = bool(self.incompat & 0x20)
        vomap, root_oid = struct.unpack_from('<QQ', vs, 0x80)
        self.root_type, = struct.unpack_from('<I', vs, 0x74)
        self.vomap = self.omap_root(vomap)
        self.root = root_oid if self.root_type & OBJ_PHYSICAL else self.omap_lookup(self.vomap, root_oid)
        # A sealed volume's hashed fs tree stores its index nodes' child oids relative to
        # the tree's root oid (measured on the 13.5 SystemOS cryptex: child + 1838 = the
        # level-2 nodes' oids); a plain tree stores them as they are.
        bt_flags, = struct.unpack_from('<I', self.block(self.root), self.bs - 40)
        self.child_base = root_oid if bt_flags & 0x80 and not self.root_type & OBJ_PHYSICAL else 0
        self.fext = None
        if self.sealed:
            self.fext, = struct.unpack_from('<Q', vs, 0x408)

    def block(self, paddr):
        return self.rb(paddr)

    def node(self, paddr):
        if paddr in self.ncache:
            return self.ncache[paddr]
        b = self.block(paddr)
        flags, level, nkeys = struct.unpack_from('<HHI', b, 32)
        toff, tlen = struct.unpack_from('<HH', b, 40)
        kstart = 56 + toff + tlen
        vend = self.bs - (40 if flags & 1 else 0)
        ents = []
        for i in range(nkeys):
            if flags & 4:      # fixed-size keys and values
                ko, vo = struct.unpack_from('<HH', b, 56 + toff + 4 * i)
                kl, vl = 16, (16 if level == 0 else 8)
            else:
                ko, kl, vo, vl = struct.unpack_from('<HHHH', b, 56 + toff + 8 * i)
            k = b[kstart + ko:kstart + ko + kl]
            v = b[vend - vo:vend - vo + vl] if vo != 0xffff else b''
            ents.append((k, v))
        n = (level, ents)
        self.ncache[paddr] = n
        if len(self.ncache) > 4096:
            self.ncache.popitem(last=False)
        return n

    # object maps
    def omap_root(self, oid):
        b = self.block(oid)
        tree, = struct.unpack_from('<Q', b, 48)
        return tree

    def omap_lookup(self, tree, oid):
        paddr = tree
        while True:
            level, ents = self.node(paddr)
            best = None
            for k, v in ents:
                ko, kx = struct.unpack_from('<QQ', k)
                if (ko, kx) <= (oid, self.xid):
                    best = (ko, v)
                else:
                    break
            if best is None:
                raise KeyError(f'oid {oid:#x} not in the object map')
            if level == 0:
                if best[0] != oid:
                    raise KeyError(f'oid {oid:#x} not in the object map')
                return struct.unpack_from('<Q', best[1], 8)[0]
            paddr, = struct.unpack_from('<Q', best[1])

    # fs tree: all records with key prefix (obj_id, type)
    def _child(self, v):
        oid, = struct.unpack_from('<Q', v)
        return oid if self.root_type & OBJ_PHYSICAL else self.omap_lookup(self.vomap, oid + self.child_base)

    def records(self, oid, typ):
        def pre(k):
            x, = struct.unpack_from('<Q', k)
            return ((x & 0x0fffffffffffffff), x >> 60)
        target = (oid, typ)
        def walk(paddr):
            level, ents = self.node(paddr)
            if level == 0:
                for k, v in ents:
                    if pre(k) == target:
                        yield k, v
                return
            for i, (k, v) in enumerate(ents):
                if pre(k) > target:
                    break
                if i + 1 < len(ents) and pre(ents[i + 1][0]) < target:
                    continue
                yield from walk(self._child(v))
        yield from walk(self.root)

    def fext_extents(self, pid):
        def walk(paddr):
            level, ents = self.node(paddr)
            if level == 0:
                for k, v in ents:
                    kp, la = struct.unpack_from('<QQ', k)
                    if kp == pid:
                        ln, pb = struct.unpack_from('<QQ', v)
                        yield la, ln & 0x00ffffffffffffff, pb
                return
            for i, (k, v) in enumerate(ents):
                kp, = struct.unpack_from('<Q', k)
                if kp > pid:
                    break
                if i + 1 < len(ents) and struct.unpack_from('<Q', ents[i + 1][0])[0] < pid:
                    continue
                yield from walk(struct.unpack_from('<Q', v)[0])
        return sorted(walk(self.fext))

    def extents(self, pid):
        if self.fext:
            return self.fext_extents(pid)
        out = []
        for k, v in self.records(pid, T_EXTENT):
            la, = struct.unpack_from('<Q', k, 8)
            ln, pb = struct.unpack_from('<QQ', v)
            out.append((la, ln & 0x00ffffffffffffff, pb))
        return sorted(out)

    # inodes and directories
    def inode(self, oid):
        for k, v in self.records(oid, T_INODE):
            parent, pid = struct.unpack_from('<QQ', v)
            mtime, = struct.unpack_from('<Q', v, 24)
            flags, = struct.unpack_from('<Q', v, 48)
            uid, gid, mode = struct.unpack_from('<IIH', v, 72)
            size = 0
            nx, used = struct.unpack_from('<HH', v, 92)
            hdr, data = 96, 96 + 4 * nx
            for i in range(nx):
                t, f, sz = struct.unpack_from('<BBH', v, hdr + 4 * i)
                if t == 8:   # INO_EXT_TYPE_DSTREAM
                    size, = struct.unpack_from('<Q', v, data)
                data += (sz + 7) & ~7
            return dict(id=oid, pid=pid, mtime=mtime, flags=flags, uid=uid, gid=gid,
                        mode=mode, size=size)
        raise KeyError(f'no inode {oid}')

    def children(self, oid):
        out = {}
        for k, v in self.records(oid, T_DREC):
            if len(k) >= 12:   # hashed: u32 len_and_hash, then the name
                nl, = struct.unpack_from('<I', k, 8)
                name = k[12:12 + (nl & 0x3ff)].rstrip(b'\0').decode()
            else:
                nl, = struct.unpack_from('<H', k, 8)
                name = k[10:10 + nl].rstrip(b'\0').decode()
            fid, = struct.unpack_from('<Q', v)
            out[name] = fid
        return out

    def lookup(self, path):
        oid = 2   # ROOT_DIR_INO_NUM
        for part in [p for p in path.split('/') if p]:
            kids = self.children(oid)
            if part not in kids:
                raise FileNotFoundError(path)
            oid = kids[part]
        return oid

    def xattrs(self, oid):
        out = {}
        for k, v in self.records(oid, T_XATTR):
            nl, = struct.unpack_from('<H', k, 8)
            name = k[10:10 + nl].rstrip(b'\0').decode()
            flags, ln = struct.unpack_from('<HH', v)
            if flags & 2:   # embedded
                out[name] = ('data', v[4:4 + ln])
            else:
                xid, size = struct.unpack_from('<QQ', v, 4)
                out[name] = ('stream', (xid, size))
        return out


# ---------------------------------------------------------------- file plans
class Plan:
    """What to write for each wanted path, and which physical blocks it needs."""
    def __init__(self, fs, paths, recurse=True):
        self.fs, self.items, self.seen = fs, [], set()
        for p in paths:
            only_dir = p.endswith('/') and p != '/'
            self.add('/' + p.strip('/'), recurse and not only_dir)

    def add(self, path, recurse):
        fs = self.fs
        oid = fs.lookup(path)
        # parents first, so directories exist with their own modes
        parts = [p for p in path.split('/') if p]
        for i in range(1, len(parts)):
            parent = '/' + '/'.join(parts[:i])
            if parent not in self.seen:
                self.seen.add(parent)
                self.items.append((parent, fs.inode(fs.lookup(parent)), None))
        self._add(path, oid, recurse)

    def _add(self, path, oid, recurse):
        if path in self.seen:
            return
        self.seen.add(path)
        ino = self.fs.inode(oid)
        fmt = ino['mode'] & S_IFMT
        if fmt == stat.S_IFDIR:
            self.items.append((path, ino, None))
            if recurse:
                for name, cid in sorted(self.fs.children(oid).items()):
                    self._add(path.rstrip('/') + '/' + name, cid, True)
            return
        x = self.fs.xattrs(oid)
        if fmt == stat.S_IFLNK:
            self.items.append((path, ino, ('link', x['com.apple.fs.symlink'][1].rstrip(b'\0'))))
        elif fmt == stat.S_IFREG:
            src = ('plain', self.fs.extents(ino['pid']), ino['size'])
            if 'com.apple.decmpfs' in x and x['com.apple.decmpfs'][0] == 'data':
                h = x['com.apple.decmpfs'][1]
                magic, ctype, usize = struct.unpack_from('<IIQ', h)
                assert magic == 0x636d7066, f'{path}: bad decmpfs header'
                kind, inline = DECMPFS.get(ctype, (None, None))
                if kind is None:
                    raise IOError(f'{path}: decmpfs type {ctype} not handled')
                if inline:
                    src = ('inline', kind, h[16:], usize)
                else:
                    rf = x.get('com.apple.ResourceFork')
                    if rf is None:
                        if usize:
                            raise IOError(f'{path}: compressed, but no resource fork')
                        src = ('inline', 'none', b'', 0)
                    elif rf[0] == 'data':     # a small fork embedded in the xattr record
                        src = ('rsrcdata', kind, rf[1], usize)
                    else:
                        xoid, rsize = rf[1]
                        src = ('rsrc', kind, self.fs.extents(xoid), rsize, usize)
            self.items.append((path, ino, src))

    def blocks(self):
        """physical block -> [(item index, byte offset in its stream)]"""
        need = {}
        for i, (_, _, src) in enumerate(self.items):
            if not src or src[0] not in ('plain', 'rsrc'):
                continue
            exts = src[1] if src[0] == 'plain' else src[2]
            for la, ln, pb in exts:
                if pb == 0:
                    continue   # sparse hole
                for j in range((ln + self.fs.bs - 1) // self.fs.bs):
                    need.setdefault(pb + j, []).append((i, la + j * self.fs.bs))
        return need

    def logical(self):
        return sum(it[1]['size'] if it[2][0] == 'plain' else it[2][-1]
                   for it in self.items if (it[1]['mode'] & S_IFMT) == stat.S_IFREG)


def decode_rsrc(kind, rs, usize, path):
    """A decmpfs resource fork (types 4/8/12/14) -> the file's bytes."""
    if kind == 'none':
        return rs[:usize]
    if kind == 'zlib':   # classic resource fork: header, then a block table
        doff, = struct.unpack_from('>I', rs, 0)
        n, = struct.unpack_from('<I', rs, doff + 4)
        out = bytearray()
        for i in range(n):
            o, ln = struct.unpack_from('<II', rs, doff + 8 + 8 * i)
            want = min(65536, usize - len(out))
            out += decmpfs_chunk(kind, rs[doff + 4 + o:doff + 4 + o + ln], want)
        return bytes(out)
    n = (usize + 65535) // 65536
    offs = struct.unpack_from('<%dI' % (n + 1), rs, 0)
    out = bytearray()
    for i in range(n):
        want = min(65536, usize - len(out))
        out += decmpfs_chunk(kind, rs[offs[i]:offs[i + 1]], want)
    if len(out) != usize:
        raise IOError(f'{path}: decmpfs gave {len(out)} of {usize} bytes')
    return bytes(out)


def write_items(plan, outdir, streams):
    """streams[i]: the raw bytes of item i's data stream (plain file or resource fork)."""
    root = os.geteuid() == 0
    dirs = []
    for i, (path, ino, src) in enumerate(plan.items):
        dst = os.path.join(outdir, path.lstrip('/'))
        fmt = ino['mode'] & S_IFMT
        if fmt == stat.S_IFDIR:
            os.makedirs(dst, exist_ok=True)
            dirs.append((dst, ino))
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.lexists(dst):
            os.unlink(dst)
        if src[0] == 'link':
            os.symlink(src[1], dst)
            if root:
                os.lchown(dst, ino['uid'], ino['gid'])
            continue
        if src[0] == 'plain':
            data = streams.get(i, b'')[:ino['size']]
            data += bytes(ino['size'] - len(data))
        elif src[0] == 'inline':
            kind, blob, usize = src[1:]
            data = blob[:usize] if kind == 'none' or not usize else decmpfs_chunk(kind, blob, usize)
        elif src[0] == 'rsrcdata':
            data = decode_rsrc(src[1], src[2], src[3], path)
        else:
            data = decode_rsrc(src[1], streams[i][:src[3]], src[4], path)
        with open(dst, 'wb') as f:
            f.write(data)
        if root:
            os.chown(dst, ino['uid'], ino['gid'])
        os.chmod(dst, ino['mode'] & 0o7777)
        t = ino['mtime'] / 1e9
        os.utime(dst, (t, t))
    for dst, ino in reversed(dirs):
        if root:
            os.chown(dst, ino['uid'], ino['gid'])
        os.chmod(dst, ino['mode'] & 0o7777)
        t = ino['mtime'] / 1e9
        os.utime(dst, (t, t))


# ---------------------------------------------------------------- members
def open_stored(rem, m):
    u = Udif(rem, rem.data_start(m), m['usize'])
    bs = [4096]
    fs = Apfs(lambda pb: u.read(pb * bs[0], bs[0]))
    bs[0] = fs.bs
    return fs, u


class BlockDict(dict):
    def __call__(self, pb):
        return self[pb]


def stream_blocks(rem, m, bs=4096):
    """Yield (block number, block) of a deflated raw-APFS member, inflating as it downloads."""
    start = rem.data_start(m)
    inf = zlib.decompressobj(-15)
    buf, pb = b'', 0
    got = 0
    for chunk in rem.stream(start, m['csize'], 4 << 20):
        buf += inf.decompress(chunk)
        got += len(chunk)
        n = len(buf) // bs
        for i in range(n):
            yield pb + i, buf[i * bs:(i + 1) * bs]
        pb += n
        buf = buf[n * bs:]
        print(f'\r  {got * 100 // m["csize"]:3d}% of {m["csize"] >> 20} MiB', end='', file=sys.stderr, flush=True)
    buf += inf.flush()
    for i in range(len(buf) // bs):
        yield pb + i, buf[i * bs:(i + 1) * bs]
    print(file=sys.stderr)


ZERO32 = bytes(32)


def headerless_node(b):
    if b[:32] != ZERO32:
        return False
    flags, level, nkeys = struct.unpack_from('<HHI', b, 32)
    toff, tlen = struct.unpack_from('<HH', b, 40)
    return (flags & 0x10 and not flags & ~0x1f and level < 16 and 0 < nkeys < 4096
            and 56 + toff + tlen < len(b))


META_TYPES = {0x1, 0x2, 0x3, 0x5, 0x6, 0x7, 0xb, 0xc, 0xd, 0x11, 0x12, 0x13, 0x1e}


def index_deflated(rem, m, path):
    if path and os.path.exists(path):
        with open(path, 'rb') as f:
            return pickle.load(f)
    print(f'pass 1 (metadata) over the deflated member', file=sys.stderr)
    blocks = BlockDict()
    for pb, b in stream_blocks(rem, m):
        t = struct.unpack_from('<I', b, 24)[0] & 0xffff
        if (pb == 0 or t in META_TYPES) and any(b[:8]) and fletcher64_ok(b):
            blocks[pb] = b
        elif headerless_node(b):
            # Sealed volumes keep their fs-tree nodes without an object header
            # (omap flag OMAP_VAL_NOHEADER): no checksum to test, so match the shape.
            blocks[pb] = b
    if path:
        with open(path + '.part', 'wb') as f:
            pickle.dump(blocks, f)
        os.replace(path + '.part', path)
    return blocks


def open_member(rem, name, index=None):
    ms = rem.members()
    if name not in ms:
        sys.exit(f'no member {name}')
    m = ms[name]
    if m['method'] == 0:
        fs, u = open_stored(rem, m)
        return fs, m, u
    blocks = index_deflated(rem, m, index)
    return Apfs(blocks), m, None


def fetch_stored(u, fs, plan):
    streams = {}
    for i, (path, ino, src) in enumerate(plan.items):
        if not src or src[0] not in ('plain', 'rsrc'):
            continue
        exts = src[1] if src[0] == 'plain' else src[2]
        size = src[2] if src[0] == 'plain' else src[3]
        data = bytearray(size)
        for la, ln, pb in exts:
            if pb and la < size:
                n = min(ln, size - la)
                data[la:la + n] = u.read(pb * fs.bs, n)
        streams[i] = bytes(data)
    return streams


def fetch_deflated(rem, m, fs, plan):
    need = plan.blocks()
    streams = {}
    for i, (path, ino, src) in enumerate(plan.items):
        if src and src[0] in ('plain', 'rsrc'):
            streams[i] = bytearray(src[2] if src[0] == 'plain' else src[3])
    if not need:
        return streams
    last = max(need)
    print(f'pass 2 (data): {len(need)} blocks up to block {last}', file=sys.stderr)
    for pb, b in stream_blocks(rem, m, fs.bs):
        for i, off in need.get(pb, ()):
            s = streams[i]
            n = min(fs.bs, len(s) - off)
            if n > 0:
                s[off:off + n] = b[:n]
        if pb >= last:
            break
    return streams


# ---------------------------------------------------------------- CLI
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('url')
    ap.add_argument('cmd', choices=['members', 'ls', 'get', 'du'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--list', help='file with one path per line')
    ap.add_argument('--index', help='pass-1 metadata index for a deflated member (cached)')
    ap.add_argument('--owners', help='get: also write a JSON {path: [uid, gid, mode]} of what was '
                    'extracted (a non-root run cannot chown; tools/macos/hfs-owners.py applies it)')
    ap.add_argument('--prefix', default='', help='get: path prefix for --owners keys (e.g. the '
                    'cryptex mount point /System/Volumes/Preboot/Cryptexes/OS)')
    a = ap.parse_args()
    rem = Remote(a.url)
    if a.cmd == 'members':
        for n, m in sorted(rem.members().items(), key=lambda x: -x[1]['usize']):
            print(f"{m['usize']:>12} {'stored' if m['method'] == 0 else 'deflated':8} {n}")
        return
    fs, m, u = open_member(rem, a.args[0], a.index)
    print(f'volume "{fs.vol_name}", block {fs.bs}, sealed {fs.sealed}', file=sys.stderr)
    if a.cmd == 'ls':
        oid = fs.lookup(a.args[1] if len(a.args) > 1 else '/')
        for name, cid in sorted(fs.children(oid).items()):
            ino = fs.inode(cid)
            x = fs.xattrs(cid) if not ino['size'] else {}
            if 'com.apple.decmpfs' in x and x['com.apple.decmpfs'][0] == 'data':
                ino['size'] = struct.unpack_from('<Q', x['com.apple.decmpfs'][1], 8)[0]
            print(f"{stat.filemode(ino['mode'])} {ino['uid']:>3} {ino['gid']:>3} {ino['size']:>12} {name}")
        return
    paths = (a.args[2:] if a.cmd == 'get' else a.args[1:])
    if a.list:
        paths += [l.split('#')[0].strip() for l in open(a.list) if l.split('#')[0].strip()]
    plan = Plan(fs, paths)
    print(f'{len(plan.items)} items, {plan.logical() / 2**20:.1f} MiB of file data', file=sys.stderr)
    if a.cmd == 'du':
        return
    if u:
        streams = fetch_stored(u, fs, plan)
    else:
        streams = fetch_deflated(rem, m, fs, plan)
    write_items(plan, a.args[1], streams)
    if a.owners:
        import json
        own = {}
        if os.path.exists(a.owners):
            own = json.load(open(a.owners))
        for path, ino, _ in plan.items:
            own[a.prefix.rstrip('/') + path] = [ino['uid'], ino['gid'], ino['mode'] & 0o7777]
        with open(a.owners, 'w') as f:
            json.dump(own, f, indent=0, sort_keys=True)
    print(f'wrote {len(plan.items)} items to {a.args[1]}; fetched {rem.fetched / 2**20:.1f} MiB', file=sys.stderr)


if __name__ == '__main__':
    main()
