"""The repartition step: the UFS GPT (sda) from what is on the phone to a table of
sbinst/ufslayout.py, through TWRP's adb, with sbinst/gpt.py doing every byte (docs/INSTALLER.md 9).

  assess()  read only: both GPT copies from the phone, the anchor (the verified copy of the phone's
            own GPT on this computer, <state>/<phone-id>/gpt/), the plan, and what is left to do:
            skip (the phone has the table and the kernel sees it), repartition, resume (a write or the
            re-read was interrupted), or a refusal.
  run()     the anchor first (read again, hashed on the phone and here, parsed: both copies valid),
            then the backup copy (one dd, started detached on the phone, read back), then the primary
            copy (same), then a reboot into TWRP so the kernel reads the new table, then every
            partition's by-name node, start and size checked against the plan.
  restore() the anchor back onto the phone, the same way (--restore-gpt).

On the phone, each copy is pushed to /tmp/sbinst-gpt/ and checked by SHA-256 there before the write;
the write runs in a background subshell that ignores SIGHUP, so a cable pull cannot stop a dd half
way: either the adb command never reached the phone (nothing written) or the dd runs to its end."""
import hashlib
import json
import os
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import gpt, ufslayout
from .common import Refused, Stopped

BS = 4096
D = '/tmp/sbinst-gpt'


@dataclass
class Assessment:
    status: str                     # skip | repartition | resume | reread
    action: str = 'done'            # gpt.next_action(): write-both | write-primary | write-backup | done
    plan: object = None             # gpt.Plan (from the anchor's original when there is one)
    disk: object = None             # gpt.Disk as on the phone now
    states: tuple = ('', '')        # (primary, backup): orig | new | invalid | other
    why: str = ''
    fresh: list = field(default_factory=list)


def sha(b):
    return hashlib.sha256(b).hexdigest()


class Repart:
    def __init__(self, installer, layout):
        self.i = installer
        self.ui, self.phone, self.r = installer.ui, installer.phone, installer.r
        self.layout = layout
        self.dir = installer.state_dir / installer.phone_id / 'gpt'
        self.a = None
        self.move_keep = tuple(getattr(installer, 'move_keep', ()) or ())

    # ---------------------------------------------------------------- reading
    @property
    def total(self):
        return (self.i.facts.sda or 0) // 8

    def read_copies(self, verify_on_phone=False):
        """(head LBA 0..5, tail last 5 LBAs) from the phone; with verify_on_phone the phone's own
        SHA-256 of the same ranges must match what arrived here (the backup rule)."""
        t = self.total
        if t < 64:
            raise Refused('the phone reports no UFS size (/sys/block/sda/size)')
        cmds = (f'dd if=/dev/block/sda bs={BS} count=6 2>/dev/null',
                f'dd if=/dev/block/sda bs={BS} skip={t - 5} count=5 2>/dev/null')
        out = []
        for c in cmds:
            data = self.phone.exec_out(c)
            if verify_on_phone:
                there = (self.phone.sh(f'{c} | sha256sum').split() or [''])[0]
                if there != sha(data):
                    raise Stopped(f'GPT backup: the phone says {there[:16]}..., this computer got {sha(data)[:16]}...: '
                                  'stopping before anything is written')
            out.append(data)
        if len(out[0]) != 6 * BS or len(out[1]) != 5 * BS:
            raise Refused(f'GPT read: got {len(out[0])} + {len(out[1])} bytes, expected {6 * BS} + {5 * BS}')
        return out

    def anchor(self):
        """(orig head, orig tail, meta) saved on this computer, or None."""
        m = self.dir / 'anchor.json'
        if not m.is_file():
            return None
        meta = json.loads(m.read_text())
        head, tail = (self.dir / 'orig-head.bin').read_bytes(), (self.dir / 'orig-tail.bin').read_bytes()
        if sha(head) != meta['orig_head_sha256'] or sha(tail) != meta['orig_tail_sha256']:
            raise Refused(f'{self.dir}: the saved GPT backup does not match its own hashes: not using it')
        return head, tail, meta

    # ---------------------------------------------------------------- assess (read only)
    def assess(self):
        lbs = self.i.facts.lbs
        if lbs not in (None, BS):
            raise Refused(f'sda has {lbs}-byte logical blocks; the layout tables are in 4096-byte blocks')
        head, tail = self.read_copies()
        now = gpt.read(head, tail, self.total)
        if now.lba != BS:
            raise Refused(f'the GPT on sda uses {now.lba}-byte blocks, not 4096')
        an = self.anchor()
        a = None
        if an is not None and an[2].get('total') == self.total:
            odisk = gpt.read(an[0], an[1], self.total)
            try:
                p = gpt.plan(odisk, self.layout, self.move_keep)
            except gpt.GPTError as e:
                raise Refused(f'the saved GPT backup no longer plans with layout {self.layout.name}: {e}') from None
            old_new = (an[2].get('new_primary_sha256'), an[2].get('new_backup_sha256'))
            ps = gpt.classify(now.primary_bytes(), odisk.primary_bytes(), p.primary, 'primary', self.total, BS)
            bs = gpt.classify(tail, an[1], p.backup, 'backup', self.total, BS)
            if bs == 'other' and sha(tail) == old_new[1] and ps == 'orig':
                bs = 'invalid'                      # a backup copy of an older plan: the table changed since
            if (ps, bs) in gpt.ACTIONS and (ps, bs) != ('orig', 'orig'):
                a = Assessment('resume', gpt.next_action(ps, bs), p, now, (ps, bs),
                               f'primary {ps}, backup {bs} (an earlier run was interrupted)', p.fresh)
                if a.action == 'done':
                    a.status, a.why = ('skip', f'the phone has the {self.layout.name} table') if self.kernel_ok(p) else \
                        ('reread', 'the table is written; the kernel has not read it yet (reboot into TWRP)')
            elif (ps, bs) == ('orig', 'orig'):
                a = Assessment('repartition', 'write-both', p, now, (ps, bs), '', p.fresh)
        if a is None:
            if not now.usable:
                raise Refused('the GPT on the phone is not valid in both copies, and there is no saved copy of it on '
                              'this computer: ' + '; '.join(now.problems) + '. Nothing is written.')
            if now.primary_only:
                self.ui.info('the backup GPT copy was never written on this phone (the last 5 blocks are zeros); the '
                             'primary is valid. The plan comes from the primary; both copies are written (backup first)')
            try:
                p = gpt.plan(now, self.layout, self.move_keep)
            except gpt.GPTError as e:
                raise Refused(str(e)) from None
            if not p.changed:
                a = Assessment('skip', 'done', p, now, ('new', 'new'), f'the phone has the {self.layout.name} table')
                if not self.kernel_ok(p):
                    a.status, a.why = 'reread', 'the table is right; the kernel shows another one (reboot into TWRP)'
            else:
                a = Assessment('repartition', 'write-both', p, now, ('orig', 'orig'), '', p.fresh)
        self.a = a
        return a

    def kernel_ok(self, p, facts=None):
        f = facts or self.i.facts
        bad = self.kernel_diff(p, f)
        return not bad

    @staticmethod
    def kernel_diff(p, f):
        bad = []
        for e in p.entries:
            got = f.parts.get(e.name)
            want = (f'sda{e.index}', e.first * 8, e.blocks * 8)
            if got != want:
                bad.append(f'{e.name}: kernel {got}, plan {want}')
        return bad

    def describe(self):
        a = self.a
        lines = gpt.side_by_side(a.plan)
        if a.status in ('skip', 'reread'):
            return lines
        lines.append('')
        lost = [r for r in a.plan.rows if r.status in ('new', 'moved', 'resized')]
        if lost:
            lines.append('contents after the change: ' + ', '.join(
                f'{r.name} ({"new" if r.old is None else r.status})' for r in lost) +
                ': whatever was in these places is gone; the installer writes each of them fresh from the bundle')
        return lines

    # ---------------------------------------------------------------- write
    def save_anchor(self):
        """The verified backup of the phone's own GPT (both copies): read again with phone-side
        hashes, parsed, and identical to what the plan was made from."""
        if self.anchor() is not None and self.a.status == 'resume':
            return
        head, tail = self.read_copies(verify_on_phone=True)
        d = gpt.read(head, tail, self.total)
        if not d.usable:
            raise Stopped('the GPT read for the backup is not valid in both copies: ' + '; '.join(d.problems))
        if d.primary_bytes() != self.a.disk.primary_bytes() or tail != self.a.disk.tail:
            raise Stopped('the GPT changed between the preflight read and the backup read: stopping')
        self.dir.mkdir(parents=True, exist_ok=True)
        if (self.dir / 'anchor.json').exists():                    # an older anchor (an earlier re-layout)
            old = self.dir / time.strftime('older-%Y%m%d-%H%M%S')
            old.mkdir()
            for f in ('anchor.json', 'orig-head.bin', 'orig-tail.bin', 'new-primary.bin', 'new-backup.bin'):
                if (self.dir / f).exists():
                    os.replace(self.dir / f, old / f)
        (self.dir / 'orig-head.bin').write_bytes(head)
        (self.dir / 'orig-tail.bin').write_bytes(tail)
        p = self.a.plan
        (self.dir / 'new-primary.bin').write_bytes(p.primary)
        (self.dir / 'new-backup.bin').write_bytes(p.backup)
        meta = {'layout': self.layout.name, 'total': self.total, 'lba': BS, 'at': time.strftime('%Y-%m-%d %H:%M:%S'),
                'orig_head_sha256': sha(head), 'orig_tail_sha256': sha(tail),
                'new_primary_sha256': sha(p.primary), 'new_backup_sha256': sha(p.backup),
                'primary_offset': p.primary_offset, 'backup_offset': p.backup_offset, 'fresh': p.fresh}
        (self.dir / 'anchor.json').write_text(json.dumps(meta, indent=1))
        bk = self.i.journal.step('backup').get('dir')
        if bk and Path(bk).is_dir():                                # and next to the other backups
            (Path(bk) / 'gpt-orig-head.bin').write_bytes(head)
            (Path(bk) / 'gpt-orig-tail.bin').write_bytes(tail)
            with open(Path(bk) / 'backups.txt', 'a') as fh:
                fh.write(f'{sha(head)}  gpt-orig-head.bin (sda LBA 0-5: protective MBR + primary GPT)\n'
                         f'{sha(tail)}  gpt-orig-tail.bin (sda last 5 LBAs: backup GPT)\n')
        self.ui.ok(f'GPT backup: {"the primary valid, the backup never written (zeros)" if d.primary_only else "both copies valid"}, phone and computer agree (head {sha(head)[:12]}..., '
                   f'tail {sha(tail)[:12]}...) in {self.i.rel(self.dir)}')

    def save_keep(self, p, a):
        """The keep rows that move by the allowance: each one read whole from its OLD place, while the old table is
        still the one the kernel uses, hashed on the phone and here, saved as gpt/keep-NAME.img (+ keep.json)."""
        moving = [r for r in p.rows if r.name in self.move_keep and r.status in ('moved', 'resized') and r.old]
        if not moving:
            return
        kp = self.dir / 'keep.json'
        meta = json.loads(kp.read_text()) if kp.exists() else {}
        for r in moving:
            f = self.dir / f'keep-{r.name}.img'
            m = meta.get(r.name)
            if m and f.exists() and sha(f.read_bytes()) == m['sha256']:
                continue
            if a.states and a.states[0] not in ('orig', ''):
                raise Stopped(f'{r.name}: no saved copy, and the primary GPT is no longer the original one: its old '
                              'place cannot be read any more. Restore the GPT (--restore-gpt) first')
            node, size = f'/dev/block/sda{r.old.index}', r.old.blocks * BS
            data = self.phone.exec_out(f'dd if={node} bs=1048576 2>/dev/null')
            there = self.phone.sh(f'dd if={node} bs=1048576 2>/dev/null | sha256sum').split()[0]
            if len(data) != size or sha(data) != there:
                raise Stopped(f'{r.name}: read {len(data)} of {size} bytes, phone {there[:16]}... vs here '
                              f'{sha(data)[:16]}...: stopping before the GPT write')
            f.write_bytes(data)
            meta[r.name] = {'sha256': there, 'size': size, 'old_index': r.old.index, 'old_first': r.old.first,
                            'old_last': r.old.last, 'new_first': r.new.first, 'new_last': r.new.last}
            kp.write_text(json.dumps(meta, indent=1))
            self.ui.ok(f'{r.name} saved before the move: {size >> 20} MiB from sda{r.old.index}, sha256 {there[:12]}... '
                       f'(phone and computer agree) -> {self.i.rel(f)}')
        self.i.journal.mark_step('move-keep', 'saved', names=sorted(meta), dir=str(self.dir))

    def write_commands(self, p):
        return {'backup': f': sbinst-gpt-backup; dd if={D}/backup.bin of=/dev/block/sda bs={BS} '
                          f'seek={p.backup_offset // BS} conv=notrunc,fsync',
                'primary': f': sbinst-gpt-primary; dd if={D}/primary.bin of=/dev/block/sda bs={BS} '
                           f'seek={p.primary_offset // BS} conv=notrunc,fsync'}

    def push(self, p):
        with tempfile.TemporaryDirectory() as t:
            for n, b in (('primary', p.primary), ('backup', p.backup)):
                Path(t, f'{n}.bin').write_bytes(b)
            self.phone.sh(f'mkdir -p {D}; rm -f {D}/*.rc {D}/*.bin', write=True, quiet=True)
            for n in ('primary', 'backup'):
                self.phone.push(str(Path(t, f'{n}.bin')), f'{D}/{n}.bin')
        out = self.phone.sh(f'sha256sum {D}/primary.bin {D}/backup.bin')
        got = [ln.split()[0] for ln in out.splitlines() if ln.strip()]
        if got != [sha(p.primary), sha(p.backup)]:
            raise Stopped('the GPT copies on the phone\'s /tmp do not match: stopping before writing')

    def write_copy(self, which, data, cmd):
        """One copy: a detached dd on the phone, its exit status polled, then read back here."""
        self.phone.sh(f'{cmd.split(";", 1)[0]}; (trap "" HUP INT; {cmd.split("; ", 1)[1]} 2>{D}/{which}.err; '
                      f'echo $? > {D}/{which}.rc.tmp; sync; mv {D}/{which}.rc.tmp {D}/{which}.rc) '
                      '</dev/null >/dev/null 2>&1 &', write=True)
        for _ in range(60):
            rc = self.phone.sh(f'cat {D}/{which}.rc 2>/dev/null', check=False)
            if rc:
                break
            time.sleep(0.5)
        else:
            raise Stopped(f'the {which} GPT write did not finish within 30 s (run again: it resumes from the copies)')
        if rc != '0':
            err = self.phone.sh(f'cat {D}/{which}.err', check=False)
            raise Stopped(f'the {which} GPT write failed on the phone (dd exit {rc}): {err[:200]}')
        off = (self.total - 5) if which == 'backup' else 1
        back = self.phone.exec_out(f'dd if=/dev/block/sda bs={BS} skip={off} count=5 2>/dev/null')
        if back != data:
            raise Stopped(f'the {which} GPT copy reads back different from what was written: stopping. '
                          'Run again (it rewrites from the saved copies) or --restore-gpt')
        self.ui.ok(f'{which} GPT copy written and read back ({len(data)} B at LBA {off}, sha256 {sha(data)[:12]}...)')

    def reboot_and_check(self, p):
        u = self.ui
        if os.environ.get('SBINST_ENV_UP') == '1':
            # the installer environment mounts nothing on sda: the kernel can re-read the table in place
            u.info('the kernel re-reads the table (blockdev --rereadpt /dev/block/sda; by-name links again)')
            self.phone.sh('sync; blockdev --rereadpt /dev/block/sda; sb-byname', write=True, check=False)
            f = self.phone.facts()
            if not self.kernel_diff(p, f):
                self.i.facts = f
                u.ok(f'the kernel sees the {getattr(self.layout, "name", "original")} table: {len(p.entries)} '
                     'partitions, by-name nodes, starts and sizes as planned')
                return
            if getattr(self.i.o, 'no_reboot', False):
                # --no-reboot (an unattended night: no restarts, no fastboot wait, no new USB identity on the Mac)
                raise Stopped('the table is written (both copies read back), but the kernel did not re-read it in place; '
                              '--no-reboot: not restarting the environment. Resume after a restart (loader fastboot, '
                              '--start-env, then the same command): the GPT step checks the table and goes on')
            u.info('not re-read in place: the environment restarts (loader fastboot, then the environment again)')
            self.phone.adb('reboot', 'bootloader', write=True)
            st = self.phone.wait_for('fastboot-starbite', 180)
            if st.kind != 'fastboot-starbite':
                raise Stopped('the loader\'s fastboot did not come back after the GPT write. The table is written: start '
                              'the fastboot (Bixby > USB FASTBOOT) and run the installer again; it checks and goes on')
            self.i.start_env()
            f = self.phone.facts()
            bad = self.kernel_diff(p, f)
            if bad:
                raise Stopped('after the restart the kernel shows another table: ' + '; '.join(bad[:4]))
            self.i.facts = f
            u.ok(f'the kernel sees the {getattr(self.layout, "name", "original")} table after the restart')
            return
        u.info('rebooting into TWRP so the kernel reads the new table (adb reboot recovery; legacy transport)...')
        self.phone.adb('reboot', 'recovery', write=True)
        time.sleep(1 if os.environ.get('FAKE_ROOT') else 8)
        st = self.phone.wait_for('recovery', 240)
        if st.kind != 'recovery':
            raise Stopped('TWRP did not come back within 4 minutes after the GPT write. The table is written; boot TWRP '
                          '(Volume Up + Bixby + Power) and run the installer again: it checks the table and goes on')
        f = self.phone.facts()
        bad = self.kernel_diff(p, f)
        if bad:
            raise Stopped('after the reboot the kernel shows another table: ' + '; '.join(bad[:4]))
        self.i.facts = f
        u.ok(f'the kernel sees the {getattr(self.layout, "name", "original")} table: {len(p.entries)} partitions, by-name nodes, starts and sizes '
             'as planned')

    def run(self, typed=True):
        a, u, j = self.a, self.ui, self.i.journal
        p = a.plan
        cmds = self.write_commands(p)
        if a.status == 'skip':
            st = j.step('gpt')
            if self.r.mode != 'dry-run' and st.get('status') in ('started', 'written'):
                j.mark_step('gpt', 'done', layout=self.layout.name, fresh=st.get('fresh', []),
                            new_sha256=st.get('new_sha256'))         # the kernel read it at a reboot since
            return
        if self.r.mode == 'dry-run':
            u.cmd(f'adb exec-out "dd if=/dev/block/sda bs={BS} count=6" + "... skip={self.total - 5} count=5"  '
                  f'(the GPT backup, phone sha256 == local sha256) -> {self.i.rel(self.dir)}/')
            if a.action in ('write-both', 'write-backup'):
                u.cmd(f'adb push backup.bin {D}/; adb shell "{cmds["backup"].split("; ", 1)[1]}"  (detached; read back)'
                      '    [dry run: not run]')
            if a.action in ('write-both', 'write-primary'):
                u.cmd(f'adb push primary.bin {D}/; adb shell "{cmds["primary"].split("; ", 1)[1]}"  (detached; read back)'
                      '    [dry run: not run]')
            u.cmd(('adb shell "blockdev --rereadpt /dev/block/sda; sb-byname"' if os.environ.get('SBINST_ENV_UP') == '1'
                   else 'adb reboot recovery (legacy TWRP)') +
                  '; then by-name, start and size of every partition == the plan    [dry run: not run]')
            return
        if a.status != 'reread':
            self.save_anchor()
            self.save_keep(p, a)
            if typed and not u.typed(f'Write the new UFS partition table ({self.layout.name})? Partitions marked new/moved/'
                                     f'resized above lose their contents.', f'repartition {self.i.phone_id}'):
                raise Refused('the partition table was not confirmed: nothing written')
            j.mark_step('gpt', 'started', layout=self.layout.name, fresh=p.fresh, new_sha256=sha(p.primary + p.backup))
            self.i.forget_fresh(p.fresh)
            self.push(p)
            if a.action in ('write-both', 'write-backup'):
                self.write_copy('backup', p.backup, cmds['backup'])
            if a.action in ('write-both', 'write-primary'):
                self.write_copy('primary', p.primary, cmds['primary'])
            head, tail = self.read_copies()
            nd = gpt.read(head, tail, self.total)
            if not nd.valid or nd.primary_bytes() != p.primary or tail != p.backup:
                raise Stopped('the GPT read back as a whole does not match the plan: ' + '; '.join(nd.problems))
            u.ok('both GPT copies on the phone are the plan (CRCs, cross references, entries)')
            j.mark_step('gpt', 'written', layout=self.layout.name, fresh=p.fresh, new_sha256=sha(p.primary + p.backup))
        self.reboot_and_check(p)
        fresh = sorted(set(j.step('gpt').get('fresh', [])) | set(p.fresh))
        j.mark_step('gpt', 'done', layout=self.layout.name, fresh=fresh, new_sha256=sha(p.primary + p.backup))

    # ---------------------------------------------------------------- restore
    def restore(self):
        """Write the saved original GPT back (--restore-gpt)."""
        u = self.ui
        an = self.anchor()
        if an is None:
            raise Refused(f'no saved GPT for this phone in {self.dir}')
        head, tail, meta = an
        if meta['total'] != self.total:
            raise Refused('the saved GPT is for a disk of another size')
        orig = gpt.read(head, tail, self.total)
        if not orig.usable:
            raise Refused('the saved GPT is not valid: ' + '; '.join(orig.problems))
        h_now, t_now = self.read_copies()
        now = gpt.read(h_now, t_now, self.total)
        new_p = (self.dir / 'new-primary.bin').read_bytes()
        new_b = (self.dir / 'new-backup.bin').read_bytes()
        # roles reversed: the plan is the original, "orig" is the table this installer wrote
        ps = gpt.classify(now.primary_bytes(), new_p, orig.primary_bytes(), 'primary', self.total, BS)
        bs = gpt.classify(t_now, new_b, tail, 'backup', self.total, BS)
        try:
            action = gpt.next_action(ps, bs)
        except gpt.GPTError:
            raise Refused(f'the phone\'s GPT is primary={ps}, backup={bs} relative to the saved original and the '
                          'installer\'s table: not restoring over something else') from None
        p = gpt.Plan('original', orig, orig.primary.entries, [], orig.primary_bytes(), tail, [])
        u.h2('Restore the saved GPT')
        u.info('\n   '.join(gpt.table_lines(orig.primary.entries)))
        if action == 'done':
            u.ok('the phone already has the saved original GPT')
            return
        cmds = self.write_commands(p)
        if self.r.mode == 'dry-run':
            for w in ('backup', 'primary'):
                if action in ('write-both', f'write-{w}'):
                    u.cmd(f'adb shell "{cmds[w].split("; ", 1)[1]}"    [dry run: not run]')
            u.ok('dry run: nothing written')
            return
        if not u.typed('Write the saved original GPT back?', f'restore gpt {self.i.phone_id}'):
            raise Refused('not confirmed: nothing written')
        self.push(p)
        if action in ('write-both', 'write-backup'):
            self.write_copy('backup', p.backup, cmds['backup'])
        if action in ('write-both', 'write-primary'):
            self.write_copy('primary', p.primary, cmds['primary'])
        self.i.journal.mark_step('gpt', 'restored', layout='original', fresh=[e.name for e in orig.primary.entries
                                                                              if e.index > 17])
        self.reboot_and_check(p)
