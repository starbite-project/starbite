#!/usr/bin/env python3
"""SBMAINT1 headers for the loader's host test, made and judged by the reference implementation
(tools/maint/sbmaint.py): a valid container and one damage per rule of docs/MAINTENANCE.md 4.2 R4.

  gen_maint.py OUTDIR DTB

Writes OUTDIR/maint-NAME.bin (the 4096-byte header) and OUTDIR/maint-cases.txt, one line per case:
"NAME LIMIT VALID SEQ TOTAL K_OFF K_SIZE K_CRC D_OFF D_SIZE I_OFF I_SIZE R_OFF R_SIZE" as
sbmaint.parse_header sees it; host_test.c's sbm_check must agree on every line. Also
"slotb SIZE B" lines from sbmaint.slot_b_offset.
"""
import importlib.util
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sbmaint_lt', REPO / 'tools' / 'maint' / 'sbmaint.py')
sm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sm)


def recrc(h):
    h = bytearray(h)
    struct.pack_into('<I', h, sm.CRC_OFF, zlib.crc32(bytes(h[:sm.CRC_OFF])))
    return bytes(h)


def poke(h, off, fmt, *v, crc=True):
    h = bytearray(h)
    struct.pack_into(fmt, h, off, *v)
    return recrc(h) if crc else bytes(h)


def main():
    out, dtbp = Path(sys.argv[1]), sys.argv[2]
    dtb = Path(dtbp).read_bytes()
    kernel = bytearray(70000)
    kernel[56:60] = b'ARMd'
    struct.pack_into('<QQ', kernel, 8, 0x80000, 0x200000)
    initrd = bytes(range(256)) * 400
    c = sm.pack(bytes(kernel), dtb, initrd, cmdline='console=tty0 sb.x=1',
                version='sbmaint test', seq=3, build_time=1794200000)
    cr = sm.pack(bytes(kernel), dtb, initrd, dtb_rescue=dtb, cmdline='', version='with rescue', seq=7)
    h = c[:sm.HDR]
    total = struct.unpack_from('<Q', h, 24)[0]
    cases = {
        'valid': (h, 128 << 20),
        'valid-rescue': (cr[:sm.HDR], 128 << 20),
        'bad-magic': (poke(h, 0, '<8s', b'SBMAINT2'), 128 << 20),
        'bad-version': (poke(h, 8, '<I', 2), 128 << 20),
        'bad-crc': (poke(h, 200, '<B', 0x41, crc=False), 128 << 20),
        'seq-0': (poke(h, 16, '<I', 0), 128 << 20),
        'total-odd': (poke(h, 24, '<Q', total + 100), 128 << 20),
        'total-over-slot': (h, total - 4096),
        'total-over-stage': (poke(h, 24, '<Q', 0x05000000), 512 << 20),
        'kind-order': (poke(h, 64 + 20, '<I', 2), 128 << 20),
        'no-kernel': (poke(h, 64, '<QQI', 0, 0, 0), 128 << 20),
        'no-initrd': (poke(h, 64 + 64, '<QQI', 0, 0, 0), 128 << 20),
        'rescue-empty-crc': (poke(h, 64 + 96 + 16, '<I', 5), 128 << 20),
        'overlap': (poke(h, 64 + 32, '<Q', 0), 128 << 20),
        'unaligned': (poke(h, 64 + 64, '<Q', struct.unpack_from('<Q', h, 64 + 64)[0] + 8), 128 << 20),
        'dtb-too-large': (poke(h, 64 + 32 + 8, '<Q', (2 << 20) + 1), 128 << 20),
        'past-total': (poke(h, 64 + 64 + 8, '<Q', total), 128 << 20),
    }
    lines = []
    for name, (hh, limit) in cases.items():
        (out / f'maint-{name}.bin').write_bytes(hh)
        try:
            p = sm.parse_header(hh, min(limit, sm.STAGE_MAX))
            s = p['sections']
            row = [1, p['seq'], p['total']] + [x for k in sm.KINDS for x in (s[k][0], s[k][1], s[k][2])]
        except sm.ContainerError:
            row = [0] + [0] * 14
        lines.append(' '.join(map(str, [name, limit] + row)))
    for size in (256 << 20, 160 << 20, (256 << 20) + 4096, 3 << 20, 1 << 20, 0):
        lines.append(f'slotb {size} {sm.slot_b_offset(size)}')
    # the reference's slot order for (valid A, valid B, seq A, seq B, prev)
    def rd(blobs):
        def read(off, n):
            return blobs.get(off, bytes(n))[:n]
        return read
    for va, vb, sa, sb_, prev in ((1, 1, 3, 4, 0), (1, 1, 4, 3, 0), (1, 1, 5, 5, 0), (1, 1, 3, 4, 1), (1, 0, 3, 0, 0),
                                  (0, 1, 0, 9, 1), (0, 0, 0, 0, 0)):
        part = 256 << 20
        b_off = sm.slot_b_offset(part)
        blobs = {}
        if va:
            blobs[0] = sm.restamp(c, sa)
        if vb:
            blobs[b_off] = sm.restamp(c, sb_)
        found, _ = sm.choose(rd(blobs), part, prev=bool(prev))
        order = ''.join(n for n, _ in found) or '-'
        lines.append(f'order {va} {vb} {sa} {sb_} {prev} {order}')
    (out / 'maint-cases.txt').write_text('\n'.join(lines) + '\n')
    print(f'{len(cases)} SBMAINT1 headers')


if __name__ == '__main__':
    main()
