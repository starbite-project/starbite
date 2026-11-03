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


# ---------------------------------------------------------------- AEA (macOS 15+ images)
class RangeCache:
    """pread() over a byte range of the remote IPSW (one stored zip member), with an LRU
    of fetched windows. Sequential misses grow the read-ahead (256 KiB .. 16 MiB), so a
    multi-GB file streams in large ranged GETs and scattered metadata reads stay small."""
    def __init__(self, rem, base, size, keep=6):
        self.rem, self.base, self.size = rem, base, size
        self.win = OrderedDict()        # start -> bytes
        self.keep, self.ra, self.last_end = keep, 256 << 10, -1
        self.lock = __import__('threading').Lock()

    def pread(self, off, n):
        with self.lock:
            return self._pread(off, n)

    def _pread(self, off, n):
        out = bytearray()
        while n > 0 and off < self.size:
            hit = None
            for s, b in reversed(self.win.items()):
                if s <= off < s + len(b):
                    hit = (s, b)
                    break
            if hit is None:
                self.ra = min(self.ra * 2, 16 << 20) if off == self.last_end else 256 << 10
                ln = min(max(n, self.ra), self.size - off)
                b = self.rem.get(self.base + off, ln)
                self.win[off] = b
                self.last_end = off + ln
                if len(self.win) > self.keep:
                    self.win.popitem(last=False)
                hit = (off, b)
            else:
                self.win.move_to_end(hit[0])
            s, b = hit
            piece = b[off - s:off - s + n]
            out += piece; off += len(piece); n -= len(piece)
        return bytes(out)


_aa = None
_PREAD = ctypes.CFUNCTYPE(ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_longlong)
_READ = ctypes.CFUNCTYPE(ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
_SEEK = ctypes.CFUNCTYPE(ctypes.c_longlong, ctypes.c_void_p, ctypes.c_longlong, ctypes.c_int)


def _libaa():
    global _aa
    if _aa is None:
        _aa = ctypes.CDLL('/usr/lib/libAppleArchive.dylib')
        vp = ctypes.c_void_p
        for name, res, args in (
                ('AACustomByteStreamOpen', vp, []),
                ('AACustomByteStreamSetPReadProc', None, [vp, _PREAD]),
                ('AACustomByteStreamSetReadProc', None, [vp, _READ]),
                ('AACustomByteStreamSetSeekProc', None, [vp, _SEEK]),
                ('AEAContextCreateWithEncryptedStream', vp, [vp]),
                ('AEAContextSetFieldBlob', ctypes.c_int, [vp, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]),
                ('AEAContextGetFieldUInt', ctypes.c_uint64, [vp, ctypes.c_uint32]),
                ('AEAContextDecryptAttributes', ctypes.c_int, [vp]),
                ('AEADecryptionRandomAccessInputStreamOpen', vp, [vp, vp, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_int]),
                ('AAByteStreamPRead', ctypes.c_ssize_t, [vp, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_longlong]),
                ('AAByteStreamSeek', ctypes.c_longlong, [vp, ctypes.c_longlong, ctypes.c_int])):
            f = getattr(_aa, name)
            f.restype, f.argtypes = res, args
    return _aa


class Aea:
    """Random access to the decrypted payload of a stored .dmg.aea member, through
    libAppleArchive's AEADecryptionRandomAccessInputStream over ranged GETs. The key is the
    archive's symmetric key (base64:...), from Apple's key service: `ipsw fw aea --key` on
    the member's first 256 KiB (aea_key() below). Same get()/fetched interface as Remote,
    so Udif and Apfs read through it unchanged."""
    def __init__(self, rem, m, key):
        aa = _libaa()
        self.rc = RangeCache(rem, rem.data_start(m), m['csize'])
        self.pos = 0
        def pread(_, buf, n, off):
            try:
                b = self.rc.pread(off, n)
            except Exception as e:      # an exception cannot cross the C callback
                print(f'\nAEA pread {off}+{n}: {e}', file=sys.stderr)
                return -1
            ctypes.memmove(buf, b, len(b))
            return len(b)
        def read(_, buf, n):
            r = pread(None, buf, n, self.pos)
            if r > 0:
                self.pos += r
            return r
        def seek(_, off, whence):
            self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.rc.size + off
            return self.pos
        self._cb = (_PREAD(pread), _READ(read), _SEEK(seek))   # keep the thunks alive
        s = aa.AACustomByteStreamOpen()
        aa.AACustomByteStreamSetPReadProc(s, self._cb[0])
        aa.AACustomByteStreamSetReadProc(s, self._cb[1])
        aa.AACustomByteStreamSetSeekProc(s, self._cb[2])
        ctx = aa.AEAContextCreateWithEncryptedStream(s)
        if not ctx:
            raise IOError('not an AEA stream (or its prologue could not be read)')
        k = __import__('base64').b64decode(key.split(':', 1)[-1])
        if aa.AEAContextSetFieldBlob(ctx, 9, 0, k, len(k)) or aa.AEAContextDecryptAttributes(ctx):
            raise IOError('AEA key rejected')            # 9: AEA_CONTEXT_FIELD_SYMMETRIC_KEY
        self.size = aa.AEAContextGetFieldUInt(ctx, 13)   # AEA_CONTEXT_FIELD_RAW_SIZE
        self.d = aa.AEADecryptionRandomAccessInputStreamOpen(s, ctx, 128 << 20, 0, 2)
        if not self.d:
            raise IOError('AEADecryptionRandomAccessInputStreamOpen failed')
        self.s, self.ctx = s, ctx
        self.rem = rem

    @property
    def fetched(self):
        return self.rem.fetched

    @fetched.setter
    def fetched(self, v):
        pass

    def get(self, start, n):
        buf = ctypes.create_string_buffer(n)
        got = 0
        while got < n:
            r = _aa.AAByteStreamPRead(self.d, ctypes.byref(buf, got), n - got, start + got)
            if r <= 0:
                raise IOError(f'AEA read {start + got}+{n - got}: {r}')
            got += r
        return buf.raw


def aea_key(rem, name, m, keyfile=None):
    """base64:... for member `name`: from KEYFILE ("member base64:..." lines) if listed there,
    else asked of Apple's key service by blacktop's ipsw (build/tools/ipsw/ipsw) on the
    member's first 256 KiB, and added to KEYFILE."""
    if keyfile and os.path.exists(keyfile):
        for line in open(keyfile):
            p = line.split()
            if len(p) == 2 and p[0] == name:
                return p[1]
    tool = os.environ.get('IPSW_TOOL', os.path.join(os.path.dirname(__file__), '../../build/tools/ipsw/ipsw'))
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.aea') as t:
        t.write(rem.get(rem.data_start(m), min(m['csize'], 262144))); t.flush()
        out = subprocess.run([tool, 'fw', 'aea', '--key', t.name], check=True,
                             capture_output=True, text=True).stdout
    key = next(w for w in out.split() if w.startswith('base64:'))
    if keyfile:
        with open(keyfile, 'a') as f:
            f.write(f'{name} {key}\n')
    return key


# ---------------------------------------------------------------- decompression
_lc = ctypes.CDLL('libcompression.dylib')
_lc.compression_decode_buffer.restype = ctypes.c_size_t
_lc.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                         ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
LZFSE, LZBITMAP, LZVN = 0x801, 0x702, 0x900   # 0x900: libcompression's raw LZVN


def lc_decode(src, size, algo=LZFSE):
    src = bytes(src)
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

    def resolve(self, path, depth=0):
        """Like lookup, but follows symlinks in the directories on the way (not the last
        component). -> (oid, real path, [paths of the symlinks passed])."""
        import posixpath
        parts = [p for p in path.split('/') if p]
        oid, cur = 2, ''
        for i, part in enumerate(parts):
            kids = self.children(oid)
            if part not in kids:
                raise FileNotFoundError(path)
            cid, here = kids[part], cur + '/' + part
            if i < len(parts) - 1 and (self.inode(cid)['mode'] & S_IFMT) == stat.S_IFLNK:
                if depth > 16:
                    raise FileNotFoundError(f'{path}: symlink loop')
                tgt = self.xattrs(cid)['com.apple.fs.symlink'][1].rstrip(b'\0').decode()
                nxt = posixpath.normpath(posixpath.join(tgt if tgt.startswith('/') else (cur or '/') + '/' + tgt,
                                                        *parts[i + 1:]))
                o, real, links = self.resolve(nxt, depth + 1)
                return o, real, [here] + links
            oid, cur = cid, here
        return oid, cur or '/', []

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

    missing = 0

    def add(self, path, recurse):
        fs = self.fs
        try:
            oid, real, links = fs.resolve(path)
        except FileNotFoundError:
            # A list names what a release may or may not ship: skip, say so.
            print(f'  not in this volume: {path}', file=sys.stderr)
            self.missing += 1
            return
        # symlinks on the way are kept as symlinks, their targets come along
        for ln in links:
            self._parents(ln)
            self._add(ln, fs.lookup(ln), False)
        self._parents(real)
        self._add(real, oid, recurse)

    def _parents(self, path):
        # parents first, so directories exist with their own modes
        fs = self.fs
        parts = [p for p in path.split('/') if p]
        for i in range(1, len(parts)):
            parent = '/' + '/'.join(parts[:i])
            if parent not in self.seen:
                self.seen.add(parent)
                self.items.append((parent, fs.inode(fs.lookup(parent)), None))

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


def write_items(plan, outdir, streams, direct=()):
    """streams[i]: the raw bytes of item i's data stream (plain file or resource fork);
    items in `direct` were already written to disk by pass 2 (only metadata is set here)."""
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
        data = None
        if i not in direct and os.path.lexists(dst):
            os.unlink(dst)
        if i in direct:
            if THIN and direct[i] != 'done' and src and src[0] in ('plain', 'rsrc') and os.path.getsize(dst) < 1 << 29:
                with open(dst, 'rb') as f:
                    whole = f.read()
                t = thin(whole)
                if len(t) != len(whole):
                    with open(dst, 'wb') as f:
                        f.write(t)
        elif src[0] == 'link':
            os.symlink(src[1], dst)
            if root:
                os.lchown(dst, ino['uid'], ino['gid'])
            continue
        elif src[0] == 'plain':
            data = streams.get(i, b'')[:ino['size']]
            data += bytes(ino['size'] - len(data))
        elif src[0] == 'inline':
            kind, blob, usize = src[1:]
            data = blob[:usize] if kind == 'none' or not usize else decmpfs_chunk(kind, blob, usize)
        elif src[0] == 'rsrcdata':
            data = decode_rsrc(src[1], src[2], src[3], path)
        else:
            data = decode_rsrc(src[1], streams[i][:src[3]], src[4], path)
        if data is not None:
            with open(dst, 'wb') as f:
                f.write(thin(data) if THIN else data)
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


THIN = False


def thin(data):
    """A universal Mach-O -> its arm64e slice (as lipo -thin arm64e); anything else as is."""
    if len(data) < 64 or data[:4] not in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        return data
    n, = struct.unpack_from('>I', data, 4)
    if not 0 < n < 16:
        return data     # a Java class file has the same magic
    for i in range(n):
        if data[:4] == b'\xca\xfe\xba\xbe':
            ct, cs, off, size, _ = struct.unpack_from('>iiIII', data, 8 + 20 * i)
        else:
            ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', data, 8 + 32 * i)
        if ct == 0x0100000c and (cs & 0xff) == 2 and data[off:off + 4] == b'\xcf\xfa\xed\xfe':
            return data[off:off + size]
    return data


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


class RawImage:
    """A raw APFS image behind a get(off, n) reader (a decrypted AEA payload): blocks with
    a small LRU, read 64 KiB at a time (metadata nodes cluster)."""
    def __init__(self, reader, bs=4096, span=16):
        self.r, self.bs, self.span = reader, bs, span
        self.cache = OrderedDict()

    def __call__(self, pb):
        base = pb - pb % self.span
        if base not in self.cache:
            self.cache[base] = self.r.get(base * self.bs, self.span * self.bs)
            if len(self.cache) > 512:
                self.cache.popitem(last=False)
        else:
            self.cache.move_to_end(base)
        o = (pb - base) * self.bs
        return self.cache[base][o:o + self.bs]


def open_member(rem, name, index=None, keyfile=None):
    ms = rem.members()
    if name not in ms:
        sys.exit(f'no member {name}')
    m = ms[name]
    if m['method'] == 0 and name.endswith('.aea'):
        aea = Aea(rem, m, aea_key(rem, name, m, keyfile))
        head = aea.get(0, 4096)
        if head[32:36] == b'NXSB':          # macOS 26: the payload is a raw APFS container
            return Apfs(RawImage(aea)), m, ('raw', aea)
        u = Udif(aea, 0, aea.size)
        fs = Apfs(lambda pb: u.read(pb * 4096, 4096))
        return fs, m, u
    if m['method'] == 0:
        fs, u = open_stored(rem, m)
        return fs, m, u
    blocks = index_deflated(rem, m, index)
    return Apfs(blocks), m, None


def fetch_raw(reader, fs, plan, outdir, run_max=4 << 20):
    """All wanted data of a random-access raw APFS image (a decrypted AEA payload).
    Small items: extents sorted by physical block, read in runs into memory. Big items
    (> DIRECT): their extents in logical order, straight to disk (RsrcSink decodes a
    compressed fork chunk by chunk). Returns (streams, direct) like fetch_deflated."""
    bs = fs.bs
    streams, direct, pieces = {}, {}, []
    big = []
    for i, (path, ino, src) in enumerate(plan.items):
        if not src or src[0] not in ('plain', 'rsrc'):
            continue
        exts = src[1] if src[0] == 'plain' else src[2]
        size = src[2] if src[0] == 'plain' else src[3]      # bytes of the stream on disk
        usize = src[2] if src[0] == 'plain' else src[4]     # bytes of the file
        if usize > DIRECT and (src[0] == 'plain' or src[1] in ('lzvn', 'lzfse', 'lzbitmap', 'raw')):
            big.append((i, exts, size))
            continue
        streams[i] = bytearray(size)
        for la, ln, pb in exts:
            if pb and la < size:
                pieces.append((pb, min(ln, size - la), i, la))
    pieces.sort()
    # runs of physically adjacent (or nearly: gaps up to 256 KiB) pieces -> one read
    runs, cur = [], []
    for p in pieces:
        if cur and (p[0] * bs - (cur[-1][0] * bs + cur[-1][1]) > 256 << 10 or
                    p[0] * bs + p[1] - cur[0][0] * bs > run_max):
            runs.append(cur); cur = []
        cur.append(p)
    if cur:
        runs.append(cur)
    print(f'{len(pieces)} small extents in {len(runs)} reads, {len(big)} big files', file=sys.stderr)
    left = {}
    for p in pieces:
        left[p[2]] = left.get(p[2], 0) + 1
    for n, run in enumerate(runs):
        lo = run[0][0] * bs
        hi = max(p[0] * bs + p[1] for p in run)
        raw = reader.get(lo, hi - lo)
        for pb, ln, i, la in run:
            streams[i][la:la + ln] = raw[pb * bs - lo:pb * bs - lo + ln]
            left[i] -= 1
            if not left[i]:          # complete: decode + write now, keep RAM flat
                path, ino, src = plan.items[i]
                dst = os.path.join(outdir, path.lstrip('/'))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.lexists(dst):
                    os.unlink(dst)
                if src[0] == 'plain':
                    data = bytes(streams[i][:ino['size']]) + bytes(max(0, ino['size'] - len(streams[i])))
                else:
                    data = decode_rsrc(src[1], streams.pop(i)[:src[3]], src[4], path)
                streams.pop(i, None)
                with open(dst, 'wb') as f:
                    f.write(thin(data) if THIN else data)
                direct[i] = 'done'
        if n % 50 == 0:
            print(f'\r  read {n + 1}/{len(runs)}, {reader.fetched >> 20} MiB fetched', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    for i, exts, size in big:
        path, ino, src = plan.items[i]
        dst = os.path.join(outdir, path.lstrip('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.lexists(dst):
            os.unlink(dst)
        f = open(dst, 'w+b')
        f.truncate(src[2] if src[0] == 'plain' else src[4])
        sink = None if src[0] == 'plain' else RsrcSink(f, src[1], src[3], src[4])
        print(f'  {path}: {size >> 20} MiB on disk', file=sys.stderr, flush=True)
        for la, ln, pb in sorted(exts):
            ln = min(ln, size - la)
            off = 0
            while off < ln:
                k = min(run_max, ln - off)
                b = bytes(k) if not pb else reader.get(pb * bs + off, k)
                if sink:
                    sink.put(la + off, b)
                else:
                    f.seek(la + off); f.write(b)
                off += k
            print(f'\r    {(la + ln) * 100 // max(size, 1):3d}%, {reader.fetched >> 20} MiB fetched', end='', file=sys.stderr, flush=True)
        print(file=sys.stderr)
        if sink and not sink.done():
            raise IOError(f'{path}: decoded {sink.k} of {sink.n} chunks')
        f.close()
        direct[i] = True
    return streams, direct


def fetch_stored(u, fs, plan, run_max=32 << 20):
    """All wanted data of a stored UDIF member, each compressed chunk fetched once:
    the extents are cut at chunk boundaries, sorted, and runs of chunks that are
    adjacent in the file go out as one ranged GET (at most run_max bytes)."""
    pieces = []           # (chunk, offset in chunk, length, item, offset in the item's stream)
    streams = {}
    for i, (path, ino, src) in enumerate(plan.items):
        if not src or src[0] not in ('plain', 'rsrc'):
            continue
        exts = src[1] if src[0] == 'plain' else src[2]
        size = src[2] if src[0] == 'plain' else src[3]
        streams[i] = bytearray(size)
        for la, ln, pb in exts:
            if not pb or la >= size:
                continue
            off, left, dst = pb * fs.bs, min(ln, size - la), la
            while left:
                c = bisect.bisect_right(u.offs, off) - 1
                co, cl = u.chunks[c][0], u.chunks[c][1]
                n = min(left, co + cl - off)
                pieces.append((c, off - co, n, i, dst))
                off += n; dst += n; left -= n
    pieces.sort()
    need = sorted({p[0] for p in pieces})
    runs, cur = [], []
    for c in need:
        t, fo, cl = u.chunks[c][2], u.chunks[c][3], u.chunks[c][4]
        if cur and (t in (0, 2) or u.chunks[cur[-1]][2] in (0, 2) or
                    not 0 <= fo - (u.chunks[cur[-1]][3] + u.chunks[cur[-1]][4]) <= 4 << 20 or
                    fo + cl - u.chunks[cur[0]][3] > run_max):
            runs.append(cur); cur = []
        cur.append(c)
    if cur:
        runs.append(cur)
    print(f'{len(pieces)} pieces in {len(need)} chunks, {len(runs)} requests', file=sys.stderr)
    k = 0
    for n, run in enumerate(runs):
        first = u.chunks[run[0]]
        if first[2] in (0, 2):
            raw = None
        else:
            last = u.chunks[run[-1]]
            raw = u.rem.get(first[3], last[3] + last[4] - first[3])
        for c in run:
            off, ln, t, fo, cl = u.chunks[c]
            if t in (0, 2):
                data = bytes(ln)
            else:
                b = raw[fo - first[3]:fo - first[3] + cl]
                data = b if t == 1 else lc_decode(b, ln) if t == 0x80000007 else zlib.decompress(b)
            while k < len(pieces) and pieces[k][0] == c:
                _, o, ln2, i, dst = pieces[k]
                streams[i][dst:dst + ln2] = data[o:o + ln2]
                k += 1
        if n % 20 == 0:
            print(f'\r  request {n + 1}/{len(runs)}, {u.rem.fetched >> 20} MiB', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    return {i: bytes(v) for i, v in streams.items()}


class RsrcSink:
    """Decodes one decmpfs resource fork (offset-table formats: LZVN, LZFSE, LZBITMAP, raw)
    while its blocks stream by, writing 64 KiB chunks straight to the destination file, so
    a multi-GB compressed file never sits in memory or on disk twice."""
    def __init__(self, f, kind, rsize, usize):
        self.f, self.kind, self.rsize, self.usize = f, kind, rsize, usize
        self.n = (usize + 65535) // 65536
        self.buf, self.base, self.pending, self.k, self.offs = bytearray(), 0, {}, 0, None

    def put(self, lo, data):
        self.pending[lo] = data
        while self.base + len(self.buf) in self.pending:
            self.buf += self.pending.pop(self.base + len(self.buf))
        end = self.base + len(self.buf)
        if self.offs is None:
            if end < 4 * (self.n + 1):
                return
            self.offs = struct.unpack_from('<%dI' % (self.n + 1), self.buf, 0)
        while self.k < self.n and self.offs[self.k + 1] <= end:
            a, b = self.offs[self.k] - self.base, self.offs[self.k + 1] - self.base
            want = min(65536, self.usize - self.k * 65536)
            self.f.seek(self.k * 65536)
            self.f.write(decmpfs_chunk(self.kind, bytes(self.buf[a:b]), want))
            self.k += 1
        if self.k < self.n:
            cut = self.offs[self.k] - self.base
            del self.buf[:cut]
            self.base += cut

    def done(self):
        return self.k == self.n


DIRECT = 8 << 20   # files above this go straight to disk during pass 2


def fetch_deflated(rem, m, fs, plan, outdir):
    """Pass 2. Returns (streams for small items, {item: written-to-disk} for big ones)."""
    need = plan.blocks()
    streams, direct = {}, {}
    for i, (path, ino, src) in enumerate(plan.items):
        if not src or src[0] not in ('plain', 'rsrc'):
            continue
        size = src[2] if src[0] == 'plain' else src[4]
        if size > DIRECT and (src[0] == 'plain' or src[1] in ('lzvn', 'lzfse', 'lzbitmap', 'raw')):
            dst = os.path.join(outdir, path.lstrip('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst):
                os.unlink(dst)
            f = open(dst, 'w+b')
            f.truncate(size)
            direct[i] = f if src[0] == 'plain' else RsrcSink(f, src[1], src[3], src[4])
        else:
            streams[i] = bytearray(src[2] if src[0] == 'plain' else src[3])
    if not need:
        return streams, direct
    last = max(need)
    print(f'pass 2 (data): {len(need)} blocks up to block {last}', file=sys.stderr)
    for pb, b in stream_blocks(rem, m, fs.bs):
        for i, off in need.get(pb, ()):
            if i in direct:
                d = direct[i]
                if isinstance(d, RsrcSink):
                    d.put(off, b)
                else:
                    n = min(fs.bs, plan.items[i][2][2] - off)
                    if n > 0:
                        d.seek(off); d.write(b[:n])
                continue
            s = streams[i]
            n = min(fs.bs, len(s) - off)
            if n > 0:
                s[off:off + n] = b[:n]
        if pb >= last:
            break
    for i, d in direct.items():
        if isinstance(d, RsrcSink):
            if not d.done():
                raise IOError(f'{plan.items[i][0]}: decoded {d.k} of {d.n} chunks')
            d.f.close()
        else:
            d.close()
    return streams, direct


# ---------------------------------------------------------------- CLI
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('url')
    ap.add_argument('cmd', choices=['members', 'fetch', 'ls', 'get', 'du'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--list', action='append', default=[], help='file with one path per line (repeatable)')
    ap.add_argument('--keys', help='AEA keys file ("member base64:KEY" lines); missing keys are '
                    'fetched from Apple with build/tools/ipsw/ipsw and appended')
    ap.add_argument('--head', type=int, default=0, help='fetch: only the first N bytes of the member')
    ap.add_argument('--index', help='pass-1 metadata index for a deflated member (cached)')
    ap.add_argument('--owners', help='get: also write a JSON {path: [uid, gid, mode]} of what was '
                    'extracted (a non-root run cannot chown; tools/macos/hfs-owners.py applies it)')
    ap.add_argument('--thin', action='store_true', help='get: keep only the arm64e slice of '
                    'universal Mach-O files (each slice carries its own code signature)')
    ap.add_argument('--prefix', default='', help='get: path prefix for --owners keys (e.g. the '
                    'cryptex mount point /System/Volumes/Preboot/Cryptexes/OS)')
    a = ap.parse_intermixed_args()   # paths may follow the options (build-macos-root.sh)
    rem = Remote(a.url)
    if a.cmd == 'members':
        for n, m in sorted(rem.members().items(), key=lambda x: -x[1]['usize']):
            print(f"{m['usize']:>12} {'stored' if m['method'] == 0 else 'deflated':8} {n}")
        return
    if a.cmd == 'fetch':      # fetch MEMBER OUTFILE: one whole member (or its first --head bytes)
        ms = rem.members()
        m = ms[a.args[0]]
        start = rem.data_start(m)
        if m['method'] == 0:
            n = a.head or m['csize']
            data = rem.get(start, n)
        else:
            data = zlib.decompressobj(-15).decompress(rem.get(start, m['csize']))
            data = data[:a.head] if a.head else data
        with open(a.args[1], 'wb') as f:
            f.write(data)
        print(f'{a.args[1]}: {len(data)} bytes', file=sys.stderr)
        return
    fs, m, u = open_member(rem, a.args[0], a.index, a.keys)
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
    for lst in a.list:
        paths += [l.split('#')[0].strip() for l in open(lst) if l.split('#')[0].strip()]
    plan = Plan(fs, paths)
    print(f'{len(plan.items)} items, {plan.logical() / 2**20:.1f} MiB of file data', file=sys.stderr)
    if a.cmd == 'du':
        return
    global THIN
    THIN = a.thin
    direct = {}
    if isinstance(u, tuple):
        streams, direct = fetch_raw(u[1], fs, plan, a.args[1])
    elif u:
        streams = fetch_stored(u, fs, plan)
    else:
        streams, direct = fetch_deflated(rem, m, fs, plan, a.args[1])
    write_items(plan, a.args[1], streams, direct)
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
