#!/usr/bin/env python3
"""Finish and check a libsb image (starbite/engineer/build.sh).

    check-image.py IN.bin OUT.img NM.map

Pads the raw binary to 4 KiB, writes image_size (+16) and the Starbite CRC32 (+88, of the image
with that field zero), then checks what the loader's image_load and crt0.S rely on: the "ARMd"
magic at +56, text_offset 0x80000 (< 2 MiB), image_size >= the file, the first word a branch to
`entry`, _image_end == the padded size (the .bss is in the file), the "SBOSIMG1" extension, the
.rela.dyn range inside the image and made of R_AARCH64_RELATIVE entries only; the size budget
(32 MiB, and at least 96 MiB of arena left below 0xa0000000 when loaded at 0x92080000), sb_vectors
2 KiB aligned for VBAR_EL1.
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
    # where it runs: boot_bare loads at 0x92000000 + text_offset; the arena (back buffer, spare
    # buffer, page pools, about 45 MiB) runs from the image end to 0xa0000000 (S-Boot's ECT)
    load = 0x92000000 + text_offset
    assert len(b) <= 32 << 20, f'image {len(b)} bytes: over the 32 MiB budget'
    assert load + len(b) + (96 << 20) <= 0xa0000000, 'image leaves under 96 MiB of arena below 0xa0000000'
    vec = sym(mapf, 'sb_vectors')
    assert vec % 2048 == 0, f'sb_vectors {vec:#x} not 2 KiB aligned (VBAR_EL1)'
    for need in ('sb_main', 'sb_start', 'sb_mmu_enable', 'sb_reloc_check', 'sb_crash_show'):
        assert sym(mapf, need) < len(b), f'{need} outside the image'
    struct.pack_into('<Q', b, 16, len(b))
    struct.pack_into('<Q', b, 88, 0)
    crc = zlib.crc32(bytes(b))
    struct.pack_into('<Q', b, 88, crc)
    open(dst, 'wb').write(b)
    print(f'{dst}: {len(b)} bytes ({len(b) / (1 << 20):.1f} MiB, ends at {load + len(b):#x}), {n} R_AARCH64_RELATIVE relocations, entry +{entry_off:#x}, '
          f'crc32 {crc:08x}; arm64 Image header OK')


if __name__ == '__main__':
    main()
