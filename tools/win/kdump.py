#!/usr/bin/env python3
"""kdump.py: which module crashed Windows, from a kernel crash dump (docs/WIN11-GPU.md 6.24).

Reads a dump with the "PAGEDU64" header: the head of pagefile.sys right after a bugcheck (before Windows boots
again and moves it out), or C:\\Windows\\Minidump\\*.dmp / MEMORY.DMP afterwards. For a small (triage) dump it
lists the loaded drivers, names the module and offset of the bugcheck's code address (BugCheckParameter2 for
0x7E/0x1E/0x3B), decodes the exception record and ARM64 context the bugcheck parameters point at when they lie
in the saved stack, and symbolises every stack word that falls inside a driver. With --map NAME=FILE.map
(lld-link /map output, e.g. sbwddm.sys=...) offsets inside our drivers become function+offset.

  kdump.py DUMP [--map sbwddm.sys=sbwddm.map ...]
"""
import re
import struct
import sys

BUGCHECK_PC_PARAM = {0x7E: 2, 0x1000007E: 2, 0x1E: 2, 0x3B: 2, 0xD1: 4, 0x50: 3}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def u64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def load_map(path):
    syms = []
    for line in open(path, errors='replace'):
        m = re.match(r'\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})', line)
        if m:
            syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
    return sorted(syms)


def sym(maps, mod, off):
    syms = maps.get(mod.lower())
    if not syms:
        return ''
    best = None
    for rva, name in syms:
        if rva <= off:
            best = (rva, name)
        else:
            break
    return f' = {best[1]}+{off - best[0]:#x}' if best else ''


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    maps = {}
    path = None
    i = 0
    while i < len(args):
        if args[i] == '--map':
            name, f = args[i + 1].split('=', 1)
            maps[name.lower()] = load_map(f)
            i += 2
        else:
            path = args[i]
            i += 1
    d = open(path, 'rb').read()
    if d[:8] != b'PAGEDU64':
        at = d.find(b'PAGEDU64')
        print(f'no PAGEDU64 header at 0 (found at {at:#x})' if at >= 0 else 'no PAGEDU64 header: not a crash dump '
              '(a pagefile Windows already reused, or the dump was not written)')
        if at < 0:
            return 1
        d = d[at:]
    machine = u32(d, 0x30)
    bc = u32(d, 0x38)
    params = [u64(d, 0x40 + 8 * k) for k in range(4)]
    dump_type = u32(d, 0xF98)
    print(f'machine {machine:#x}, bugcheck {bc:#x} ({", ".join(hex(p) for p in params)}), dump type {dump_type}')
    print(f'PsLoadedModuleList {u64(d, 0x20):#x}, KdDebuggerDataBlock {u64(d, 0x80):#x}')
    if dump_type != 4:
        print('not a small (triage) dump: the module list needs the kernel\'s page tables; open it in WinDbg '
              '(lm; .ecxr; k) or send it on')
        return 0
    t = 0x2000
    (spb, size, valid, ctx_off, exc_off, mm_off, unl_off, prcb_off, proc_off, thr_off, stk_off, stk_size,
     drv_off, drv_count, str_off, str_size, broken_off, options) = struct.unpack_from('<18I', d, t)
    top = u64(d, t + 0x48)
    print(f'triage: {drv_count} drivers at {drv_off:#x}, stack {stk_size:#x} bytes at {stk_off:#x} '
          f'(top {top:#x}), context at {ctx_off:#x}')

    def dstr(off):
        if off == 0 or off + 4 > len(d):
            return '?'
        n = u32(d, off)
        return d[off + 4:off + 4 + 2 * n].decode('utf-16-le', 'replace')

    # entry size: the one at which every entry's DllBase looks like a kernel address
    mods = []
    for esize in range(0x98, 0x140, 8):
        cand = []
        ok = True
        for k in range(drv_count):
            e = drv_off + k * esize
            if e + esize > len(d):
                ok = False
                break
            base, size_img = u64(d, e + 8 + 0x30), u32(d, e + 8 + 0x40)
            if base < 0xFFFF800000000000 or not 0x1000 <= size_img < 0x10000000:
                ok = False
                break
            cand.append((base, size_img, dstr(u32(d, e))))
        if ok and cand:
            mods = cand
            break
    if not mods:
        print('could not read the driver list')
    for base, size_img, name in sorted(mods):
        print(f'  {base:#018x} {size_img:#9x} {name}')

    def where(a):
        for base, size_img, name in mods:
            if base <= a < base + size_img:
                return f'{name}+{a - base:#x}{sym(maps, name, a - base)}'
        return None

    k = BUGCHECK_PC_PARAM.get(bc)
    if k:
        print(f'code address (parameter {k}) {params[k - 1]:#x}: {where(params[k - 1]) or "in no listed driver"}')
    stack = d[stk_off:stk_off + stk_size]

    def at_stack(va, n):
        if top <= va and va + n <= top + len(stack):
            return stack[va - top:va - top + n]
        return None
    if bc in (0x7E, 0x1000007E, 0x1E):
        er = at_stack(params[2], 0x98)
        cx = at_stack(params[3], 0x390)
        if er:
            code, flags = struct.unpack_from('<II', er, 0)
            addr, nparams = u64(er, 0x10), u32(er, 0x18)
            info = [u64(er, 0x20 + 8 * j) for j in range(min(nparams, 4))]
            print(f'exception {code:#x} at {addr:#x} ({where(addr) or "?"}), info {[hex(x) for x in info]}')
        if cx:
            x = [u64(cx, 8 + 8 * j) for j in range(29)]
            fp, lr, sp, pc = u64(cx, 0xF0), u64(cx, 0xF8), u64(cx, 0x100), u64(cx, 0x108)
            print(f'context: pc {pc:#x} ({where(pc) or "?"}) lr {lr:#x} ({where(lr) or "?"}) sp {sp:#x} fp {fp:#x}')
            print('  ' + ' '.join(f'x{j}={v:#x}' for j, v in enumerate(x)))
    print('stack words inside drivers:')
    for o in range(0, len(stack) - 7, 8):
        v = u64(stack, o)
        w = where(v)
        if w:
            print(f'  [{top + o:#x}] {v:#x} {w}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
