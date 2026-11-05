#!/usr/bin/env python3
"""Finish and check a libsb image (starbite/engineer/build.sh).

    check-image.py IN.bin OUT.img NM.map

Pads the raw binary to 4 KiB, writes image_size (+16) and the Starbite CRC32 (+88, of the image
with that field zero), then checks what the loader's image_load and crt0.S rely on: the "ARMd"
magic at +56, text_offset 0x80000 (< 2 MiB), image_size >= the file, the first word a branch to
`entry`, _image_end == the padded size (the .bss is in the file), the "SBOSIMG1" extension, the
.rela.dyn range inside the image and made of R_AARCH64_RELATIVE entries only.
"""
import struct
import sys
import zlib


def sym(mapf, name):
    for line in open(mapf):
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f'check-image: no symbol {name}')


def main():
    src, dst, mapf = sys.argv[1:4]
    b = bytearray(open(src, 'rb').read())
    b += bytes((-len(b)) % 4096)
    end = sym(mapf, '_image_end')
    assert end == len(b), f'_image_end {end:#x} != padded size {len(b):#x} (.bss must be in the file)'
    assert b[56:60] == b'ARMd', 'missing arm64 Image magic'
    text_offset, = struct.unpack_from('<Q', b, 8)
    assert text_offset == 0x80000, f'text_offset {text_offset:#x}'
    assert b[64:72] == b'SBOSIMG1', 'missing SBOSIMG1 extension'
    ver, entry_off = struct.unpack_from('<II', b, 72)
    assert entry_off == sym(mapf, 'entry'), 'SBOSIMG1 entry offset != entry'
    insn, = struct.unpack_from('<I', b, 0)
    assert insn >> 26 == 0b000101, 'first word is not a B'
    imm = insn & 0x3ffffff
    assert imm * 4 == entry_off, f'B at 0 goes to {imm * 4:#x}, entry is {entry_off:#x}'
    rs, re_ = sym(mapf, '__rela_start'), sym(mapf, '__rela_end')
    assert rs <= re_ <= len(b) and (re_ - rs) % 24 == 0, 'bad .rela.dyn range'
    n = (re_ - rs) // 24
    for i in range(n):
        off, info, add = struct.unpack_from('<QQq', b, rs + 24 * i)
        assert info == 1027, f'relocation {i}: type {info & 0xffffffff} (only RELATIVE allowed)'
        assert off < len(b) and off % 8 == 0, f'relocation {i}: offset {off:#x} outside the image'
        assert 0 <= add <= len(b), f'relocation {i}: addend {add:#x} outside the image'   # == : _image_end
    struct.pack_into('<Q', b, 16, len(b))
    struct.pack_into('<Q', b, 88, 0)
    crc = zlib.crc32(bytes(b))
    struct.pack_into('<Q', b, 88, crc)
    open(dst, 'wb').write(b)
    print(f'{dst}: {len(b)} bytes, {n} R_AARCH64_RELATIVE relocations, entry +{entry_off:#x}, '
          f'crc32 {crc:08x}; arm64 Image header OK')


if __name__ == '__main__':
    main()
