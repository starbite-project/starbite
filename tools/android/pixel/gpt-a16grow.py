#!/usr/bin/env python3
"""gpt-a16grow.py: grow A16DATA (sda28) downwards by shrinking WINDOWS (sda27). Host only, never touches a phone.

  gpt-a16grow.py show HEAD TAIL
      parse and check a GPT backup (HEAD = first 6+ blocks of sda, e.g. backup/<date>/sda-head-4M.img;
      TAIL = last 5 blocks, sda-tail-gpt.img) and print the table.
  gpt-a16grow.py gen HEAD TAIL OUTDIR [--a16-gib 16] [--keep-type]
      write OUTDIR/primary.bin (blocks 0..5) and OUTDIR/backup.bin (last 5 blocks) for the new table,
      OUTDIR/primary-orig.bin + backup-orig.bin (the restore set, cut from the same backup), and
      OUTDIR/MANIFEST.txt (sizes, sha256, the NTFS size limit for WINDOWS).
  gpt-a16grow.py verify HEAD TAIL OUTDIR
      re-run every check on an existing OUTDIR (used again right before the phone write).

What changes, and nothing else (checked byte by byte):
  entry 27 WINDOWS  last LBA   13518591 -> A16_FIRST - 1
  entry 28 A16DATA  first LBA  13518592 -> A16_FIRST   (last LBA 15615994 unchanged)
  entry 28 type     Microsoft basic data -> Linux filesystem data (so Windows gives it no drive letter;
                    --keep-type leaves it); entry 28's unique GUID and name stay
  header CRCs (entry-array CRC and header CRC), in both copies.
Entries 1-26 (Samsung's 1-24, USERDATA, WINESP), entries 29-128, the protective MBR, disk GUID, header
fields and the alternate-LBA layout are byte-identical, or the tool refuses.
The tool refuses unless the input is exactly S9 #1's layout A (docs/STARBITE-MASTER-PLAN.md section 2.2).
"""
import hashlib, os, struct, sys, uuid, zlib

BS = 4096
TOTAL = 15616000                      # sda in 4 KiB blocks (62464000 KiB in /proc/partitions)
ENTRIES_LBA, NENT, ESZ = 2, 128, 128
EBLOCKS = NENT * ESZ // BS            # 4
LINUX_DATA = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')
MS_BASIC = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
LAYOUT_A = {25: ('USERDATA', 1698432, 6613759), 26: ('WINESP', 6613760, 6744831),
            27: ('WINDOWS', 6744832, 13518591), 28: ('A16DATA', 13518592, 15615994)}
SAMSUNG_END = 1698431                 # entries 1-24 lie below this
WIN_FIRST, A16_LAST = 6744832, 15615994


def die(msg):
    sys.exit(f'REFUSED: {msg}')


def parse_header(blk, where):
    if blk[:8] != b'EFI PART':
        die(f'{where}: no "EFI PART" signature')
    f = struct.unpack('<8sIIIIQQQQ16sQIII', blk[:92])
    hsz = f[2]
    h = bytearray(blk[:hsz]); h[16:20] = b'\0\0\0\0'
    if zlib.crc32(h) != f[3]:
        die(f'{where}: header CRC wrong')
    return dict(rev=f[1], hsz=hsz, crc=f[3], cur=f[5], alt=f[6], first=f[7], last=f[8],
                disk=uuid.UUID(bytes_le=f[9]), elba=f[10], n=f[11], esz=f[12], ecrc=f[13])


def parse_entries(arr):
    ents = {}
    for i in range(NENT):
        e = arr[i * ESZ:(i + 1) * ESZ]
        t, u, first, last, attr = struct.unpack('<16s16sQQQ', e[:56])
        if first == 0 and last == 0 and t == bytes(16):
            continue
        ents[i + 1] = dict(type=uuid.UUID(bytes_le=t), guid=uuid.UUID(bytes_le=u), first=first, last=last,
                           attr=attr, name=e[56:128].decode('utf-16le').rstrip('\0'))
    return ents


def load(head_path, tail_path):
    head = open(head_path, 'rb').read()
    tail = open(tail_path, 'rb').read()
    if len(head) < 6 * BS:
        die(f'{head_path}: shorter than 6 blocks')
    if len(tail) != 5 * BS:
        die(f'{tail_path}: {len(tail)} bytes, expected the last 5 blocks ({5 * BS})')
    primary = head[:6 * BS]
    ph = parse_header(primary[BS:2 * BS], 'primary')
    bh = parse_header(tail[4 * BS:5 * BS], 'backup')
    parr = primary[2 * BS:6 * BS]
    barr = tail[:4 * BS]
    for h, arr, nm in ((ph, parr, 'primary'), (bh, barr, 'backup')):
        if (h['n'], h['esz']) != (NENT, ESZ):
            die(f'{nm}: {h["n"]} x {h["esz"]} entries, expected 128 x 128')
        if zlib.crc32(arr) != h['ecrc']:
            die(f'{nm}: entry array CRC wrong')
    if parr != barr:
        die('primary and backup entry arrays differ')
    if (ph['cur'], ph['alt'], ph['elba']) != (1, TOTAL - 1, 2):
        die(f'primary header LBAs {ph["cur"]}/{ph["alt"]}/{ph["elba"]}: not this phone (expected 1/{TOTAL - 1}/2)')
    if (bh['cur'], bh['alt'], bh['elba']) != (TOTAL - 1, 1, TOTAL - 5):
        die(f'backup header LBAs {bh["cur"]}/{bh["alt"]}/{bh["elba"]}: not this phone')
    if ph['last'] != A16_LAST or bh['last'] != A16_LAST or ph['disk'] != bh['disk']:
        die('last usable LBA or disk GUID differ from S9 #1')
    if primary[510:512] != b'\x55\xaa' or primary[450] != 0xEE:
        die('block 0 is not a protective MBR')
    ents = parse_entries(parr)
    return primary, tail, ph, bh, ents


def check_layout_a(ents):
    if sorted(ents) != list(range(1, 29)):
        die(f'entries {sorted(ents)}: expected exactly 1..28')
    for i in range(1, 25):
        if ents[i]['last'] > SAMSUNG_END:
            die(f'entry {i} {ents[i]["name"]} ends at {ents[i]["last"]}, past Samsung\'s area')
    for i, (nm, a, b) in LAYOUT_A.items():
        e = ents[i]
        if (e['name'], e['first'], e['last']) != (nm, a, b):
            die(f'entry {i} is {e["name"]} {e["first"]}..{e["last"]}, expected {nm} {a}..{b} (layout A)')


def show(ents, title):
    print(title)
    for i, e in sorted(ents.items()):
        n = e['last'] - e['first'] + 1
        print(f'  {i:3d} {e["name"]:12s} {e["first"]:>9d}..{e["last"]:<9d} {n * BS / 2**30:8.3f} GiB  {e["type"]}')


def build(primary, tail, ents, a16_first, keep_type):
    arr = bytearray(primary[2 * BS:6 * BS])
    o27, o28 = 26 * ESZ, 27 * ESZ
    struct.pack_into('<Q', arr, o27 + 40, a16_first - 1)          # WINDOWS last LBA
    struct.pack_into('<Q', arr, o28 + 32, a16_first)              # A16DATA first LBA
    if not keep_type:
        arr[o28:o28 + 16] = LINUX_DATA.bytes_le
    arr = bytes(arr)
    ecrc = zlib.crc32(arr)

    def hdr(blk):
        h = bytearray(blk)
        hsz = struct.unpack('<I', h[12:16])[0]
        struct.pack_into('<I', h, 88, ecrc)
        h[16:20] = b'\0\0\0\0'
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:hsz])))
        return bytes(h)
    new_primary = primary[:BS] + hdr(primary[BS:2 * BS]) + arr
    new_backup = arr + hdr(tail[4 * BS:5 * BS])
    return new_primary, new_backup


def verify(primary, tail, new_primary, new_backup, a16_first, keep_type):
    """Every byte that may differ is listed; anything else differing is a refusal."""
    if new_primary[:BS] != primary[:BS]:
        die('protective MBR would change')
    allowed = set()
    for base in (2 * BS,):                      # entries inside the primary image
        allowed |= set(range(base + 26 * ESZ + 40, base + 26 * ESZ + 48))
        allowed |= set(range(base + 27 * ESZ + 32, base + 27 * ESZ + 40))
        if not keep_type:
            allowed |= set(range(base + 27 * ESZ, base + 27 * ESZ + 16))
    allowed |= {BS + 16 + k for k in range(4)} | {BS + 88 + k for k in range(4)}
    diff = {i for i in range(len(primary)) if primary[i] != new_primary[i]}
    if diff - allowed:
        die(f'primary: unexpected changed bytes at {sorted(diff - allowed)[:8]}')
    allowed_b = {i - 2 * BS for i in allowed if 2 * BS <= i < 6 * BS}
    allowed_b |= {4 * BS + 16 + k for k in range(4)} | {4 * BS + 88 + k for k in range(4)}
    diff_b = {i for i in range(len(tail)) if tail[i] != new_backup[i]}
    if diff_b - allowed_b:
        die(f'backup: unexpected changed bytes at {sorted(diff_b - allowed_b)[:8]}')
    ph = parse_header(new_primary[BS:2 * BS], 'new primary')
    bh = parse_header(new_backup[4 * BS:], 'new backup')
    if zlib.crc32(new_primary[2 * BS:]) != ph['ecrc'] or zlib.crc32(new_backup[:4 * BS]) != bh['ecrc']:
        die('new entry CRC wrong')
    ents = parse_entries(new_primary[2 * BS:])
    old = parse_entries(primary[2 * BS:6 * BS])
    for i in range(1, 27):
        if ents[i] != old[i]:
            die(f'entry {i} changed')
    w, a = ents[27], ents[28]
    if (w['first'], w['last'], a['first'], a['last']) != (WIN_FIRST, a16_first - 1, a16_first, A16_LAST):
        die('new 27/28 ranges wrong')
    if (w['guid'], w['name'], a['guid'], a['name']) != (old[27]['guid'], 'WINDOWS', old[28]['guid'], 'A16DATA'):
        die('27/28 GUID or name changed')
    return ents


def main():
    a = sys.argv[1:]
    if not a or a[0] not in ('show', 'gen', 'verify'):
        sys.exit(__doc__)
    gib = 16
    if '--a16-gib' in a:
        i = a.index('--a16-gib'); gib = int(a[i + 1]); del a[i:i + 2]
    keep_type = '--keep-type' in a
    a = [x for x in a if x != '--keep-type']
    cmd, head, tail_p = a[0], a[1], a[2]
    primary, tail, ph, bh, ents = load(head, tail_p)
    check_layout_a(ents)
    if cmd == 'show':
        show(ents, f'{head}: disk {ph["disk"]}, layout A, both copies valid')
        return
    out = a[3]
    a16_first = A16_LAST + 1 - (gib << 30) // BS                    # 4194304 blocks per 16 GiB, ...
    a16_first = (a16_first // 256) * 256                           # 1 MiB aligned, rounds A16DATA up
    if not (8 <= gib <= 20) or a16_first <= WIN_FIRST:
        die(f'--a16-gib {gib}: outside 8..20 (WINDOWS holds 13.86 GiB of data)')
    new_primary, new_backup = build(primary, tail, ents, a16_first, keep_type)
    new = verify(primary, tail, new_primary, new_backup, a16_first, keep_type)
    win_bytes = (a16_first - WIN_FIRST) * BS
    if cmd == 'verify':
        for nm, want in (('primary.bin', new_primary), ('backup.bin', new_backup),
                         ('primary-orig.bin', primary), ('backup-orig.bin', tail)):
            if open(os.path.join(out, nm), 'rb').read() != want:
                die(f'{out}/{nm} differs from what this backup gives')
        show(new, f'{out}: verified against {head}')
        return
    os.makedirs(out, exist_ok=True)
    files = {'primary.bin': new_primary, 'backup.bin': new_backup,
             'primary-orig.bin': primary, 'backup-orig.bin': tail}
    for nm, d in files.items():
        open(os.path.join(out, nm), 'wb').write(d)
    with open(os.path.join(out, 'MANIFEST.txt'), 'w') as f:
        f.write(f'source_head {head} sha256 {hashlib.sha256(open(head, "rb").read()).hexdigest()}\n')
        f.write(f'source_tail {tail_p} sha256 {hashlib.sha256(tail).hexdigest()}\n')
        f.write(f'a16data_first {a16_first} last {A16_LAST} blocks {A16_LAST - a16_first + 1} '
                f'({(A16_LAST - a16_first + 1) * BS / 2**30:.3f} GiB)\n')
        f.write(f'windows_first {WIN_FIRST} last {a16_first - 1} bytes {win_bytes} '
                f'({win_bytes / 2**30:.3f} GiB)\n')
        f.write(f'ntfsresize_size {win_bytes}   # ntfsresize --size: exactly the new WINDOWS, so the NTFS backup boot '
                f'sector lands in its last sector (rehearsed: tools/android/pixel/ntfs-rehearse.sh)\n')
        f.write(f'a16data_type {"kept" if keep_type else LINUX_DATA}\n')
        for nm, d in files.items():
            f.write(f'{nm} {len(d)} sha256 {hashlib.sha256(d).hexdigest()}\n')
    show(ents, 'before:')
    show(new, 'after:')
    print(open(os.path.join(out, 'MANIFEST.txt')).read())


if __name__ == '__main__':
    main()
