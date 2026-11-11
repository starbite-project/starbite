#!/usr/bin/env python3
"""Rewrite arm64e code in place so cores without ARMv8.3 (the S9) run it natively.

One instruction for one, in S_ATTR_PURE_INSTRUCTIONS sections only:
  RETAA/RETAB                 -> RET
  BRAA/BRAB/BRAAZ/BRABZ Xn    -> BR Xn
  BLRAA/BLRAB/BLRAAZ/BLRABZ Xn-> BLR Xn
  PAC*/AUT*/XPAC* (register)  -> NOP   (pointers are never signed here)
  LDRAA/LDRAB, no writeback   -> LDR (offset 0..32760) or LDUR (-256..-8)
  LDAPR{B,H,,} (LRCPC)        -> LDAR{B,H,,}   (stronger ordering; the Mongoose M3 lacks LRCPC)
Left for the kernel (sweet_a64e.c) to emulate on trap: PACGA, FJCVTZS, FCADD,
LDRA* with writeback or out-of-range offsets.

  a64e-rewrite.py cache DIR/dyld_shared_cache_arm64e [--only SUBSTR ...] [--dry-run]
  a64e-rewrite.py macho FILE ... [--dry-run]       (thin or fat; arm64e slices)
  a64e-rewrite.py root DIR [--skip SUBSTR ...] [--log FILE] [--dry-run]
      every Mach-O under DIR (no symlinks followed); each rewritten slice is re-hashed in place
      and made ad hoc (resign_slice: entitlements, identifier, flags and size kept)
Files are modified in place (clone them first: cp -c)."""
import argparse, glob, hashlib, os, stat, struct, sys
import numpy as np

NOP = 0xD503201F
RET = 0xD65F03C0


def rewrite_words(w):
    """w: uint32 array (writable view). Returns a dict of counts."""
    c = {}
    m = (w & 0xFFFFFBFF) == 0xD65F0BFF                       # RETAA/RETAB
    w[m] = RET; c['ret'] = int(m.sum())
    m = (w & 0xFE9FF800) == 0xD61F0800                       # BRA*/BLRA* (+ RETA* already gone)
    op = (w >> 21) & 3
    rn = (w >> 5) & 31
    br = m & (op == 0); blr = m & (op == 1)
    w[br] = (0xD61F0000 | (rn[br] << 5)).astype(np.uint32)
    w[blr] = (0xD63F0000 | (rn[blr] << 5)).astype(np.uint32)
    c['br'] = int(br.sum()); c['blr'] = int(blr.sum())
    m = ((w & 0xFFFF8000) == 0xDAC10000) & (((w >> 10) & 0x3f) <= 17)   # PAC/AUT/XPAC reg forms
    w[m] = NOP; c['pac'] = int(m.sum())
    m = (w & 0xFF200400) == 0xF8200400                       # LDRAA/LDRAB
    wb = (w >> 11) & 1
    imm = (((w >> 12) & 0x1ff) | (((w >> 22) & 1) << 9)).astype(np.int64)
    imm = np.where(imm & 0x200, imm - 0x400, imm) * 8
    rt = w & 31; rn = (w >> 5) & 31
    pos = m & (wb == 0) & (imm >= 0)
    neg = m & (wb == 0) & (imm < 0) & (imm >= -256)
    w[pos] = (0xF9400000 | ((imm[pos] // 8).astype(np.uint32) << 10) | (rn[pos] << 5) | rt[pos]).astype(np.uint32)
    w[neg] = (0xF8400000 | ((imm[neg] & 0x1ff).astype(np.uint32) << 12) | (rn[neg] << 5) | rt[neg]).astype(np.uint32)
    c['ldra'] = int(pos.sum() + neg.sum()); c['ldra_left'] = int(m.sum() - pos.sum() - neg.sum())
    m = (w & 0x3FFFFC00) == 0x38BFC000                       # LDAPR{B,H,,} Rt, [Xn]
    w[m] = ((w[m] & 0xC00003FF) | 0x08DFFC00).astype(np.uint32)
    c['ldapr'] = int(m.sum())
    c['pacga_left'] = int(((w & 0xFFE0FC00) == 0x9AC03000).sum())
    c['fjcvtzs_left'] = int(((w & 0xFFFFFC00) == 0x1E7E0000).sum())
    c['fcadd_left'] = int(((w & 0xBF20EC00) == 0x2E00E400).sum())
    return c


def add(t, c):
    for k, v in c.items():
        t[k] = t.get(k, 0) + v


def text_sections(lc, ncmds):
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                addr, size, foff = struct.unpack_from('<QQI', lc, so + 32)
                flags = struct.unpack_from('<I', lc, so + 64)[0]
                if flags & 0x80000000 and size:
                    yield addr, size, foff
        off += csz


def do_cache(main, only, dry, skip=()):
    maps = []
    files = {}
    for p in sorted(glob.glob(main + '*')):
        if p.endswith(('.map', '.atlas')):
            continue
        with open(p, 'rb') as f:
            h = f.read(0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        mo, mc = struct.unpack_from('<II', h, 0x10)
        with open(p, 'rb') as f:
            f.seek(mo)
            for _ in range(mc):
                a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
                maps.append((a, s, p, o))
        files[p] = np.memmap(p, dtype=np.uint8, mode='r' if dry else 'r+')

    def loc(addr):
        for a, s, p, o in maps:
            if a <= addr < a + s:
                return p, o + addr - a
        return None, None

    def read(addr, n):
        p, o = loc(addr)
        return None if p is None else bytes(files[p][o:o + n])

    h = open(main, 'rb').read(0x200)
    io, ic = struct.unpack_from('<II', h, 0x1c0)
    if not ic:
        io, ic = struct.unpack_from('<II', h, 0x18)
    with open(main, 'rb') as f:
        f.seek(io)
        imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
        paths = []
        for im in imgs:
            f.seek(im[3]); paths.append(f.read(256).split(b'\0')[0].decode())
    tot, nimg, dirty = {}, 0, 0
    for im, path in zip(imgs, paths):
        if only and not any(o in path for o in only):
            continue
        if any(k in path for k in skip):
            continue
        mh = read(im[0], 32)
        if not mh or struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
            continue
        ncmds, sz = struct.unpack_from('<II', mh, 16)
        lc = read(im[0] + 32, sz)
        for addr, size, _ in text_sections(lc, ncmds):
            p, o = loc(addr)
            if p is None:
                continue
            mm = files[p]
            buf = np.array(mm[o:o + (size & ~3)]).view(np.uint32) if dry else mm[o:o + (size & ~3)].view(np.uint32)
            add(tot, rewrite_words(buf))
            dirty += size
        nimg += 1
        if not dry and dirty > 256 << 20:   # write back as it goes: a slow (USB) image would pile up dirty pages
            for mm in files.values():
                mm.flush()
            dirty = 0
    if not dry:
        for mm in files.values():
            mm.flush()
    return nimg, tot


REWRITES = ('ret', 'br', 'blr', 'pac', 'ldra', 'ldapr')
HASHES = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256, 4: hashlib.sha384}


def resign_slice(data, base, lc, ncmds, spans):
    """Re-hash, in place, the code slots of every CodeDirectory of the slice at `base` whose
    pages overlap `spans` (slice-relative (start, end) of the rewritten sections), then make
    the signature ad hoc: CS_ADHOC in each CD's flags, the CMS blob emptied to its 8-byte
    header. Identifier, entitlements (XML + DER), requirements, launch constraints, team,
    runtime and exec-segment fields are kept as Apple wrote them; the file size does not
    change. Returns 'resigned', 'unsigned' or a reason it was left alone."""
    cs = None
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x1d:                                     # LC_CODE_SIGNATURE
            cs = struct.unpack_from('<II', lc, off + 8)
        off += csz
    if not cs:
        return 'unsigned'
    sb = base + cs[0]
    magic, _, count = struct.unpack('>III', bytes(data[sb:sb + 12]))
    if magic != 0xfade0cc0:
        return 'no superblob'
    idx = bytes(data[sb + 12:sb + 12 + 8 * count])
    cds, cms = [], None
    for i in range(count):
        typ, boff = struct.unpack_from('>II', idx, 8 * i)
        b = sb + boff
        bm, bl = struct.unpack('>II', bytes(data[b:b + 8]))
        if bm == 0xfade0c02:
            cds.append((b, bl))
        elif bm == 0xfade0b01 and typ == 0x10000:
            cms = b
    if not cds:
        return 'no CodeDirectory'
    for b, bl in cds:
        cd = bytes(data[b:b + bl])
        version, flags, hoff = struct.unpack_from('>III', cd, 8)
        ncode, limit = struct.unpack_from('>II', cd, 28)
        hsize, htype, pshift = cd[36], cd[37], cd[39]
        if version >= 0x20300:
            limit = struct.unpack_from('>Q', cd, 56)[0] or limit
        if htype not in HASHES:
            return f'hash type {htype}'
        ps = (1 << pshift) if pshift else limit
        slots = set()
        for s, e in spans:
            slots.update(range(s // ps, min((e + ps - 1) // ps, ncode)))
        for k in sorted(slots):
            h = HASHES[htype](bytes(data[base + k * ps:base + min((k + 1) * ps, limit)])).digest()[:hsize]
            o = b + hoff + k * hsize
            data[o:o + hsize] = np.frombuffer(h, np.uint8)
        data[b + 12:b + 16] = np.frombuffer(struct.pack('>I', flags | 0x2), np.uint8)   # CS_ADHOC
    if cms is not None:
        data[cms + 4:cms + 8] = np.frombuffer(struct.pack('>I', 8), np.uint8)
    return 'resigned'


def do_macho(path, dry, resign=False):
    data = np.memmap(path, dtype=np.uint8, mode='r' if dry else 'r+')
    raw = bytes(data[:4096])
    magic = struct.unpack_from('>I', raw)[0]
    slices = []
    if magic in (0xcafebabe, 0xcafebabf):
        n = struct.unpack_from('>I', raw, 4)[0]
        for i in range(n):
            if magic == 0xcafebabe:
                ct, cs, off, size, _ = struct.unpack_from('>iiIII', raw, 8 + i * 20)
            else:
                ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', raw, 8 + i * 32)
            if ct == 0x0100000c and (cs & 0xff) == 2:
                slices.append(off)
    elif struct.unpack_from('<I', raw)[0] == 0xfeedfacf:
        if struct.unpack_from('<i', raw, 4)[0] == 0x0100000c and (struct.unpack_from('<i', raw, 8)[0] & 0xff) == 2:
            slices.append(0)
    tot = {}
    for base in slices:
        hdr = bytes(data[base:base + 32])
        ncmds, sz = struct.unpack_from('<II', hdr, 16)
        lc = bytes(data[base + 32:base + 32 + sz])
        sl, spans = {}, []
        for _, size, foff in text_sections(lc, ncmds):
            o = base + foff
            buf = np.array(data[o:o + (size & ~3)]).view(np.uint32) if dry else data[o:o + (size & ~3)].view(np.uint32)
            c = rewrite_words(buf)
            add(sl, c)
            if any(c.get(k) for k in REWRITES):
                spans.append((foff, foff + size))
        add(tot, sl)
        if resign and spans and not dry:
            r = resign_slice(data, base, lc, ncmds, spans)
            tot[r] = tot.get(r, 0) + 1
    if not dry:
        data.flush()
    return len(slices), tot


def do_root(root, dry, skip=(), log=None):
    """Every regular file under ROOT (symlinks not followed) whose magic is a 64-bit Mach-O or
    a fat file: arm64e slices rewritten and re-signed ad hoc (resign_slice). Prints totals;
    with LOG, one line per changed file."""
    tot, nfile, nslice = {}, 0, 0
    out = open(log, 'w') if log else None
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for fn in sorted(files):
            p = os.path.join(d, fn)
            rel = p[len(root.rstrip('/')):]
            if any(k in rel for k in skip):
                continue
            try:
                st = os.lstat(p)
                if not stat.S_ISREG(st.st_mode) or st.st_size < 4096:
                    continue
                with open(p, 'rb') as f:
                    m = f.read(4)
            except OSError as e:
                print(f'{rel}: {e}', file=sys.stderr)
                continue
            if m not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
                continue
            # build roots carry Apple's modes (0555, 0444, setuid): writable for the rewrite,
            # then mode and times put back as they were
            mode = stat.S_IMODE(st.st_mode)
            if not dry and not mode & stat.S_IWUSR:
                try:
                    os.chmod(p, mode | stat.S_IWUSR | stat.S_IRUSR)
                except OSError as e:
                    print(f'{rel}: {e}', file=sys.stderr)
                    continue
            try:
                n, c = do_macho(p, dry, resign=True)
            except (OSError, ValueError, struct.error) as e:
                print(f'{rel}: {e}', file=sys.stderr)
                n, c = 0, {}
            if not dry:
                if stat.S_IMODE(os.lstat(p).st_mode) != mode:
                    os.chmod(p, mode)
                os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
            if n:
                nfile += 1; nslice += n
                add(tot, c)
                if out and any(c.get(k) for k in REWRITES):
                    out.write(f'{rel}\t' + ' '.join(f'{k}={v}' for k, v in sorted(c.items()) if v) + '\n')
    if out:
        out.close()
    return nfile, nslice, tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kind', choices=['cache', 'macho', 'root'])
    ap.add_argument('--log', help='root: write one line per rewritten file here')
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--skip', nargs='*', default=[],
                    help='leave images whose path contains one of these untranslated '
                         '(libcorecrypto: its FIPS self-test MACs its own code)')
    a = ap.parse_args()
    if a.kind == 'cache':
        n, tot = do_cache(a.paths[0], a.only, a.dry_run, a.skip)
        print(f'{n} images', tot)
    elif a.kind == 'root':
        nf, ns, tot = do_root(a.paths[0], a.dry_run, a.skip, a.log)
        print(f'{nf} Mach-O files, {ns} arm64e slices', tot)
    else:
        for p in a.paths:
            n, tot = do_macho(p, a.dry_run)
            print(f'{p}: {n} arm64e slice(s)', tot)


if __name__ == '__main__':
    main()
