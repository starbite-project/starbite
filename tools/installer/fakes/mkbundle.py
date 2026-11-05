#!/usr/bin/env python3
"""A small rehearsal bundle with the same items as installer-manifest.example.toml, made of fake
images (random bytes; real OS store images built with tools/store/sbstore2.py; real ext4 Linux
roots from tests/fixtures). Windows and iOS 26 are left out on purpose (optional items).

  mkbundle.py OUT_DIR"""
import gzip
import os
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


def main(out):
    b = Path(out)
    if b.exists():
        shutil.rmtree(b)
    b.mkdir(parents=True)
    shutil.copy(INST / 'installer-manifest.example.toml', b / mf.MANIFEST_NAME)
    (b / 'store').mkdir()
    shutil.copy(REPO / 'tools' / 'store' / 'catalog.toml', b / 'store' / 'catalog.toml')
    put(b / 'boot' / 'starbite-boot.img', os.urandom(6 * MIB))
    put(b / 'boot' / 'twrp-starbite.img', os.urandom(5 * MIB))
    # the OS stores: every file the visible catalog entries name, plus the rescue system
    cat = tomllib.loads((b / 'store' / 'catalog.toml').read_text())
    names = {'rescue.image', 'rescue.dtb', 'rescue.initrd'}
    for e in cat['entry']:
        for f in e.get('files', []):
            alt = f.split('|')[0].split(':', 1)[-1]
            if alt != 'catalog':
                names.add(alt)
    payload = b / 'store' / 'payload'
    args = []
    for n in sorted(names):
        put(payload / n, os.urandom(64 << 10))
        args.append(f'{n}={payload / n}')
    sb = [sys.executable, '-I', str(REPO / 'tools' / 'store' / 'sbstore2.py'), 'build']
    subprocess.run(sb + [str(b / 'store' / 'ufs-store.img'), '--medium', 'ufs', '--catalog', str(b / 'store' / 'catalog.toml'),
                         *args], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(sb + [str(b / 'store' / 'sd-store.img'), '--medium', 'sd', f'engineer.img={payload / "engineer.img"}'],
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
    text = (b / mf.MANIFEST_NAME).read_text().replace('name = "Starbite S9 - main phone set (example)"',
                                                      'name = "Starbite S9 - REHEARSAL bundle (fake images)"')
    (b / mf.MANIFEST_NAME).write_text(text)
    for f in mf.update_hashes(b):
        print(f'  {f}')
    print(f'rehearsal bundle in {b}')


if __name__ == '__main__':
    main(sys.argv[1])
