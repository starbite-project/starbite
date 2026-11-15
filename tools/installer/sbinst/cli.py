"""Command line of tools/installer/starbite-install (docs/INSTALLER.md)."""
import argparse
import os
import sys
import time
from pathlib import Path

from . import manifest as mf
from . import rescue
from .common import INSTALLER_DIR, Refused, Stopped, cardlayout, default_state_dir, human
from .device import Phone, Runner
from .steps import Installer, mbr_parts
from .ui import UI

USAGE = """\
starbite-install --bundle DIR [--dry-run | --rehearse --fake-root DIR | --real] [options]
starbite-install --make-rescue-image FILE
starbite-install --make-rescue-usb /dev/diskN [--image FILE] [--dry-run | --real]
starbite-install --verify-bundle DIR | --update-hashes DIR | --describe-phone
starbite-install --bundle DIR --start-env                (only start the installer environment)
starbite-install --restore-gpt [--dry-run | --real]        (the phone's own GPT, saved before repartitioning)
starbite-install --build-bundle DIR (--from-phone | --from-local) [--card-layout sd64] [--only KEY] [--skip KEY]
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
    ap.add_argument('--transport', choices=('env', 'twrp'), default='env',
                    help='env (default): the Starbite installer environment from the loader\'s fastboot; twrp: '
                         'legacy, TWRP\'s adb (S9 #1\'s own maintenance only; never the stock flow)')
    ap.add_argument('--repartition', action='store_true',
                    help='write the UFS partition table of the bundle\'s [ufs] layout when the phone has another one '
                         '(a stock phone; partitions that change are written fresh)')
    ap.add_argument('--start-env', action='store_true',
                    help='with --bundle: only start the installer environment (Download mode / loader fastboot -> '
                         'the environment from RAM), e.g. for --restore-gpt or --build-bundle --from-phone')
    ap.add_argument('--restore-gpt', action='store_true',
                    help='write back the phone\'s own GPT as saved before the repartitioning (phone in TWRP)')
    ap.add_argument('--build-bundle', type=Path, metavar='DIR', help='make a bundle (installer-manifest.toml + files)')
    src = ap.add_mutually_exclusive_group()
    src.add_argument('--from-phone', action='store_true', help='--build-bundle: read the main phone (TWRP; read only)')
    src.add_argument('--from-local', action='store_true', help='--build-bundle: only local build outputs')
    ap.add_argument('--card-layout', default=None, help='--build-bundle: the card layout the bundle is for '
                    '(sd64: any card >= 64 GB; default: the source\'s)')
    ap.add_argument('--sources', type=Path, metavar='FILE', help='--build-bundle: a TOML file `key = "local path"` '
                    'naming local sources (overrides the built-in candidates)')
    ap.add_argument('--only', action='append', default=[], metavar='KEY', help='--build-bundle: just these items')
    ap.add_argument('--skip', action='append', default=[], metavar='KEY', help='--build-bundle: leave these out')
    ap.add_argument('--fresh-backup', action='store_true', help='take a new backup even if a complete one exists')
    ap.add_argument('--serial', metavar='SERIAL', default=os.environ.get('SBINST_SERIAL') or None,
                    help='the phone to work on (adb devices / fastboot devices); every adb and fastboot call names '
                         'it, other phones are ignored, and a Download-mode flash needs every other phone unplugged. '
                         'Required with --real')
    ap.add_argument('--backup-only', action='store_true',
                    help='with --bundle: start the phone as usual, take the backup (step 1, read only on the phone), '
                         'check every file on this computer, and stop')
    ap.add_argument('--check-backup', type=Path, metavar='DIR',
                    help='check a backup directory on this computer (backups.txt vs every file), no phone')
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
        os.environ['PATH'] = f'{INSTALLER_DIR / "fakes"}{os.pathsep}{os.environ["PATH"]}'   # adb fastboot heimdall ssh
        os.environ['SBINST_FAKE_USB'] = str(root / 'usb')
        ui.info(f'fake phone: {root.name} (fake adb, fastboot, heimdall and ssh first on PATH)')
    elif a.mode == 'rehearse' and not a.make_rescue_usb:
        raise Refused('--rehearse needs --fake-root DIR')


def open_log(ui, state_dir, what):
    d = state_dir / 'logs'
    d.mkdir(parents=True, exist_ok=True)
    p = d / f'{time.strftime("%Y%m%d-%H%M%S")}-{what}.log'
    ui.log = open(p, 'w')
    return p


def describe_phone(ui, runner):
    from .common import default_state_dir
    from .device import InstallerEnv
    p = Phone(runner, InstallerEnv(runner, default_state_dir() / 'env'))
    st = p.detect()
    if st.kind == 'env':
        p.env.activate()
    elif st.kind != 'recovery':
        raise Refused(f'--describe-phone needs the installer environment (--bundle B --start-env) or, legacy, TWRP '
                      f'(found: {st.kind})')
    f = p.facts()
    ui.emit('# the phone\'s layout (read only), for installer-manifest.toml')
    ui.emit(f'[phone]\nufs_blocks = {(f.sda or 0) // 8}\n')
    for n in ('SYSTEM', 'VENDOR', 'ODM', 'USERDATA', 'WINESP', 'WINDOWS', 'A16DATA', 'BOOT', 'RECOVERY'):
        if n in f.parts:
            node, start, size = f.parts[n]
            ui.emit(f'[[layout]]\nname = "{n}"\nnode = "{node}"\nstart = {start // 8}\nblocks = {size // 8}\n')
    from . import gpt, ufslayout
    t = (f.sda or 0) // 8
    d = gpt.read(p.exec_out('dd if=/dev/block/sda bs=4096 count=6 2>/dev/null'),
                 p.exec_out(f'dd if=/dev/block/sda bs=4096 skip={t - 5} count=5 2>/dev/null'), t)
    match = []
    for L in ufslayout.LAYOUTS.values():
        try:
            pl = gpt.plan(d, L)
            if all(r.status in ('same', 'changed') for r in pl.rows):     # same places (GUIDs may differ)
                match.append(L.name + ('' if not pl.changed else ' (places only; GUIDs differ)'))
        except gpt.GPTError:
            pass
    ui.emit(f'# UFS GPT: {"both copies valid" if d.valid else "PROBLEMS: " + "; ".join(d.problems)}; '
            f'ufslayout: {", ".join(match) or "none of tools/installer/sbinst/ufslayout.py exactly"}'
            + (f'\n[ufs]\nlayout = "{match[0]}"\n' if match and match[0] in ufslayout.TARGETS else ''))
    mbr = p.exec_out('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null')
    parts = mbr_parts(mbr) or []
    ui.emit(f'[card]\nsectors = {f.card or 0}')
    for n, (t, s, c) in enumerate(parts[:2], 1):
        if c:
            ui.emit(f'p{n} = {{ type = 0x{t:02x}, start = {s}, sectors = {c} }}')
    name = cardlayout.identify(parts, f.card or 0)
    fits = cardlayout.for_card(f.card or 0)
    lay = cardlayout.LAYOUTS.get(name)
    ui.emit(f'# card layout: {name or "none of tools/store/cardlayout.py"}' +
            (' (in a manifest: layout = "sd64" without sectors/p1/p2)' if lay is not None and not lay.exact else '') +
            (f'; this card fits the {fits} layout (`layout = "{fits}"`)' if fits and fits != name else ''))
    return 0


def main(argv=None):
    a = parse(argv if argv is not None else sys.argv[1:])
    if a.yes and a.mode == 'real':
        print('REFUSED: --yes is not accepted with --real: every confirmation is typed by a person')
        return 2
    ui = UI(assume_yes=a.yes, allow_yes_typed=(a.mode == 'rehearse'))
    state_dir = (a.state_dir or default_state_dir()).resolve()
    if a.serial:
        os.environ['SBINST_SERIAL'] = os.environ['ANDROID_SERIAL'] = a.serial    # adb and fastboot honour it too
    try:
        setup_fake(a, ui)
        if a.check_backup:
            from .steps import check_backup_dir
            oks, problems = check_backup_dir(a.check_backup)
            for ln in oks:
                ui.ok(ln)
            for ln in problems:
                ui.bad(ln)
            if problems:
                return 3
            ui.ok(f'backup verified: {len(oks)} files, each equal to the SHA-256 the phone computed')
            return 0
        if a.mode == 'real' and not a.serial and not a.make_rescue_usb:
            raise Refused('--real needs --serial SERIAL: every adb, fastboot and Download-mode step is then for that '
                          'phone only (adb devices shows the serial)')
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
        if a.build_bundle:
            from . import bundler
            if not (a.from_phone or a.from_local):
                raise Refused('--build-bundle needs --from-phone (the main phone in TWRP, read only) or --from-local')
            open_log(ui, state_dir, 'build-bundle')
            # a dry-run runner on purpose: the builder only reads; any write command would only be printed
            return bundler.build(ui, Runner(ui, 'dry-run'), a.build_bundle, from_phone=a.from_phone,
                                 card_layout=a.card_layout, only=a.only, skip=a.skip, state_dir=state_dir,
                                 sources_file=a.sources)
        if a.restore_gpt:
            open_log(ui, state_dir, f'restore-gpt-{a.mode}')
            return Installer(ui, Runner(ui, a.mode), None, a, state_dir).restore_gpt()
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
        lay = mf.layout_problems(m)
        if lay:
            raise Refused(f'the bundle does not match its card layout ({m.card_layout.name}):\n  ' + '\n  '.join(lay))
        if m.card_layout is not None:
            ui.ok(f'card layout {m.card_layout.name}: catalog roots and payload sdroot= match '
                  f'({", ".join(f"{r.name} {r.start}" for r in m.card_layout.roots())})')
        missing = [i.key for i in m.items if not i.present]
        ui.ok(f'bundle verified: {len(m.items) - len(missing)} items' +
              (f'; optional items not in it: {", ".join(missing)}' if missing else ''))
        if a.verify_bundle:
            return 0
        runner = Runner(ui, a.mode)
        if a.start_env:
            st = Installer(ui, runner, m, a, state_dir).bring_up()
            ui.ok(f'phone: {st.kind if st else "not started (dry run)"}')
            return 0
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
