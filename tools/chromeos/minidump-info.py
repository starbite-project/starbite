#!/usr/bin/env python3
"""Read a Breakpad/Crashpad minidump (arm64 Linux, ChromeOS crash_reporter .dmp) without Breakpad's tools.

Prints the exception (signal, fault address), the crashing thread's pc/lr/sp/fp resolved to module+offset,
and every word on that thread's stack that points into an executable module (return-address candidates),
so a crash in a stripped binary can be located and disassembled with llvm-objdump --start-address.

  minidump-info.py FILE.dmp [--stack N]   (N stack words to scan, default 2048)
"""
import struct, sys

STREAM_THREAD_LIST, STREAM_MODULE_LIST, STREAM_EXCEPTION = 3, 4, 6
d = open(sys.argv[1], 'rb').read()
nscan = int(sys.argv[sys.argv.index('--stack') + 1]) if '--stack' in sys.argv else 2048
sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', d, 0)
assert sig == 0x504d444d, 'not a minidump'
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', d, dir_rva + 12 * i)
    streams[t] = (size, rva)


def mdstring(rva):
    n = struct.unpack_from('<I', d, rva)[0]
    return d[rva + 4:rva + 4 + n].decode('utf-16-le', 'replace')


mods = []
size, rva = streams[STREAM_MODULE_LIST]
n = struct.unpack_from('<I', d, rva)[0]
for i in range(n):
    m = rva + 4 + 108 * i
    base, msize = struct.unpack_from('<QI', d, m)
    name_rva = struct.unpack_from('<I', d, m + 20)[0]
    mods.append((base, base + msize, mdstring(name_rva)))
mods.sort()


def where(a):
    for b, e, nm in mods:
        if b <= a < e:
            return f'{nm.rsplit("/", 1)[-1]}+0x{a - b:x}'
    return None


size, rva = streams[STREAM_EXCEPTION]
tid = struct.unpack_from('<I', d, rva)[0]
code, flags = struct.unpack_from('<II', d, rva + 8)
fault = struct.unpack_from('<Q', d, rva + 24)[0]
ctx_size, ctx_rva = struct.unpack_from('<II', d, rva + 8 + 152)
print(f'thread {tid}: signal {code} (code 0x{flags:x}) fault address 0x{fault:x} {where(fault) or ""}')
# arm64 context (Crashpad MinidumpContextARM64): u32 context_flags, u32 cpsr, u64 x[31], sp, pc, ...
cflags, cpsr = struct.unpack_from('<II', d, ctx_rva)
off = ctx_rva + 8
x = struct.unpack_from('<31Q', d, off)
sp, pc = struct.unpack_from('<QQ', d, off + 31 * 8)
print(f'pc 0x{pc:x} {where(pc)}\nlr 0x{x[30]:x} {where(x[30])}\nfp 0x{x[29]:x}  sp 0x{sp:x}')
for i in range(0, 29, 4):
    print('  ' + '  '.join(f'x{j:<2}={x[j]:016x}' for j in range(i, min(i + 4, 29))))

size, rva = streams[STREAM_THREAD_LIST]
n = struct.unpack_from('<I', d, rva)[0]
for i in range(n):
    t = rva + 4 + 48 * i
    if struct.unpack_from('<I', d, t)[0] != tid:
        continue
    sbase, ssize, srva = struct.unpack_from('<QII', d, t + 24)
    print(f'stack 0x{sbase:x} +0x{ssize:x}: words pointing into modules')
    start = max(0, sp - sbase)
    for k in range(start, min(ssize, start + 8 * nscan), 8):
        v = struct.unpack_from('<Q', d, srva + k)[0]
        w = where(v)
        if w:
            print(f'  [sp+0x{sbase + k - sp:x}] 0x{v:x} {w}')
print(f'{len(mods)} modules; first: ' + ', '.join(f'{nm.rsplit("/", 1)[-1]}@0x{b:x}' for b, e, nm in mods[:6]))
