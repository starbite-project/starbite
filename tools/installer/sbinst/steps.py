"""The install flow: welcome, the two confirmations, phone state, preflight (read only), plan,
then the steps. Every step is idempotent (it checks the target first), resumable (the journal and
per-item chunk state) and verified (read-back SHA-256 on the phone)."""
import hashlib
import os
import re
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from . import nopassword, pit, repart, rescue
from .common import (BACKUP_OPTIONAL, BACKUP_RAW, BACKUP_REQUIRED, DOWNLOAD_MODE_USB, GIB, MIB, NATIVE_PARTS,
                     ONEUI_DATA_RESERVE, PART_WRITE_PY, PROTECTED, REPO, Refused, Stopped, cardlayout,
                     default_state_dir, human)
from .device import (Heimdall, InstallerEnv, Phone, android_props, chunk_write, is_stock_samsung, target_serial,
                     usb_ids)
from .journal import Journal
from .manifest import MKFS_ACTIONS, coverage, writer_of

STORE_UFS_OFFSET = 16 * GIB                 # store-flash.sh: USERDATA + 16 GiB
# S-Boot of the one S9 the Starbite loader has run under (S9 #1, firmware G960FXXUHFVG6). Another S-Boot is not
# refused, but the plan warns: the loader reads S-Boot's FDT and log pages, and only this version is proven.
PROVEN_SBOOT = 'G960FXXUHFVG6'


def phone_id_of(serial):
    """phone-<first 10 hex digits of SHA-256(serial)>: the serial itself is never stored or printed."""
    return 'phone-' + hashlib.sha256(serial.encode()).hexdigest()[:10]


def check_backup_dir(d, required=True):
    """Re-read a finished backup on this computer: every file named in backups.txt hashed again (.img.gz
    decompressed) and compared with the SHA-256 the phone computed when it was taken; with `required`,
    every name the installer must back up has to be there. Returns (ok_lines, problems)."""
    import gzip as _gz
    d = Path(d)
    oks, problems = [], []
    log = d / 'backups.txt'
    if not log.is_file():
        return oks, [f'{log}: missing']
    seen = {}
    for line in log.read_text().splitlines():
        bits = line.split()
        if len(bits) < 2:
            continue
        want, name = bits[0], bits[1]
        base = name[:-4] if name.endswith('.img') else name
        seen[base] = (want, name)
    for base, (want, name) in sorted(seen.items()):
        f = d / name
        gz = d / (name + '.gz')
        src = f if f.is_file() else gz if gz.is_file() else None
        if src is None:
            problems.append(f'{base}: no {name}(.gz) in {d}')
            continue
        h = hashlib.sha256()
        with (_gz.open(src, 'rb') if src.suffix == '.gz' else open(src, 'rb')) as fh:
            for b in iter(lambda: fh.read(1 << 20), b''):
                h.update(b)
        if h.hexdigest() != want:
            problems.append(f'{base}: {src.name} hashes to {h.hexdigest()[:16]}..., the phone said {want[:16]}...')
        else:
            oks.append(f'{base}: {src.name} sha256 {want[:16]}... = the phone\'s')
    if required:
        need = [n for n in BACKUP_REQUIRED] + [n for n, _, _ in BACKUP_RAW]
        miss = [n for n in need if n not in seen]
        if miss:
            problems.append('not backed up: ' + ' '.join(miss))
    return oks, problems
TOOL = {k: str(REPO / v) for k, v in {
    'part-write': 'tools/android/pixel/part-write.py',
    'part-backup': 'tools/android/pixel/part-backup.sh',
    'metadata-reset': 'tools/android/pixel/metadata-reset.sh',
    'slot-write': 'tools/refport/slot-write.py',
    'store-ufs': 'tools/store/store-flash.sh',
    'store-sd': 'tools/store/store-flash-sd.sh',
    'card-range': 'tools/macos/card-range-flash.py'}.items()}


def rank(item):
    """Install order. BOOT is last: until it is written, the phone still starts its old loader (on a stock
    phone BOOT was written first, in Download mode, and is then skipped as identical). RECOVERY is never
    written (manifest.parse_dest refuses it). Android: SYSTEM, VENDOR, then LineageOS's /data image and its
    fresh /metadata (ODM, mkfs-metadata) together; One UI's ODM image (action write) comes before its /data
    (A16DATA, mkfs-oneui-data), so the images are all in before the file system is made."""
    d = item.dest
    if d.kind == 'slot':
        return 10
    if d.kind == 'store':
        return 20 if d.name == 'ufs' else 50
    if d.kind == 'part':
        r = {'SYSTEM': 30, 'VENDOR': 31, 'A16DATA': 33, 'ODM': 34, 'WINESP': 40, 'WINDOWS': 41, 'BOOT': 99}[d.name]
        return 32 if d.name == 'ODM' and getattr(item, 'action', 'write') == 'write' else r
    return 60 if d.kind == 'card-lba' else 70


@dataclass
class Step:
    key: str
    title: str
    where: str
    status: str            # write resume replace skip keep verify host BLOCKED
    reason: str = ''
    item: object = None


def build_mbr(p1, p2, signature=b'SB01'):
    mbr = bytearray(512)
    mbr[440:444] = signature
    for i, p in enumerate((p1, p2)):
        struct.pack_into('<B3sB3sII', mbr, 446 + 16 * i, 0, b'\xfe\xff\xff', p['type'], b'\xfe\xff\xff',
                         p['start'], p['sectors'])
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr)


def mbr_parts(mbr):
    if len(mbr) < 512 or mbr[510:512] != b'\x55\xaa':
        return None
    return [struct.unpack_from('<4xB3xII', mbr, 446 + 16 * i) for i in range(4)]


class Installer:
    def __init__(self, ui, runner, manifest, opts, state_dir):
        self.ui, self.r, self.m, self.o = ui, runner, manifest, opts
        self.phone = Phone(runner)
        self.state_dir = Path(state_dir)
        self.facts = self.journal = self.phone_id = None
        self.mode = runner.mode
        self.done = {'written': 0, 'skipped': 0, 'kept': 0}
        self.warnings = []
        self.card_parts = None      # [p1, p2] dicts for this card (the manifest's, or its card layout's)
        self.rp = None              # repart.Repart when the bundle names a UFS layout ([ufs] layout)
        self.heimdall = Heimdall(runner, str(self.state_dir / 'download' / 'heimdall-session-open'))
        self.env = InstallerEnv(runner, default_state_dir() / 'env')   # one key per computer, whatever --state-dir
        self.phone.env = self.env

    # ------------------------------------------------------------------ welcome / state
    def welcome(self):
        u = self.ui
        u.banner()
        u.info(f'bundle:  {self.m.name}  ({len(self.m.items)} items, manifest sha256 {self.m.sha256[:12]}...)')
        u.info(f'mode:    {self.mode.upper()}' + {'dry-run': '  (reads the phone, writes nothing, prints every command)',
                                                     'rehearse': '  (the real flow against the fake phone)',
                                                     'real': '  (WRITES TO THE PHONE)'}[self.mode])
        u.info('This makes the phone match the main Starbite S9: every OS in the bundle, verified by read-back.')
        u.info('It never writes EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB/DTBO,')
        u.info('the GPT or the boot LUNs, and it backs those up to this computer before anything else.')
        u.h1('Two questions first')
        if not u.confirm('1. Is the bootloader unlocked? (Developer options > OEM unlocking ON; Download mode shows\n'
                         '   OEM LOCK: OFF (U), FRP LOCK: OFF, KG STATE not Prenormal/Active)'):
            raise Refused('the bootloader must be unlocked first: docs/INSTALLER.md "Before you start"')
        if not u.confirm('2. Is a microSD card of at least 64 GB inserted in the phone? (the plan shows exactly\n'
                         '   what is written to it; a card with another layout is erased only with --erase-card)'):
            raise Refused('insert a microSD card of at least 64 GB first')

    def bring_up(self):
        """Get a root shell on the phone that the installer drives. v2 (--transport env, the default):
        the Starbite installer environment, a Linux kernel + initramfs started from RAM by the
        Starbite loader's fastboot (`fastboot boot`), reached over USB networking (ssh). Nothing in
        the stock flow uses TWRP, and nothing ever writes or boots RECOVERY:
          stock Android (USB debugging)  -> adb reboot download
          Download mode                  -> PIT check, then BOOT = the Starbite loader (the only
                                            Download-mode write), then the user starts its fastboot
          the loader's fastboot          -> the installer environment from RAM
        --transport twrp (legacy, S9 #1's own maintenance): TWRP's adb as in v1."""
        u, p = self.ui, self.phone
        u.h1('Phone state')
        st = p.detect()
        u.info(f'found: {st.kind}' + (f' ({st.detail})' if st.detail else ''))
        legacy = getattr(self.o, 'transport', 'env') == 'twrp'
        if st.kind == 'env':
            if legacy:
                raise Refused('the Starbite installer environment is running; --transport twrp needs TWRP')
            self.env.activate()
            u.ok('the Starbite installer environment is up (USB networking, ssh)')
            return st
        if st.kind == 'recovery':
            if legacy:
                return st
            raise Refused('the phone is in TWRP. The installer works from its own environment, started from the '
                          'Starbite loader\'s fastboot: reboot, hold Bixby after the Samsung splash and pick USB FASTBOOT '
                          '(or hold Volume Up alone while it starts). For S9 #1\'s own maintenance: --transport twrp')
        if st.kind == 'unauthorized':
            raise Refused('adb says "unauthorized": allow USB debugging on the phone, then run again')
        if st.kind == 'fastboot-other':
            raise Refused(f'a fastboot device that is not the Starbite loader ({st.detail}) is connected')
        if st.kind == 'none':
            if self.mode == 'dry-run':
                u.warn('no phone found: the plan below is shown without the phone checks')
                return None
            raise Refused('no phone found. A stock phone: Download mode (phone off; hold Volume Down + Bixby, plug in '
                          'USB, then Volume Up). A Starbite phone: the loader\'s fastboot (Bixby after the Samsung '
                          'splash > USB FASTBOOT)')
        if st.kind == 'android':
            props = android_props(p)
            model = props.get('ro.product.model', '')
            u.info(f'Android: {model or "?"} ({props.get("ro.build.PDA") or props.get("ro.lineage.version") or "?"})')
            if model and model not in self.m.phone['models']:
                raise Refused(f'this is a {model}, not a Galaxy S9 SM-G960F: nothing was done')
            if props.get('ro.boot.flash.locked') == '1' or props.get('ro.boot.verifiedbootstate') == 'green':
                raise Refused('the bootloader is locked (ro.boot.flash.locked=1 / verifiedbootstate green): unlock it '
                              'first (Developer options > OEM unlocking, then Download mode: long-press Volume Up)')
            if is_stock_samsung(props):
                return self.from_stock_android(props)
            if legacy:
                cmd = ['adb', 'reboot', 'recovery']
            else:
                raise Refused('Starbite\'s Android is running: reboot and start the loader\'s fastboot (Bixby after the '
                              'Samsung splash > USB FASTBOOT), then run again')
        elif st.kind == 'fastboot-starbite':
            if not legacy:
                return self.start_env()
            raise Refused('--transport twrp: boot TWRP yourself (it is S9 #1\'s own maintenance path)')
        elif st.kind == 'download':
            return self.from_download_mode()
        else:
            raise Refused(f'unexpected phone state {st.kind}')
        # legacy: Starbite Android -> TWRP
        if self.mode == 'dry-run':
            u.cmd(' '.join(cmd) + '    [dry run: not run]')
            return None
        if not u.confirm('Reboot the phone into TWRP now? (legacy transport)'):
            raise Refused('the phone has to be in TWRP for --transport twrp')
        self.r.run(cmd)
        st = p.wait_for('recovery', 180)
        if st.kind != 'recovery':
            raise Refused(f'TWRP did not come up within 3 minutes (state: {st.kind})')
        return st

    def from_stock_android(self, props=None):
        """Stock Samsung Android with USB debugging: `adb reboot download`, then from_download_mode()."""
        u = self.ui
        props = props or {}
        bl = props.get('ro.boot.bootloader', '')
        u.info(f'S-Boot (bootloader) {bl or "?"}, firmware {props.get("ro.build.PDA") or "?"}: keep this; the rollback '
               'for BOOT is that firmware\'s boot.img (heimdall cannot read partitions)')
        if bl and bl != PROVEN_SBOOT:
            u.warn(f'S-Boot {bl} is not {PROVEN_SBOOT}, the only S-Boot the Starbite loader has run under (S9 #1)')
        u.info('stock Samsung firmware: the Starbite loader is not on this phone yet. The way in is Download mode:')
        u.cmd(f'adb {"-s " + self.phone.serial + " " if self.phone.serial else ""}reboot download' +
              ('    [dry run: not run]' if self.mode == 'dry-run' else ''))
        if self.mode == 'dry-run':
            u.warn('dry run: the phone is not rebooted. Next: put it in Download mode (the command above, or the keys) '
                   'and run the same dry run again: it reads the PIT (read only) and checks it')
            return None
        if not u.confirm('Reboot the phone into Download mode now? (it shows a blue "Downloading..." screen)'):
            raise Refused('the phone has to be in Download mode or the Starbite loader\'s fastboot')
        self.phone.adb('reboot', 'download')
        self.heimdall.closed()
        u.info('waiting up to 90 s for Download mode (USB 04e8:685d)...')
        for _ in range(45):
            if DOWNLOAD_MODE_USB in usb_ids():
                break
            time.sleep(2)
        else:
            raise Refused('Download mode did not appear. With the phone off: hold Volume Down + Bixby, plug in the '
                          'USB cable, then press Volume Up at the warning; run the installer again')
        if self.phone.detect().kind != 'download':
            raise Refused('Download mode did not appear on USB; run the installer again with the phone in Download mode')
        return self.from_download_mode()

    def from_download_mode(self):
        """A phone in Download mode: `heimdall detect`, its PIT read (`download-pit`, read only) and checked
        against the SM-G960F's table (an S9+ or any other phone is refused here), then the Starbite
        loader into BOOT (`heimdall flash --BOOT ... --no-reboot`: the ONLY Download-mode write; never
        RECOVERY, which keeps Samsung's stock recovery), then the user starts the loader's fastboot."""
        u = self.ui
        if self.o.transport == 'twrp':
            raise Refused('--transport twrp does not start from Download mode (the stock flow uses the loader)')
        boot = next((i for i in self.m.items if str(i.dest) == 'part:BOOT' and i.present), None)
        if boot is None:
            raise Refused('the phone is in Download mode, and the bundle has no BOOT (Starbite loader) image to flash')
        for k in ('env-kernel', 'env-initrd', 'env-dtb'):
            if not any(i.dest.kind == 'env' and i.dest.name == k[4:] and i.present for i in self.m.items):
                raise Refused(f'the bundle has no installer environment ({k}): after the loader nothing could go on')
        u.info('Download mode. First, read only: is this an SM-G960F? (heimdall detect, heimdall download-pit)')
        if self.r.run(['heimdall', 'detect'], check=False).returncode:
            raise Refused('heimdall does not see the phone in Download mode (heimdall detect). Install heimdall '
                          '(macOS: brew install heimdall; Linux: the heimdall-flash package), use a data USB cable, '
                          'and on Linux allow USB access (udev rule for 04e8:685d) or run as root')
        pdir = self.state_dir / 'download'
        pdir.mkdir(parents=True, exist_ok=True)
        pf = pdir / time.strftime('pit-%Y%m%d-%H%M%S.bin')
        self.heimdall.run(['download-pit', '--output', str(pf), '--no-reboot'])
        try:
            ok, unit, lines = pit.check(pf.read_bytes())
        except (OSError, pit.PitError) as e:
            raise Refused(f'could not read the phone\'s PIT ({e}): nothing was flashed') from None
        for ln in lines:
            u.info(ln)
        if not ok:
            raise Refused('the PIT is not an SM-G960F\'s (an S9+ SM-G965F, or another phone): nothing was flashed')
        u.ok(f'PIT: an SM-G960F ({self.rel(pf)})')
        serial = self.phone.serial
        jr = Journal(self.state_dir / phone_id_of(serial) / 'journal.json') if serial else None
        gate = f'download:{self.m.sha256}'
        alone = self.phone.download_alone()
        if alone:
            (u.warn if self.mode == 'dry-run' else u.info)(f'before a flash: {alone}')
        else:
            u.ok('only this phone is on USB (one Download-mode device, no other phone): heimdall cannot pick another')
        cmd = ['flash', '--BOOT', str(boot.path), '--no-reboot']
        u.info('The Starbite loader goes into BOOT. It is the only thing written in Download mode; RECOVERY keeps')
        u.info('Samsung\'s stock recovery (the installer never writes it). BOOT\'s stock image is in the backup')
        u.info('the installer takes next, and in Samsung\'s firmware.')
        u.cmd('heimdall ' + ' '.join(cmd) + ('    [dry run: not run]' if self.mode == 'dry-run' else ''))
        self.key_instructions()
        if self.mode == 'dry-run':
            if jr is not None:
                jr.record_dry_run(gate)
                u.ok(f'Download-mode dry run recorded for {phone_id_of(serial)}: the PIT is an SM-G960F\'s. '
                     '--real may now flash BOOT on this phone (the same bundle)')
            u.warn('dry run: BOOT is not flashed, so the plan below has no phone checks')
            return None
        if self.mode == 'real' and jr is None:
            raise Refused('--real needs --serial SERIAL (the phone\'s serial, from adb devices)')
        if jr is not None:          # --real always; a rehearsal when it names a serial
            if not jr.had_dry_run(gate):
                raise Refused(f'no Download-mode dry run of this bundle on {phone_id_of(serial)}: with the phone in '
                              'Download mode, run the same command with --dry-run first (it reads the PIT, writes '
                              'nothing), then --real')
        if alone:
            raise Refused(f'not flashing: {alone}')
        if not u.typed('Flash the Starbite loader into BOOT now?', 'flash boot'):
            raise Refused('BOOT not flashed')
        self.heimdall.run(cmd, write=True)
        u.ok('the Starbite loader is in BOOT. Now the keys (above).')
        u.info('waiting up to 5 minutes for the loader\'s fastboot (USB 18d1:4ee0)...')
        st = self.phone.wait_for('fastboot-starbite', 300)
        if st.kind != 'fastboot-starbite':
            raise Refused('the loader\'s fastboot did not appear. Start it with the keys above and run the installer '
                          'again (it finds the phone in fastboot and goes on)')
        self.heimdall.closed()
        u.ok('the Starbite loader\'s fastboot is up')
        return self.start_env()

    def start_env(self):
        """The installer environment from RAM through the loader's fastboot (nothing is written to the
        phone's storage: a dry run does this too)."""
        u = self.ui
        items = {i.dest.name: i for i in self.m.items if i.dest.kind == 'env' and i.present}
        missing = [k for k in ('kernel', 'initrd', 'dtb') if k not in items]
        if missing:
            raise Refused(f'the bundle has no installer environment ({", ".join("env:" + k for k in missing)})')
        serial, product = self.env.fastboot_info()
        if product and product != 'starlte':
            raise Refused(f'the loader says product {product}, not starlte (Galaxy S9)')
        u.info('starting the Starbite installer environment from RAM (Linux + initramfs; USB networking, ssh with a '
               'key made for this computer). Nothing is written to the phone by this.')
        self.env.boot(items['kernel'].path, items['initrd'].path, items['dtb'].path,
                      self.m.raw.get('env', {}).get('cmdline', 'sb.installer=1'), serial)
        u.ok('the installer environment is up: ssh root@172.16.42.1 (USB networking)')
        return self.phone.detect()

    def key_instructions(self):
        u = self.ui
        u.info('Then, on the phone, exactly:')
        u.info('  1. Hold Volume Down + Power (about 7 s) until the screen goes black: Download mode ends.')
        u.info('  2. At once hold Volume Up ALONE while the phone starts; let go when "STARBITE FASTBOOT" shows.')
        u.info('     (Or: hold Bixby right after the Samsung splash, and pick USB FASTBOOT in Starbite Recovery.)')
        u.info('  3. Leave the cable in. The installer finds the fastboot by itself.')

    # ------------------------------------------------------------------ preflight
    def preflight(self, st):
        u, f = self.ui, self.phone.facts()
        self.facts = f
        u.h1('Preflight (read only)')
        rows, fatal = [], []

        def row(name, value, ok, why='', hard=True):
            rows.append((name, value, 'ok' if ok else ('FAIL' if hard else 'warn')))
            if not ok:
                (fatal if hard else self.warnings).append(f'{name}: {why or value}')
        pr = f.props
        model, device = pr.get('ro.product.model', ''), pr.get('ro.product.device', '')
        row('model', f'{model or "?"} / {device or "?"}', model in self.m.phone['models'] or device in self.m.phone['devices'],
            'not a Galaxy S9 SM-G960F (starlte)')
        bl = pr.get('ro.boot.bootloader', '')
        row('bootloader', bl or '?', bl.startswith('G960F') or not bl, 'not a G960F bootloader')
        shell = 'TWRP ' + (pr.get('ro.twrp.version') or '?') if getattr(self.o, 'transport', 'env') == 'twrp' else \
            'installer environment ' + (pr.get('ro.starbite.env') or '?')
        row('unlocked', f'{shell} runs; flash.locked={pr.get("ro.boot.flash.locked") or "?"}'
            f' vbstate={pr.get("ro.boot.verifiedbootstate") or "?"}', pr.get('ro.boot.flash.locked') != '1',
            'ro.boot.flash.locked=1', hard=False)
        row('battery', f'{f.battery}%' if f.battery is not None else '?',
            (f.battery or 0) >= self.m.phone['min_battery'], f'below {self.m.phone["min_battery"]}%: charge first')
        want_ufs = self.m.phone.get('ufs_blocks')
        row('UFS', f'{human((f.sda or 0) * 512)} ({(f.sda or 0) // 8} blocks)',
            not want_ufs or (f.sda or 0) == want_ufs * 8, f'not {want_ufs} blocks')
        missing = [n for n in PROTECTED if n not in f.parts]
        row('protected areas', 'all present' if not missing else 'missing ' + ' '.join(missing), not missing,
            'this is not a stock-partitioned S9')
        bad = []
        layout_rows = [] if self.m.ufs_layout is not None else self.m.layout
        for L in layout_rows:
            p = f.parts.get(L['name'])
            if p is None:
                bad.append(f'{L["name"]} missing')
                continue
            node, start, size = p
            if L.get('node') and node != L['node']:
                bad.append(f'{L["name"]} is {node}, not {L["node"]}')
            if 'start' in L and start != L['start'] * 8:
                bad.append(f'{L["name"]} starts at block {start // 8}, not {L["start"]}')
            if size != L['blocks'] * 8:
                bad.append(f'{L["name"]} has {size // 8} blocks, not {L["blocks"]}')
        if self.m.ufs_layout is None:
            row('UFS layout', f'{len(self.m.layout)} partitions as the bundle expects' if not bad else '; '.join(bad[:4]),
                not bad, 'the GPT is not the Starbite layout this bundle was made for, and the bundle names no '
                '[ufs] layout to repartition to: see docs/INSTALLER.md 9')
        cbytes = (f.card or 0) * 512
        row('microSD', f'{human(cbytes)} ({f.card or 0} sectors) {f.card_name}'.strip() if f.card else 'none',
            cbytes >= self.m.card['min_bytes'], 'no card, or smaller than 64 GB')
        L = self.m.card_layout
        if f.card and L is not None:
            try:
                self.card_parts = L.parts_dicts(f.card)
                self.card_parts[0]['label'] = self.m.card['p1'].get('label', L.p1_label)
                ok, why = True, ''
            except cardlayout.LayoutError as e:
                ok, why = False, (f'{e}: this bundle\'s card roots and payloads are made for the {L.name} card layout; '
                                  f'this card needs a bundle made for the {cardlayout.for_card(f.card)} layout'
                                  if cardlayout.for_card(f.card) else str(e))
            row('card layout fit', f'bundle made for the {L.name} card layout ({L.title})', ok, why)
        elif f.card and self.m.card.get('sectors'):
            row('card layout fit', f'bundle made for a {self.m.card["sectors"]}-sector card',
                f.card == self.m.card['sectors'],
                f'this card has {f.card} sectors: the bundle\'s card roots are at fixed sectors (payload command '
                'lines), so it needs a bundle made for this card size')
        for t in ('gzip', 'sha256sum', 'blockdev'):
            if t not in f.tools:
                row(f'phone tool {t}', 'missing', False)
        free = self._host_free()
        row('host free space', human(free), free >= 512 * MIB, 'less than 512 MiB free for the backups')
        if free < 2 * GIB:
            self.warnings.append(f'host: only {human(free)} free at {self.state_dir}')
        serial = pr.get('ro.boot.serialno') or st.serial
        if self.phone.serial and serial != self.phone.serial:
            fatal.append(f'serial: the phone reports {serial or "none"}, this run is for {self.phone.serial}')
        self.phone_id = phone_id_of(serial)
        rows.append(('phone id', f'{self.phone_id} (a hash of the serial number)', 'ok'))
        self.journal = Journal(self.state_dir / self.phone_id / 'journal.json')
        if self.m.ufs_layout is not None and not fatal:
            self.rp = repart.Repart(self, self.m.ufs_layout)
            try:
                a = self.rp.assess()
                txt = {'skip': f'{self.m.ufs_layout.name} (both GPT copies valid)',
                       'reread': f'{self.m.ufs_layout.name}: written, the kernel has not read it yet',
                       'resume': f'{self.m.ufs_layout.name}: an earlier write was interrupted ({a.why}): resumes',
                       'repartition': f'another table: repartition to {self.m.ufs_layout.name} '
                                      f'({sum(r.status != "same" for r in a.plan.rows)} rows change)'}[a.status]
                row('UFS layout', txt, True)
                row('UFS block size', f'{f.lbs or "?"} bytes', f.lbs in (None, 4096), 'not 4096')
            except Refused as e:
                row('UFS layout', 'REFUSED', False, str(e))
        u.table(rows, ('check', 'value', ''))
        if fatal:
            raise Refused('preflight failed:\n  ' + '\n  '.join(fatal))

    @staticmethod
    def rel(p):
        p = str(p)
        return p[len(str(REPO)) + 1:] if p.startswith(str(REPO) + '/') else p

    def _host_free(self):
        p = self.state_dir
        while not p.exists():
            p = p.parent
        st = os.statvfs(p)
        return st.f_bavail * st.f_frsize

    # ------------------------------------------------------------------ targets
    def target(self, item):
        """(node, byte offset) of an item's destination on the phone."""
        d, f = item.dest, self.facts
        if d.kind == 'part':
            return f.node(d.name) or self.planned_node(d.name), 0
        if d.kind == 'slot':
            return f.node('USERDATA'), d.offset
        if d.kind == 'store':
            return (f.node('USERDATA'), STORE_UFS_OFFSET) if d.name == 'ufs' else ('/dev/block/mmcblk0p2', 0)
        if d.kind == 'card-lba':
            return '/dev/block/mmcblk0', d.offset * 512
        return '/external_sd' + d.name, 0

    def planned_node(self, name):
        """The node a partition will have after the repartitioning (dry run on a phone without it)."""
        if self.rp is not None and self.rp.a is not None:
            e = next((e for e in self.rp.a.plan.entries if e.name == name), None)
            if e is not None:
                return f'/dev/block/sda{e.index}'
        return None

    def planned_size(self, name):
        if self.rp is not None and self.rp.a is not None:
            e = next((e for e in self.rp.a.plan.entries if e.name == name), None)
            if e is not None:
                return e.blocks * 4096
        return self.facts.size(name)

    @staticmethod
    def part_of(item):
        d = item.dest
        if d.kind == 'part':
            return d.name
        if d.kind == 'slot' or (d.kind == 'store' and d.name == 'ufs'):
            return 'USERDATA'
        return None

    def is_fresh(self, item):
        """The item's partition was (or is about to be) re-laid by the GPT step and the item has not been
        written there since: whatever the place holds is not the item's data, so it is written without
        --replace. The list is per item and an item leaves it once written (journal 'fresh_items'), so
        data an OS writes later is never mistaken for a fresh place."""
        if self.journal and item.key in self.journal.d.get('fresh_items', []):
            return True
        a = getattr(getattr(self, 'rp', None), 'a', None)
        return a is not None and a.status in ('repartition', 'resume') and self.part_of(item) in a.fresh

    def forget_fresh(self, names):
        """The GPT step re-lays these partitions: earlier records of items on them no longer hold."""
        keys = set(self.journal.d.get('fresh_items', []))
        for it in self.m.items:
            if self.part_of(it) in names:
                self.journal.d['items'].pop(it.key, None)
                self.journal.chunk_state(it).unlink(missing_ok=True)
                keys.add(it.key)
        self.journal.d['fresh_items'] = sorted(keys)
        self.journal.save()

    def store_sums(self, item):
        hdr = open(item.path, 'rb').read(4096 * 2)
        magic, _, _, cap, used = struct.unpack_from('<8sIIQQ', hdr, 4096)
        if magic != b'SBSTORE2':
            raise Refused(f'{item.key}: {item.file} is not a version 2 store')
        with open(item.path, 'rb') as fh:
            a = hashlib.sha256(fh.read(8192)).hexdigest()
            fh.seek(12288)
            b = hashlib.sha256(fh.read(used * 4096 - 12288)).hexdigest()
        return used, a, b

    def matches(self, item):
        """The target already holds exactly this item (stores: all but the phone's settings block)."""
        node, off = self.target(item)
        if item.dest.kind == 'store':
            used, a, b = self.store_sums(item)
            return (self.phone.range_sha(node, off, 8192) == a and
                    self.phone.range_sha(node, off + 12288, used * 4096 - 12288) == b)
        if item.dest.kind == 'card-file':
            out = self.phone.sh(f'[ -f {node} ] && sha256sum {node}', check=False).split()
            return bool(out) and out[0] == item.sha256
        if item.action in MKFS_ACTIONS:           # a file system made on the phone is never "the same bytes"
            return False
        return self.phone.range_sha(node, off, item.size) == item.sha256

    def probe(self, item):
        """blank | data | differs | missing | too-big | no-part"""
        node, off = self.target(item)
        d = item.dest
        if d.kind == 'part' and item.action == 'write':
            psize = self.facts.size(d.name)
            if psize is None or item.size > psize:
                return 'too-big'
        if item.action in MKFS_ACTIONS and self.facts.size(d.name) is None:
            return 'no-part'
        if d.kind == 'card-file':
            if not self.facts.mount_of('/external_sd'):
                return 'missing'
            return 'data' if self.phone.sh(f'[ -e {node} ] && echo yes', check=False) == 'yes' else 'missing'
        if d.kind == 'store':
            if d.name == 'sd' and not self.card_ready():
                return 'blank'
            hdr = self.phone.range_sha(node, off, 12288)
            return 'blank' if hdr == hashlib.sha256(bytes(12288)).hexdigest() else 'differs'
        if d.kind == 'card-lba' and not self.card_ready():
            return 'blank'
        size = self.facts.size(d.name) if item.action in MKFS_ACTIONS else item.size    # mkfs: the whole partition
        if self.phone.is_blank(node, off, size):
            return 'blank'
        return 'data' if item.data else 'differs'

    def card_ready(self):
        return getattr(self, '_card_status', 'skip') == 'skip'

    # ------------------------------------------------------------------ plan
    def plan(self):
        o, j, steps = self.o, self.journal, []
        b = j.step('backup')
        if b.get('status') == 'done' and not o.fresh_backup:
            steps.append(Step('backup', 'Back up the protected areas', 'this computer', 'skip',
                              f'verified backup from {b["at"]} ({Path(b["dir"]).name})'))
        else:
            steps.append(Step('backup', 'Back up the protected areas', 'this computer', 'host',
                              'read only on the phone; resumes' if b.get('status') == 'started' else 'read only on the phone'))
        if self.rp is None:
            steps.append(Step('layout', 'Check the UFS layout (GPT)', '-', 'verify', 'checked in preflight; never written'))
        else:
            a, name = self.rp.a, self.m.ufs_layout.name
            title = f'UFS partition table ({name})'
            if a.status == 'skip':
                steps.append(Step('gpt', title, 'UFS GPT (sda)', 'skip', a.why))
            elif a.status in ('resume', 'reread'):
                steps.append(Step('gpt', title, 'UFS GPT (sda)', 'resume', a.why))
            elif o.repartition:
                steps.append(Step('gpt', title, 'UFS GPT (sda)', 'repartition',
                                  'REPARTITION (--repartition): ' + ', '.join(a.fresh) + ' get new places or sizes and '
                                  'are written fresh; the GPT is backed up first'))
            else:
                steps.append(Step('gpt', title, 'UFS GPT (sda)', 'BLOCKED',
                                  'the phone has another partition table; ' + ', '.join(a.fresh) + ' would get new '
                                  'places or sizes and lose their contents (a stock phone: its Android data): run again '
                                  'with --repartition'))
        steps += self.plan_card()
        items = sorted([i for i in self.m.items if i.dest.kind != 'env'], key=rank)   # env: RAM only
        cls = {i.key: self.classify(i) for i in items}
        # LineageOS's /data (an A16DATA image) and /metadata (ODM, mkfs-metadata) belong together: /metadata holds
        # /data's encryption keys. Only that pair: One UI's /data (mkfs-oneui-data) has no /metadata, and its ODM
        # is an image (action write), classified on its own.
        a16 = next((i for i in items if str(i.dest) == 'part:A16DATA' and i.action == 'write'), None)
        odm = next((i for i in items if i.action == 'mkfs-metadata'), None)
        if a16 and odm:
            renew = ('write', 'resume', 'replace')
            if cls[a16.key][0] in renew and cls[odm.key][0] not in renew:
                cls[odm.key] = ('write', 'fresh /metadata to go with the new /data')
            elif cls[odm.key][0] in renew and cls[a16.key][0] not in renew and a16.present:
                cls[odm.key] = ('keep', 'kept: /data is kept, and /metadata holds its keys')
        for i in items:
            st, why = cls[i.key]
            steps.append(Step(i.key, i.title, i.dest.where(), st, why, item=i))
        steps.append(Step('no-password', 'No user password on any OS', '-', 'verify', 'see the list after the plan'))
        steps.append(Step('finish', 'Finish: rescue USB offer, reboot into Starbite', '-', 'verify'))
        return steps

    def classify(self, i):
        o, j = self.o, self.journal
        if not i.present:
            return 'skip', 'not in this bundle (optional)'
        if self.is_fresh(i):                      # before the journal: the GPT step forgets earlier records
            a = getattr(getattr(self, 'rp', None), 'a', None)
            pending = a is not None and a.status in ('repartition', 'resume') and self.part_of(i) in a.fresh
            if not pending and j.chunk_state(i).exists():
                return 'resume', 'interrupted earlier (after the repartitioning): resumes'
            if i.dest.kind == 'part' and i.action == 'write' and i.size > (self.planned_size(i.dest.name) or 0):
                return 'BLOCKED', f'image ({human(i.size)}) is larger than {i.dest.name} in the new table'
            return 'write', f'{self.part_of(i)} is new in the repartitioned table: written fresh'
        if j.item_done(i):
            if i.data:
                return 'keep', 'installed and verified earlier by this installer; in use, not re-read'
            if self.matches(i):
                return 'skip', 'verified again: identical'
            return 'write', 'changed since this installer wrote it'
        if j.chunk_state(i).exists() or (j.item_rec(i) or {}).get('status') == 'started' and j.wrote_dest_before(i):
            return 'resume', 'interrupted earlier: resumes'
        if i.dest.kind in ('card-lba', 'card-file', 'store') and i.dest.name != 'ufs' and self._card_status in ('write', 'replace'):
            return 'write', 'the card is laid out new'
        p = self.probe(i)
        if p == 'too-big':
            return 'BLOCKED', f'image ({human(i.size)}) is larger than {i.dest.name}'
        if p == 'no-part':
            return 'BLOCKED', f'the phone has no {i.dest.name} partition'
        if p in ('blank', 'missing'):
            return 'write', 'target is empty'
        if not i.data and self.matches(i):
            return 'skip', 'already identical (verified by hash)'
        if i.data:
            if o.replace_all or i.key in o.replace:
                return 'replace', 'has data: REPLACED (--replace)'
            return 'keep', f'has data: kept (--replace {i.key} overwrites it)'
        return 'write', 'differs: overwritten (holds no user data)'

    def want_parts(self):
        """[p1, p2] as dicts: the card layout's for this card (preflight), else the manifest's."""
        if self.card_parts:
            return self.card_parts
        c = self.m.card
        return [c['p1'], c['p2']] if 'p1' in c and 'p2' in c and 'sectors' in c['p1'] else None

    def plan_card(self):
        self._card_status = 'skip'
        w = self.want_parts()
        if not w:
            return []
        mbr = self.phone.exec_out('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null')
        parts = mbr_parts(mbr)
        want = [(p['type'], p['start'], p['sectors']) for p in w]
        L = self.m.card_layout
        other = cardlayout.identify(parts, self.facts.card or 0) if parts and self.facts else None
        named = f' ({L.name})' if L is not None else ''
        if parts and parts[:2] == want:
            st, why = 'skip', f'the card already has the Starbite MBR{named}'
        elif not parts or not any(p[2] for p in parts):
            st, why = 'write', f'blank card{named}'
        elif self.o.erase_card:
            st, why = 'replace', (f'the Starbite {other} layout, this bundle is for {L.name if L else "another"}: ERASED '
                                  if other else 'another layout: ERASED ') + '(--erase-card): ' + self._parts_text(parts)
        else:
            st, why = 'BLOCKED', ((f'the card has the Starbite {other} card layout and this bundle is made for '
                                   f'{L.name if L else "another one"} (' if other else 'the card has another layout (') +
                                  self._parts_text(parts) + '); everything on it would be lost: run again with --erase-card')
        self._card_status = st
        steps = [Step('card-layout', 'microSD partition table (p1 exFAT, p2 raw)', 'microSD sector 0', st, why)]
        p1 = w[0]
        boot = self.phone.exec_out(f'dd if=/dev/block/mmcblk0 bs=512 skip={p1["start"]} count=1 2>/dev/null')
        has_exfat = boot[3:11] == b'EXFAT   '
        if st in ('write', 'replace'):
            fst, fwhy = 'write', f'exFAT "{p1.get("label", "STARBITE")}" on p1'
        elif has_exfat:
            fst, fwhy = 'skip', 'p1 is already exFAT'
        elif self.o.erase_card:
            fst, fwhy = 'replace', 'p1 is not exFAT: formatted (--erase-card)'
        else:
            fst, fwhy = 'BLOCKED', 'p1 is not exFAT: run again with --erase-card to format it'
        steps.append(Step('card-format', 'Format microSD p1 (exFAT)', 'microSD p1', fst, fwhy))
        return steps

    @staticmethod
    def _parts_text(parts):
        return ', '.join(f'p{n + 1} type 0x{t:02x} {s}+{c}' for n, (t, s, c) in enumerate(parts) if c)

    def show_plan(self, steps):
        u = self.ui
        u.h1('The plan')
        rows = []
        for n, s in enumerate(steps, 1):
            w = writer_of(s.item) if s.item else ''
            size = human(s.item.size) if s.item and s.item.size and s.item.present else ''
            rows.append((n, s.status.upper() if s.status in ('replace', 'BLOCKED') else s.status, s.title, s.where, size, w))
        u.table(rows, ('#', 'action', 'what', 'where', 'size', 'by'))
        u.emit()
        for n, s in enumerate(steps, 1):
            if s.reason:
                u.info(f'{n:>2}. {s.reason}')
        cov_rows, cov_warn = coverage(self.m)
        if cov_rows:
            u.h2('Menu entries this bundle covers')
            u.table(cov_rows, ('key', 'OS', 'files + root'))
        self.warnings += cov_warn
        self.show_card_layout()
        u.h2('No user password on any OS')
        self.plan_status = {s.key: s.status for s in steps}
        checks = nopassword.report(self.m, self.journal, self.plan_status)
        u.table([(c.os, c.status, c.evidence) for c in checks], ('OS', 'status', 'how / evidence'))
        for c in checks:
            if c.status == 'OPEN':
                self.warnings.append(f'password: {c.os}: {c.evidence}')

    # ------------------------------------------------------------------ run
    def run(self):
        self.welcome()
        if self.mode == 'rehearse':
            v = self.r.run(['adb', 'version'], check=False).stdout or ''
            if 'starbite fake adb' not in v:
                raise Refused('--rehearse needs the fake adb first on PATH (--fake-root DIR)')
        st = self.bring_up()
        if st is None:
            return self.offline_plan()
        self.preflight(st)
        if getattr(self.o, 'backup_only', False):
            return self.backup_only()
        if self.mode in ('real', 'rehearse') and not self.journal.had_dry_run(self.m.sha256):
            raise Refused(f'no finished dry run of this bundle on {self.phone_id}: run the same command with '
                          '--dry-run first (it writes nothing), then again')
        steps = self.plan()
        self.show_plan(steps)
        blocked = [s for s in steps if s.status == 'BLOCKED']
        if blocked:
            raise Refused('the plan has blocked steps: ' + '; '.join(f'{s.title}: {s.reason}' for s in blocked))
        if self.warnings:
            self.ui.h2('Warnings')
            for w in self.warnings:
                self.ui.warn(w)
        if self.mode == 'dry-run':
            self.ui.h1('Dry run: the commands each step would run')
        else:
            n_w = sum(s.status in ('write', 'resume', 'replace') for s in steps)
            if not self.ui.typed(f'{n_w} steps write to the phone ({self.mode}). Start?', f'install {self.phone_id}'):
                raise Refused('not confirmed: nothing written')
        for n, s in enumerate(steps, 1):
            self.ui.h1(f'Step {n}/{len(steps)}: {s.title}  [{s.status}]')
            self.execute(s)
        if self.mode == 'dry-run':
            self.journal.record_dry_run(self.m.sha256)
            self.ui.h1('Dry run finished')
            self.ui.ok(f'nothing was written ({len(self.r.planned)} write commands printed). '
                       'The real mode (--real, or --rehearse on a fake phone) is now allowed for this bundle on this phone.')
        return 0

    def backup_only(self):
        """--backup-only: step 1 alone (it reads the phone and writes only to this computer), then every
        file is read back here and checked against the phone's SHA-256. No dry run needed; nothing else runs."""
        u = self.ui
        b = self.journal.step('backup')
        if b.get('status') == 'done' and not self.o.fresh_backup:
            u.ok(f'a complete backup exists ({b["at"]}, {self.rel(b["dir"])}); --fresh-backup takes another')
            d = Path(b['dir'])
        else:
            u.h1('Back up the protected areas (read only on the phone)')
            self.do_backup(Step('backup', 'Back up the protected areas', 'this computer', 'host'))
            if self.mode == 'dry-run':
                return 0
            d = Path(self.journal.step('backup')['dir'])
        return self.report_backup(d)

    def report_backup(self, d):
        u = self.ui
        oks, problems = check_backup_dir(d)
        for ln in oks:
            u.ok(ln)
        if problems:
            raise Stopped(f'the backup in {self.rel(d)} does not check out:\n  ' + '\n  '.join(problems))
        u.ok(f'backup verified on this computer: {len(oks)} files, each equal to the phone\'s SHA-256 '
             f'({self.rel(d)})')
        return 0

    def show_card_layout(self):
        """The bundle's card layout, and what it still needs (a rebuild, the phone): PENDING lines."""
        L, u = self.m.card_layout, self.ui
        if L is None:
            return
        u.h2(f'microSD card layout: {L.name}')
        u.info(f'{L.title}; tools/store/cardlayout.py show {L.name}')
        w = self.want_parts()
        if w:
            u.info(f'this card: p1 exFAT {w[0]["start"]}+{w[0]["sectors"]}, p2 0xda {w[1]["start"]}+{w[1]["sectors"]}')
        for r in L.roots():
            u.info(f'  root {r.name:<6} sectors {r.start}+{r.sectors} ({r.what}; menu {", ".join(r.keys)})')
        for p in L.pending:
            u.info(f'  PENDING: {p}')
            if f'card layout {L.name}: PENDING: {p}' not in self.warnings:
                self.warnings.append(f'card layout {L.name}: PENDING: {p}')

    def offline_plan(self):
        u = self.ui
        u.h1('The plan (no phone: targets not checked)')
        rows = [(n, i.title, i.dest.where(), human(i.size) if i.size else '', writer_of(i),
                 'present' if i.present else 'NOT IN BUNDLE')
                for n, i in enumerate(sorted([i for i in self.m.items if i.dest.kind != 'env'], key=rank), 1)]
        u.table(rows, ('#', 'what', 'where', 'size', 'by', 'file'))
        cov_rows, cov_warn = coverage(self.m)
        if cov_rows:
            u.h2('Menu entries this bundle covers')
            u.table(cov_rows, ('key', 'OS', 'files + root'))
        self.show_card_layout()
        u.h2('No user password on any OS')
        u.table([(c.os, c.status, c.evidence) for c in nopassword.report(self.m)], ('OS', 'status', 'how / evidence'))
        for w in cov_warn:
            u.warn(w)
        u.ok('offline dry run: nothing was read from or written to a phone')
        return 0

    def execute(self, s):
        if s.status in ('skip', 'keep'):
            self.ui.ok(f'{s.status}: {s.reason}')
            self.done['skipped' if s.status == 'skip' else 'kept'] += 1
            if s.item is not None and s.status == 'keep' and self.mode != 'dry-run':
                if not self.journal.item_done(s.item):
                    self.journal.mark_item(s.item, 'kept')
            elif s.item is not None and s.status == 'skip' and s.item.present and self.mode != 'dry-run':
                self.journal.mark_item(s.item, 'done', fresh=False)
            return
        fn = {'backup': self.do_backup, 'layout': lambda s: self.ui.ok('the UFS layout matched in preflight'),
              'gpt': self.do_gpt,
              'card-layout': self.do_card_layout, 'card-format': self.do_card_format,
              'no-password': self.do_no_password, 'finish': self.do_finish}.get(s.key)
        if fn:
            return fn(s)
        return self.do_item(s)

    # ------------------------------------------------------------------ steps
    def do_backup(self, s):
        u, j = self.ui, self.journal
        rec = j.step('backup')
        if rec.get('status') == 'started' and not self.o.fresh_backup:
            d = Path(rec['dir'])
        else:
            d = self.state_dir / self.phone_id / 'backups' / time.strftime('%Y%m%d-%H%M%S')
        done = set(rec.get('names', [])) if rec.get('dir') == str(d) else set()
        if self.mode != 'dry-run':
            d.mkdir(parents=True, exist_ok=True)
            j.mark_step('backup', 'started', dir=str(d), names=sorted(done))
        u.info(f'to {self.rel(d)}')
        env = {'BACKUP_LOG': str(d / 'backups.txt')}
        names = [n for n in BACKUP_REQUIRED + BACKUP_OPTIONAL if n in self.facts.parts]
        for n in BACKUP_OPTIONAL:
            if n not in self.facts.parts:
                u.info(f'{n}: not on this phone, skipped')
        for n in names:
            if n in done:
                u.ok(f'{n}: done earlier')
                continue
            self.r.run(['bash', TOOL['part-backup'], 'mac', n, str(d)], write=True, env_extra=env, stream=True)
            if self.mode != 'dry-run':
                done.add(n)
                j.mark_step('backup', 'started', dir=str(d), names=sorted(done))
        for name, dev, how in BACKUP_RAW:
            if name in done:
                continue
            if self.mode == 'dry-run':
                u.cmd(f'adb exec-out "dd if=/dev/block/{dev} ..." > {name}  ({how}; phone sha256 == local sha256)'
                      '    [dry run: not run]')
                continue
            if self.phone.sh(f'[ -e /dev/block/{dev} ] && echo yes', check=False) != 'yes':
                u.info(f'{dev}: not on this phone, skipped')
                continue
            if how == 'head':
                cmd = f'dd if=/dev/block/{dev} bs=4096 count=1024 2>/dev/null'
            elif how == 'tail':
                blocks = (self.facts.sda or 0) // 8
                cmd = f'dd if=/dev/block/{dev} bs=4096 skip={blocks - 5} count=5 2>/dev/null'
            else:
                cmd = f'dd if=/dev/block/{dev} bs=1048576 2>/dev/null'
            data = self.phone.exec_out(cmd)
            there = self.phone.sh(f'{cmd} | sha256sum').split()[0]
            here = hashlib.sha256(data).hexdigest()
            if here != there or not data:
                raise Stopped(f'backup of {name}: local {here[:16]} != phone {there[:16]}: stopping (nothing was written)')
            (d / f'{name}.img').write_bytes(data)
            with open(d / 'backups.txt', 'a') as fh:
                fh.write(f'{here}  {name}.img (raw {how} of {dev})\n')
            u.ok(f'{name}: {human(len(data))}, sha256 {here[:16]}... (phone and local agree)')
            done.add(name)
            j.mark_step('backup', 'started', dir=str(d), names=sorted(done))
        if self.mode != 'dry-run':
            oks, problems = check_backup_dir(d)
            if problems:
                raise Stopped(f'backup in {self.rel(d)}: read back on this computer, it does not check out:\n  ' +
                              '\n  '.join(problems) + '\nnothing was written to the phone')
            j.mark_step('backup', 'done', dir=str(d), names=sorted(done))
            u.ok(f'backup complete: {len(done)} items in {self.rel(d)}, every file re-read here = the phone\'s SHA-256')

    def do_gpt(self, s):
        u = self.ui
        for ln in self.rp.describe():
            u.info(ln)
        self.rp.run()
        if self.mode != 'dry-run' and s.status != 'skip':
            self.facts = self.phone.facts()

    def restore_gpt(self):
        """--restore-gpt: the phone's own GPT, as saved before the repartitioning, written back."""
        self.ui.banner()
        st = self.phone.detect()
        if st.kind == 'fastboot-starbite' and self.o.transport != 'twrp':
            raise Refused('--restore-gpt: start the installer environment first (run the installer with --bundle once, '
                          'or --start-env --bundle B)')
        if st.kind == 'env':
            self.env.activate()
        elif not (st.kind == 'recovery' and self.o.transport == 'twrp'):
            raise Refused(f'--restore-gpt needs the installer environment (found: {st.kind})')
        self.facts = self.phone.facts()
        serial = self.facts.props.get('ro.boot.serialno') or st.serial
        self.phone_id = 'phone-' + hashlib.sha256(serial.encode()).hexdigest()[:10]
        self.journal = Journal(self.state_dir / self.phone_id / 'journal.json')
        repart.Repart(self, None).restore()
        return 0

    def do_card_layout(self, s):
        p, u = self.phone, self.ui
        p1, p2 = self.want_parts()
        mbr = build_mbr(p1, p2)
        p2_head = p2['start'] // 8
        u.info('p1 type 0x%02x %d+%d, p2 type 0x%02x %d+%d' % (p1['type'], p1['start'], p1['sectors'],
                                                               p2['type'], p2['start'], p2['sectors']) +
               (f' (card layout {self.m.card_layout.name})' if self.m.card_layout else ''))
        if self.mode == 'dry-run':
            u.cmd('adb push mbr.bin /tmp/sbinst-mbr.bin; adb shell "umount /external_sd; dd if=/tmp/sbinst-mbr.bin '
                  'of=/dev/block/mmcblk0 bs=512 count=1 conv=notrunc,fsync"    [dry run: not run]')
            u.cmd(f'adb shell "dd if=/dev/zero of=/dev/block/mmcblk0 bs=4096 seek={p2_head} count=3 conv=notrunc,fsync; '
                  'blockdev --rereadpt /dev/block/mmcblk0"    [dry run: not run]')
            return
        import tempfile
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'mbr.bin').write_bytes(mbr)
            p.push(str(Path(t, 'mbr.bin')), '/tmp/sbinst-mbr.bin')
        p.sh('umount /external_sd 2>/dev/null; true', write=True)
        p.sh('dd if=/tmp/sbinst-mbr.bin of=/dev/block/mmcblk0 bs=512 count=1 conv=notrunc,fsync 2>/dev/null; '
             'rm -f /tmp/sbinst-mbr.bin', write=True)
        back = p.exec_out('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null')
        if back != mbr:
            raise Stopped('card MBR read back differs: stopping')
        # the store tools refuse a p2 that holds old data: clear its first 3 blocks (store header area)
        p.sh(f'dd if=/dev/zero of=/dev/block/mmcblk0 bs=4096 seek={p2_head} count=3 conv=notrunc,fsync 2>/dev/null', write=True)
        p.sh('blockdev --rereadpt /dev/block/mmcblk0', write=True)
        for _ in range(10):
            if p.sh('cat /sys/class/block/mmcblk0p1/start 2>/dev/null', check=False) == str(p1['start']):
                break
            time.sleep(1)
        else:
            raise Stopped('the kernel did not pick up the new card partitions (run the installer again: it restarts the environment)')
        u.ok('card MBR written and read back; partitions re-read')

    def do_card_format(self, s):
        p, u, f = self.phone, self.ui, self.facts
        p1 = self.want_parts()[0]
        label = p1.get('label', 'STARBITE')
        tool = 'mkfs.exfat' if 'mkfs.exfat' in f.tools else 'mkexfatfs' if 'mkexfatfs' in f.tools else None
        cmd = f'umount /external_sd 2>/dev/null; {tool or "mkfs.exfat"} -n {label} /dev/block/mmcblk0p1'
        if self.mode == 'dry-run':
            u.cmd(f'adb shell "{cmd}"    [dry run: not run]')
            if not tool:
                u.warn('the phone shows no mkfs.exfat/mkexfatfs: the real run would stop here (format p1 as exFAT another way)')
            return
        if not tool:
            raise Stopped('the phone has no exFAT formatter (mkfs.exfat / mkexfatfs): format microSD p1 as exFAT '
                          f'"{label}" from a computer, then run again')
        p.sh(cmd, write=True)
        boot = p.exec_out(f'dd if=/dev/block/mmcblk0 bs=512 skip={p1["start"]} count=1 2>/dev/null')
        if boot[3:11] != b'EXFAT   ':
            raise Stopped('p1 does not read back as exFAT after formatting')
        u.ok(f'p1 formatted exFAT "{label}"')

    def ensure_card_mounted(self):
        if self.facts.mount_of('/external_sd'):
            return
        f = self.facts = self.phone.facts()
        if f.mount_of('/external_sd'):
            return
        cmd = ('mkdir -p /external_sd; mount -t exfat /dev/block/mmcblk0p1 /external_sd 2>/dev/null || '
               'exfat-fuse /dev/block/mmcblk0p1 /external_sd')
        if self.mode == 'dry-run':
            self.ui.cmd(f'adb shell "{cmd}"    [dry run: not run]')
            return
        self.phone.sh(cmd, write=True)
        self.facts = self.phone.facts()
        if not self.facts.mount_of('/external_sd'):
            raise Stopped('could not mount microSD p1 at /external_sd')

    def unmount_target(self, node, name=None):
        """Unmount whatever is mounted from node (or, given the partition name, from its by-name link)."""
        devs = {node} | ({f'/dev/block/by-name/{name}'} if name else set())
        hit = [mp for dev, mp, _ in self.facts.mounts if dev in devs or dev.endswith('/' + node.rsplit('/', 1)[-1])]
        for mp in hit:
            self.phone.sh(f'umount {mp}', write=True)
        if hit and self.mode != 'dry-run':
            self.facts = self.phone.facts()
            if [m for m in self.facts.mounts if m[0] in devs]:
                raise Stopped(f'{node} is still mounted after umount: stopping')

    # One UI's /data: a fresh ext4 that ends ONEUI_DATA_RESERVE blocks before the partition's end (its fstab:
    # length=-20480). The command is docs/ANDROID-ONEUI.md 4's, run by the phone's own mke2fs (the installer
    # environment's e2fsprogs with its 4.9-kernel mke2fs.conf, or TWRP's).
    def oneui_data_blocks(self, item, node):
        """(partition blocks, file system blocks), 4 KiB blocks: the phone's own blockdev in a real run, the
        planned table in a dry run (the partition may only exist after the GPT step)."""
        if self.mode == 'dry-run':
            size = self.planned_size(item.dest.name)
        else:
            out = self.phone.sh(f'blockdev --getsize64 {node}', check=False)
            size = int(out) if out.isdigit() else None
        if not size or size % 4096 or size // 4096 <= ONEUI_DATA_RESERVE:
            raise Stopped(f'{item.key}: cannot tell the size of {item.dest.name} ({node}: {size!r} bytes): stopping')
        return size // 4096, size // 4096 - ONEUI_DATA_RESERVE

    @staticmethod
    def oneui_data_cmd(node, fs_blocks):
        return f'mke2fs -t ext4 -F -L data -b 4096 {node} {fs_blocks}'

    def mkfs_oneui_data(self, item, node):
        u = self.ui
        part, fs = self.oneui_data_blocks(item, node)
        u.info(f'{item.dest.name} = {node}: {part} blocks of 4 KiB; ext4 "data" of {fs} blocks '
               f'({ONEUI_DATA_RESERVE} blocks = {ONEUI_DATA_RESERVE * 4096} bytes left free: One UI\'s length=-20480)')
        if 'mke2fs' not in self.facts.tools:
            if self.mode == 'dry-run':
                u.warn('the phone shows no mke2fs: the real run would stop here')
            else:
                raise Stopped(f'{item.key}: the phone has no mke2fs (the installer environment carries e2fsprogs)')
        out = self.phone.sh(self.oneui_data_cmd(node, fs), write=True)
        if out:
            u.info('| ' + out.splitlines()[-1])

    def ext4_superblock(self, node):
        """(TYPE, LABEL, block count, block size) as blkid and dumpe2fs -h (or tune2fs -l) see node."""
        out = self.phone.sh(f'blkid {node}', check=False)
        typ = re.search(r'(?<![A-Z_])TYPE="([^"]*)"', out)
        lab = re.search(r'(?<![A-Z_])LABEL="([^"]*)"', out)
        sb = self.phone.sh(f'dumpe2fs -h {node} 2>/dev/null || tune2fs -l {node}', check=False)
        cnt = re.search(r'^Block count:\s*(\d+)', sb, re.M)
        bsz = re.search(r'^Block size:\s*(\d+)', sb, re.M)
        return (typ.group(1) if typ else '', lab.group(1) if lab else '',
                int(cnt.group(1)) if cnt else None, int(bsz.group(1)) if bsz else None)

    def do_item(self, s):
        i, u, j, p = s.item, self.ui, self.journal, self.phone
        d = i.dest
        u.info(f'{i.file or i.action} -> {d.where()}' + (f'  ({human(i.size)}, sha256 {i.sha256[:16]}...)' if i.size else ''))
        if i.note:
            u.info(i.note)
        if d.kind == 'card-file':
            self.ensure_card_mounted()
        node, off = self.target(i)
        if self.mode != 'dry-run':
            j.mark_item(i, 'started')
        state = str(j.chunk_state(i))
        py = [sys.executable, '-I']
        if d.kind == 'part' and i.action == 'write':
            self.unmount_target(node)
        if i.action == 'mkfs-metadata':
            self.unmount_target(node)
            bk = Path(j.step('backup').get('dir', '')) / 'backups.txt'
            self.r.run(['bash', TOOL['metadata-reset']], write=True, env_extra={'BACKUP_LOG': str(bk)}, stream=True)
        elif i.action == 'mkfs-oneui-data':
            self.unmount_target(node, d.name)
            self.mkfs_oneui_data(i, node)
        elif d.kind == 'part' and d.name in PART_WRITE_PY:
            self.r.run([*py, TOOL['part-write'], d.name, i.path, '--state', state], write=True, stream=True)
        elif d.kind == 'part' and d.name in NATIVE_PARTS:
            chunk_write(p, u, i.path, i.size, node, 0, state, d.name)
        elif d.kind == 'slot':
            self.r.run([*py, TOOL['slot-write'], i.path, str(d.offset)], write=True, stream=True)
        elif d.kind == 'store':
            self.r.run(['bash', TOOL['store-ufs' if d.name == 'ufs' else 'store-sd'], i.path], write=True, stream=True)
        elif d.kind == 'card-lba':
            L = self.m.card_layout                # sd128 keeps the tool's default (the 128 GB card only)
            lay = ['--layout', L.name] if L is not None and L.name != 'sd128' else []
            self.r.run([*py, TOOL['card-range'], i.path, str(d.offset), '--state', state, *lay], write=True, stream=True)
        elif d.kind == 'card-file':
            self.write_card_file(i, node, state)
        if self.mode == 'dry-run':
            u.cmd(self.verify_text(i, node, off))
            return
        self.verify(i, node, off)
        j.mark_item(i, 'done', fresh=True)
        if i.key in j.d.get('fresh_items', []):
            j.d['fresh_items'].remove(i.key)
            j.save()
        Path(state).unlink(missing_ok=True)
        self.done['written'] += 1

    def write_card_file(self, i, node, state):
        p, u = self.phone, self.ui
        part = node + '.part'
        parent = node.rsplit('/', 1)[0]
        if self.mode != 'dry-run':
            free = (self.facts.sdfree or 0) * 1024
            if free and free < i.size:
                raise Stopped(f'{i.key}: {human(free)} free on the card, {human(i.size)} needed')
            if not os.path.exists(state):
                p.sh(f'mkdir -p {parent}; rm -f {part}', write=True)
        else:
            u.cmd(f'adb shell "mkdir -p {parent}; rm -f {part}"    [dry run: not run]')
        chunk_write(p, u, i.path, i.size, part, 0, state, i.dest.name, file_mode=True)
        if self.mode != 'dry-run':
            out = p.sh(f'wc -c < {part}; sha256sum {part}').split()
            if len(out) < 2 or int(out[0]) != i.size or out[1] != i.sha256:
                raise Stopped(f'{i.dest.name}: the written file is {out[:2]}, expected {i.size} bytes {i.sha256[:16]}...')
            p.sh(f'mv -f {part} {node}', write=True)
        else:
            u.cmd(f'adb shell "mv -f {part} {node}"    [dry run: not run]')

    def verify_text(self, i, node, off):
        if i.action == 'mkfs-metadata':
            return f'verify: metadata-reset.sh checks blkid TYPE="ext4" LABEL="metadata" on {node}'
        if i.action == 'mkfs-oneui-data':
            part, fs = self.oneui_data_blocks(i, node)
            return (f'verify: adb shell "blkid {node}" has TYPE="ext4" LABEL="data"; adb shell "dumpe2fs -h {node}" '
                    f'(or tune2fs -l) has Block count {fs} (= {part} - {ONEUI_DATA_RESERVE}), Block size 4096')
        if i.dest.kind == 'store':
            return 'verify: the store area on the phone == the image, except block 2 (the phone\'s settings record, kept)'
        if i.dest.kind == 'card-file':
            return f'verify: adb shell "sha256sum {node}" == {i.sha256[:16]}...'
        return f'verify: adb shell "dd if={node} ... skip={off} | head -c {i.size} | sha256sum" == {i.sha256[:16]}...'

    def verify(self, i, node, off):
        if i.action == 'mkfs-metadata':
            out = self.phone.sh(f'blkid {node}', check=False)
            ok = 'TYPE="ext4"' in out and 'LABEL="metadata"' in out
            what = out
        elif i.action == 'mkfs-oneui-data':
            part, fs = self.oneui_data_blocks(i, node)
            typ, label, count, bsize = self.ext4_superblock(node)
            ok = (typ, label, count, bsize) == ('ext4', 'data', fs, 4096)
            what = f'TYPE {typ or "?"} LABEL {label or "?"}, {count} blocks of {bsize}' + \
                ('' if ok else f'; want ext4 "data", {fs} (= {part} - {ONEUI_DATA_RESERVE}) blocks of 4096')
        elif i.dest.kind == 'card-file':
            out = self.phone.sh(f'sha256sum {node}').split()
            ok, what = bool(out) and out[0] == i.sha256, out[0] if out else ''
        else:
            ok = self.matches(i)
            what = '' if ok else 'store differs' if i.dest.kind == 'store' else self.phone.range_sha(node, off, i.size)
        if not ok:
            raise Stopped(f'{i.key}: VERIFY FAILED on the phone ({what[:160]}): stopping. A re-run writes it again.')
        self.ui.ok(f'{i.key}: verified on the phone (read back)' + (f': {what}' if i.action == 'mkfs-oneui-data' else ''))

    def do_no_password(self, s):
        u = self.ui
        for c in nopassword.report(self.m, self.journal, getattr(self, 'plan_status', None)):
            (u.ok if c.status in ('ok', 'by-design', 'n/a') else u.warn)(f'{c.os}: {c.status}: {c.evidence}')

    def do_finish(self, s):
        u = self.ui
        u.h2('Starbite rescue USB disk')
        u.info('Rescue mode (hold Bixby at boot > Starbite Recovery > Starbite Rescue mode) asks for a rescue USB')
        u.info('disk. Make one now from a USB stick (up to 256 GB; it is erased):')
        img = self.state_dir / 'rescue' / 'starbite-rescue-usb.img'
        u.cmd(f'tools/installer/starbite-install --make-rescue-image {img}')
        u.cmd(f'tools/installer/starbite-install --make-rescue-usb /dev/diskN --image {img} --real')
        if self.mode != 'dry-run' and self.o.rescue_usb:
            if not img.is_file():
                rescue.build_image(img, u)
            rescue.write_stick(img, self.o.rescue_usb, u, self.mode)
        u.h2('Summary')
        u.info(f'written {self.done["written"]}, skipped {self.done["skipped"]}, kept {self.done["kept"]}'
               f'; {self.r.writes} write commands ran')
        for w in self.warnings:
            u.warn(w)
        if self.mode == 'dry-run':
            return
        if self.o.no_reboot or not u.confirm('Reboot into the Starbite menu now?'):
            u.info('not rebooted: adb reboot when ready')
            return
        self.phone.adb('reboot')
        u.ok('rebooting: the Starbite menu should appear after the Samsung splash')
