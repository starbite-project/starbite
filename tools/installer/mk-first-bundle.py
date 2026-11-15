#!/usr/bin/env python3
"""The bundle for a first install on a stock S9 (S9 #2, 2026-11-15): the Starbite loader in BOOT, the
installer environment (RAM only) and a fresh card store for any microSD of at least 64 GB (sd64). Nothing
on UFS but BOOT: no repartitioning, stock Android's USERDATA stays as it is.

  mk-first-bundle.py OUT --boot-from S9_1_BOOT.img --loader LOADER.Image --env-dir DIR
                     --store-from STORE.img --catalog CATALOG.toml [--keep KEY ...]

--boot-from  S9 #1's BOOT as pulled (or a verified copy): tools/store/boot-card.py takes its payload, menu
             assets and header and puts LOADER.Image in (+ sbstore=card: the card is the only store, the UFS
             store at USERDATA +16 GiB is never opened, which on a stock phone is Android's /data).
--loader     the loader Image (FW4: shared notes loader-fw4-boot.Image, built -DSTORE_CARD_ONLY
             -DM7_MODE_DEFAULT=2 -DSBFW_SHELL_DEFAULT=1).
--env-dir    the installer environment = Starbite Maintenance: maint.image, maint.initrd, maint.dtb.
--store-from a store image to take the kept entries' files from (a card store pull of S9 #1, compact or not).
             Only the files the kept catalog rows name are copied: never S9 #1's settings, crash log,
             sbos-state, Wi-Fi networks (wifinet) or photos.
--catalog    the catalog TOML the rows come from (the one S9 #1's store was built from).
--keep       catalog keys (default: starbite-next-gpu engineer utility). Every kept row must be RAM only
             (no root on UFS or the card): S9 #2 has none of S9 #1's OS roots.

The card store gets a fresh 1 MiB sbos-state (Starbite OS settings) and an empty crash log, and is built
--medium sd64 --home: it must fit the sd64 store area (64 MiB). Then the manifest, --update-hashes and
--verify-bundle. Host only; reads nothing from a phone."""
import argparse
import importlib.util
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SI = [sys.executable, '-B', str(HERE / 'starbite-install')]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def catalog_subset(text, keep):
    """The catalog's preamble (comments, [defaults.*]) and the [[entry]] blocks whose key is kept."""
    parts = re.split(r'(?m)^\[\[entry\]\]\s*$', text)
    head, blocks = parts[0], parts[1:]
    out, found = [head.rstrip() + '\n'], []
    for b in blocks:
        m = re.search(r'(?m)^key\s*=\s*"([^"]+)"', b)
        if m and m.group(1) in keep:
            out.append('\n[[entry]]' + b.rstrip() + '\n')
            found.append(m.group(1))
    missing = [k for k in keep if k not in found]
    if missing:
        sys.exit(f'catalog: no row for {" ".join(missing)}')
    return ''.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--boot-from', type=Path, required=True)
    ap.add_argument('--loader', type=Path, required=True)
    ap.add_argument('--env-dir', type=Path, required=True)
    ap.add_argument('--store-from', type=Path, required=True)
    ap.add_argument('--catalog', type=Path, required=True)
    ap.add_argument('--keep', nargs='+', default=['starbite-next-gpu', 'engineer', 'utility'])
    a = ap.parse_args()
    out = a.out
    if out.exists() and any(out.iterdir()):
        sys.exit(f'{out} exists and is not empty')
    sb = load('sbstore2_mk', REPO / 'tools' / 'store' / 'sbstore2.py')

    # 1. BOOT: the loader into S9 #1's BOOT (boot-card.py checks payload, ramdisk and header byte for byte)
    (out / 'boot').mkdir(parents=True)
    boot = out / 'boot' / 'starbite-boot.img'
    subprocess.run([sys.executable, str(REPO / 'tools' / 'store' / 'boot-card.py'), str(a.boot_from), str(a.loader),
                    str(boot)], check=True)
    data = boot.read_bytes()
    if b'sbstore=card' not in data:
        sys.exit('the BOOT image has no sbstore=card')
    if len(data) % 4096:            # UFS items are whole 4 KiB blocks; zeros after the boot image's last section
        boot.write_bytes(data + bytes(-len(data) % 4096))   # are ignored (the header gives every size)
        print(f'BOOT padded {len(data)} -> {len(data) + (-len(data) % 4096)} bytes (4 KiB blocks)')

    # 2. the installer environment (Starbite Maintenance, installer mode: sb.installer=1)
    (out / 'env').mkdir()
    for src, dst in (('maint.image', 'installer.image'), ('maint.initrd', 'installer.initrd'), ('maint.dtb', 'installer.dtb')):
        shutil.copyfile(a.env_dir / src, out / 'env' / dst)
    if (out / 'env' / 'installer.image').read_bytes()[0x38:0x3c] != b'ARMd':
        sys.exit('env: maint.image is not an arm64 Image')

    # 3. the card store: the kept rows and their files only, fresh volatile entries
    rows_text = catalog_subset(a.catalog.read_text(), a.keep)
    rows = tomllib.loads(rows_text)['entry']
    for r in rows:
        root = r.get('root', '')
        if not root.startswith(('none', '-', 'initramfs')) and r.get('method') != 'bare':
            sys.exit(f'{r["key"]}: its root is {root!r}: S9 #2 has none of S9 #1\'s OS roots; keep RAM-only rows')
    store = {name: blob for name, _, blob in sb.read_store(a.store_from)}
    (out / 'store' / 'files').mkdir(parents=True)
    (out / 'store' / 'catalog.toml').write_text(rows_text)
    names = []
    for r in rows:
        for f in r['files']:
            alts = [x.split(':', 1)[-1] for x in f.split('|') if not x.startswith('@')]
            got = next((x for x in alts if x in store), None)
            if got is None:
                sys.exit(f'{r["key"]}: none of {alts} is in {a.store_from}')
            if got not in names:
                (out / 'store' / 'files' / got).write_bytes(store[got])
                names.append(got)
    img = out / 'store' / 'sd-store.img'
    subprocess.run([sys.executable, str(REPO / 'tools' / 'store' / 'sbstore2.py'), 'build', str(img), '--medium', 'sd64',
                    '--home', '--catalog', str(out / 'store' / 'catalog.toml'), '--volatile', 'sbos-state=1048576',
                    *[f'{n}={out / "store" / "files" / n}' for n in names]], check=True)
    subprocess.run([sys.executable, str(REPO / 'tools' / 'store' / 'sbstore2.py'), 'list', str(img)], check=True)
    shutil.rmtree(out / 'store' / 'files')

    # 4. the manifest, the hashes, the check
    items = [('env-kernel', 'Installer environment: kernel (RAM only)', 'starbite', 'env/installer.image', 'env:kernel'),
             ('env-initrd', 'Installer environment: initramfs (RAM only)', 'starbite', 'env/installer.initrd', 'env:initrd'),
             ('env-dtb', 'Installer environment: device tree (RAM only)', 'starbite', 'env/installer.dtb', 'env:dtb'),
             ('sd-store', 'OS store (microSD p2): ' + ', '.join(a.keep), 'starbite', 'store/sd-store.img', 'store:sd'),
             ('boot', 'Starbite loader + menu (BOOT)', 'starbite', 'boot/starbite-boot.img', 'part:BOOT')]
    m = ['format = 1', f'name = "First install, S9 #2: Starbite loader (BOOT) + card store ({", ".join(a.keep)})"',
         'catalog = "store/catalog.toml"', '', '[phone]', 'models = ["SM-G960F"]', 'devices = ["starlte"]',
         'min_battery = 50', '', '[card]', 'min_bytes = 60000000000', 'layout = "sd64"', '',
         '[env]', 'cmdline = "sb.installer=1"', '']
    for key, title, os_, f, dest in items:
        m += ['[[item]]', f'key = "{key}"', f'title = "{title}"', f'os = "{os_}"', f'file = "{f}"', 'size = 0',
              f'sha256 = "{"0" * 64}"', f'dest = "{dest}"', '']
    (out / 'installer-manifest.toml').write_text('\n'.join(m))
    subprocess.run(SI + ['--update-hashes', str(out)], check=True)
    subprocess.run(SI + ['--verify-bundle', str(out)], check=True)
    print(f'bundle ready: {out}')


if __name__ == '__main__':
    main()
