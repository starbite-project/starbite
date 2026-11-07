#!/usr/bin/env python3
"""sparse2raw.py IMAGE [--size] : an Android sparse image (or a raw one) as a raw stream on stdout, without
writing a temp file (for `... | tools/android/pixel/part-write.py NAME - --size N` straight from the build
disk). --size prints only the raw size in bytes. DONT_CARE chunks come out as zeros; CRC32 chunks are checked
against the stream so far when present. Refuses anything malformed."""
import struct
import sys
import zlib

MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


def main():
    path = sys.argv[1]
    f = open(path, 'rb')
    hdr = f.read(28)
    out = sys.stdout.buffer
    if len(hdr) < 28 or struct.unpack_from('<I', hdr)[0] != MAGIC:
        f.seek(0, 2)
        n = f.tell()
        if '--size' in sys.argv:
            print(n)
            return
        f.seek(0)
        while True:
            b = f.read(1 << 22)
            if not b:
                return
            out.write(b)
    magic, major, minor, fhs, chs, bs, total_blks, total_chunks, _ = struct.unpack('<IHHHHIIII', hdr)
    if major != 1 or fhs < 28 or chs < 12:
        sys.exit(f'{path}: unsupported sparse header {major}.{minor} {fhs}/{chs}')
    if '--size' in sys.argv:
        print(total_blks * bs)
        return
    f.seek(fhs)
    crc, written = 0, 0
    zero = bytes(bs * 256)
    for k in range(total_chunks):
        ch = f.read(chs)
        ctype, _, csz, tsz = struct.unpack_from('<HHII', ch)
        f.seek(chs - 12, 1)
        n = csz * bs
        if ctype == RAW:
            if tsz != chs + n:
                sys.exit(f'chunk {k}: raw size {tsz} != {chs + n}')
            left = n
            while left:
                b = f.read(min(left, 1 << 22))
                if not b:
                    sys.exit(f'chunk {k}: truncated')
                crc = zlib.crc32(b, crc)
                out.write(b)
                left -= len(b)
        elif ctype in (FILL, DONT_CARE):
            if ctype == FILL:
                if tsz != chs + 4:
                    sys.exit(f'chunk {k}: fill size {tsz}')
                pat = f.read(4) * (bs * 256 // 4)
            else:
                if tsz != chs:
                    sys.exit(f'chunk {k}: dont-care size {tsz}')
                pat = zero
            left = n
            while left:
                b = pat[:min(left, len(pat))]
                crc = zlib.crc32(b, crc)
                out.write(b)
                left -= len(b)
        elif ctype == CRC:
            want = struct.unpack('<I', f.read(4))[0]
            if want != crc:
                sys.exit(f'chunk {k}: CRC32 {want:08x} != stream {crc:08x}')
            n = 0
        else:
            sys.exit(f'chunk {k}: unknown type {ctype:#x}')
        written += n
    if written != total_blks * bs:
        sys.exit(f'wrote {written} B, header says {total_blks * bs}')


if __name__ == '__main__':
    main()
