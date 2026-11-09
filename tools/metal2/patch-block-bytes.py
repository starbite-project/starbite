#!/usr/bin/env python3
"""Patch metal2vulkan's pooled-block size in a built StarbiteMetal (the Metal bridge in backboardd) without a rebuild.
    patch-block-bytes.py IN OUT MIB          e.g. 8 (the bridge's BLOCK_BYTES, patch 0100 made it 24 MiB)
The runtime computes block_bytes = min(maxMemoryAllocationSize, BLOCK_BYTES) once per allocator:
    movz wN, #0x180, lsl #16      (0x1800000 = 24 MiB)
    cmp  xA, xN
    csel xD, xA, xN, lo
and derives its "dedicated allocation above half a block" threshold from the stored field, so one immediate sets
both. 2026-11-09: backboardd held 17 such 24 MiB blocks (408 MiB) for ~150 MiB of render targets and textures.
Refuses unless exactly one site matches. OUT is re-signed ad hoc (codesign -f -s -)."""
import struct
import subprocess
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    src, dst, mib = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if not 2 <= mib <= 64 or (mib << 20) & 0xffff:
        sys.exit('MIB must be 2..64 (a whole multiple of 64 KiB)')
    data = bytearray(open(src, 'rb').read())
    hits = []
    for off in range(0, len(data) - 12, 4):
        w0, w1, w2 = struct.unpack_from('<III', data, off)
        # movz wN, #0x180, lsl #16
        if w0 & 0xffffffe0 != 0x52a03000:
            continue
        rn = w0 & 0x1f
        # cmp xA, xN == subs xzr, xA, xN (shifted register, no shift): 0xeb000000 | Rm<<16 | Rn<<5 | 31
        if w1 & 0xffe0fc1f != 0xeb00001f or (w1 >> 16) & 0x1f != rn:
            continue
        # csel xD, xA, xN, lo (cond 0b0011): 0x9a800000 | Rm<<16 | cond<<12 | Rn<<5 | Rd
        if w2 & 0xffe0fc00 != 0x9a803000 or (w2 >> 16) & 0x1f != rn:
            continue
        hits.append((off, rn))
    if len(hits) != 1:
        sys.exit(f'{len(hits)} matching sites (want exactly 1): refusing')
    off, rn = hits[0]
    imm = mib << 4                      # MiB -> the immediate shifted by 16: (MiB << 20) >> 16
    struct.pack_into('<I', data, off, 0x52a00000 | imm << 5 | rn)
    open(dst, 'wb').write(data)
    print(f'patched file offset {off:#x}: block {24} MiB -> {mib} MiB (w{rn})')
    subprocess.run(['codesign', '-f', '-s', '-', dst], check=True, capture_output=True)
    print(f'{dst}: re-signed ad hoc')


if __name__ == '__main__':
    main()
