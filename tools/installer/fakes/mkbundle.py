#!/usr/bin/env python3
"""A small rehearsal bundle with the same items as installer-manifest.example.toml, made of fake
images (random bytes; real OS store images built with tools/store/sbstore2.py; real ext4 Linux
roots from tests/fixtures; the XNU payloads of the card-root entries as SWEETXNU stand-ins whose
packed boot-args carry the root's sdroot=). Windows and iOS 26 are left out on purpose (optional items).

  mkbundle.py OUT_DIR [--layout sd128|sd64]

--layout sd64 makes the bundle for any card of at least 64 GB the way a real bundle builder would
(docs/INSTALLER.md 11.4): the catalog's roots from `cardlayout.py catalog sd64` (iOS 26 hidden),
the macOS payload retargeted with `cardlayout.py payload sd64 mac13`, the iOS 26 payloads left out,
the card store built with --medium sd64, and the manifest's [card] = layout "sd64", the macOS root
as card:root:mac13 and no iOS 26 item."""
import gzip
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
INST = HERE.parent
REPO = INST.parent.parent
sys.path.insert(0, str(INST))
from sbinst import manifest as mf  # noqa: E402
from sbinst.common import cardlayout  # noqa: E402

MIB = 1 << 20


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def sparse_image(path, size, pieces):
    """A sparse file of `size` with random bytes at (offset, length) pieces."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'wb') as f:
        f.truncate(size)
        for off, n in pieces:
            f.seek(off)
            f.write(os.urandom(n))


def fake_payload(start, count, size=64 << 10):
    """A SWEETXNU stand-in (magic, version 1, the 608-byte cmdline at 0x48) whose boot-args put the
    root on the card at start+count, like the real mac13 / ios26 payloads (mkpayload.py layout)."""
    blob = bytearray(os.urandom(size))
    blob[0:12] = cardlayout.PAYLOAD_MAGIC + (1).to_bytes(4, 'little')
    line = (f'debug=0xc serial=3 cpus=8 -v rd=sdroot0 cs_enforcement_disable=1 sdroot=lba:{start},{count} '
            'sweetreboot=86400 sdrw=1 sdlog=1').encode()
    blob[cardlayout.PAYLOAD_CMDLINE:cardlayout.PAYLOAD_CMDLINE + cardlayout.PAYLOAD_CMDLINE_LEN] = \
        line.ljust(cardlayout.PAYLOAD_CMDLINE_LEN, b'\0')
    return bytes(blob)


def manifest_for(text, layout):
    """The example manifest made for another card layout (sd64): [card] from the layout, card roots
    as card:root:KEY, items whose root has no slot on the layout dropped."""
    L = cardlayout.get(layout)
    out = re.sub(r'(?ms)^layout = "sd128".*?^p2 = \{[^\n]*\}\n',
                 f'layout = "{L.name}"                         # {L.title} (tools/store/cardlayout.py)\n', text)
    blocks = re.split(r'(?m)^(?=\[\[item\]\]\n)', out)
    keep = []
    for b in blocks:
        m = re.search(r'^dest = "card:lba:(\d+)"', b, re.M)
        if m:
            old = cardlayout.get('sd128').area_at(int(m.group(1)), 1)
            if L.root(old.name) is None:
                continue                                  # no slot on this layout (iOS 26 on sd64)
            b = b.replace(m.group(0), f'dest = "card:root:{old.name}"').replace(
                f'sdroot=lba:{old.start},', f'sdroot=lba:{L.root(old.name).start},')
        keep.append(b)
    return ''.join(keep)


def main(out, layout='sd128'):
    b = Path(out)
    if b.exists():
        shutil.rmtree(b)
    b.mkdir(parents=True)
    text = (INST / 'installer-manifest.example.toml').read_text()
    if layout != 'sd128':
        text = manifest_for(text, layout)
    (b / mf.MANIFEST_NAME).write_text(text)
    (b / 'store').mkdir()
    cat_text = (REPO / 'tools' / 'store' / 'catalog.toml').read_text()
    (b / 'store' / 'catalog.toml').write_text(cardlayout.catalog_text(cat_text, layout))
    put(b / 'boot' / 'starbite-boot.img', os.urandom(6 * MIB))
    put(b / 'boot' / 'twrp-starbite.img', os.urandom(5 * MIB))
    # the OS stores: every file the visible catalog entries name, plus the rescue system
    cat = tomllib.loads((b / 'store' / 'catalog.toml').read_text())
    names = {'rescue.image', 'rescue.dtb', 'rescue.initrd'}
    roots = {}                                            # payload file -> its entry's card root (sd128 sectors)
    sd128 = {e['key']: e for e in tomllib.loads(cat_text)['entry']}
    for e in cat['entry']:
        for f in e.get('files', []):
            alt = f.split('|')[0].split(':', 1)[-1]
            if alt == 'catalog':
                continue
            m = re.match(r'sd:lba (\d+)\+(\d+)', sd128[e['key']].get('root', ''))
            if e.get('method') == 'xnu' and m:
                if cardlayout.get(layout).root(e['key']) is None:
                    continue                              # no root slot on this layout: payload left out
                roots[alt] = (e['key'], int(m.group(1)), int(m.group(2)))
            names.add(alt)
    payload = b / 'store' / 'payload'
    args = []
    for n in sorted(names):
        if n in roots:
            key, start, count = roots[n]
            blob = cardlayout.retarget_payload(fake_payload(start, count), layout, key)
        else:
            blob = os.urandom(64 << 10)
        put(payload / n, blob)
        args.append(f'{n}={payload / n}')
    sb = [sys.executable, '-I', str(REPO / 'tools' / 'store' / 'sbstore2.py'), 'build']
    subprocess.run(sb + [str(b / 'store' / 'ufs-store.img'), '--medium', 'ufs', '--catalog', str(b / 'store' / 'catalog.toml'),
                         *args], check=True, stdout=subprocess.DEVNULL)
    medium = 'sd' if layout == 'sd128' else layout
    subprocess.run(sb + [str(b / 'store' / 'sd-store.img'), '--medium', medium, f'engineer.img={payload / "engineer.img"}'],
                   check=True, stdout=subprocess.DEVNULL)
    shutil.rmtree(payload)
    # iOS 16 slot: an APFS container superblock ("NXSB" at 32, block size, block count) as store-flash.sh expects
    slot = bytearray(os.urandom(2 * MIB))
    slot[32:36] = b'NXSB'
    slot[36:48] = (4096).to_bytes(4, 'little') + (512).to_bytes(8, 'little')
    put(b / 'apple' / 'ios16-slot.img', bytes(slot))
    # Android: SYSTEM spans three 64 MiB chunks (data, all zero, a tail), so a cable pull can hit the middle
    sparse_image(b / 'android' / 'system.img', 129 * MIB, [(0, 16 * MIB), (128 * MIB, MIB)])
    put(b / 'android' / 'vendor.img', os.urandom(3 * MIB))
    sparse_image(b / 'android' / 'a16data.img', 4 * MIB, [(0, 4096 * 2)])
    put(b / 'windows' / 'winesp.img', os.urandom(2 * MIB))
    put(b / 'apple' / 'mac13-root.img', os.urandom(3 * MIB))
    # Linux roots: real ext4 images (no password / with a PIN), ChromiumOS: random bytes
    fx = INST / 'tests' / 'fixtures'
    put(b / 'card' / 'distros' / 'pmos.img', gzip.decompress((fx / 'linux-nopin.ext4.gz').read_bytes()))
    put(b / 'card' / 'distros' / 'ubuntu.img', gzip.decompress((fx / 'linux-pin.ext4.gz').read_bytes()))
    put(b / 'card' / 'distros' / 'chromeos.img', os.urandom(2 * MIB))
    text = (b / mf.MANIFEST_NAME).read_text().replace(
        'name = "Starbite S9 - main phone set (example)"',
        f'name = "Starbite S9 - REHEARSAL bundle (fake images, {layout} card layout)"')
    (b / mf.MANIFEST_NAME).write_text(text)
    for f in mf.update_hashes(b):
        print(f'  {f}')
    print(f'rehearsal bundle in {b} ({layout} card layout)')


if __name__ == '__main__':
    a = sys.argv[1:]
    lay = a[a.index('--layout') + 1] if '--layout' in a else 'sd128'
    main(a[0], lay)
