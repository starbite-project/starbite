#!/usr/bin/env python3
"""Starbite OS app files (docs/STARBITE-OS.md 6.1). Standard library only (runs in the VM).

    mk-sba.py app ELF BIN OUT.sba       fill the header of a linked app (entry, sizes, relocations, CRC)
    mk-sba.py pack OUT.pack A.sba ...   the built-in app pack the kernel image embeds

The ELF's symbols (nm) give _sbapp_entry, __rela_start/__rela_end, _file_end and _bss_end; BIN is
`objcopy -O binary` of it (the file runs from the header to _file_end; the bss is not in it).
"""
import os
import struct
import subprocess
import sys
import zlib

HDR = 256
R_AARCH64_RELATIVE = 1027


def syms(elf):
    nm = subprocess.run([os.environ.get('NM', 'nm'), elf], capture_output=True, text=True, check=True).stdout
    out = {}
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def app(elf, binf, out):
    s = syms(elf)
    b = bytearray(open(binf, 'rb').read())
    file_end, bss_end = s['_file_end'], s['_bss_end']
    assert len(b) <= file_end, f'{binf}: {len(b)} bytes, _file_end {file_end:#x}'
    b += bytes(file_end - len(b))                     # the file runs to _file_end (zero .data tail)
    assert b[:8] == b'SBAPP001', f'{elf}: the header is not first'
    entry = s['_sbapp_entry']
    rs, re_ = s['__rela_start'], s['__rela_end']
    n = (re_ - rs) // 24
    for i in range(n):
        off, info, add = struct.unpack_from('<QQq', b, rs + 24 * i)
        assert info & 0xffffffff == R_AARCH64_RELATIVE, f'{elf}: relocation type {info:#x}'
        assert HDR <= off < file_end and 0 <= add <= bss_end, f'{elf}: relocation {off:#x} -> {add:#x}'
    struct.pack_into('<IIIII', b, 0x44, entry, len(b), bss_end - file_end, rs if n else 0, n)
    struct.pack_into('<I', b, 0x60, 0)
    crc = zlib.crc32(bytes(b)) & 0xffffffff
    struct.pack_into('<I', b, 0x60, crc)
    open(out, 'wb').write(b)
    name = b[0x28:0x40].split(b'\0')[0].decode()
    print(f'{out}: {name}, {len(b)} bytes + {bss_end - file_end} bss, {n} relocations, entry {entry:#x}, crc {crc:08x}')


def pack(out, files):
    blobs = [open(f, 'rb').read() for f in files]
    head = 16 + 8 * len(blobs)
    off = (head + 15) & ~15
    dirs, data = [], bytearray()
    for bl in blobs:
        dirs.append(struct.pack('<II', off + len(data), len(bl)))
        data += bl
        data += bytes((-len(data)) % 16)
    blob = b'SBAPPACK' + struct.pack('<II', len(blobs), 0) + b''.join(dirs)
    blob += bytes(off - len(blob)) + data
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blobs)} apps, {len(blob)} bytes')


if __name__ == '__main__':
    if sys.argv[1] == 'app':
        app(sys.argv[2], sys.argv[3], sys.argv[4])
    elif sys.argv[1] == 'pack':
        pack(sys.argv[2], sys.argv[3:])
    else:
        sys.exit('usage: mk-sba.py app ELF BIN OUT | pack OUT A.sba ...')
