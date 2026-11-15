#!/usr/bin/env python3
"""Update a phone's live card store without losing what the phone wrote into it (docs/INSTALLER.md 17): new or
replaced entries and catalog rows go in, while the volatile entries keep their bytes. Those are the loader's crash
log, Starbite OS's settings (sbos-state), update slots and photos, and anything else flagged volatile. For S9 #2's
later steps (One UI's android.*, the Sequoia and iOS 27 payloads, the Windows .uefi files, linux.*), after the core
install. The phone runs the Starbite installer environment; reached over ssh, nothing else.

  store-update.py --serial S2 --catalog NEW.toml [NAME=FILE[:TYPE] ...] [--omit NAME] [--drop KEY]
                  [--dry-run | --real] [--state-dir DIR]
  store-update.py ... --rehearse --fake-root DIR --yes        (a fake phone: tools/installer/fakes/)

1. Pull: the card's MBR gives p2 (entry 2, type 0xDA); the store header gives its used blocks; that range comes to the
   Mac over ssh, hashed by the phone too, and every entry's CRC is checked (sbstore2.py). It is kept as the rollback
   image: <state>/<phone id>/store/pull-<time>.img.
2. Build: `sbstore2.py build NEW --medium LAYOUT --home --catalog NEW.toml --import PULL NAME=FILE ...`. --import keeps
   every entry, the volatile ones with their bytes; NAME=FILE adds or replaces; --omit drops an entry; the catalog may
   not lose a key the card's catalog has unless --drop KEY. The plan lists added / replaced / removed / kept entries.
3. Dry run (the default): nothing written; it records the dry run of this exact result (the new image's SHA-256).
   --real rebuilds from a fresh pull, needs the same SHA-256 (the card has not changed since), asks for a typed
   `update store on phone-XXXXXXXXXX`, then writes:
   - the data first (store blocks 7..), in 16 MiB pieces, only the pieces that differ, each read back on the phone.
     Until the head is written, the card's old header stays in place;
   - then the 7 head blocks, with the card's own settings block (block 2) put back in;
   - then the whole store area read back and compared with the new image.
   The new image is saved first (<state>/<phone id>/store/new-<sha12>.img), so a cable pull resumes: the same --real
   writes the saved image again (its pieces are skipped where they match), without pulling.
Only layouts whose store is laid out as on the card (sd64, sd64l: data from block 7). S9 #1's sd128 compact home
store is tools/store/card-store.sh's. Writes only card p2's store area; never UFS.
Rollback: `store-update.py --serial S2 --restore PULL.img --real` writes a pulled image back the same way. Mac space:
the pull + the new image, at most 2 x the 1 GiB store area (sd64l)."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
from sbinst.common import Refused, Stopped, cardlayout, default_state_dir, human  # noqa: E402
from sbinst.device import InstallerEnv  # noqa: E402
from sbinst.journal import Journal  # noqa: E402
from sbinst.steps import mbr_parts, phone_id_of  # noqa: E402

SB = REPO / 'tools' / 'store' / 'sbstore2.py'
BLOCK, HEAD_BLOCKS, SETTINGS = 4096, 7, 2
PIECE = 16 << 20
P2 = '/dev/block/mmcblk0p2'


def load_sbstore2():
    import importlib.util
    spec = importlib.util.spec_from_file_location('sbstore2_su', SB)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class Phone:
    def __init__(self, serial, env):
        self.serial, self.env = serial, env
        self.ie = InstallerEnv(None, Path(os.environ.get('SBINST_STATE', default_state_dir())) / 'env')

    def sh(self, cmd, data=None, binary=False, check=True):
        p = subprocess.run(self.ie.ssh_argv(cmd, 5), input=data, capture_output=True, env=self.env,
                           stdin=None if data is not None else subprocess.DEVNULL)
        if check and p.returncode:
            raise Stopped(f'{cmd[:120]} failed ({p.returncode}): {p.stderr.decode(errors="replace").strip()[-300:]}')
        return p.stdout if binary else p.stdout.decode(errors='replace')

    def detached(self, cmd, tag, timeout=None):
        """`cmd` runs on the phone in a subshell that ignores SIGHUP, so a dropped ssh link cannot cut a write in half
        (S9 #2's cable, 2026-11-15); its exit status lands in /tmp/TAG.rc, which is polled here. A link that stays
        down past `timeout` stops the run (STOPPED): the same command resumes, and the write itself finishes."""
        rc = f'/tmp/{tag}.rc'
        timeout = timeout or int(os.environ.get('SBINST_DETACH_TIMEOUT', 180))   # (rehearsals shorten it)
        self.sh(f'rm -f {rc} {rc}.tmp; ( trap "" HUP INT; {cmd}; echo $? > {rc}.tmp; sync; mv {rc}.tmp {rc} ) '
                '</dev/null >/dev/null 2>&1 &')
        t0 = time.time()
        while time.time() - t0 < timeout:
            r = self.sh(f'cat {rc} 2>/dev/null', check=False).strip()
            if r:
                self.sh(f'rm -f {rc}', check=False)
                return r
            time.sleep(0.5)
        raise Stopped(f'no answer from the phone for {timeout} s while {tag} was being written there (the link?): '
                      'the write goes on on the phone; run the same command again to resume')

    def wait_leftovers(self, pattern, timeout=180):
        """An earlier run that lost its link may have left a detached dd running: let it finish first."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not self.sh(f"ps -o args 2>/dev/null | grep '[d]d if={pattern}' || true", check=False).strip():
                return
            time.sleep(1)
        raise Stopped(f'an earlier write ({pattern}) is still running on the phone after {timeout} s')


class Update:
    def __init__(self, a):
        self.a = a
        self.mode = 'real' if a.real else 'rehearse' if a.rehearse else 'dry-run'
        env = dict(os.environ)
        if a.fake_root:
            if self.mode == 'real':
                raise Refused('--fake-root and --real do not mix')
            env['PATH'] = str(HERE / 'fakes') + os.pathsep + env['PATH']
            env['FAKE_ROOT'] = str(Path(a.fake_root).resolve())
        elif self.mode == 'rehearse':
            raise Refused('--rehearse needs --fake-root')
        self.p = Phone(a.serial, env)
        self.pid = phone_id_of(a.serial)
        self.state_dir = Path(a.state_dir or default_state_dir()).resolve()
        self.dir = self.state_dir / self.pid / 'store'
        self.journal = Journal(self.state_dir / self.pid / 'journal.json')
        self.sb = load_sbstore2()

    def say(self, s):
        print(s, flush=True)

    # ------------------------------------------------------------------ the phone and its card
    def check(self):
        p = subprocess.run(self.p.ie.ssh_argv('getprop ro.boot.serialno', 5), capture_output=True, text=True,
                           env=self.p.env, stdin=subprocess.DEVNULL) if self.p.ie.key.is_file() else None
        got = p.stdout.strip() if p is not None and p.returncode == 0 else ''
        if got != self.a.serial:
            raise Refused(f'the installer environment on 172.16.42.1 is {"phone " + got if got else "not answering"}, '
                          f'not {self.a.serial} (start it: starbite-install --bundle B --serial S2 --start-env)')
        mbr = self.p.sh('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null', binary=True)
        sectors = int(self.p.sh('cat /sys/block/mmcblk0/size').strip() or 0)
        parts = mbr_parts(mbr) or []
        name = cardlayout.identify(parts, sectors)
        if name is None:
            raise Refused('the card has no Starbite card layout (the core install, step A, lays it out)')
        L = cardlayout.get(name)
        if L.store_data is not None:
            raise Refused(f'the card is {name}: its home store is compact (data in the store data area): '
                          'tools/store/card-store.sh updates it, not this tool')
        if parts[1][0] != 0xDA:
            raise Refused('MBR entry 2 is not the 0xDA store partition')
        self.L = L
        self.say(f'ok   phone {self.a.serial} ({self.pid}) in the installer environment; card layout {name}, store area '
                 f'{human(L.store_area_blocks * BLOCK)}')
        if self.p.sh(f'grep -c "^{P2} " /proc/mounts || true').strip() not in ('', '0'):
            raise Refused(f'{P2} is mounted')

    def pull(self):
        head = self.p.sh(f'dd if={P2} bs=4096 count={HEAD_BLOCKS} 2>/dev/null', binary=True)
        if head[BLOCK:BLOCK + 8] != b'SBSTORE2':
            raise Refused('card p2 holds no version 2 store (the core install writes one)')
        n, cap, used, _ = self.sb.check_header(head[BLOCK:BLOCK + 52], 'card store')
        flags, data_at = self.sb.home_ext(head[BLOCK:2 * BLOCK])
        if data_at != 7 or not flags & self.sb.HOME_FLAG:
            raise Refused(f'the card store is not a home store with data from block 7 (flags {flags}, data at {data_at})')
        if cap != self.L.store_blocks:
            raise Refused(f'the card store\'s capacity {cap} is not the {self.L.name} layout\'s {self.L.store_blocks}')
        cmd = f'dd if={P2} bs=4096 count={used} 2>/dev/null'
        data = self.p.sh(cmd, binary=True)
        there = self.p.sh(cmd + ' | sha256sum').split()[0]
        if len(data) != used * BLOCK or hashlib.sha256(data).hexdigest() != there:
            raise Stopped(f'pull: {len(data)} bytes, the phone hashed {there[:16]}...: read again')
        self.dir.mkdir(parents=True, exist_ok=True)
        pull = self.dir / f'pull-{time.strftime("%Y%m%d-%H%M%S")}.img'
        pull.write_bytes(data)
        self.sb.read_store(pull)                      # header, directory, every non-volatile CRC (exits if bad)
        self.say(f'ok   pulled the card store: {n} entries, {used} blocks ({human(used * BLOCK)}), phone and Mac agree '
                 f'(sha256 {there[:16]}...) -> {pull.relative_to(self.state_dir)}')
        return pull

    def build(self, pull):
        out = self.dir / 'build-tmp.img'
        argv = [sys.executable, str(SB), 'build', str(out), '--medium', self.L.name, '--home', '--catalog',
                str(self.a.catalog), '--import', str(pull)]
        for x in self.a.omit:
            argv += ['--omit', x]
        for k in self.a.drop:
            argv += ['--drop', k]
        argv += self.a.files
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode:
            raise Refused(f'sbstore2.py build refused: {(r.stderr or r.stdout).strip()[-600:]}')
        sha = hashlib.sha256(out.read_bytes()).hexdigest()
        new = self.dir / f'new-{sha[:12]}.img'
        os.replace(out, new)
        return new, sha

    def diff(self, pull, new):
        old = {n: (t, b, fl) for n, t, b, fl in self.sb.read_store(pull, with_flags=True)}
        nw = {n: (t, b, fl) for n, t, b, fl in self.sb.read_store(new, with_flags=True)}
        rows = []
        for n in nw:
            if n not in old:
                rows.append(('add', n, len(nw[n][1])))
            elif nw[n][1] != old[n][1]:
                rows.append(('replace', n, len(nw[n][1])))
            else:
                rows.append(('volatile, kept' if nw[n][2] & self.sb.SF_VOLATILE else 'kept', n, len(nw[n][1])))
        rows += [('REMOVE', n, len(old[n][1])) for n in old if n not in nw]
        for what, n, size in rows:
            self.say(f'   {what:<15} {n:<26} {human(size)}')
        lost = [n for n in old if old[n][2] & self.sb.SF_VOLATILE and (n not in nw or nw[n][1] != old[n][1])]
        if lost and not self.a.omit:
            raise Stopped(f'volatile entries would change: {", ".join(lost)} (a bug: --import keeps them)')

    # ------------------------------------------------------------------ writing
    def write(self, new, sha):
        img = bytearray(new.read_bytes())
        st_path = self.dir / 'writing.json'
        st_path.write_text(json.dumps({'image': str(new), 'sha256': sha}))
        self.p.wait_leftovers('/tmp/sbsu-')
        blocks = len(img) // BLOCK
        wrote = 0
        for b0 in range(HEAD_BLOCKS, blocks, PIECE // BLOCK):          # the data, head untouched
            nb = min(PIECE // BLOCK, blocks - b0)
            piece = bytes(img[b0 * BLOCK:(b0 + nb) * BLOCK])
            want = hashlib.sha256(piece).hexdigest()
            rd = f'dd if={P2} bs=4096 skip={b0} count={nb} 2>/dev/null | sha256sum'
            if self.p.sh(rd).split()[0] == want:
                continue
            tmp = f'/tmp/sbsu-{b0}.piece'
            self.p.sh(f'cat > {tmp}', data=piece)
            r = self.p.detached(f'dd if={tmp} of={P2} bs=4096 seek={b0} conv=notrunc,fsync 2>/dev/null', f'sbsu-{b0}')
            self.p.sh(f'rm -f {tmp}', check=False)
            if r != '0' or self.p.sh(rd).split()[0] != want:
                raise Stopped(f'store block {b0}: read back differs: stopped (the same --real resumes)')
            wrote += nb
        # the head last, with the card's own settings block (the loader's and Starbite OS's settings)
        cur = self.p.sh(f'dd if={P2} bs=4096 skip={SETTINGS} count=1 2>/dev/null', binary=True)
        if len(cur) == BLOCK and cur[:8] == b'SBSETTNG':
            img[SETTINGS * BLOCK:(SETTINGS + 1) * BLOCK] = cur
        head = bytes(img[:HEAD_BLOCKS * BLOCK])
        self.p.sh('cat > /tmp/sbsu-head.piece', data=head)
        r = self.p.detached(f'dd if=/tmp/sbsu-head.piece of={P2} bs=4096 conv=notrunc,fsync 2>/dev/null', 'sbsu-head')
        self.p.sh('rm -f /tmp/sbsu-head.piece', check=False)
        if r != '0':
            raise Stopped(f'the head write failed on the phone (dd exit {r}): the same command writes it again')
        back = self.p.sh(f'dd if={P2} bs=4096 count={blocks} 2>/dev/null | sha256sum').split()[0]
        if back != hashlib.sha256(bytes(img)).hexdigest():
            raise Stopped('the card store does not read back as the new image (its settings block kept): stopped')
        st_path.unlink()
        self.say(f'ok   card store written: {human(wrote * BLOCK)} of data changed, then the head; the whole store '
                 'area reads back as the new image (the card\'s settings block kept)')

    def drop_copies(self, pull, new):
        """A run that writes nothing keeps no images on the Mac (a restore's image is the user's own file)."""
        if not self.a.restore:
            for f in (pull, new):
                f.unlink(missing_ok=True)

    # ------------------------------------------------------------------ run
    def run(self):
        pull = None
        self.say(f'== store-update ({self.mode.upper()}) ==')
        self.check()
        st_path = self.dir / 'writing.json'
        if st_path.is_file() and self.mode != 'dry-run':
            st = json.loads(st_path.read_text())
            new = Path(st['image'])
            if new.is_file() and hashlib.sha256(new.read_bytes()).hexdigest() == st['sha256']:
                self.say(f'   an earlier write of {new.name} was interrupted: writing it again (no new pull)')
                self.write(new, st['sha256'])
                self.journal.mark_step('store-update', 'done', sha256=st['sha256'], image=str(new))
                if new.name.startswith('new-'):
                    new.unlink()                  # on the card now; the earlier pull stays as the rollback image
                return 0
        if self.a.restore:
            new = Path(self.a.restore)
            self.sb.read_store(new)
            sha = hashlib.sha256(new.read_bytes()).hexdigest()
            self.say(f'   restore: {new} ({human(new.stat().st_size)}, sha256 {sha[:16]}...)')
        else:
            if not self.a.catalog:
                raise Refused('--catalog NEW.toml is needed (the catalog the updated store carries)')
            pull = self.pull()
            try:
                new, sha = self.build(pull)
            except Refused:
                pull.unlink()                     # nothing to roll back to: nothing will be written
                raise
            self.say(f'== the new store: {new.name} ({human(new.stat().st_size)}) ==')
            self.diff(pull, new)
        gate = f'store-update:{sha}'
        if self.mode == 'dry-run':
            self.journal.record_dry_run(gate)
            if not self.a.restore:                # the real run pulls and builds again; keep no copies of a dry run
                pull.unlink()
                new.unlink()
            self.say('ok   dry run: nothing written. --real may now write exactly this store (it pulls again and must '
                     'get the same result)')
            return 0
        if not self.journal.had_dry_run(gate):
            self.drop_copies(pull, new)
            raise Refused('no dry run gave this exact store (the card or the inputs changed since): run --dry-run again')
        if self.mode == 'real':
            want = f'update store on {self.pid}'
            print(f'Write the card store on {self.a.serial}? Type exactly: {want}')
            try:
                got = input('   > ').strip()
            except EOFError:
                got = ''
            if got != want:
                self.drop_copies(pull, new)
                raise Refused('not confirmed: nothing written')
        elif not self.a.yes:
            self.drop_copies(pull, new)
            raise Refused('a rehearsal needs --yes')
        self.journal.mark_step('store-update', 'started', sha256=sha, image=str(new))
        self.write(new, sha)
        self.journal.mark_step('store-update', 'done', sha256=sha, image=str(new))
        if not self.a.restore:
            new.unlink()                          # on the card now; the pull stays as the rollback image
            self.say(f'   rollback image: {pull}')
        return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--catalog', type=Path)
    ap.add_argument('files', nargs='*', help='NAME=FILE[:TYPE]: entries to add or replace')
    ap.add_argument('--omit', action='append', default=[])
    ap.add_argument('--drop', action='append', default=[])
    ap.add_argument('--restore', help='write this pulled image back (rollback)')
    m = ap.add_mutually_exclusive_group()
    m.add_argument('--dry-run', action='store_true')
    m.add_argument('--real', action='store_true')
    m.add_argument('--rehearse', action='store_true')
    ap.add_argument('--fake-root')
    ap.add_argument('--yes', action='store_true')
    ap.add_argument('--state-dir')
    a = ap.parse_args(argv)
    for f in a.files:
        if not re.fullmatch(r'[^=]{1,31}=.+', f):
            print(f'REFUSED: {f}: NAME=FILE[:TYPE]')
            return 2
    if a.yes and a.real:
        print('REFUSED: --yes is not accepted with --real')
        return 2
    try:
        return Update(a).run()
    except Refused as e:
        print(f'REFUSED: {e}\n   nothing was written by this step.')
        return 2
    except Stopped as e:
        print(f'STOPPED: {e}\n   run the same command again: it resumes.')
        return 3
    except SystemExit as e:                         # sbstore2.py's own refusals (read_store)
        if e.code in (0, None):
            raise
        print(f'REFUSED: {e.code}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
