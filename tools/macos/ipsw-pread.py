#!/usr/bin/env python3
"""ipsw-pread.py: a byte range of ONE file inside an IPSW disk image, without fetching the file.

  python3 -I tools/macos/ipsw-pread.py URL MEMBER KEYS PATH OFFSET LENGTH OUT [OFFSET LENGTH OUT ...]

Uses ipsw-apfs.py's readers (ranged GETs on Apple's CDN). Handles plain files and decmpfs files whose data is in
the resource fork with an offset table (LZVN / LZFSE / LZBITMAP / raw: 64 KiB chunks), which is how the 15.6 system
cryptex stores the dyld shared cache (1 GB compressed): 28 KiB of libsystem_sandbox cost about 10 MiB of traffic.
Apple software stays local: write OUT under build/ or a scratch directory, never into the repository.
"""
import importlib.util, os, struct, sys

spec = importlib.util.spec_from_file_location('ia', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ipsw-apfs.py'))
ia = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ia)


def main():
    url, member, keys, path = sys.argv[1:5]
    jobs = sys.argv[5:]
    rem = ia.Remote(url)
    fs, m, u = ia.open_member(rem, member, None, keys)
    if isinstance(u, tuple):                # a raw APFS container inside the AEA stream: bytes straight from it
        rd = u[1].get
    elif u:
        rd = u.read                         # the APFS partition of a UDIF image
    else:
        sys.exit('a deflated member has no random access; use ipsw-apfs.py get')
    oid = fs.lookup(path)
    ino = fs.inode(oid)
    x = fs.xattrs(oid)

    def read_stream(exts, off, ln):         # bytes [off, off+ln) of a data stream given its extents
        buf = bytearray(ln)
        for la, eln, pb in exts:
            lo, hi = max(la, off), min(la + eln, off + ln)
            if lo < hi and pb:
                buf[lo - off:hi - off] = rd(pb * fs.bs + (lo - la), hi - lo)
        return bytes(buf)

    if 'com.apple.decmpfs' in x and x['com.apple.decmpfs'][0] == 'data':
        magic, ctype, usize = struct.unpack_from('<IIQ', x['com.apple.decmpfs'][1])
        kind, inline = ia.DECMPFS[ctype]
        rf = x.get('com.apple.ResourceFork')
        if inline or kind == 'zlib' or rf is None or rf[0] != 'stream':
            sys.exit(f'{path}: decmpfs type {ctype} is not an offset-table resource fork; use ipsw-apfs.py get')
        exts = fs.extents(rf[1][0])
        n = (usize + 65535) // 65536
        table = struct.unpack('<%dI' % (n + 1), read_stream(exts, 0, 4 * (n + 1)))

        def pread(off, ln):
            out = bytearray()
            for c in range(off // 65536, (off + ln + 65535) // 65536):
                want = min(65536, usize - 65536 * c)
                out += ia.decmpfs_chunk(kind, read_stream(exts, table[c], table[c + 1] - table[c]), want)
            s = off - 65536 * (off // 65536)
            return bytes(out[s:s + ln])
        size = usize
    else:
        exts = fs.extents(ino['pid'])
        size = ino['size']

        def pread(off, ln):
            return read_stream(exts, off, ln)
    print(f'{path}: {size} bytes', file=sys.stderr)
    for i in range(0, len(jobs), 3):
        off, ln, out = int(jobs[i], 0), int(jobs[i + 1], 0), jobs[i + 2]
        data = pread(off, min(ln, size - off))
        with open(out, 'wb') as f:
            f.write(data)
        print(f'{out}: {len(data)} bytes from {off:#x}', file=sys.stderr)
    print(f'fetched {rem.fetched / 2**20:.1f} MiB', file=sys.stderr)


main()
