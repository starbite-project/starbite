#!/usr/bin/env python3
"""verify-x86.py sbumd.dll [sbumd.map]: is this the 32-bit x86 user-mode driver the D3D runtime can load?

Checks, on the file alone (nothing is run): PE32 / i386 / DLL; relocatable (ASLR) and NX; the two functions the
runtime looks up, OpenAdapter10 and OpenAdapter10_2, exported under exactly those names (the runtime calls
GetProcAddress with the undecorated name; the functions are __stdcall, _OpenAdapter10@4 in the objects, so an
export that kept the decoration would not be found); no export with a decoration left; imports only from the
DLLs the 64-bit sbumd imports from (no vcruntime140 / msvcp140: the compiler runtime is linked in, the UCRT is
the system's api-ms-win-crt-* set, present in SysWOW64); a TLS directory (the shim's __declspec(thread) state);
a SafeSEH table. With the map: both exports point at the stdcall functions, and the two functions that have a
weak default (malikm_fd_dup, panfrost_sb_rotate_bos) are the real ones.
Exit 0 = every check passed.
"""
import re
import struct
import sys

ALLOWED = {'gdi32.dll', 'kernel32.dll', 'user32.dll', 'api-ms-win-core-synch-l1-2-0.dll'}
CRT = re.compile(r'^api-ms-win-crt-[a-z]+-l1-1-0\.dll$')


def main(a):
    d = open(a[0], 'rb').read()
    ok = True

    def check(what, good, detail=''):
        nonlocal ok
        ok &= bool(good)
        print(('ok    ' if good else 'FAIL  ') + what + (f': {detail}' if detail != '' else ''))

    pe = struct.unpack_from('<I', d, 0x3c)[0]
    machine, nsec, _, _, _, optsz, chars = struct.unpack_from('<HHIIIHH', d, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    check('PE32 (magic 0x10b), machine i386 (0x14c), a DLL', magic == 0x10b and machine == 0x14c and chars & 0x2000,
          f'magic 0x{magic:x} machine 0x{machine:x} characteristics 0x{chars:x}')
    dllchars = struct.unpack_from('<H', d, opt + 70)[0]
    check('DYNAMIC_BASE and NX_COMPAT', dllchars & 0x40 and dllchars & 0x100, f'0x{dllchars:x}')
    dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * i) for i in range(16)]
    secs = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, max(vsz, rsz), raw))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return rva - va + raw
        raise SystemExit(f'rva 0x{rva:x} is in no section')

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b'\0', o)].decode('latin-1')

    check('base relocations present', dirs[5][1] > 0 and not chars & 1, f'{dirs[5][1]} bytes')
    check('TLS directory present', dirs[9][1] > 0)
    # exports
    e = off(dirs[0][0])
    nfunc, nname, afunc, aname, aord = struct.unpack_from('<IIIII', d, e + 20)
    names = {}
    for i in range(nname):
        n = cstr(struct.unpack_from('<I', d, off(aname) + 4 * i)[0])
        o = struct.unpack_from('<H', d, off(aord) + 2 * i)[0]
        names[n] = struct.unpack_from('<I', d, off(afunc) + 4 * o)[0]
    check('exports OpenAdapter10 and OpenAdapter10_2, undecorated', 'OpenAdapter10' in names and 'OpenAdapter10_2' in names,
          f'{len(names)} named exports, internal name {cstr(struct.unpack_from("<I", d, e + 12)[0])}')
    deco = [n for n in names if '@' in n or n.startswith('_')]
    check('no decorated export name', not deco, ' '.join(deco[:6]))
    # imports
    dlls = []
    i = off(dirs[1][0])
    while True:
        ilt, _, _, name, iat = struct.unpack_from('<IIIII', d, i)
        if not name:
            break
        dlls.append(cstr(name).lower())
        i += 20
    bad = [n for n in dlls if n not in ALLOWED and not CRT.match(n)]
    check('imports only system DLLs and the UCRT api set (no vcruntime140 / msvcp140)', not bad,
          ' '.join(bad) if bad else f'{len(dlls)} DLLs')
    check('no delay imports', dirs[13][1] == 0)
    # load config: SafeSEH
    if dirs[10][1]:
        lc = off(dirs[10][0])
        tbl, cnt = struct.unpack_from('<II', d, lc + 64)
        check('SafeSEH table', tbl != 0 and cnt > 0, f'{cnt} handlers')
    else:
        check('load config directory', False)
    if len(a) > 1:
        m = open(a[1], errors='replace').read()
        base = struct.unpack_from('<I', d, opt + 28)[0]

        # lld's MSVC-style map: " 0001:00012e20  _OpenAdapter10@4  0000000010013e20  libd3d10umd:Adapter.cpp.obj"
        syms = {}
        for r in re.finditer(r'^ [0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:[fi]\s+)*(\S+)\s*$', m, re.M):
            syms.setdefault(r.group(1), (int(r.group(2), 16), r.group(3)))

        def sym(name):
            return syms[name][0] if name in syms else None

        def owner(name):
            return syms[name][1] if name in syms else '?'

        for n in ('OpenAdapter10', 'OpenAdapter10_2'):
            s = sym(f'_{n}@4')
            got = names.get(n)
            check(f'export {n} is _{n}@4', s is not None and got is not None and s in (got, got + base),
                  f'export rva 0x{got or 0:x}, map 0x{s or 0:x}')
        check('malikm_fd_dup is the shim\'s', 'malikm_drm' in owner('_malikm_fd_dup'), owner('_malikm_fd_dup'))
        check('panfrost_sb_rotate_bos is Panfrost\'s', 'pan_resource' in owner('_panfrost_sb_rotate_bos'),
              owner('_panfrost_sb_rotate_bos'))
    print('VERIFY ' + ('PASSED' if ok else 'FAILED') + f' ({a[0]}, {len(d)} bytes)')
    return 0 if ok else 1


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1:]))
