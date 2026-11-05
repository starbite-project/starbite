"""Command line of tools/installer/starbite-install (docs/INSTALLER.md)."""
import argparse
import os
import sys
import time
from pathlib import Path

from . import manifest as mf
from . import rescue
from .common import INSTALLER_DIR, Refused, Stopped, default_state_dir, human
from .device import Phone, Runner
from .steps import Installer, mbr_parts
from .ui import UI

USAGE = """\
starbite-install --bundle DIR [--dry-run | --rehearse --fake-root DIR | --real] [options]
starbite-install --make-rescue-image FILE
starbite-install --make-rescue-usb /dev/diskN [--image FILE] [--dry-run | --real]
starbite-install --verify-bundle DIR | --update-hashes DIR | --describe-phone
"""


def parse(argv):
    ap = argparse.ArgumentParser(prog='starbite-install', usage=USAGE,
                                 description='Install Starbite on a Galaxy S9 (SM-G960F) from macOS or Linux. '
                                             'The default mode is --dry-run: it writes nothing.')
    m = ap.add_mutually_exclusive_group()
    m.add_argument('--dry-run', action='store_true', help='(default) read the phone, print every command, write nothing')
    m.add_argument('--rehearse', action='store_true', help='run the real flow against the fake phone (--fake-root)')
    m.add_argument('--real', action='store_true', help='write to the phone (needs a finished dry run and a typed confirmation)')
    ap.add_argument('--bundle', type=Path, help='the bundle directory (installer-manifest.toml + files)')
    ap.add_argument('--fake-root', type=Path, help='a fake phone (tools/installer/fakes/fakephone.py create DIR)')
    ap.add_argument('--state-dir', type=Path, default=None, help='journal, backups and logs (default build/installer)')
    ap.add_argument('--replace', action='append', default=[], metavar='KEY',
                    help='overwrite this item even though its target holds data (repeatable)')
    ap.add_argument('--replace-all', action='store_true', help='a fresh install: overwrite every target that holds data')
    ap.add_argument('--erase-card', action='store_true', help='erase a microSD that has another layout')
    ap.add_argument('--fresh-backup', action='store_true', help='take a new backup even if a complete one exists')
    ap.add_argument('--rescue-usb', metavar='DEV', help='at the end, write the rescue USB disk to DEV (shown and typed first)')
    ap.add_argument('--no-reboot', action='store_true', help='do not offer the reboot at the end')
    ap.add_argument('--yes', action='store_true', help='answer yes to questions (dry run and rehearsal only)')
    ap.add_argument('--make-rescue-image', type=Path, metavar='FILE')
    ap.add_argument('--make-rescue-usb', metavar='DEV')
    ap.add_argument('--image', type=Path, help='the rescue image for --make-rescue-usb')
    ap.add_argument('--verify-bundle', type=Path, metavar='DIR')
    ap.add_argument('--update-hashes', type=Path, metavar='DIR')
    ap.add_argument('--describe-phone', action='store_true', help='print the phone\'s layout as manifest TOML (read only)')
    a = ap.parse_args(argv)
    a.mode = 'real' if a.real else 'rehearse' if a.rehearse else 'dry-run'
    return a


def setup_fake(a, ui):
    if a.fake_root:
        if a.mode == 'real':
            raise Refused('--fake-root and --real do not mix')
        root = a.fake_root.resolve()
        if not (root / 'state').is_file():
            raise Refused(f'{root} is not a fake phone (tools/installer/fakes/fakephone.py create {root})')
        os.environ['FAKE_ROOT'] = str(root)
        os.environ['PATH'] = f'{INSTALLER_DIR / "fakes"}{os.pathsep}{os.environ["PATH"]}'
        os.environ['SBINST_FAKE_USB'] = str(root / 'usb')
        ui.info(f'fake phone: {root.name} (fake adb, fastboot and heimdall first on PATH)')
    elif a.mode == 'rehearse' and not a.make_rescue_usb:
        raise Refused('--rehearse needs --fake-root DIR')


def open_log(ui, state_dir, what):
    d = state_dir / 'logs'
    d.mkdir(parents=True, exist_ok=True)
    p = d / f'{time.strftime("%Y%m%d-%H%M%S")}-{what}.log'
    ui.log = open(p, 'w')
    return p


def describe_phone(ui, runner):
    p = Phone(runner)
    st = p.detect()
    if st.kind != 'recovery':
        raise Refused(f'--describe-phone needs the phone in TWRP (found: {st.kind})')
    f = p.facts()
    ui.emit('# the phone\'s layout (read only), for installer-manifest.toml')
    ui.emit(f'[phone]\nufs_blocks = {(f.sda or 0) // 8}\n')
    for n in ('SYSTEM', 'VENDOR', 'ODM', 'USERDATA', 'WINESP', 'WINDOWS', 'A16DATA', 'BOOT', 'RECOVERY'):
        if n in f.parts:
            node, start, size = f.parts[n]
            ui.emit(f'[[layout]]\nname = "{n}"\nnode = "{node}"\nstart = {start // 8}\nblocks = {size // 8}\n')
    mbr = p.exec_out('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null')
    parts = mbr_parts(mbr) or []
    ui.emit(f'[card]\nsectors = {f.card or 0}')
    for n, (t, s, c) in enumerate(parts[:2], 1):
        if c:
            ui.emit(f'p{n} = {{ type = 0x{t:02x}, start = {s}, sectors = {c} }}')
    return 0


def main(argv=None):
    a = parse(argv if argv is not None else sys.argv[1:])
    if a.yes and a.mode == 'real':
        print('REFUSED: --yes is not accepted with --real: every confirmation is typed by a person')
        return 2
    ui = UI(assume_yes=a.yes, allow_yes_typed=(a.mode == 'rehearse'))
    state_dir = (a.state_dir or default_state_dir()).resolve()
    try:
        setup_fake(a, ui)
        if a.make_rescue_image:
            open_log(ui, state_dir, 'rescue-image')
            rescue.build_image(a.make_rescue_image, ui)
            return 0
        if a.make_rescue_usb:
            open_log(ui, state_dir, 'rescue-usb')
            img = a.image or state_dir / 'rescue' / 'starbite-rescue-usb.img'
            if not img.is_file():
                if a.mode == 'dry-run':
                    ui.warn(f'no image at {img}: --make-rescue-image {img} builds it')
                else:
                    rescue.build_image(img, ui)
            if img.is_file() or a.mode != 'dry-run':
                rescue.write_stick(img, a.make_rescue_usb, ui, a.mode)
            return 0
        if a.update_hashes:
            for f in mf.update_hashes(a.update_hashes):
                ui.ok(f'hashed {f}')
            return 0
        if a.describe_phone:
            return describe_phone(ui, Runner(ui, 'dry-run'))
        bundle = a.verify_bundle or a.bundle
        if not bundle:
            ui.emit(USAGE)
            return 2
        m = mf.load(bundle)
        cache = mf.HashCache(state_dir / 'hash-cache.json')
        log = open_log(ui, state_dir, 'verify' if a.verify_bundle else a.mode)
        ui.h1(f'Bundle {bundle}')
        problems = mf.verify_bundle(m, cache, lambda d, t: ui.info(f'  hashing... {human(d)} of {human(t)}'))
        for it in m.items:
            ui.info(f'{"ok  " if it.present else "--  "} {it.key:<16} {it.file or it.action:<40} -> {it.dest}')
        if problems:
            raise Refused('the bundle does not match its manifest:\n  ' + '\n  '.join(problems))
        missing = [i.key for i in m.items if not i.present]
        ui.ok(f'bundle verified: {len(m.items) - len(missing)} items' +
              (f'; optional items not in it: {", ".join(missing)}' if missing else ''))
        if a.verify_bundle:
            return 0
        runner = Runner(ui, a.mode)
        rc = Installer(ui, runner, m, a, state_dir).run()
        ui.info(f'log: {log}')
        return rc
    except Refused as e:
        ui.refused(str(e))
        ui.info('nothing was written by this step.')
        return 2
    except Stopped as e:
        ui.stopped(str(e))
        ui.info('run the same command again to resume; finished items are skipped after a check.')
        return 3
    except KeyboardInterrupt:
        ui.bad('interrupted: run the same command again to resume')
        return 130
