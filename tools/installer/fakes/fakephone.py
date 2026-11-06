#!/usr/bin/env python3
"""A fake Galaxy S9 for installer rehearsals: a directory that stands in for the phone, and the
logic behind the fake adb, fastboot and heimdall next to this file. Works on macOS and Linux
(sparse files: the 64 GB UFS and the 128 GB card cost almost no disk).

  fakephone.py create DIR [--state fastboot|download|android|env|recovery] [--card blank|starbite|foreign]
                          [--card-sectors N] [--layout sd128|sd64] [--battery N]
                          [--gpt starbite-a|stock|LAYOUT] [--stock] [--model SM-G960F|SM-G965F]
      --card starbite lays the card out as --layout (default sd128, which needs the 249999360-sector card)
      --gpt: the UFS GPT the fake sda carries (a layout of tools/installer/sbinst/ufslayout.py; stock =
      Samsung's table with random GUIDs from SYSTEM on, as a stock phone has); --stock: stock Samsung
      firmware (its Android sets ro.build.PDA; RECOVERY holds the stock recovery: no adb there)
  fakephone.py rereadpt DIR          (the fake `blockdev --rereadpt /dev/block/mmcblk0`)
  fakephone.py rereadpt-sda DIR      (the kernel reading sda's GPT: at boot and `blockdev --rereadpt sda`)
  fakephone.py mkexfat DIR NODE      (the fake mkfs.exfat)

UFS: sda is one sparse file holding a real GPT (protective MBR, both copies) built from the layout
table; the partition nodes sdaN are made from that GPT the way Linux does it (block/partitions/efi.c:
the primary is used when it is valid, the backup is never used without gpt=force, an invalid primary
means no partitions; this parser is independent of sbinst/gpt.py). A re-read keeps a node whose place
is unchanged, keeps the contents of one that only changed size (same start), and gives a moved or new
one stale bytes at both ends, as a re-laid UFS has. Samsung's small partitions are filled with random
bytes, big ones have random ends, so backups and the "protected areas untouched" check mean
something. CPEFS is sdd1 (as on S9 #1). Fake limitations (docs/INSTALLER.md 8): partition nodes are
separate files, not windows into the sda file; `[ -b` is read as `[ -e`; mke2fs/e2fsck/blkid/
mkfs.exfat/mount are shell stand-ins."""
import os
import random
import struct
import sys
import uuid
import zlib

STATE_USB = {'download': '04e8:685d', 'fastboot': '18d1:4ee0'}
UFS_BLOCKS = 15616000
sys.dont_write_bytecode = True                     # no __pycache__ in tools/store or sbinst
HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'store'))
sys.path.insert(0, os.path.join(HERE, '..'))
import cardlayout  # noqa: E402  (tools/store/cardlayout.py: the card layouts)
from sbinst import gpt, pit, ufslayout  # noqa: E402  (the layout table; the engine builds the fake's GPT bytes)
CARD_SECTORS = cardlayout.get('sd128').exact                     # 249999360
CARD_P1, CARD_P2 = cardlayout.get('sd128').parts(CARD_SECTORS)    # (0x07, 8192, 182878208), (0xDA, 182886400, 67112960)
MIB = 1 << 20
BS = 4096


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
    try:
        os.unlink(os.path.join(root, 'heimdall-session'))          # a reboot ends a Download-mode session
    except OSError:
        pass
    if state != 'download':
        rereadpt_sda(root)                                         # the kernel reads the GPT at boot


def read_state(root):
    return open(os.path.join(root, 'state')).read().strip()


def mbr(parts):
    m = bytearray(512)
    m[440:444] = b'OLD!'
    for i, (t, s, c) in enumerate(parts):
        struct.pack_into('<B3sB3sII', m, 446 + 16 * i, 0, b'\xfe\xff\xff', t, b'\xfe\xff\xff', s, c)
    m[510:512] = b'\x55\xaa'
    return bytes(m)


def gpt_bytes(layout_name, rnd, total=UFS_BLOCKS):
    """(LBA 0..5, the last 5 LBAs) of a GPT laid out by a ufslayout table, with Samsung's header
    (revision 1.0, header size 512, disk GUID "ANDROID MMC DISK"). `stock`: random unique GUIDs from
    SYSTEM on, as a phone fresh from Samsung's PIT has."""
    L = ufslayout.get(layout_name)
    ents = []
    for i, p, s, e in L.resolve(6, total - 6):
        g = p.unique_guid()
        if layout_name == 'stock' and p.content != ufslayout.PROTECTED:
            g = str(uuid.UUID(bytes=bytes(rnd.getrandbits(8) for _ in range(16)), version=4))
        ents.append(gpt.Entry(i, p.name, s, e, p.type, g, p.attrs))
    hdr = bytearray(BS)
    arr = gpt.build_array(ents, 128, 128)
    struct.pack_into('<8sIIIIQQQQ16sQIII', hdr, 0, b'EFI PART', 0x10000, 512, 0, 0, 1, total - 1, 6, total - 6,
                     uuid.UUID(ufslayout.SAMSUNG_DISK_GUID).bytes_le, 2, 128, 128, zlib.crc32(arr))
    struct.pack_into('<I', hdr, 16, zlib.crc32(bytes(hdr[:512])))
    pmbr = bytearray(BS)
    struct.pack_into('<B3sB3sII', pmbr, 446, 0, bytes(3), 0xEE, bytes(3), 1, min(total - 1, 0xFFFFFFFF))  # as Samsung writes it
    pmbr[510:512] = b'\x55\xaa'
    disk = gpt.Disk(BS, total, bytes(pmbr) + bytes(hdr) + arr, b'')
    disk.primary = gpt.parse_copy(bytes(hdr), arr, 'fake')
    prim, back = gpt.build_copies(disk, ents)
    return bytes(pmbr) + prim, back


def create(root, state='fastboot', card='blank', card_sectors=CARD_SECTORS, battery=87, seed=9, layout='sd128',
           gpt_layout='starbite-a', stock=False, model='SM-G960F'):
    rnd = random.Random(seed)
    dev = os.path.join(root, 'dev', 'block')
    for d in (os.path.join(dev, 'by-name'), os.path.join(root, 'tmp'), os.path.join(root, 'proc'),
              os.path.join(root, 'blkid'), os.path.join(root, 'sys', 'block', 'sda', 'queue'),
              os.path.join(root, 'sys', 'block', 'mmcblk0', 'device'),
              os.path.join(root, 'sys', 'class', 'power_supply', 'battery')):
        os.makedirs(d, exist_ok=True)
    # UFS LU0: protective MBR + GPT (both copies), random bytes after the primary (the PIT area)
    head, tail = gpt_bytes(gpt_layout, rnd)
    sparse(os.path.join(dev, 'sda'), UFS_BLOCKS * BS, head + os.urandom(250 * BS), tail)
    open(os.path.join(root, 'sys', 'block', 'sda', 'size'), 'w').write(f'{UFS_BLOCKS * 8}\n')
    open(os.path.join(root, 'sys', 'block', 'sda', 'queue', 'logical_block_size'), 'w').write(f'{BS}\n')
    rereadpt_sda(root)
    # the boot LUNs; CPEFS is on sdd (sdd1), not in sda's GPT
    for n, mib in (('sdb', 4), ('sdc', 4), ('sdd', 2), ('sde', 2)):
        with open(os.path.join(dev, n), 'wb') as f:
            f.write(os.urandom(mib * MIB))
    with open(os.path.join(dev, 'sdd1'), 'wb') as f:
        f.write(os.urandom(MIB))
    _link(root, 'CPEFS', 'sdd1', 256 * 8, 256 * 8)
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
    device = 'starlte' if model == 'SM-G960F' else 'star2lte'
    bl = model[3:] + 'XXUHFVG6'
    props = {'ro.product.model': model, 'ro.product.device': device, 'ro.boot.bootloader': bl,
             'ro.boot.serialno': 'FAKE0000SERIAL', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange',
             'ro.boot.warranty_bit': '1', 'ro.twrp.version': '3.7.0_9-0', 'ro.starbite.env': 'fake environment'}
    open(os.path.join(root, 'props'), 'w').write(''.join(f'{k}={v}\n' for k, v in props.items()))
    android = {k: v for k, v in props.items() if k not in ('ro.twrp.version', 'ro.starbite.env')}
    if stock:
        android.update({'ro.build.PDA': bl, 'ro.build.version.oneui': '20100', 'ro.product.system.brand': 'samsung'})
    else:
        android.update({'ro.lineage.version': '24.0', 'ro.product.system.brand': 'google'})
    open(os.path.join(root, 'props-android'), 'w').write(''.join(f'{k}={v}\n' for k, v in android.items()))
    open(os.path.join(root, 'recovery-kind'), 'w').write('stock\n' if stock and state != 'recovery' else 'twrp\n')
    open(os.path.join(root, 'pit.bin'), 'wb').write(pit.build(pit.stock_entries(model)))
    write_state(root, state)
    print(f'fake phone in {root}: state {state}, {"stock firmware, " if stock else ""}{model}, GPT {gpt_layout}, '
          f'card {card}{f" {layout}" if card == "starbite" else ""} ({card_sectors} sectors)')


def _link(root, name, node, start, size):
    byname = os.path.join(root, 'dev', 'block', 'by-name', name)
    if os.path.lexists(byname):
        os.unlink(byname)
    os.symlink(os.path.join(root, 'dev', 'block', node), byname)
    sd = os.path.join(root, 'sys', 'class', 'block', node)
    os.makedirs(sd, exist_ok=True)
    open(os.path.join(sd, 'start'), 'w').write(f'{start}\n')
    open(os.path.join(sd, 'size'), 'w').write(f'{size}\n')


def kernel_gpt(sda):
    """What Linux 4.9 makes of sda (efi.c without gpt=force): [(index, name, first, last)] from a valid
    primary GPT (protective MBR, header CRC, entry-array CRC), else None."""
    with open(sda, 'rb') as f:
        head = f.read(6 * BS)
    if head[510:512] != b'\x55\xaa' or not any(head[446 + 16 * i + 4] == 0xEE for i in range(4)):
        return None
    h = head[BS:2 * BS]
    if h[:8] != b'EFI PART':
        return None
    hsz, hcrc = struct.unpack_from('<II', h, 12)
    x = bytearray(h[:hsz])
    x[16:20] = bytes(4)
    if zlib.crc32(x) != hcrc:
        return None
    my, alt, first, last = struct.unpack_from('<QQQQ', h, 24)
    elba, n, esz, ecrc = struct.unpack_from('<QIII', h, 72)
    arr = head[elba * BS:elba * BS + n * esz]
    if my != 1 or zlib.crc32(arr) != ecrc:
        return None
    out = []
    for i in range(n):
        t, u, a, b = struct.unpack_from('<16s16sQQ', arr, i * esz)
        if t != bytes(16):
            out.append((i + 1, arr[i * esz + 56:i * esz + 128].decode('utf-16le').split('\0')[0], a, b))
    return out


def rereadpt_sda(root):
    dev = os.path.join(root, 'dev', 'block')
    sda = os.path.join(dev, 'sda')
    if not os.path.exists(sda):
        return
    want = {f'sda{i}': (name, a, b) for i, name, a, b in (kernel_gpt(sda) or [])}
    for f in os.listdir(dev):                                     # partitions that are gone
        if f.startswith('sda') and f[3:].isdigit() and f not in want:
            os.unlink(os.path.join(dev, f))
    for name in os.listdir(os.path.join(dev, 'by-name')):
        link = os.path.join(dev, 'by-name', name)
        tgt = os.path.basename(os.readlink(link))
        if tgt.startswith('sda') and (tgt not in want or want[tgt][0] != name):
            os.unlink(link)
    for node, (name, a, b) in sorted(want.items(), key=lambda kv: int(kv[0][3:])):
        p = os.path.join(dev, node)
        sd = os.path.join(root, 'sys', 'class', 'block', node, 'start')
        size = (b - a + 1) * BS
        old_start = int(open(sd).read()) // 8 if os.path.exists(sd) else None
        if os.path.exists(p) and old_start == a:
            if os.path.getsize(p) != size:
                with open(p, 'r+b') as f:                         # same start, new size: contents stay
                    f.truncate(size)
        elif size <= 2 * MIB:
            with open(p, 'wb') as f:
                f.write(os.urandom(size))
        else:                                                     # moved or new: stale bytes at both ends
            sparse(p, size, os.urandom(MIB), os.urandom(MIB))
        _link(root, name, node, a * 8, (b - a + 1) * 8)


def rereadpt(root):
    dev = os.path.join(root, 'dev', 'block')
    with open(os.path.join(dev, 'mmcblk0'), 'rb') as f:
        m = f.read(512)
    for n in range(1, 5):
        p = os.path.join(dev, f'mmcblk0p{n}')
        sd = os.path.join(root, 'sys', 'class', 'block', f'mmcblk0p{n}')
        t, s, c = struct.unpack_from('<4xB3xII', m, 446 + 16 * (n - 1)) if m[510:512] == b'\x55\xaa' else (0, 0, 0)
        if not c:
            if os.path.exists(p):
                os.unlink(p)
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
        create(root, opt('--state', 'fastboot'), opt('--card', 'blank'), int(opt('--card-sectors', CARD_SECTORS)),
               int(opt('--battery', 87)), layout=opt('--layout', 'sd128'), gpt_layout=opt('--gpt', 'starbite-a'),
               stock='--stock' in a, model=opt('--model', 'SM-G960F'))
    elif cmd == 'rereadpt':
        rereadpt(root)
    elif cmd == 'set-state':                      # sb-reboot in the installer environment
        write_state(root, {'menu': 'menu', 'fastboot': 'fastboot', 'download': 'download'}.get(sys.argv[3], 'menu'))
    elif cmd == 'rereadpt-sda':
        rereadpt_sda(root)
    elif cmd == 'mkexfat':
        mkexfat(root, sys.argv[3])
    else:
        sys.exit(__doc__)
