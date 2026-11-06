#!/usr/bin/env python3
"""Make the S-Boot FDT test fixture: a copy of S-Boot's run-time FDT (as read back from the phone,
build/linux-bundle/sboot.dtb) with the phone's identity blanked, byte length kept, so offsets and the
FDT structure stay exactly as S-Boot wrote them.

  scrub-sboot-fdt.py IN.dtb OUT.dtb

Blanked (each character of the value becomes '0', or 'x' for the letters of a key-like value):
/chosen/serial (u64), and in /chosen/bootargs androidboot.serialno, androidboot.ap_serial,
androidboot.em.did, android.nonce, androidboot.bootloader, sales_code, androidboot.carrierid,
region1, region2. The output is checked: none of the original values is left anywhere in it.
"""
import re
import struct
import sys
from pathlib import Path

KEYS = ['androidboot.serialno', 'androidboot.ap_serial', 'androidboot.em.did', 'android.nonce',
        'androidboot.bootloader', 'sales_code', 'androidboot.carrierid', 'region1', 'region2']


def main():
    src, dst = sys.argv[1:3]
    b = bytearray(Path(src).read_bytes())
    assert struct.unpack_from('>I', b, 0)[0] == 0xd00dfeed, 'not an FDT'
    secrets = []
    for k in KEYS:
        for m in re.finditer(re.escape(k.encode()) + rb'=([^ \x00]+)', bytes(b)):
            v = m.group(1)
            secrets.append(v)
            b[m.start(1):m.end(1)] = bytes(ord('0') for _ in v)
    # /chosen/serial: the property named "serial" (8 bytes): find its name offset, then its PROP token
    ostruct, ostr = struct.unpack_from('>II', b, 8)
    strs = bytes(b[ostr:])
    name_off = strs.find(b'\x00serial\x00') + 1
    p = ostruct
    while p + 12 <= len(b):
        tok = struct.unpack_from('>I', b, p)[0]
        if tok == 1:
            e = b.index(0, p + 4)
            p = (e + 4) & ~3
        elif tok == 3:
            ln, no = struct.unpack_from('>II', b, p + 4)
            if no == name_off and ln == 8:
                secrets.append(bytes(b[p + 12:p + 20]))
                b[p + 12:p + 20] = bytes(8)
            p = (p + 12 + ln + 3) & ~3
        elif tok in (2, 4):
            p += 4
        else:
            break
    for s in secrets:
        if len(s) >= 6 and s in b:
            sys.exit(f'{s!r} is still in the output')
    Path(dst).write_bytes(bytes(b))
    print(f'{dst}: {len(b)} bytes, {len(secrets)} values blanked')


if __name__ == '__main__':
    main()
