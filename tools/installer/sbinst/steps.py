"""The install flow: welcome, the two confirmations, phone state, preflight (read only), plan,
then the steps. Every step is idempotent (it checks the target first), resumable (the journal and
per-item chunk state) and verified (read-back SHA-256 on the phone)."""
import hashlib
import os
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from . import nopassword, rescue
from .common import (BACKUP_OPTIONAL, BACKUP_RAW, BACKUP_REQUIRED, GIB, MIB, NATIVE_PARTS, PART_WRITE_PY,
                     PROTECTED, REPO, Refused, Stopped, human)
from .device import Phone, chunk_write
from .journal import Journal
from .manifest import coverage, writer_of

STORE_UFS_OFFSET = 16 * GIB                 # store-flash.sh: USERDATA + 16 GiB
TOOL = {k: str(REPO / v) for k, v in {
    'part-write': 'tools/android/pixel/part-write.py',
    'part-backup': 'tools/android/pixel/part-backup.sh',
    'metadata-reset': 'tools/android/pixel/metadata-reset.sh',
    'slot-write': 'tools/refport/slot-write.py',
    'store-ufs': 'tools/store/store-flash.sh',
    'store-sd': 'tools/store/store-flash-sd.sh',
    'card-range': 'tools/macos/card-range-flash.py'}.items()}


def rank(item):
    """Install order. BOOT is last: until it is written, the phone still starts TWRP's way."""
    d = item.dest
    if d.kind == 'slot':
        return 10
    if d.kind == 'store':
        return 20 if d.name == 'ufs' else 50
    if d.kind == 'part':
        return {'SYSTEM': 30, 'VENDOR': 31, 'A16DATA': 32, 'ODM': 33, 'WINESP': 40, 'WINDOWS': 41,
                'RECOVERY': 90, 'BOOT': 99}[d.name]
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

    def bring_to_twrp(self):
        u, p = self.ui, self.phone
        u.h1('Phone state')
        st = p.detect()
        u.info(f'found: {st.kind}' + (f' ({st.detail})' if st.detail else ''))
        live = self.mode != 'dry-run'
        if st.kind == 'recovery':
            return st
        if st.kind == 'unauthorized':
            raise Refused('adb says "unauthorized": allow USB debugging on the phone, then run again')
        if st.kind == 'fastboot-other':
            raise Refused(f'a fastboot device that is not the Starbite loader ({st.detail}) is connected')
        if st.kind == 'none':
            if self.mode == 'dry-run':
                u.warn('no phone found: the plan below is shown without the phone checks')
                return None
            raise Refused('no phone found. Put it in TWRP (Vol Up + Bixby + Power), the Starbite menu\'s fastboot, '
                          'or Download mode (Vol Down + Bixby + Power, then Vol Up), and connect USB')
        if st.kind in ('android', 'fastboot-starbite'):
            cmd = ['adb', 'reboot', 'recovery'] if st.kind == 'android' else ['fastboot', 'reboot', 'recovery']
            u.info(f'the installer works from TWRP: {" ".join(cmd)}')
            if not live:
                u.cmd(' '.join(cmd) + '    [dry run: not run]')
                u.warn('dry run: the phone is not rebooted, so the plan below has no phone checks')
                return None
            if not u.confirm('Reboot the phone into TWRP now?'):
                raise Refused('the phone has to be in TWRP')
            self.r.run(cmd)
            st = p.wait_for('recovery', 180)
            if st.kind != 'recovery':
                raise Refused(f'TWRP did not come up within 3 minutes (state: {st.kind})')
            u.ok('TWRP is up')
            return st
        if st.kind == 'download':
            return self.from_download_mode()
        raise Refused(f'unexpected phone state {st.kind}')

    def from_download_mode(self):
        """A phone in Download mode gets TWRP into RECOVERY with Heimdall (the only Download-mode write
        the installer makes; RECOVERY's stock image is in Samsung's firmware), then the user boots it."""
        u = self.ui
        twrp = next((i for i in self.m.items if str(i.dest) == 'part:RECOVERY' and i.present), None)
        if twrp is None:
            raise Refused('the phone is in Download mode, and the bundle has no RECOVERY (TWRP) image to flash')
        cmd = ['heimdall', 'flash', '--RECOVERY', str(twrp.path), '--no-reboot']
        u.info('Download mode: TWRP goes into RECOVERY first (raw image; nothing else is written in Download mode).')
        u.cmd(' '.join(cmd) + ('    [dry run: not run]' if self.mode == 'dry-run' else ''))
        u.info('Then: hold Volume Down + Power until the screen goes black, and at once hold Volume Up + Bixby +')
        u.info('Power until TWRP shows. Do not let stock Android start (it may put the stock recovery back).')
        if self.mode == 'dry-run':
            u.warn('dry run: TWRP is not flashed, so the plan below has no phone checks')
            return None
        if not u.typed('Flash TWRP into RECOVERY now?', 'flash twrp'):
            raise Refused('TWRP not flashed')
        self.r.run(cmd, write=True)
        u.info('waiting up to 5 minutes for TWRP (adb)...')
        st = self.phone.wait_for('recovery', 300)
        if st.kind != 'recovery':
            raise Refused('TWRP did not appear; boot it with the keys above and run the installer again')
        u.ok('TWRP is up')
        return st

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
        row('unlocked', f'TWRP {pr.get("ro.twrp.version") or "?"} runs; flash.locked={pr.get("ro.boot.flash.locked") or "?"}'
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
        for L in self.m.layout:
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
        row('UFS layout', f'{len(self.m.layout)} partitions as the bundle expects' if not bad else '; '.join(bad[:4]),
            not bad, 'the GPT is not the Starbite layout this bundle was made for. v1 never writes the GPT: '
            'see docs/INSTALLER.md "Repartitioning"')
        cbytes = (f.card or 0) * 512
        row('microSD', f'{human(cbytes)} ({f.card or 0} sectors) {f.card_name}'.strip() if f.card else 'none',
            cbytes >= self.m.card['min_bytes'], 'no card, or smaller than 64 GB')
        if f.card and self.m.card.get('sectors'):
            row('card layout fit', f'bundle made for a {self.m.card["sectors"]}-sector card',
                f.card == self.m.card['sectors'],
                f'this card has {f.card} sectors: the bundle\'s card roots are at fixed sectors (payload command '
                'lines), so it needs a bundle made for this card size')
        for t in ('gzip', 'sha256sum', 'blockdev'):
            if t not in f.tools:
                row(f'TWRP tool {t}', 'missing', False)
        free = self._host_free()
        row('host free space', human(free), free >= 512 * MIB, 'less than 512 MiB free for the backups')
        if free < 2 * GIB:
            self.warnings.append(f'host: only {human(free)} free at {self.state_dir}')
        serial = pr.get('ro.boot.serialno') or st.serial
        self.phone_id = 'phone-' + hashlib.sha256(serial.encode()).hexdigest()[:10]
        rows.append(('phone id', f'{self.phone_id} (a hash of the serial number)', 'ok'))
        u.table(rows, ('check', 'value', ''))
        if fatal:
            raise Refused('preflight failed:\n  ' + '\n  '.join(fatal))
        self.journal = Journal(self.state_dir / self.phone_id / 'journal.json')

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
            return f.node(d.name), 0
        if d.kind == 'slot':
            return f.node('USERDATA'), d.offset
        if d.kind == 'store':
            return (f.node('USERDATA'), STORE_UFS_OFFSET) if d.name == 'ufs' else ('/dev/block/mmcblk0p2', 0)
        if d.kind == 'card-lba':
            return '/dev/block/mmcblk0', d.offset * 512
        return '/external_sd' + d.name, 0

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
        if item.action == 'mkfs-metadata':
            return False
        return self.phone.range_sha(node, off, item.size) == item.sha256

    def probe(self, item):
        """blank | data | differs | missing | too-big"""
        node, off = self.target(item)
        d = item.dest
        if d.kind == 'part' and item.action == 'write':
            psize = self.facts.size(d.name)
            if psize is None or item.size > psize:
                return 'too-big'
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
        size = self.facts.size('ODM') if item.action == 'mkfs-metadata' else item.size
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
        steps.append(Step('layout', 'Check the UFS layout (GPT)', '-', 'verify', 'checked in preflight; never written'))
        steps += self.plan_card()
        items = sorted([i for i in self.m.items], key=rank)
        cls = {i.key: self.classify(i) for i in items}
        # Android's /data and /metadata belong together: /metadata holds /data's encryption keys
        a16 = next((i for i in items if str(i.dest) == 'part:A16DATA'), None)
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
        if p in ('blank', 'missing'):
            return 'write', 'target is empty'
        if not i.data and self.matches(i):
            return 'skip', 'already identical (verified by hash)'
        if i.data:
            if o.replace_all or i.key in o.replace:
                return 'replace', 'has data: REPLACED (--replace)'
            return 'keep', f'has data: kept (--replace {i.key} overwrites it)'
        return 'write', 'differs: overwritten (holds no user data)'

    def plan_card(self):
        self._card_status = 'skip'
        c = self.m.card
        if 'p1' not in c or 'p2' not in c:
            return []
        mbr = self.phone.exec_out('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null')
        parts = mbr_parts(mbr)
        want = [(c['p1']['type'], c['p1']['start'], c['p1']['sectors']), (c['p2']['type'], c['p2']['start'], c['p2']['sectors'])]
        if parts and parts[:2] == want:
            st, why = 'skip', 'the card already has the Starbite MBR'
        elif not parts or not any(p[2] for p in parts):
            st, why = 'write', 'blank card'
        elif self.o.erase_card:
            st, why = 'replace', 'another layout: ERASED (--erase-card): ' + self._parts_text(parts)
        else:
            st, why = 'BLOCKED', ('the card has another layout (' + self._parts_text(parts) +
                                  '); everything on it would be lost: run again with --erase-card')
        self._card_status = st
        steps = [Step('card-layout', 'microSD partition table (p1 exFAT, p2 raw)', 'microSD sector 0', st, why)]
        p1 = c['p1']
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
        st = self.bring_to_twrp()
        if st is None:
            return self.offline_plan()
        self.preflight(st)
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

    def offline_plan(self):
        u = self.ui
        u.h1('The plan (no phone: targets not checked)')
        rows = [(n, i.title, i.dest.where(), human(i.size) if i.size else '', writer_of(i),
                 'present' if i.present else 'NOT IN BUNDLE')
                for n, i in enumerate(sorted(self.m.items, key=rank), 1)]
        u.table(rows, ('#', 'what', 'where', 'size', 'by', 'file'))
        cov_rows, cov_warn = coverage(self.m)
        if cov_rows:
            u.h2('Menu entries this bundle covers')
            u.table(cov_rows, ('key', 'OS', 'files + root'))
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
            j.mark_step('backup', 'done', dir=str(d), names=sorted(done))
            u.ok(f'backup complete: {len(done)} items in {self.rel(d)}')

    def do_card_layout(self, s):
        c, p, u = self.m.card, self.phone, self.ui
        mbr = build_mbr(c['p1'], c['p2'])
        p2_head = c['p2']['start'] // 8
        u.info('p1 type 0x%02x %d+%d, p2 type 0x%02x %d+%d' % (c['p1']['type'], c['p1']['start'], c['p1']['sectors'],
                                                               c['p2']['type'], c['p2']['start'], c['p2']['sectors']))
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
            if p.sh('cat /sys/class/block/mmcblk0p1/start 2>/dev/null', check=False) == str(c['p1']['start']):
                break
            time.sleep(1)
        else:
            raise Stopped('the kernel did not pick up the new card partitions (reboot TWRP and run again)')
        u.ok('card MBR written and read back; partitions re-read')

    def do_card_format(self, s):
        p, u, f = self.phone, self.ui, self.facts
        label = self.m.card['p1'].get('label', 'STARBITE')
        tool = 'mkfs.exfat' if 'mkfs.exfat' in f.tools else 'mkexfatfs' if 'mkexfatfs' in f.tools else None
        cmd = f'umount /external_sd 2>/dev/null; {tool or "mkfs.exfat"} -n {label} /dev/block/mmcblk0p1'
        if self.mode == 'dry-run':
            u.cmd(f'adb shell "{cmd}"    [dry run: not run]')
            if not tool:
                u.warn('TWRP shows no mkfs.exfat/mkexfatfs: the real run would stop here (format p1 as exFAT another way)')
            return
        if not tool:
            raise Stopped('TWRP has no exFAT formatter (mkfs.exfat / mkexfatfs): format microSD p1 as exFAT '
                          f'"{label}" from a computer, then run again')
        p.sh(cmd, write=True)
        boot = p.exec_out(f'dd if=/dev/block/mmcblk0 bs=512 skip={self.m.card["p1"]["start"]} count=1 2>/dev/null')
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

    def unmount_target(self, node):
        hit = [mp for dev, mp, _ in self.facts.mounts if dev == node or dev.endswith('/' + node.rsplit('/', 1)[-1])]
        for mp in hit:
            self.phone.sh(f'umount {mp}', write=True)
        if hit and self.mode != 'dry-run':
            self.facts = self.phone.facts()
            if [m for m in self.facts.mounts if m[0] == node]:
                raise Stopped(f'{node} is still mounted after umount: stopping')

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
        elif d.kind == 'part' and d.name in PART_WRITE_PY:
            self.r.run([*py, TOOL['part-write'], d.name, i.path, '--state', state], write=True, stream=True)
        elif d.kind == 'part' and d.name in NATIVE_PARTS:
            chunk_write(p, u, i.path, i.size, node, 0, state, d.name)
        elif d.kind == 'slot':
            self.r.run([*py, TOOL['slot-write'], i.path, str(d.offset)], write=True, stream=True)
        elif d.kind == 'store':
            self.r.run(['bash', TOOL['store-ufs' if d.name == 'ufs' else 'store-sd'], i.path], write=True, stream=True)
        elif d.kind == 'card-lba':
            self.r.run([*py, TOOL['card-range'], i.path, str(d.offset), '--state', state], write=True, stream=True)
        elif d.kind == 'card-file':
            self.write_card_file(i, node, state)
        if self.mode == 'dry-run':
            u.cmd(self.verify_text(i, node, off))
            return
        self.verify(i, node, off)
        j.mark_item(i, 'done', fresh=True)
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
        elif i.dest.kind == 'card-file':
            out = self.phone.sh(f'sha256sum {node}').split()
            ok, what = bool(out) and out[0] == i.sha256, out[0] if out else ''
        else:
            ok = self.matches(i)
            what = '' if ok else 'store differs' if i.dest.kind == 'store' else self.phone.range_sha(node, off, i.size)
        if not ok:
            raise Stopped(f'{i.key}: VERIFY FAILED on the phone ({what[:64]}): stopping. A re-run writes it again.')
        self.ui.ok(f'{i.key}: verified on the phone (read back)')

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
