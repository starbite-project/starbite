#!/usr/bin/env python3
"""Serve the decrypted contents of an AEA-encrypted disk image that sits inside a
remote IPSW, over local HTTP with Range support, so `hdiutil attach` can mount it
without the multi-GB image ever landing on disk.

  aea-remote-serve.py serve IPSW-URL MEMBER [--port 8771] [--key base64:...] [--plain]
  aea-remote-serve.py serve-file FILE [--port 8771]       (plain file, for testing)
  aea-remote-serve.py key IPSW-URL MEMBER                 (print the AEA key only)

then   hdiutil attach   (a --plain UDIF member: drop the -imagekey) -readonly -nobrowse -noverify \
           -imagekey diskimage-class=CRawDiskImage http://127.0.0.1:8771/image.dmg

How: an IPSW member that is stored (zip method 0) is one contiguous byte range of
the IPSW, so every AEA segment can be fetched with an HTTP range request. AEA
(Apple Encrypted Archive, profile 1) is clustered: each cluster header lists its
segments' sizes, so one small read per cluster builds a map from plaintext offset
to (encrypted range, keys); a segment is then decrypted on its own (HKDF-SHA256 key
derivation, HMAC-SHA256 check, AES-256-CTR, optional LZFSE etc via libcompression).
Format as in blacktop/ipsw pkg/aea (decrypt.go, encrypt.go). Nothing is written to
disk except a small segment cache in RAM. The key comes from Apple's public key
service through build/tools/ipsw (it needs only the AEA header + auth data, which
are fetched into a temporary file of a few KiB).

Apple files are never stored in the repository; this tool stores none at all."""
import argparse, ctypes, ctypes.util, hashlib, hmac, http.server, os, re, socketserver
import struct, subprocess, sys, tempfile, threading, time, base64, socket, ssl, urllib.parse
import http.client, queue
from collections import OrderedDict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ref', 'ios-herolte', 'tools'))
import remotezip as rz  # noqa: E402

IPSW_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'tools', 'ipsw', 'ipsw')

# ---------------------------------------------------------------- crypto helpers
_cc = ctypes.CDLL('/usr/lib/system/libcommonCrypto.dylib')
_cmp = ctypes.CDLL(ctypes.util.find_library('compression'))
_cmp.compression_decode_buffer.restype = ctypes.c_size_t
_cmp.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                           ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
COMP = {ord('e'): 0x801, ord('4'): 0x100, ord('z'): 0x205, ord('x'): 0x306, ord('b'): 0x702}


def aes_ctr(key, iv, data):
    ref = ctypes.c_void_p()
    # CCCryptorCreateWithMode(op=decrypt 1, mode=CTR 4, alg=AES 0, padding 0, iv, key, keylen,
    #                         tweak, tweaklen, rounds, options=kCCModeOptionCTR_BE 2, &ref)
    st = _cc.CCCryptorCreateWithMode(1, 4, 0, 0, iv, key, len(key), None, 0, 0, 2, ctypes.byref(ref))
    assert st == 0, f'CCCryptorCreateWithMode {st}'
    out = ctypes.create_string_buffer(len(data))
    moved = ctypes.c_size_t()
    st = _cc.CCCryptorUpdate(ref, data, len(data), out, len(data), ctypes.byref(moved))
    _cc.CCCryptorRelease(ref)
    assert st == 0 and moved.value == len(data), f'CCCryptorUpdate {st}'
    return out.raw


def hkdf_extract(salt, ikm):
    return hmac.new(salt or b'\0' * 32, ikm, hashlib.sha256).digest()


def hkdf_expand(prk, info, n):
    out, t, i = b'', b'', 1
    while len(out) < n:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t; i += 1
    return out[:n]


def hkdf(ikm, salt, info, n):
    return hkdf_expand(hkdf_extract(salt, ikm), info, n)


def hkey(b):            # headerKey: MAC 32, Key 32, IV 16
    return b[:32], b[32:64], b[64:80]


def aea_hmac(key, data, salt=b''):
    m = hmac.new(key, salt + data, hashlib.sha256)
    m.update(struct.pack('<Q', len(salt)))
    return m.digest()


# ---------------------------------------------------------------- byte sources
class Remote:
    """A stored member of a remote zip, as random-access bytes."""
    def __init__(self, url, member):
        self.url = url
        ent = [e for e in rz.central_dir(url) if e[0] == member]
        assert ent, f'{member} not in {url}'
        name, method, csize, usize, lho, crc = ent[0]
        assert method == 0, f'{member} is compressed in the zip (method {method})'
        hdr = rz.get(url, lho, lho + 29)
        nlen, xlen = struct.unpack_from('<HH', hdr, 26)
        self.base = lho + 30 + nlen + xlen
        self.size = csize
        self.fetched = 0

        u = urllib.parse.urlsplit(url)
        self.host, self.path = u.hostname, u.path
        self.pool = queue.LifoQueue()

    def _new_conn(self):
        # Keep-alive HTTPS connections over IPv4 (this host's IPv6 route to the CDN is
        # broken, see remotezip.py), pooled across threads: the HTTP server starts a thread
        # per client connection, and a warm connection keeps its TCP window, so a 4 MiB
        # range costs a fraction of a new curl + TLS handshake + slow start.
        ip = socket.getaddrinfo(self.host, 443, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
        c = http.client.HTTPSConnection(self.host, 443, timeout=120)
        raw = socket.create_connection((ip, 443), timeout=120)
        c.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=self.host)
        return c

    def read(self, off, n):
        if n <= 0:
            return b''
        a, b = self.base + off, self.base + off + n - 1
        for attempt in range(6):
            try:
                c = self.pool.get_nowait()
            except queue.Empty:
                c = None
            try:
                if c is None or attempt > 0:
                    if c is not None:
                        c.close()
                    c = self._new_conn()
                c.request('GET', self.path, headers={'Range': f'bytes={a}-{b}', 'Host': self.host})
                r = c.getresponse()
                d = r.read()
                assert r.status == 206 and len(d) == n, f'bad range reply {r.status} {len(d)}'
                self.fetched += len(d)
                self.pool.put(c)
                return d
            except Exception:           # noqa: BLE001  (transient CDN errors)
                if c is not None:
                    c.close()
                if attempt == 5:
                    raise
                time.sleep(1 + attempt * 3)


class Local:
    def __init__(self, path):
        self.f = open(path, 'rb'); self.size = os.path.getsize(path); self.lock = threading.Lock()

    def read(self, off, n):
        with self.lock:
            self.f.seek(off); return self.f.read(n)


# ---------------------------------------------------------------- AEA random access
class AEA:
    def __init__(self, src, key):
        self.src = src
        h = src.read(0, 12)
        magic, prof, alen = struct.unpack('<4sII', h)
        assert magic == b'AEA1' and prof & 0xffffff == 1, (magic, prof)
        pro = src.read(12, alen + 32 + 32 + 48 + 32)
        auth = pro[:alen]
        salt = pro[alen:alen + 32]
        rh_mac, rh_data, cl_mac = pro[alen + 32:alen + 64], pro[alen + 64:alen + 112], pro[alen + 112:alen + 144]
        self.main = hkdf(key, salt, b'AEA_AMK' + struct.pack('<I', prof), 32)
        mk, k, iv = hkey(hkdf(self.main, b'', b'AEA_RHEK', 80))
        m = hmac.new(mk, cl_mac + auth + rh_data, hashlib.sha256)
        m.update(struct.pack('<Q', 32 + alen))
        assert hmac.compare_digest(m.digest(), rh_mac), 'root header HMAC mismatch (wrong key?)'
        rh = aes_ctr(k, iv, rh_data)
        self.raw_size, self.enc_size, self.seg_size, self.spc = struct.unpack_from('<QQII', rh)
        self.comp, self.cks = rh[24], rh[25]
        assert self.enc_size == src.size, (self.enc_size, src.size)
        cksz = {0: 0, 1: 8, 2: 32}[self.cks]
        ent = 8 + cksz
        hsz = ent * self.spc
        # walk cluster headers: (plain_off, enc_off, comp_size, cluster, index, mac, prk)
        self.segs = []
        pos = 12 + alen + 144
        out = 0; cl = 0
        while out < self.raw_size:
            ck = hkdf(self.main, None, b'AEA_CK' + struct.pack('<I', cl), 32)
            cmk, ckk, civ = hkey(hkdf(ck, None, b'AEA_CHEK', 80))
            blob = src.read(pos, hsz + 32 + 32 * self.spc)
            heads, macs = blob[:hsz], blob[hsz:]
            assert hmac.compare_digest(aea_hmac(cmk, heads, macs), cl_mac), f'cluster {cl} HMAC'
            cl_mac = macs[:32]
            heads = aes_ctr(ckk, civ, heads)
            prk = hkdf_extract(None, ck)
            pos += len(blob)
            for i in range(self.spc):
                dsz, csz = struct.unpack_from('<II', heads, i * ent)
                if dsz == 0:
                    continue
                self.segs.append((out, pos, csz, dsz, i, macs[32 + 32 * i:64 + 32 * i], prk))
                pos += csz; out += dsz
            cl += 1
        self.clusters = cl
        self.cache = OrderedDict(); self.lock = threading.Lock(); self.cache_max = 96
        self.inflight = {}
        from concurrent.futures import ThreadPoolExecutor
        self.pool = ThreadPoolExecutor(6)

    GROUP = 4          # segments fetched per range request (~4 MiB: a warm connection's sweet spot)
    READAHEAD = 2      # groups prefetched after a miss (file reads are mostly sequential)

    def _decrypt(self, si, data):
        out, epos, csz, dsz, idx, mac, prk = self.segs[si]
        mk, k, iv = hkey(hkdf_expand(prk, b'AEA_SK' + struct.pack('<I', idx), 80))
        assert hmac.compare_digest(aea_hmac(mk, data), mac), f'segment {si} HMAC'
        data = aes_ctr(k, iv, data)
        if csz != dsz:
            buf = ctypes.create_string_buffer(dsz + 1)
            n = _cmp.compression_decode_buffer(buf, dsz + 1, data, csz, None, COMP[self.comp])
            assert n == dsz, f'segment {si}: decompressed {n} != {dsz}'
            data = buf.raw[:dsz]
        return data

    def _load_group(self, g):
        """Fetch + decrypt segment group g into the cache (one caller per group)."""
        with self.lock:
            if g in self.cache:
                self.cache.move_to_end(g); return self.cache[g]
            ev = self.inflight.get(g)
            if ev is None:
                ev = self.inflight[g] = threading.Event(); mine = True
            else:
                mine = False
        if not mine:
            ev.wait()
            with self.lock:
                if g in self.cache:
                    return self.cache[g]
            return self._load_group(g)
        try:
            lo = g * self.GROUP; hi = min(lo + self.GROUP, len(self.segs))
            e0 = self.segs[lo][1]; e1 = self.segs[hi - 1][1] + self.segs[hi - 1][2]
            blob = self.src.read(e0, e1 - e0)
            segs = [self._decrypt(si, blob[self.segs[si][1] - e0:self.segs[si][1] - e0 + self.segs[si][2]])
                    for si in range(lo, hi)]
            with self.lock:
                self.cache[g] = segs
                while len(self.cache) > max(8, self.cache_max // self.GROUP):
                    self.cache.popitem(last=False)
            return segs
        finally:
            with self.lock:
                self.inflight.pop(g, None)
            ev.set()

    def segment(self, si):
        g = si // self.GROUP
        with self.lock:
            hit = g in self.cache
        if not hit:
            ng = (len(self.segs) + self.GROUP - 1) // self.GROUP
            for k in range(1, self.READAHEAD + 1):
                if g + k < ng:
                    with self.lock:
                        busy = (g + k) in self.cache or (g + k) in self.inflight
                    if not busy:
                        self.pool.submit(self._load_group, g + k)
        return self._load_group(g)[si - g * self.GROUP]

    @property
    def size(self):
        return self.raw_size

    def read(self, off, n):
        import bisect
        res = []
        if not hasattr(self, '_starts'):
            self._starts = [s[0] for s in self.segs]
        end = min(off + n, self.raw_size)
        while off < end:
            si = bisect.bisect_right(self._starts, off) - 1
            d = self.segment(si)
            o = off - self.segs[si][0]
            take = min(len(d) - o, end - off)
            res.append(d[o:o + take]); off += take
        return b''.join(res)


# ---------------------------------------------------------------- HTTP server
def make_handler(src):
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *a):
            pass

        def _head(self, code, length, rng=None):
            self.send_response(code)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Length', str(length))
            self.send_header('Last-Modified', 'Thu, 01 Jan 2026 00:00:00 GMT')
            self.send_header('ETag', '"aea-remote"')
            if rng:
                self.send_header('Content-Range', f'bytes {rng[0]}-{rng[1]}/{src.size}')
            self.end_headers()

        def do_HEAD(self):
            self._head(200, src.size)

        def do_GET(self):
            r = self.headers.get('Range')
            if r:
                m = re.match(r'bytes=(\d*)-(\d*)', r)
                a = int(m.group(1)) if m.group(1) else src.size - int(m.group(2))
                b = int(m.group(2)) if m.group(1) and m.group(2) else src.size - 1
                b = min(b, src.size - 1)
                if a > b:
                    self.send_response(416); self.send_header('Content-Length', '0'); self.end_headers(); return
                self._head(206, b - a + 1, (a, b))
            else:
                a, b = 0, src.size - 1
                self._head(200, src.size)
            off = a
            while off <= b:
                n = min(4 << 20, b - off + 1)
                self.wfile.write(src.read(off, n)); off += n
    return H


class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def get_key(url, member):
    """The AEA key via Apple's public key service (ipsw fw aea --key), from the header only."""
    r = Remote(url, member)
    h = r.read(0, 12)
    alen = struct.unpack_from('<I', h, 8)[0]
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'head.dmg.aea')
        with open(p, 'wb') as f:
            f.write(r.read(0, 12 + alen + 4096))
        out = subprocess.run([IPSW_TOOL, 'fw', 'aea', '--key', p], capture_output=True, text=True).stdout
    m = re.search(r'base64:[A-Za-z0-9+/=]+', out)
    assert m, f'no key from ipsw: {out!r}'
    return m.group(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['serve', 'serve-file', 'key'])
    ap.add_argument('args', nargs='+')
    ap.add_argument('--port', type=int, default=8771)
    ap.add_argument('--key')
    ap.add_argument('--aea-file', action='store_true', help='serve-file: the file is AEA (needs --key)')
    ap.add_argument('--plain', action='store_true', help='serve: the member is not AEA (a UDIF dmg, say)')
    ap.add_argument('--cache-mib', type=int, default=256, help='decrypted segments kept in RAM (1 MiB each)')
    a = ap.parse_args()
    if a.cmd == 'key':
        print(get_key(*a.args)); return
    if a.cmd == 'serve-file':
        src = Local(a.args[0])
        if a.aea_file:
            src = AEA(src, decode_key(a.key))
    else:
        url, member = a.args
        rem = Remote(url, member)
        if a.plain:
            src = rem
            print(f'plain member: {rem.size:,} bytes', flush=True)
        else:
            key = a.key or get_key(url, member)
            src = AEA(rem, decode_key(key))
            src.cache_max = a.cache_mib
            print(f'AEA: {src.raw_size:,} bytes plain, {len(src.segs)} segments of {src.seg_size}, '
              f'{src.clusters} clusters, compression {chr(src.comp)!r}, checksum {src.cks}', flush=True)
    srv = TS(('127.0.0.1', a.port), make_handler(src))
    print(f'serving http://127.0.0.1:{a.port}/image.dmg ({src.size:,} bytes)', flush=True)
    if a.cmd == 'serve':
        def stats():
            while True:
                time.sleep(60)
                print(f'fetched {rem.fetched / 2**20:,.0f} MiB from the CDN', flush=True)
        threading.Thread(target=stats, daemon=True).start()
    srv.serve_forever()


def decode_key(k):
    if k.startswith('base64:'):
        return base64.b64decode(k[7:])
    if k.startswith('hex:'):
        return bytes.fromhex(k[4:])
    return base64.b64decode(k)


if __name__ == '__main__':
    main()
