#!/usr/bin/env python3
"""Instruction census for loose Mach-O files (a mounted root) or a dyld shared cache:
how many sites of each instruction class the S9's cores cannot run natively.

  macho-isa-census.py root DIR [--json OUT] [--label NAME] [--jobs 8] [--skip-ext]
  macho-isa-census.py dsc  DIR/dyld_shared_cache_arm64e [--json OUT] [--label NAME]
  macho-isa-census.py file FILE ...                     (quick look, prints per file)

S9 cores: Cortex-A55 = ARMv8.2 + LSE + RCpc (LDAPR) + DotProd + FP16 + RDM, no PAC;
Exynos M3 (Mongoose) = ARMv8.0, no LSE, no RCpc, no PAC.

Root mode walks DIR without following symlinks, takes every regular file that is a
thin or fat Mach-O, and from it the arm64e slice (else the arm64 slice, reported
apart: such binaries have no PAC). Only sections with S_ATTR_PURE_INSTRUCTIONS, or
S_ATTR_SOME_INSTRUCTIONS in an executable segment, are scanned, so __cstring and
__const data in __TEXT do not count; literal pools and jump tables inside __text
can still give a few false hits, so every count is an upper bound. Classes are
tried in order and the first match wins (specific encodings before broad ones).
Encodings were checked against llvm-mc output (armv9.5-a+pauth-lr+mops+cssc).

dsc mode reads the same classes from every image of a dyld shared cache (the
mapping logic of tools/dsc-isa-census.py).

The class table extends tools/dsc-isa-census.py with: arm64e.x1 / FEAT_PAuth_LR
forms (RETA*SPPC, AUT*SPPC, PAC*SPPC, *171615, ADDPT/SUBPT/MADDPT/MSUBPT), LDRA
with and without writeback apart, XPACI/XPACD apart, the XPACLRI and PACM hints,
and MRS Xt, CNTFRQ_EL0."""
import argparse, collections, glob, json, os, stat, struct, sys, time
from concurrent.futures import ThreadPoolExecutor
import numpy as np

# (name, mask, value, group, lacks, handled-by)
# group: what the H3 table rolls up to.  lacks: which S9 core cannot run it.
PATS = [
 # --- PAC, not in hint space: must be rewritten (or trapped)
 ('RETAA/RETAB',            0xFFFFFBFF, 0xD65F0BFF, 'pac',   'both', 'rewrite'),
 ('RETAASPPCR/RETABSPPCR x1',0xFFFFFBE0, 0xD65F0BE0, 'pac-x1', 'both', 'none (x1)'),
 ('ERETAA/ERETAB',          0xFFFFFBFF, 0xD69F0BFF, 'pac',   'both', 'none (EL1 only)'),
 ('BRA*/BLRA* (+Z)',        0xFE9FF800, 0xD61F0800, 'pac',   'both', 'rewrite'),
 ('LDRAA/LDRAB',            0xFF200C00, 0xF8200400, 'pac',   'both', 'rewrite'),
 ('LDRAA/LDRAB writeback',  0xFF200C00, 0xF8200C00, 'pac',   'both', 'trap'),
 ('PAC*/AUT* register',     0xFFFFC000, 0xDAC10000, 'pac',   'both', 'rewrite (-> NOP)'),
 ('XPACI/XPACD',            0xFFFFFBE0, 0xDAC143E0, 'pac',   'both', 'rewrite (-> NOP)'),
 ('PACGA',                  0xFFE0FC00, 0x9AC03000, 'pac',   'both', 'trap'),
 ('PAuth_LR DAC1 x1 (PAC*SPPC, AUT*SPPCR, *171615)', 0xFFFF8000, 0xDAC18000, 'pac-x1', 'both', 'none (x1)'),
 ('AUTIASPPC/AUTIBSPPC x1', 0xFFC0001F, 0xF380001F, 'pac-x1', 'both', 'none (x1)'),
 ('RETAASPPC/RETABSPPC x1', 0xFFC0001F, 0x5500001F, 'pac-x1', 'both', 'none (x1)'),
 ('ADDPT/SUBPT x1',         0xBFE0E000, 0x9A002000, 'pac-x1', 'both', 'none (x1)'),
 ('MADDPT/MSUBPT x1',       0xFFE00000, 0x9B600000, 'pac-x1', 'both', 'none (x1)'),
 # --- PAC hint space: NOP on both cores
 ('hint PAC (PACIBSP, AUTIBSP, ...)', 0xFFFFFD1F, 0xD503211F, 'pac-hint', 'hint', 'nop'),
 ('hint XPACLRI',           0xFFFFFFFF, 0xD50320FF, 'pac-hint', 'hint', 'nop'),
 ('hint PACM x1',           0xFFFFFFFF, 0xD50324FF, 'pac-hint', 'hint', 'nop'),
 # --- the classes H3 asks for
 ('LDAPR (RCpc)',           0x3FFFFC00, 0x38BFC000, 'ldapr', 'M3',   'rewrite (-> LDAR)'),
 ('LDAPUR/STLUR (RCpc2)',   0x3F000C00, 0x19000000, 'ldapr', 'both', 'trap (0350)'),
 ('MOPS CPY*/SET*',         0x3B200C00, 0x19000400, 'mops',  'both', 'none'),
 ('LSE CAS',                0x3FA07C00, 0x08A07C00, 'lse',   'M3',   'trap (M3)'),
 ('LSE CASP',               0xBFA07C00, 0x08207C00, 'lse',   'M3',   'trap (M3)'),
 ('LSE LD<op>/ST<op>/SWP',  0x3F200C00, 0x38200000, 'lse',   'M3',   'trap (M3)'),
 ('FCMLA vector',           0xBF20E400, 0x2E00C400, 'fcma',  'both', 'none'),
 ('FCMLA by element',       0xBF009400, 0x2F001000, 'fcma',  'both', 'none'),
 ('FCADD',                  0xBF20EC00, 0x2E00E400, 'fcma',  'both', 'trap'),
 ('CSSC CTZ/CNT',           0x7FFFF800, 0x5AC01800, 'cssc',  'both', 'none'),
 ('CSSC ABS',               0x7FFFFC00, 0x5AC02000, 'cssc',  'both', 'none'),
 ('CSSC SMAX/UMAX/SMIN/UMIN reg', 0x7FE0F000, 0x1AC06000, 'cssc', 'both', 'none'),
 ('CSSC SMAX/UMAX/SMIN/UMIN imm', 0x7FC00000, 0x11C00000, 'cssc', 'both', 'none'),
 ('MRS Xt, CNTFRQ_EL0',     0xFFFFFFE0, 0xD53BE000, 'cntfrq', 'n/a', 'reads the timer frequency'),
 # --- the rest of tools/dsc-isa-census.py's table, for completeness
 ('FJCVTZS (JSCVT)',        0xFFFFFC00, 0x1E7E0000, 'other', 'both', 'trap'),
 ('DotProd',                0xBFE0FC00, 0x0E809400, 'other', 'M3',   'none (M3)'),
 ('DotProd U',              0xBFE0FC00, 0x2E809400, 'other', 'M3',   'none (M3)'),
 ('DotProd idx',            0x9FC0F400, 0x0F80E000, 'other', 'M3',   'none (M3)'),
 ('SQRDMLAH/SH (RDM)',      0xBF20F400, 0x2E008400, 'other', 'M3',   'none (M3)'),
 ('SB',                     0xFFFFFFFF, 0xD50330FF, 'other', 'both', 'trap (0352)'),
 ('CFINV (FlagM)',          0xFFFFFFFF, 0xD500401F, 'other', 'both', 'none'),
 ('AXFLAG/XAFLAG',          0xFFFFFFDF, 0xD500405F, 'other', 'both', 'none'),
 ('RMIF (FlagM)',           0xFFE07C10, 0xBA000400, 'other', 'both', 'none'),
 ('SETF8/16 (FlagM)',       0xFFFFBC1F, 0x3A00080D, 'other', 'both', 'none'),
 ('MSR DIT imm',            0xFFFFF0FF, 0xD503405F, 'other', 'both', 'trap (0350)'),
 ('MSR SSBS imm',           0xFFFFF0FF, 0xD503403F, 'other', 'both', 'none'),
 ('SMSTART/SMSTOP (SME)',   0xFFFFF0FF, 0xD503407F, 'other', 'both', 'none'),
 ('MRS/MSR DIT reg',        0xFFDFFFE0, 0xD51B42A0, 'other', 'both', 'trap (0350)'),
 ('MRS/MSR SSBS reg',       0xFFDFFFE0, 0xD51B42C0, 'other', 'both', 'none'),
 ('MRS/MSR SVCR (SME)',     0xFFDFFFE0, 0xD51B4240, 'other', 'both', 'none'),
 ('MRS/MSR TPIDR2 (SME)',   0xFFDFFFE0, 0xD51BD0A0, 'other', 'both', 'none'),
 ('MRS CNTxCTSS v8.6',      0xFFFFFFC0, 0xD53BE080, 'other', 'both', 'none'),
 ('MRS/MSR S3_x_C15',       0xFFD8F000, 0xD518F000, 'other', 'both', 'none (IMPDEF)'),
 ('DC CVADP (DPB2)',        0xFFFFFFE0, 0xD50B7D20, 'other', 'both', 'none'),
 ('WFET/WFIT',              0xFFFFFFC0, 0xD5031000, 'other', 'both', 'none'),
 ('FRINT32/64 scalar',      0xFF3E7C00, 0x1E284000, 'other', 'both', 'none'),
 ('FRINT32/64 vector',      0x9FBFEC00, 0x0E21E800, 'other', 'both', 'none'),
 ('BFCVT',                  0xFFFFFC00, 0x1E634000, 'other', 'both', 'none'),
 ('BFCVTN',                 0xBFFFFC00, 0x0EA16800, 'other', 'both', 'none'),
 ('BFDOT/BFMLAL',           0xBF20FC00, 0x2E00FC00, 'other', 'both', 'none'),
 ('BFDOT/BFMLAL idx',       0xBF00F400, 0x0F00F000, 'other', 'both', 'none'),
 ('BFMMLA',                 0xFFE0FC00, 0x6E40EC00, 'other', 'both', 'none'),
 ('SMMLA/UMMLA/USMMLA',     0xDFE0F400, 0x4E80A400, 'other', 'both', 'none'),
 ('USDOT',                  0xBFE0FC00, 0x0E809C00, 'other', 'both', 'none'),
 ('EOR3 (SHA3)',            0xFFE08000, 0xCE000000, 'other', 'both', 'trap (0352)'),
 ('BCAX (SHA3)',            0xFFE08000, 0xCE200000, 'other', 'both', 'trap (0352)'),
 ('RAX1 (SHA3)',            0xFFE0FC00, 0xCE608C00, 'other', 'both', 'trap (0352)'),
 ('XAR (SHA3)',             0xFFE00000, 0xCE800000, 'other', 'both', 'trap (0352)'),
 ('SHA512H/H2/SU1',         0xFFE0F000, 0xCE608000, 'other', 'both', 'trap (0352)'),
 ('SHA512SU0',              0xFFFFFC00, 0xCEC08000, 'other', 'both', 'trap (0352)'),
 ('FMLAL/FMLSL vector',     0xBF60FC00, 0x0E20EC00, 'other', 'both', 'trap (0351)'),
 ('FMLAL2/FMLSL2 vector',   0xBF60FC00, 0x2E20CC00, 'other', 'both', 'trap (0351)'),
 ('FMLAL/FMLSL elem',       0xBFC0B400, 0x0F800000, 'other', 'both', 'trap (0351)'),
 ('FMLAL2/FMLSL2 elem',     0xBFC0B400, 0x2F808000, 'other', 'both', 'trap (0351)'),
 ('SVE space',              0x1E000000, 0x04000000, 'other', 'both', 'none'),
 ('SME space',              0x9E000000, 0x80000000, 'other', 'both', 'none'),
 ('AMX (Apple)',            0xFFFFFC00, 0x00201000, 'other', 'both', 'none (Apple only)'),
 ('FP16 scalar (+FCVT H)',  0xFFE00000, 0x1EE00000, 'other', 'M3',   'none (M3)'),
 ('hint BTI',               0xFFFFFF3F, 0xD503241F, 'other', 'hint', 'nop'),
]
NAMES = [p[0] for p in PATS]
MASK = np.array([p[1] for p in PATS], dtype=np.uint32)
VAL = np.array([p[2] for p in PATS], dtype=np.uint32)
GROUPS = ['pac', 'pac-x1', 'pac-hint', 'ldapr', 'lse', 'fcma', 'mops', 'cssc', 'cntfrq']
GIDX = {g: [i for i, p in enumerate(PATS) if p[3] == g] for g in GROUPS}
# per top byte, the patterns that can match (in table order: first match wins)
BYTOP = [[i for i, p in enumerate(PATS) if (b & (p[1] >> 24)) == ((p[2] >> 24) & (p[1] >> 24))]
         for b in range(256)]


def classify(w):
    """w: uint32 array -> int64 counts per PATS entry."""
    cnt = np.zeros(len(PATS), dtype=np.int64)
    if w.size == 0:
        return cnt
    top = (w >> 24).astype(np.uint8)
    order = np.argsort(top, kind='stable')
    ws = w[order]
    nb = np.bincount(top, minlength=256)
    ends = np.cumsum(nb); starts = ends - nb
    for b in np.nonzero(nb)[0]:
        pats = BYTOP[b]
        if not pats:
            continue
        seg = ws[starts[b]:ends[b]]
        left = np.ones(seg.size, bool)
        for i in pats:
            hit = left & ((seg & MASK[i]) == VAL[i])
            c = int(hit.sum())
            if c:
                cnt[i] += c; left &= ~hit
    return cnt


# ---------------------------------------------------------------- Mach-O files
CPU_ARM64 = 0x0100000c
FT = {1: 'object', 2: 'execute', 6: 'dylib', 7: 'dylinker', 8: 'bundle', 9: 'dylib_stub',
      0xa: 'dsym', 0xb: 'kext', 0xc: 'fileset'}


def exec_sections(lc, ncmds):
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            seg = lc[off + 8:off + 24].split(b'\0')[0].decode(errors='replace')
            initprot = struct.unpack_from('<i', lc, off + 60)[0]
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                sect = lc[so:so + 16].split(b'\0')[0].decode(errors='replace')
                size, foff = struct.unpack_from('<QI', lc, so + 40)
                flags = struct.unpack_from('<I', lc, so + 64)[0]
                if (flags & 0xff) in (1, 0xc, 0x12):          # zerofill kinds
                    continue
                if size and (flags & 0x80000000 or (flags & 0x400 and initprot & 4)):
                    yield seg, sect, size, foff
        if csz == 0:
            break
        off += csz


def macho_slice(f, head):
    """-> (offset, cpusubtype, kind) of the slice to scan, or None."""
    magic_be = struct.unpack_from('>I', head)[0]
    if magic_be in (0xcafebabe, 0xcafebabf):
        n = struct.unpack_from('>I', head, 4)[0]
        if not 0 < n < 32:
            return None                    # a Java class file, not fat Mach-O
        arches = []
        for i in range(n):
            if magic_be == 0xcafebabe:
                ct, cs, off, size, _ = struct.unpack_from('>iiIII', head, 8 + i * 20)
            else:
                ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', head, 8 + i * 32)
            arches.append((ct, cs & 0xffffffff, off))
        e = [a for a in arches if a[0] == CPU_ARM64 and (a[1] & 0xff) == 2]
        if e:
            return e[0][2], e[0][1], 'arm64e'
        p = [a for a in arches if a[0] == CPU_ARM64]
        if p:
            return p[0][2], p[0][1], 'arm64'
        return None
    if struct.unpack_from('<I', head)[0] == 0xfeedfacf:
        ct, cs = struct.unpack_from('<iI', head, 4)
        if ct == CPU_ARM64:
            return 0, cs, 'arm64e' if (cs & 0xff) == 2 else 'arm64'
    return None


def scan_file(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
            if len(head) < 32:
                return None
            sl = macho_slice(f, head)
            if sl is None:
                return None
            base, cs, kind = sl
            f.seek(base)
            mh = f.read(32)
            magic, ct, cs2, ftype, ncmds, sz = struct.unpack_from('<IiIIII', mh)
            if magic != 0xfeedfacf:
                return None
            lc = f.read(sz)
            cnt = np.zeros(len(PATS), dtype=np.int64); n = 0; secs = []
            for seg, sect, size, foff in exec_sections(lc, ncmds):
                f.seek(base + foff)
                b = f.read(size & ~3)
                w = np.frombuffer(b[:len(b) & ~3], dtype='<u4')
                cnt += classify(w); n += w.size
                secs.append(f'{seg},{sect}')
            return dict(path=path, kind=kind, cpusubtype=cs2, filetype=FT.get(ftype, hex(ftype)),
                        words=int(n), counts=cnt, sections=sorted(set(secs)))
    except (OSError, struct.error, ValueError) as e:
        return dict(path=path, error=str(e))


SKIP_EXT = {'.png', '.car', '.plist', '.strings', '.stringsdict', '.nib', '.loctable', '.json', '.ttf',
            '.ttc', '.otf', '.jpg', '.jpeg', '.heic', '.pdf', '.caf', '.wav', '.m4a', '.mp3', '.mp4',
            '.mov', '.aiff', '.aif', '.html', '.css', '.js', '.txt', '.xml', '.svg', '.gif', '.tiff',
            '.der', '.pem', '.cer', '.db', '.sqlite', '.dat', '.mlmodelc', '.mil', '.metallib',
            '.lproj', '.mom', '.omo', '.momd', '.sb', '.ucas', '.astc', '.ktx', '.atx', '.mlmodel',
            '.espresso', '.weights', '.shape', '.net', '.csv', '.icns', '.ico', '.bom', '.tbd', '.h',
            '.modulemap', '.swiftinterface', '.swiftmodule', '.swiftdoc', '.abi', '.md', '.rtf', '.webp',
            '.dict', '.trie', '.lm', '.fst', '.mar', '.otf2', '.zip', '.gz', '.pb', '.proto', '.lst',
            '.storyboardc', '.momc', '.hyb', '.ane', '.plb', '.sinf', '.supf', '.supp', '.mpkg'}


def walk(root, skip_ext):
    for dp, dns, fns in os.walk(root, followlinks=False):
        for fn in fns:
            p = os.path.join(dp, fn)
            if skip_ext and os.path.splitext(fn)[1].lower() in SKIP_EXT:
                continue
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if stat.S_ISREG(st.st_mode) and st.st_size >= 4096:
                yield p, st.st_size


# ---------------------------------------------------------------- dyld cache
def scan_dsc(main):
    maps = []
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
    fhs = {}

    def read(addr, n):
        for a, s, p, o in maps:
            if a <= addr < a + s:
                fh = fhs.get(p) or fhs.setdefault(p, open(p, 'rb'))
                fh.seek(o + addr - a)
                return fh.read(n)
        return None
    h = open(main, 'rb').read(0x200)
    io, ic = struct.unpack_from('<II', h, 0x1c0)
    if not ic:
        io, ic = struct.unpack_from('<II', h, 0x18)
    with open(main, 'rb') as f:
        f.seek(io); imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
        paths = []
        for im in imgs:
            f.seek(im[3]); paths.append(f.read(256).split(b'\0')[0].decode())
    for im, path in zip(imgs, paths):
        mh = read(im[0], 32)
        if not mh or struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
            continue
        ncmds, sz = struct.unpack_from('<II', mh, 16)
        cs = struct.unpack_from('<I', mh, 8)[0]
        lc = read(im[0] + 32, sz)
        cnt = np.zeros(len(PATS), dtype=np.int64); n = 0
        off = 0
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from('<II', lc, off)
            if cmd == 0x19:
                nsect = struct.unpack_from('<I', lc, off + 64)[0]
                for s in range(nsect):
                    so = off + 72 + s * 80
                    addr, size = struct.unpack_from('<QQ', lc, so + 32)
                    flags = struct.unpack_from('<I', lc, so + 64)[0]
                    if flags & 0x80000000 and size:
                        b = read(addr, size)
                        if not b:
                            continue
                        w = np.frombuffer(b[:len(b) & ~3], dtype='<u4')
                        cnt += classify(w); n += w.size
            off += csz
        yield dict(path=path, kind='arm64e', cpusubtype=cs, filetype='dsc image', words=int(n), counts=cnt)


# ---------------------------------------------------------------- report
def summarize(results, label):
    tot = np.zeros(len(PATS), dtype=np.int64); words = 0
    imgs = np.zeros(len(PATS), dtype=np.int64)
    for r in results:
        tot += r['counts']; words += r['words']; imgs += r['counts'] > 0
    print(f'== {label}: {len(results)} images, {words:,} instructions ({words * 4 / 2**20:,.0f} MiB of code)')
    print(f'  {"class":48s} {"sites":>12s} {"per 1000":>9s} {"images":>7s}  lacks  handled by')
    for i, p in enumerate(PATS):
        if tot[i] or p[3] != 'other':
            print(f'  {p[0]:48s} {tot[i]:12,d} {tot[i] / max(words, 1) * 1e3:9.4f} {imgs[i]:7d}  {p[4]:5s}  {p[5]}')
    return tot, words, imgs


def to_json(results):
    out = []
    for r in results:
        r = dict(r)
        if 'counts' in r:
            r['counts'] = {NAMES[i]: int(c) for i, c in enumerate(r['counts']) if c}
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['root', 'dsc', 'file'])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--json')
    ap.add_argument('--label', default='')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--skip-ext', action='store_true',
                    help='root: do not open files whose extension is never Mach-O (png, car, plist, ...)')
    a = ap.parse_args()
    t0 = time.time()
    results, errors = [], []
    if a.mode == 'file':
        for p in a.paths:
            r = scan_file(p)
            if r and 'counts' in r:
                print(p, r['kind'], r['filetype'], r['words'],
                      {NAMES[i]: int(c) for i, c in enumerate(r['counts']) if c})
            else:
                print(p, r)
        return
    if a.mode == 'dsc':
        results = list(scan_dsc(a.paths[0]))
    else:
        files = []
        for root in a.paths:
            files += list(walk(root, a.skip_ext))
        print(f'{len(files):,} candidate files ({sum(s for _, s in files) / 2**30:.2f} GiB)', flush=True)
        done = 0
        with ThreadPoolExecutor(a.jobs) as ex:
            for r in ex.map(scan_file, [p for p, _ in files]):
                done += 1
                if done % 500 == 0:
                    print(f'  {done:,}/{len(files):,} files, {len(results):,} Mach-O, '
                          f'{time.time() - t0:.0f} s', flush=True)
                if r is None:
                    continue
                if 'error' in r:
                    errors.append(r); continue
                results.append(r)
    summarize(results, a.label or a.paths[0])
    print(f'{len(errors)} unreadable files; {time.time() - t0:.0f} s')
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(dict(label=a.label, mode=a.mode, paths=a.paths, classes=[list(p) for p in PATS],
                           results=to_json(results), errors=errors), f)


if __name__ == '__main__':
    main()
