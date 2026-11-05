#!/usr/bin/env python3
"""fetch-gsi.py URL SHA256 OUTDIR [--dry-run]

Download one of Google's GSI zips (developer.android.com/topic/generic-system-image/releases) in a
single stream, without ever storing the zip: the stream is hashed (SHA-256 of the whole zip, compared
with the release page's value) and its members are inflated on the fly into OUTDIR (system.img,
vbmeta.img, build.prop), each checked against its CRC-32 from the zip. Peak disk = the unpacked
system.img (about 4 GB) instead of zip + image (about 6 GB).

If anything does not match, every output file is deleted and the exit code is 1.
--dry-run only reads the zip's central directory over HTTP ranges and prints what would be written.
Run it with python3 -I in a directory that holds nothing else (outputs are untrusted data)."""
import hashlib, os, struct, sys, urllib.request, zlib

ALLOWED = {'system.img', 'vbmeta.img', 'build.prop'}
CHUNK = 1 << 20


def central_directory(url):
    def rng(a, b):
        return urllib.request.urlopen(urllib.request.Request(url, headers={'Range': f'bytes={a}-{b}'})).read()
    size = int(urllib.request.urlopen(urllib.request.Request(url, method='HEAD')).headers['Content-Length'])
    tail = rng(max(0, size - 70000), size - 1)
    j = tail.rfind(b'PK\x06\x06')
    if j >= 0:
        cdsize, cdoff = struct.unpack('<QQ', tail[j + 40:j + 56])
    else:
        i = tail.rfind(b'PK\x05\x06')
        cdsize, cdoff = struct.unpack('<II', tail[i + 12:i + 20])
    cd = rng(cdoff, cdoff + cdsize - 1)
    out, p = [], 0
    while p < len(cd) and cd[p:p + 4] == b'PK\x01\x02':
        meth, = struct.unpack('<H', cd[p + 10:p + 12])
        crc, cs, us = struct.unpack('<III', cd[p + 16:p + 28])
        nl, el, cl = struct.unpack('<HHH', cd[p + 28:p + 34])
        lho, = struct.unpack('<I', cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode()
        ex, q = cd[p + 46 + nl:p + 46 + nl + el], 0
        while q < len(ex):
            hid, hs = struct.unpack('<HH', ex[q:q + 4])
            if hid == 1:
                vals, k = list(struct.unpack('<%dQ' % (hs // 8), ex[q + 4:q + 4 + hs])), 0
                if us == 0xffffffff: us = vals[k]; k += 1
                if cs == 0xffffffff: cs = vals[k]; k += 1
                if lho == 0xffffffff: lho = vals[k]; k += 1
            q += 4 + hs
        out.append(dict(name=name, method=meth, crc=crc, comp=cs, size=us, off=lho))
        p += 46 + nl + el + cl
    return size, sorted(out, key=lambda e: e['off'])


def main():
    args = [a for a in sys.argv[1:] if a != '--dry-run']
    dry = '--dry-run' in sys.argv
    if len(args) != 3:
        sys.exit(__doc__)
    url, want, outdir = args
    want = want.lower()
    total, members = central_directory(url)
    for m in members:
        if m['name'] not in ALLOWED or '/' in m['name'] or m['method'] not in (0, 8):
            sys.exit(f"unexpected member {m['name']!r} (method {m['method']}): refusing")
        print(f"  {m['name']:12s} {m['size']:>12d} B  crc32 {m['crc']:08x}  at {m['off']}")
    print(f'zip {total} B; sha256 expected {want}')
    if dry:
        print('dry run: nothing downloaded')
        return
    os.makedirs(outdir, exist_ok=True)
    st = os.statvfs(outdir)
    need = sum(m['size'] for m in members)
    if st.f_bavail * st.f_frsize < need + (4 << 30):
        sys.exit(f'{outdir}: {st.f_bavail * st.f_frsize >> 20} MiB free < {need >> 20} MiB + 4 GiB margin')
    sha = hashlib.sha256()
    resp = urllib.request.urlopen(url)
    pos, buf, written = 0, b'', []

    def read(n):                       # exactly n bytes from the stream (hashed)
        nonlocal pos, buf
        while len(buf) < n:
            c = resp.read(CHUNK)
            if not c:
                raise EOFError('stream ended early')
            sha.update(c)
            buf += c
        d, buf = buf[:n], buf[n:]
        pos += n
        return d
    try:
        for m in members:
            if pos != m['off']:
                read(m['off'] - pos)
            h = read(30)
            assert h[:4] == b'PK\x03\x04', 'bad local header'
            nl, el = struct.unpack('<HH', h[26:30])
            read(nl + el)
            path = os.path.join(outdir, m['name'])
            written.append(path)
            crc, n, left = 0, 0, m['comp']
            dec = zlib.decompressobj(-15) if m['method'] == 8 else None
            with open(path, 'wb') as f:
                while left:
                    c = read(min(CHUNK, left))
                    left -= len(c)
                    d = dec.decompress(c) if dec else c
                    crc = zlib.crc32(d, crc); n += len(d); f.write(d)
                if dec:
                    d = dec.flush(); crc = zlib.crc32(d, crc); n += len(d); f.write(d)
            if n != m['size'] or crc != m['crc']:
                raise ValueError(f"{m['name']}: size {n}/{m['size']} crc {crc:08x}/{m['crc']:08x}")
            print(f"  {m['name']} ok", flush=True)
        while True:                    # the rest of the zip (central directory) for the hash
            c = resp.read(CHUNK)
            if not c:
                break
            sha.update(c)
        got = sha.hexdigest()
        if got != want:
            raise ValueError(f'zip sha256 {got} != {want}')
    except Exception as e:
        for p in written:
            if os.path.exists(p):
                os.unlink(p)
        sys.exit(f'FAILED, outputs removed: {e}')
    with open(os.path.join(outdir, 'SOURCE.txt'), 'w') as f:
        f.write(f'url {url}\nzip_sha256 {got}\n')
        for m in members:
            p = os.path.join(outdir, m['name'])
            f.write(f"{m['name']} {m['size']} crc32 {m['crc']:08x} sha256 {hashlib.sha256(open(p, 'rb').read()).hexdigest() if m['size'] < 1 << 20 else '-'}\n")
    print(f'zip sha256 {got} matches; outputs in {outdir}')


if __name__ == '__main__':
    main()
