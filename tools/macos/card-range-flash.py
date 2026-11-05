#!/usr/bin/env python3
"""Write a root image into a sector range of the S9's microSD card through TWRP's adb.

  card-range-flash.py IMAGE START_SECTOR [--state FILE] [--check-only] [--layout sd128|sd64]

The range is START_SECTOR .. START_SECTOR + size/512 - 1 of /dev/block/mmcblk0. Before
anything is written it checks, on the phone:
  - adb is in TWRP (recovery) and mmcblk0 is the 128 GB card (249999360 sectors);
  - the MBR's p2 is type 0xDA and the whole range lies inside it, at or past the card
    store's used end (SBSTORE2 header, p2 block 1, CRC checked), or is exactly the
    64 MiB test range at 249868288; it never overlaps p1 (the user's exFAT);
  - START and the image size are 1 MiB multiples.
Then, as starlte-flash-root.py does for USERDATA: 64 MiB chunks; all-zero chunks are
written from /dev/zero on the phone; others are gzip'd, pushed to /tmp, written with
dd and read back for a SHA-256 comparison. --state records finished chunks (resume).
Nothing outside the range is written. --check-only runs the checks and stops.

--layout NAME (tools/store/cardlayout.py) selects a card layout explicitly: the card must fit it
(sd128: exactly the 128 GB card; sd64: any card of at least 64 GB), its MBR's p1/p2 must be that
layout's, and the range must lie inside one of the layout's root slots (or be exactly its 64 MiB
tail, the test range). Without --layout nothing changes: the 128 GB card only, as above.
"""
import argparse, gzip, hashlib, os, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

sys.dont_write_bytecode = True                     # no __pycache__ in tools/store
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'store'))
import cardlayout  # noqa: E402  (the card geometry: tools/store/cardlayout.py)

DEV = '/dev/block/mmcblk0'
SD128 = cardlayout.get('sd128')
CARD_SECTORS = SD128.exact                              # 249999360
TEST_RANGE = (SD128.tail.start, SD128.tail.sectors)     # (249868288, 131072)
CHUNK = 64 << 20
MIB = 1 << 20


def adb(args, binary=False):
    r = subprocess.run(['adb', *args], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode(errors='replace').strip()


def read_sectors(first, count):
    return adb(['exec-out', f'dd if={DEV} bs=512 skip={first} count={count} 2>/dev/null'], True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('start', type=int)
    ap.add_argument('--state')
    ap.add_argument('--check-only', action='store_true')
    ap.add_argument('--layout', choices=cardlayout.names(), help='check the card and the range against this layout')
    a = ap.parse_args()
    size = os.path.getsize(a.image)
    count = size // 512
    if size % MIB or a.start % 2048:
        sys.exit('START and the image size must be 1 MiB multiples')
    if adb(['get-state']) != 'recovery':
        sys.exit('the phone is not in TWRP')
    sectors = int(adb(['shell', 'cat /sys/block/mmcblk0/size']))
    lay, test_range = None, TEST_RANGE
    if a.layout is None:
        if sectors != CARD_SECTORS:
            sys.exit(f'mmcblk0 has {sectors} sectors, not the 128 GB card ({CARD_SECTORS}): refusing')
    else:
        lay = cardlayout.get(a.layout)
        try:
            want = lay.parts(sectors)
        except cardlayout.LayoutError as err:
            sys.exit(f'mmcblk0: {err}: refusing')
        test_range = (lay.tail.start, lay.tail.sectors)
    mbr = read_sectors(0, 1)
    if len(mbr) != 512 or mbr[510:512] != b'\x55\xaa':
        sys.exit('no MBR on the card')
    parts = [struct.unpack_from('<4xB3xII', mbr, 446 + 16 * i) for i in range(4)]
    p1, p2 = parts[0], parts[1]
    if lay is not None and parts[:2] != want:
        other = cardlayout.identify(parts, sectors)
        sys.exit(f'the card\'s MBR (p1 {p1[1]}+{p1[2]}, p2 {p2[1]}+{p2[2]}) is not the {lay.name} layout '
                 f'(p1 {want[0][1]}+{want[0][2]}, p2 {want[1][1]}+{want[1][2]})'
                 + (f': it is {other}' if other else '') + ': refusing')
    s, e = a.start, a.start + count
    if s < p1[1] + p1[2] and p1[1] < e:
        sys.exit('the range overlaps p1 (the exFAT data partition): refusing')
    if (s, count) == test_range:
        where = 'the 64 MiB test range'
    else:
        if lay is not None:
            slot = lay.area_at(s, count)
            if slot is None or slot.kind != 'root':
                roots = ', '.join(f'{r.name} {r.start}+{r.sectors}' for r in lay.roots())
                sys.exit(f'the range {s}+{count} is not inside a root slot of the {lay.name} layout ({roots}): refusing')
        if p2[0] != 0xDA or s < p2[1] or e > p2[1] + p2[2]:
            sys.exit(f'the range {s}+{count} is not inside p2 (type 0x{p2[0]:02x} {p2[1]}+{p2[2]}): refusing')
        hdr = read_sectors(p2[1] + 8, 1)
        if hdr[:8] == b'SBSTORE2':
            if zlib.crc32(hdr[:48]) != struct.unpack_from('<I', hdr, 48)[0]:
                sys.exit('p2 store header CRC bad: refusing')
            used, = struct.unpack_from('<Q', hdr, 24)
            if s < p2[1] + used * 8:
                sys.exit(f'the range starts below the card store\'s used end (sector {p2[1] + used * 8}): refusing')
            where = f'p2 past the store\'s used end (block {used})'
        elif read_sectors(p2[1], 1)[:8] == b'SBSTORE1':
            sys.exit('p2 has an SBSTORE1 store without a v2 header: used end unknown, refusing')
        else:
            where = 'p2, which holds no store'
    print(f'{a.image}: {size >> 20} MiB -> {DEV} sectors {s}..{e - 1}, inside {where}')
    if a.check_only:
        return
    state = a.state or f'build/macos13/card-flash-{s}.state'
    done = set(open(state).read().split()) if os.path.exists(state) else set()
    with open(a.image, 'rb') as f, tempfile.TemporaryDirectory() as tmp:
        for i in range((size + CHUNK - 1) // CHUNK):
            data = f.read(CHUNK)
            if str(i) in done:
                continue
            seek = s // 2048 + i * (CHUNK // MIB)          # MiB units
            n = len(data) // MIB
            if not data.strip(b'\0'):
                adb(['shell', f'dd if=/dev/zero of={DEV} bs=1048576 seek={seek} count={n} conv=fsync 2>/dev/null'])
            else:
                gz = os.path.join(tmp, 'chunk.gz')
                with gzip.open(gz, 'wb', compresslevel=1) as g:
                    g.write(data)
                adb(['push', gz, '/tmp/chunk.gz'])
                adb(['shell', f'gunzip -c /tmp/chunk.gz | dd of={DEV} bs=1048576 seek={seek} conv=fsync 2>/dev/null;'
                     ' rm -f /tmp/chunk.gz'])
            back = adb(['shell', f'dd if={DEV} bs=1048576 skip={seek} count={n} 2>/dev/null | sha256sum']).split()[0]
            if back != hashlib.sha256(data).hexdigest():
                sys.exit(f'chunk {i}: read-back SHA-256 differs: stop (state kept in {state})')
            with open(state, 'a') as st:
                st.write(f'{i}\n')
            print(f'  chunk {i}: {n} MiB ok', flush=True)
    whole = adb(['shell', f'dd if={DEV} bs=1048576 skip={s // 2048} count={size // MIB} 2>/dev/null | sha256sum']).split()[0]
    h = hashlib.sha256(open(a.image, 'rb').read()).hexdigest() if size <= 256 * MIB else None
    print(f'range SHA-256 on the card: {whole}' + ('' if h is None else f' ({"matches" if h == whole else "DIFFERS from"} the image)'))


if __name__ == '__main__':
    main()
