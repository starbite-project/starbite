#!/usr/bin/env python3
"""Turn an Apple arm64e kext binary into a plain-arm64 one for a Starbite KC (plan H5).

  kext-arm64.py census FILE ...            counts only (PAC hint / non-hint, LSE, LDAPR, LRCPC2)
  kext-arm64.py convert IN OUT             thin the arm64e slice, apply the uniform rewrite
                                           (tools/a64e/a64e-rewrite.py rewrite_words), then mark
                                           the header CPU_SUBTYPE_ARM64_ALL (caps cleared)

The chained fixups are left as they are (arm64e kexts use the 64-bit kernel-cache pointer
format with auth bits; kmutil re-emits them and a non-ptrauth kernel rebases them unsigned).
Apple binaries are read from the KDK and written under build/ only (never into git)."""
import importlib.util, json, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'a64e', os.path.join(HERE, '..', 'a64e', 'a64e-rewrite.py'))
a64e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a64e)

PAC_HINTS = {7, 8, 10, 12, 14, 24, 25, 26, 27, 28, 29, 30, 31}   # XPACLRI, *1716, *Z, *SP


def arm64e_slice(buf):
    magic = struct.unpack_from('>I', buf)[0]
    if magic in (0xcafebabe, 0xcafebabf):
        n = struct.unpack_from('>I', buf, 4)[0]
        for i in range(n):
            if magic == 0xcafebabe:
                ct, cs, off, size, _ = struct.unpack_from('>iiIII', buf, 8 + i * 20)
            else:
                ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', buf, 8 + i * 32)
            if ct == 0x0100000c and (cs & 0xff) == 2:
                return bytearray(buf[off:off + size])
        raise SystemExit('no arm64e slice')
    if struct.unpack_from('<I', buf)[0] == 0xfeedfacf:
        return bytearray(buf)
    raise SystemExit('not a Mach-O')


def census_words(w):
    c = {}
    hint = (w & 0xFFFFF01F) == 0xD503201F
    imm = (w >> 5) & 0x7F
    c['pac_hint'] = int((hint & np.isin(imm, list(PAC_HINTS))).sum())
    d = a64e.rewrite_words(w.copy())
    c['pac_nonhint'] = d['ret'] + d['br'] + d['blr'] + d['pac'] + d['ldra'] + d['ldra_left'] + d['pacga_left']
    c['pac_nonhint_left_after_rewrite'] = d['ldra_left'] + d['pacga_left']
    ldapr = (w & 0x3FFFFC00) == 0x38BFC000
    amo = ((w & 0x3F200C00) == 0x38200000) & ~ldapr
    cas = (w & 0x3FA07C00) == 0x08A07C00
    casp = (w & 0xBFA07C00) == 0x08207C00
    c['lse'] = int(amo.sum() + cas.sum() + casp.sum())
    c['ldapr'] = int(ldapr.sum())
    c['lrcpc2_ldapur_stlur'] = int(((w & 0x3F200C00) == 0x19000000).sum())
    c['fjcvtzs'] = d['fjcvtzs_left']
    return c


def text_words(m):
    ncmds, sz = struct.unpack_from('<II', m, 16)
    lc = bytes(m[32:32 + sz])
    for _, size, foff in a64e.text_sections(lc, ncmds):
        yield foff, size & ~3


def census(m):
    tot = {}
    for foff, size in text_words(m):
        a64e.add(tot, census_words(np.frombuffer(bytes(m[foff:foff + size]), dtype=np.uint32)))
    return tot


def convert(src, dst):
    m = arm64e_slice(open(src, 'rb').read())
    before = census(m)
    done = {}
    for foff, size in text_words(m):
        w = np.frombuffer(bytes(m[foff:foff + size]), dtype=np.uint32).copy()
        a64e.add(done, a64e.rewrite_words(w))
        m[foff:foff + size] = w.tobytes()
    struct.pack_into('<i', m, 8, 0)          # CPU_SUBTYPE_ARM64_ALL, no PTR_AUTH caps
    after = census(m)
    with open(dst, 'wb') as f:
        f.write(m)
    os.chmod(dst, 0o755)
    return {'before': before, 'rewritten': done, 'after': after}


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'census':
        for p in sys.argv[2:]:
            print(p, json.dumps(census(arm64e_slice(open(p, 'rb').read()))))
    elif len(sys.argv) == 4 and sys.argv[1] == 'convert':
        print(json.dumps(convert(sys.argv[2], sys.argv[3])))
    else:
        sys.exit(__doc__)
