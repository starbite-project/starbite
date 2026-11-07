#!/usr/bin/env python3
"""Give the release's PPTT the A55 caches at build time (docs/WIN11-USB.md section 10).

  pptt-patch.py ACPI.ffs OUT.ffs [--dump OUT.aml] [--table PPTT.aml]

--table: put this ready-made PPTT in (tools/woa/pptt-gen.py, the OEM-shaped table of
docs/WIN11-24H2-A55.md) instead of PpttBuild's cache-augmented copy of the release's.

ACPI.ffs is the woa release's platform table file (FREEFORM 11111111-2222-3333-4444-555555555501,
one RAW section per table: FACP, GTDT, APIC, PPTT, DSDT, SSDT), as `dxe-replace.py --extract`
writes it. The PPTT section is replaced by PpttBuild's output (the same C as the DXE's runtime
path, through test/pptt-tool.c) with the caches measured on the phone, every other section is
copied unchanged, and the FFS header (size, header checksum) is rebuilt. The image build kills
the old file (--kill-blank: a pad file, which the DXE core's lookups skip) and adds this one.

Why build time: the release has no EFI_ACPI_SDT_PROTOCOL, so the runtime fix (PpttFix.c) can't
find the installed PPTT ("=PPTT skip: no ACPI protocols (Not Found)", combined image 059f406f).

The caches, from the phone (zone 0 of combo1, 2026-11-07, PpttFix's CLIDR/CCSIDR reads on A55
core 0): CLIDR 0x82000023 = L1 separate I+D, L2 unified, no L3. L1D and L1I 64-byte lines, 4
ways, 128 sets (32 KiB each), per core; L2 64-byte lines, 16 ways, 512 sets (512 KiB), shared by
the cluster (the A55s here have no private L2; this is the cluster cache), so PrivateLevels = 1.
"""
import os
import subprocess
import sys
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.join(HERE, 'modules/Star2LteSsdtDxe')
ACPI_FILE = uuid.UUID('11111111-2222-3333-4444-555555555501')
PRIVATE_LEVELS = '1'
CACHES = ['1:d:64:128:4', '1:i:64:128:4', '2:u:64:512:16']
RAW = 0x19


def sections(body):
    out, q = [], 0
    while q + 4 <= len(body):
        ln = int.from_bytes(body[q:q + 3], 'little')
        if ln < 4 or ln == 0xFFFFFF or q + ln > len(body):
            sys.exit(f'bad section at {q:#x}')
        out.append(body[q:q + ln])
        q = (q + ln + 3) & ~3
    return out


def main():
    args = sys.argv[1:]
    dump = None
    table = None
    if '--table' in args:
        i = args.index('--table')
        table = open(args[i + 1], 'rb').read()
        del args[i:i + 2]
        if table[:4] != b'PPTT' or int.from_bytes(table[4:8], 'little') != len(table) or sum(table) & 0xFF:
            sys.exit('--table: not a PPTT with a valid length and checksum')
    if '--dump' in args:
        i = args.index('--dump')
        dump = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    ffs = open(args[0], 'rb').read()
    if uuid.UUID(bytes_le=ffs[:16]) != ACPI_FILE:
        sys.exit(f'{args[0]}: not the platform table file {ACPI_FILE}')
    attr = ffs[19]
    if attr & 1:
        sys.exit('large-file header not expected')
    if attr & 0x40:
        sys.exit('file checksum attribute not expected (the release file has none)')
    size = int.from_bytes(ffs[20:23], 'little')
    secs = sections(ffs[24:size])
    pptt = [s for s in secs if s[3] == RAW and s[4:8] == b'PPTT']
    if len(pptt) != 1:
        sys.exit(f'{len(pptt)} PPTT sections')
    old = pptt[0][4:]
    if table is not None:
        new = table
    else:
        with tempfile.TemporaryDirectory() as td:
            tool = os.path.join(td, 'pptt-tool')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-o', tool,
                            os.path.join(MOD, 'test/pptt-tool.c'), os.path.join(MOD, 'PpttBuild.c')], check=True)
            src, dst = os.path.join(td, 'old.aml'), os.path.join(td, 'new.aml')
            open(src, 'wb').write(old)
            subprocess.run([tool, src, dst, PRIVATE_LEVELS] + CACHES, check=True, stdout=subprocess.DEVNULL)
            new = open(dst, 'rb').read()
    if dump:
        open(dump, 'wb').write(new)
    body = bytearray()
    for s in secs:
        if s is pptt[0]:
            s = (4 + len(new)).to_bytes(3, 'little') + bytes([RAW]) + new
        body += s
        while len(body) % 4:
            body += b'\0'
    # trailing alignment padding after the last section isn't part of the file
    last = len(body) - ((-len(secs[-1] if secs[-1] is not pptt[0] else 4 + len(new))) % 4)
    body = body[:last]
    hdr = bytearray(ffs[:24])
    total = 24 + len(body)
    hdr[20:23] = total.to_bytes(3, 'little')
    hdr[16] = 0
    hdr[17] = 0
    hdr[23] = 0                               # state: not part of the checksum
    hdr[16] = (-sum(hdr)) & 0xFF
    hdr[17] = 0xAA                            # no FFS_ATTRIB_CHECKSUM: fixed file checksum
    hdr[23] = ffs[23]
    open(args[1], 'wb').write(bytes(hdr) + bytes(body))
    print(f'{ACPI_FILE}: PPTT {len(old)} -> {len(new)} bytes, file {size:#x} -> {total:#x}')


if __name__ == '__main__':
    main()
