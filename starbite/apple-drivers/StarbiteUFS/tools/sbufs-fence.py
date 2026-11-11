#!/usr/bin/env python3
# SPDX-License-Identifier: APSL-2.0
"""Pack the StarbiteUFS fence properties for /arm-io/ufs (format: ../NOTES.md "Fence property format").

    sbufs-fence.py --gpt LU0-HEAD.bin --allow USERDATA[:root][:rw][:MIB] [--allow ...] --out DIR
    sbufs-fence.py --selftest-gpt GPT.bin           write a synthetic S9-like LU0 head (tests only)

--gpt is LU 0's first 6 blocks of 4 KiB (protective MBR, GPT header, 128 x 128-byte entries), read-only from TWRP:
    adb shell dd if=/dev/block/sda bs=4096 count=6 > lu0-head.bin
Writes DIR/allowed-ranges.bin, DIR/denied-ranges.bin and DIR/fence.txt (what was packed, for the log).
Allowed: each --allow names a GPT partition; ":root" sets ROOT, ":rw" sets WRITABLE (writes still need
starbite,writes-allowed = 1 AND sbufsrw=1), ":MIB" keeps only the first MIB MiB (e.g. USERDATA:root:19200).
Denied: EVERY other partition by name, the GPT head (LBA 0 .. first usable - 1) and tail (last usable + 1 .. the
backup header): stricter than the driver needs, so a wrong allow-list entry is refused at boot, not obeyed.
UUIDs: the partition's unique GUID, stored in RFC 4122 byte order (as printed)."""
import argparse
import struct
import sys
import uuid
from pathlib import Path

BLOCK = 4096
ALLOW_MAGIC, DENY_MAGIC, VERSION = 0x52464253, 0x44464253, 1
WRITABLE, ROOT = 1, 2
NEVER = {'EFS', 'CPEFS', 'PARAM', 'KEYSTORAGE', 'PERSISTENT', 'STEADY', 'BOTA0', 'BOTA1', 'BOTA2', 'DTB', 'DTBO',
         'BOOT', 'RECOVERY', 'RADIO', 'UP_PARAM'}


def read_gpt(blob):
    h = blob[BLOCK:BLOCK + 92]
    if h[:8] != b'EFI PART':
        sys.exit('no GPT header at LBA 1 (4 KiB blocks)')
    alt, first_use, last_use = struct.unpack_from('<QQQ', h, 32)
    ent_lba, n, esz = struct.unpack_from('<QII', h, 72)
    parts = []
    for i in range(n):
        o = ent_lba * BLOCK + i * esz
        e = blob[o:o + esz]
        if len(e) < 128 or e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', e, 32)
        name = e[56:128].decode('utf-16-le').split('\0')[0]
        parts.append(dict(name=name, first=first, last=last, guid=uuid.UUID(bytes_le=bytes(e[16:32]))))
    return dict(alt=alt, first_use=first_use, last_use=last_use, parts=parts)


def pack(gpt, allows):
    recs, deny, lines = [], [], []
    names = {}
    for spec in allows:
        bits = spec.split(':')
        p = next((x for x in gpt['parts'] if x['name'] == bits[0]), None)
        if p is None:
            sys.exit(f'no partition {bits[0]} in the GPT')
        if p['name'] in NEVER:
            sys.exit(f'{p["name"]} is never Starbite-owned')
        flags, count = 0, p['last'] - p['first'] + 1
        for b in bits[1:]:
            if b == 'root':
                flags |= ROOT
            elif b == 'rw':
                flags |= WRITABLE
            else:
                count = min(count, int(b) * (1 << 20) // BLOCK)
        names[p['name']] = True
        recs.append(struct.pack('<IIQQ16s16sQ', 0, flags, p['first'], count, p['guid'].bytes,
                                p['name'].encode()[:16], 0))
        lines.append(f'allow lun 0 {p["name"]:<12} lba {p["first"]} +{count} flags {flags} uuid {str(p["guid"]).upper()}')
    deny.append(('GPT-HEAD', 0, gpt['first_use']))
    deny.append(('GPT-TAIL', gpt['last_use'] + 1, gpt['alt'] - gpt['last_use']))
    for p in gpt['parts']:
        if p['name'] not in names:
            deny.append((p['name'], p['first'], p['last'] - p['first'] + 1))
    if len(recs) > 8 or len(deny) > 48:
        sys.exit('too many ranges for the driver (8 allowed, 48 denied)')
    drecs = [struct.pack('<IIQQ16s', 0, 0, f, c, n.encode()[:16]) for n, f, c in deny]
    lines += [f'deny  lun 0 {n:<12} lba {f} +{c}' for n, f, c in deny]
    allow_blob = struct.pack('<IIII', ALLOW_MAGIC, VERSION, 64, len(recs)) + b''.join(recs)
    deny_blob = struct.pack('<IIII', DENY_MAGIC, VERSION, 40, len(drecs)) + b''.join(drecs)
    return allow_blob, deny_blob, lines


def selftest_gpt(path):
    """An S9-like LU 0 head: NOTES.md (phone, 2026-10-25) for BOOT and USERDATA, made-up values elsewhere."""
    layout = [('BOTA0', 6, 1024), ('BOTA1', 1030, 1024), ('EFS', 2054, 5120), ('CPEFS', 7174, 2048),
              ('PARAM', 9222, 2048), ('KEYSTORAGE', 11270, 128), ('PERSISTENT', 11398, 512), ('STEADY', 11910, 256),
              ('DTB', 12166, 2048), ('DTBO', 14214, 2048), ('BOOT', 19458, 14080), ('RECOVERY', 33538, 16384),
              ('RADIO', 49922, 18432), ('SYSTEM', 68736, 1000000), ('USERDATA', 1698432, 13917563)]
    last = 1698432 + 13917563 - 1
    blob = bytearray(6 * BLOCK)
    hdr = struct.pack('<8sIIIIQQQQ16sQII', b'EFI PART', 0x10000, 92, 0, 0, 1, last + 6, 6, last,
                      bytes(16), 2, 128, 128)
    blob[BLOCK:BLOCK + len(hdr)] = hdr
    for i, (n, f, c) in enumerate(layout):
        e = struct.pack('<16s16sQQQ72s', uuid.uuid4().bytes_le, uuid.uuid5(uuid.NAMESPACE_DNS, n).bytes_le, f,
                        f + c - 1, 0, n.encode('utf-16-le'))
        blob[2 * BLOCK + i * 128:2 * BLOCK + i * 128 + 128] = e
    Path(path).write_bytes(bytes(blob))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gpt')
    ap.add_argument('--allow', action='append', default=[])
    ap.add_argument('--out')
    ap.add_argument('--selftest-gpt')
    a = ap.parse_args()
    if a.selftest_gpt:
        selftest_gpt(a.selftest_gpt)
        return
    if not (a.gpt and a.allow and a.out):
        ap.error('--gpt, --allow and --out are needed')
    allow, deny, lines = pack(read_gpt(Path(a.gpt).read_bytes()), a.allow)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'allowed-ranges.bin').write_bytes(allow)
    (out / 'denied-ranges.bin').write_bytes(deny)
    (out / 'fence.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
