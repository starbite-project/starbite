#!/usr/bin/env python3
"""A fake Galaxy S9 for installer rehearsals: a directory that stands in for the phone, and the
logic behind the fake adb, fastboot and heimdall next to this file. Works on macOS and Linux
(sparse files: the 64 GB UFS and the 128 GB card cost almost no disk).

  fakephone.py create DIR [--state recovery|download|fastboot|android] [--card blank|starbite|foreign]
                          [--card-sectors N] [--layout sd128|sd64] [--battery N]
      --card starbite lays the card out as --layout (default sd128, which needs the 249999360-sector card)
  fakephone.py rereadpt DIR          (the fake `blockdev --rereadpt /dev/block/mmcblk0`)
  fakephone.py mkexfat DIR NODE      (the fake mkfs.exfat)

Layout: the main phone's GPT ("layout A", docs/ANDROID-PIXEL.md 2.1) for SYSTEM..A16DATA, at
their real block numbers and sizes; Samsung's small partitions are scaled down and filled with
random bytes, so backups and the "protected areas untouched" check mean something. Fake
limitations (docs/INSTALLER.md): partition nodes are separate files, not windows into one disk
file; `[ -b` is read as `[ -e`; mke2fs/e2fsck/blkid/mkfs.exfat/mount are shell stand-ins."""
import os
import random
import struct
import sys

STATE_USB = {'download': '04e8:685d', 'fastboot': '18d1:4ee0'}
# (sdaN, name, KiB, fill) for Samsung's partitions; starts are allocated from block 6
SAMSUNG = [(1, 'BOTA0', 1024), (2, 'BOTA1', 1024), (3, 'EFS', 2048), (4, 'CPEFS', 1024), (5, 'PARAM', 1024),
           (6, 'UP_PARAM', 1024), (7, 'KEYSTORAGE', 512), (8, 'PERSISTENT', 512), (9, 'STEADY', 512),
           (10, 'BOTA2', 1024), (11, 'MISC', 1024), (12, 'DTB', 1024), (13, 'DTBO', 1024),
           (14, 'BOOT', 56320), (15, 'RECOVERY', 66560), (16, 'RADIO', 2048), (17, 'OMR', 1024)]
# (sdaN, name, start block, blocks): S9 #1's layout A
STARBITE = [(18, 'SYSTEM', 68736, 1245184), (19, 'VENDOR', 1313920, 185600), (20, 'ODM', 1499520, 179200),
            (25, 'USERDATA', 1698432, 4915328), (26, 'WINESP', 6613760, 131072),
            (27, 'WINDOWS', 6744832, 6773760), (28, 'A16DATA', 13518592, 2097403)]
UFS_BLOCKS = 15616000
sys.dont_write_bytecode = True                     # no __pycache__ in tools/store
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', '..', 'store'))
import cardlayout  # noqa: E402  (tools/store/cardlayout.py: the card layouts)
CARD_SECTORS = cardlayout.get('sd128').exact                     # 249999360
CARD_P1, CARD_P2 = cardlayout.get('sd128').parts(CARD_SECTORS)    # (0x07, 8192, 182878208), (0xDA, 182886400, 67112960)
MIB = 1 << 20


def sparse(path, size, head=b'', tail=b''):
    with open(path, 'wb') as f:
        f.truncate(size)
        if head:
            f.write(head)
        if tail:
            f.seek(size - len(tail))
            f.write(tail)


def write_state(root, state):
    open(os.path.join(root, 'state'), 'w').write(state + '\n')
    open(os.path.join(root, 'usb'), 'w').write(STATE_USB.get(state, '') + '\n')


def read_state(root):
    return open(os.path.join(root, 'state')).read().strip()


def mbr(parts):
    m = bytearray(512)
    m[440:444] = b'OLD!'
    for i, (t, s, c) in enumerate(parts):
        struct.pack_into('<B3sB3sII', m, 446 + 16 * i, 0, b'\xfe\xff\xff', t, b'\xfe\xff\xff', s, c)
    m[510:512] = b'\x55\xaa'
    return bytes(m)


def create(root, state='recovery', card='blank', card_sectors=CARD_SECTORS, battery=87, seed=9, layout='sd128'):
    rnd = random.Random(seed)
    rb = lambda n: bytes(rnd.getrandbits(8) for _ in range(n)) if n <= 4096 else os.urandom(n)  # noqa: E731
    dev, byname = os.path.join(root, 'dev', 'block'), os.path.join(root, 'dev', 'block', 'by-name')
    for d in (byname, os.path.join(root, 'tmp'), os.path.join(root, 'proc'), os.path.join(root, 'blkid'),
              os.path.join(root, 'sys', 'block', 'sda'), os.path.join(root, 'sys', 'block', 'mmcblk0', 'device'),
              os.path.join(root, 'sys', 'class', 'power_supply', 'battery')):
        os.makedirs(d, exist_ok=True)

    def node(n, name, start, blocks, fill):
        p = os.path.join(dev, f'sda{n}')
        size = blocks * 4096
        if fill == 'rand':
            with open(p, 'wb') as f:
                f.write(os.urandom(size))
        else:                                   # stale data at both ends, sparse in between
            sparse(p, size, os.urandom(MIB), os.urandom(MIB))
        link = os.path.join(byname, name)
        if os.path.lexists(link):
            os.unlink(link)
        os.symlink(p, link)
        sd = os.path.join(root, 'sys', 'class', 'block', f'sda{n}')
        os.makedirs(sd, exist_ok=True)
        open(os.path.join(sd, 'start'), 'w').write(f'{start * 8}\n')
        open(os.path.join(sd, 'size'), 'w').write(f'{blocks * 8}\n')

    blk = 6
    for n, name, kib in SAMSUNG:
        node(n, name, blk, kib // 4, 'rand' if kib <= 2048 else 'stale')
        blk += kib // 4
    for n, name, start, blocks in STARBITE:
        node(n, name, start, blocks, 'stale')
    # whole UFS LU0 (for the GPT head/tail backup) and the boot LUNs
    head = bytearray(os.urandom(4 * MIB))
    head[0:512] = mbr([(0xEE, 1, 0xFFFFFFFF)])
    head[4096:4104] = b'EFI PART'
    sparse(os.path.join(dev, 'sda'), UFS_BLOCKS * 4096, bytes(head), os.urandom(5 * 4096))
    for n, mib in (('sdb', 4), ('sdc', 4), ('sdd', 2), ('sde', 2)):
        with open(os.path.join(dev, n), 'wb') as f:
            f.write(os.urandom(mib * MIB))
    open(os.path.join(root, 'sys', 'block', 'sda', 'size'), 'w').write(f'{UFS_BLOCKS * 8}\n')
    # the card
    card_path = os.path.join(dev, 'mmcblk0')
    sparse(card_path, card_sectors * 512)
    open(os.path.join(root, 'sys', 'block', 'mmcblk0', 'size'), 'w').write(f'{card_sectors}\n')
    gb = card_sectors * 512 / 1e9
    open(os.path.join(root, 'sys', 'block', 'mmcblk0', 'device', 'name'), 'w').write(
        f'SD{128 if gb > 96 else 64 if gb > 48 else 32}\n')
    open(os.path.join(root, 'proc', 'mounts'), 'w').write('')
    if card in ('starbite', 'foreign'):
        parts = cardlayout.get(layout).parts(card_sectors) if card == 'starbite' else [(0x0C, 2048, card_sectors - 2048)]
        with open(card_path, 'r+b') as f:
            f.write(mbr(parts))
        rereadpt(root)
        if card == 'starbite':
            mkexfat(root, os.path.join(dev, 'mmcblk0p1'))
            ext = os.path.join(root, 'external_sd')
            os.makedirs(ext, exist_ok=True)
            open(os.path.join(root, 'proc', 'mounts'), 'a').write(f'/dev/fuse {ext} fuseblk rw 0 0\n')
    open(os.path.join(root, 'sys', 'class', 'power_supply', 'battery', 'capacity'), 'w').write(f'{battery}\n')
    props = {'ro.product.model': 'SM-G960F', 'ro.product.device': 'starlte', 'ro.boot.bootloader': 'G960FXXUHFVG6',
             'ro.boot.serialno': 'FAKE0000SERIAL', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange',
             'ro.boot.warranty_bit': '1', 'ro.twrp.version': '3.7.0_9-0'}
    open(os.path.join(root, 'props'), 'w').write(''.join(f'{k}={v}\n' for k, v in props.items()))
    write_state(root, state)
    print(f'fake phone in {root}: state {state}, card {card}{f" {layout}" if card == "starbite" else ""} '
          f'({card_sectors} sectors)')


def rereadpt(root):
    dev = os.path.join(root, 'dev', 'block')
    with open(os.path.join(dev, 'mmcblk0'), 'rb') as f:
        m = f.read(512)
    for n in range(1, 5):
        p = os.path.join(dev, f'mmcblk0p{n}')
        sd = os.path.join(root, 'sys', 'class', 'block', f'mmcblk0p{n}')
        t, s, c = struct.unpack_from('<4xB3xII', m, 446 + 16 * (n - 1)) if m[510:512] == b'\x55\xaa' else (0, 0, 0)
        if not c:
            for x in (p,):
                if os.path.exists(x):
                    os.unlink(x)
            continue
        if not os.path.exists(p) or os.path.getsize(p) != c * 512:
            sparse(p, c * 512)
        os.makedirs(sd, exist_ok=True)
        open(os.path.join(sd, 'start'), 'w').write(f'{s}\n')
        open(os.path.join(sd, 'size'), 'w').write(f'{c}\n')


def mkexfat(root, node_path):
    name = os.path.basename(node_path)
    n = int(name.rsplit('p', 1)[1])
    start = int(open(os.path.join(root, 'sys', 'class', 'block', name, 'start')).read())
    boot = bytearray(512)
    boot[0:3] = b'\xeb\x76\x90'
    boot[3:11] = b'EXFAT   '
    boot[510:512] = b'\x55\xaa'
    with open(node_path, 'r+b') as f:
        f.write(boot)
    with open(os.path.join(root, 'dev', 'block', 'mmcblk0'), 'r+b') as f:
        f.seek(start * 512)
        f.write(boot)
    ext = os.path.join(root, 'external_sd')
    if os.path.isdir(ext):
        import shutil
        shutil.rmtree(ext)
    print(f'fake mkfs.exfat: p{n} formatted')


if __name__ == '__main__':
    cmd, root = sys.argv[1], os.path.abspath(sys.argv[2])
    if cmd == 'create':
        a = sys.argv[3:]
        opt = lambda k, d: a[a.index(k) + 1] if k in a else d  # noqa: E731
        os.makedirs(root, exist_ok=True)
        create(root, opt('--state', 'recovery'), opt('--card', 'blank'), int(opt('--card-sectors', CARD_SECTORS)),
               int(opt('--battery', 87)), layout=opt('--layout', 'sd128'))
    elif cmd == 'rereadpt':
        rereadpt(root)
    elif cmd == 'mkexfat':
        mkexfat(root, sys.argv[3])
    else:
        sys.exit(__doc__)
