#!/usr/bin/env python3
"""Phone-to-phone copy through the Mac's RAM (docs/INSTALLER.md 16): one partition, card range or card file of a
SOURCE phone in TWRP (read only) into a TARGET phone in the Starbite installer environment, 64 MiB at a time, without
a copy on the Mac. For S9 #2's big systems (One UI, the Linux distros, macOS Sequoia, Windows 11, iOS 27), whose images
do not fit on the Mac (shared notes installer-s9b-space-plan.md, section 5).

  stream-copy.py --from-serial S1 --to-serial S2 --source SPEC --target SPEC [--size BYTES] [--dry-run | --real]
                 [--restart] [--state-dir DIR]
  stream-copy.py ... --rehearse --fake-src DIR --fake-dst DIR      (two fake phones: tools/installer/fakes/)

SPEC (source, read only on the source phone)       SPEC (target, the installer environment)
  part:NAME                                           part:NAME   SYSTEM VENDOR ODM A16DATA WINESP WINDOWS USERDATA only
  card:lba:START+SECTORS   (the raw card)             card:lba:START, or card:root:KEY: inside a ROOT slot of the card
                                                      layout the target card has (tools/store/cardlayout.py), never
                                                      p1, the store or the tail
  file:/distros/X.img | file:/starbite/...  (card p1)  file:/distros/... | file:/starbite/...  (written as NAME.part, moved
                                                      into place only after the whole-file check)

The source is the whole partition / range / file unless --size cuts it (e.g. an EROFS image's own size). The target
must be at least as large.

Per 64 MiB chunk:
  1. the SOURCE phone computes the chunk's SHA-256 (`dd if=... | sha256sum`; TWRP, `adb -s S1`);
  2. an all-zero chunk is written on the target from /dev/zero (nothing crosses USB);
  3. otherwise the chunk is read into the Mac's RAM (`adb exec-out dd if=...`), hashed here: must equal (1);
  4. pushed to the target's /tmp (RAM) over ssh and `dd`'d at its offset with conv=notrunc,fsync;
  5. the TARGET phone reads it back and hashes it: must equal (1).
Finished chunks go to a state file (<state>/<target phone id>/stream/<label>.json) with their hashes, so a cable pull
resumes from the next chunk. A resume first checks the source's first and last chunk against the state file (a source
that changed meanwhile is refused: --restart starts over). At the end both phones hash the whole range: they must agree,
and the target phone's journal records the copy (source, size, SHA-256).

Safety, enforced here:
  - the source phone only ever runs read commands (`dd if=`, `sha256sum`, `readlink`, `blockdev --getsize64`, `wc -c`,
    `cat /proc/mounts`, `cat /sys/...`); anything else raises before it runs. Its partitions must not be mounted rw;
  - every adb call names the source serial (`adb -s`), the target is reached only by ssh to the installer environment,
    and the environment's serial must be --to-serial (another phone's environment on 172.16.42.1 is refused);
  - nothing is flashed, and the copy refuses to start while any phone is in Download mode (Heimdall picks the first
    one: no Download-mode work while two phones are connected);
  - target partitions only from the list above (never RECOVERY, EFS, PARAM ... or BOOT), not mounted; card ranges only
    inside a root slot; files only under /distros/ or /starbite/ on p1;
  - --dry-run (the default) resolves both sides read only, prints every command and records a dry run; --real needs that
    dry run (same source, target and size) and a typed confirmation.
Python 3 standard library; macOS and Linux."""
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from sbinst.common import CHUNK, DOWNLOAD_MODE_USB, MIB, Refused, Stopped, cardlayout, default_state_dir, human  # noqa: E402
from sbinst.device import InstallerEnv, usb_list  # noqa: E402
from sbinst.journal import Journal  # noqa: E402
from sbinst.steps import mbr_parts, phone_id_of  # noqa: E402

TARGET_PARTS = ('SYSTEM', 'VENDOR', 'ODM', 'A16DATA', 'WINESP', 'WINDOWS', 'USERDATA')
FILE_DIRS = ('/distros/', '/starbite/')
READ_ONLY = re.compile(r"^(dd if=\S+ bs=1048576( skip=\d+)? count=\d+ 2>/dev/null \| head -c \d+( \| sha256sum)?"
                       r"|readlink -f \S+|blockdev --getsize64 \S+|wc -c < \S+|cat /proc/mounts|cat /sys/\S+"
                       r"|\[ -e \S+ \] && echo yes)$")
_ZERO = {}


def zero_sha(n):
    if n not in _ZERO:
        _ZERO[n] = hashlib.sha256(bytes(n)).hexdigest()
    return _ZERO[n]


def q(s):
    return shlex.quote(s)


def range_cmd(node, off, ln, sha):
    """The one read command both sides use: `ln` bytes at MiB-aligned `off`."""
    return (f'dd if={q(node)} bs=1048576 skip={off // MIB} count={(ln + MIB - 1) // MIB} 2>/dev/null | head -c {ln}'
            + (' | sha256sum' if sha else ''))


class Spec:
    def __init__(self, text, side):
        self.text, self.side = text, side
        self.kind, self.name, self.start, self.sectors, self.path = None, None, None, None, None
        m = re.fullmatch(r'part:([A-Z0-9_]+)', text)
        if m:
            self.kind, self.name = 'part', m.group(1)
            if side == 'target' and self.name not in TARGET_PARTS:      # checked before either phone is asked anything
                raise Refused(f'target part:{self.name}: only {", ".join(TARGET_PARTS)} (never RECOVERY, BOOT, EFS, '
                              'PARAM ...)')
            return
        m = re.fullmatch(r'card:lba:(\d+)(?:\+(\d+))?', text)
        if m:
            self.kind, self.start = 'card', int(m.group(1))
            self.sectors = int(m.group(2)) if m.group(2) else None
            if side == 'source' and self.sectors is None:
                raise Refused(f'source {text}: card:lba:START+SECTORS (the source range needs its length)')
            return
        m = re.fullmatch(r'card:root:([a-z0-9-]+)', text)
        if m and side == 'target':
            self.kind, self.name = 'root', m.group(1)
            return
        m = re.fullmatch(r'file:(/[A-Za-z0-9_./-]+)', text)
        if m and '..' not in m.group(1).split('/') and m.group(1).startswith(FILE_DIRS):
            self.kind, self.path = 'file', m.group(1)
            return
        raise Refused(f'{side} {text!r}: not part:NAME, card:lba:START{"+SECTORS" if side == "source" else ""}'
                      f'{", card:root:KEY" if side == "target" else ""} or file:/distros/... | file:/starbite/...')


class Side:
    """Shell access to one phone. env: the environment for its commands (fake roots in a rehearsal)."""

    def __init__(self, label, env):
        self.label, self.env = label, env

    def run(self, argv, data=None, binary=False, check=True):
        p = subprocess.run(argv, input=data, capture_output=True, env=self.env,
                           stdin=None if data is not None else subprocess.DEVNULL)
        if check and p.returncode:
            raise Stopped(f'{self.label}: {" ".join(argv[-1:])[:160]} failed ({p.returncode}): '
                          f'{p.stderr.decode(errors="replace").strip()[-300:]}')
        return p if binary else subprocess.CompletedProcess(p.args, p.returncode, p.stdout.decode(errors='replace'),
                                                            p.stderr.decode(errors='replace'))


class Source(Side):
    """The source phone in TWRP: `adb -s SERIAL`, read commands only."""

    def __init__(self, serial, env):
        super().__init__(f'source {serial}', env)
        self.serial = serial

    def sh(self, cmd, binary=False, check=True):
        if not READ_ONLY.fullmatch(cmd):
            raise Refused(f'source: refusing a command that is not on the read-only list: {cmd[:120]}')
        return self.run(['adb', '-s', self.serial, 'exec-out' if binary else 'shell', cmd], binary=binary, check=check)

    def state(self):
        p = self.run(['adb', '-s', self.serial, 'get-state'], check=False)
        return p.stdout.strip()


class Target(Side):
    """The target phone in the Starbite installer environment, over ssh (no adb: the source owns `adb`)."""

    def __init__(self, serial, env, key_dir):
        super().__init__(f'target {serial}', env)
        self.serial = serial
        self.ie = InstallerEnv(None, key_dir)

    def sh(self, cmd, data=None, binary=False, check=True):
        return self.run(self.ie.ssh_argv(cmd, 5), data=data, binary=binary, check=check)

    def detached(self, cmd, tag, timeout=None):
        """`cmd` runs on the phone in a subshell that ignores SIGHUP (a dropped ssh link cannot cut the write in half);
        its exit status lands in /tmp/TAG.rc, polled here. A link down past `timeout` stops the run (a re-run resumes)."""
        rc = f'/tmp/{tag}.rc'
        timeout = timeout or int(os.environ.get('SBINST_DETACH_TIMEOUT', 180))   # (rehearsals shorten it)
        self.sh(f'rm -f {rc} {rc}.tmp; ( trap "" HUP INT; {cmd}; echo $? > {rc}.tmp; sync; mv {rc}.tmp {rc} ) '
                '</dev/null >/dev/null 2>&1 &')
        t0 = time.time()
        while time.time() - t0 < timeout:
            r = self.sh(f'cat {rc} 2>/dev/null', check=False).stdout.strip()
            if r:
                self.sh(f'rm -f {rc}', check=False)
                return r
            time.sleep(0.5)
        raise Stopped(f'no answer from the target for {timeout} s while {tag} was being written there (the link?): '
                      'the write goes on on the phone; run the same command again to resume')

    def wait_leftovers(self, timeout=180):
        """An earlier run that lost its link may have left a detached dd running: let it finish first."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not self.sh("ps -o args 2>/dev/null | grep '[d]d if=/tmp/sbsc-' || true", check=False).stdout.strip() and \
                    not self.sh("ps -o args 2>/dev/null | grep '[d]d of=.*sbsc-zero' || true", check=False).stdout.strip():
                return
            time.sleep(1)
        raise Stopped(f'an earlier chunk write is still running on the target after {timeout} s')

    def reachable(self):
        if not self.ie.key.is_file():
            return False
        p = self.sh('echo sbinst-env-ok', check=False)
        return p.returncode == 0 and 'sbinst-env-ok' in p.stdout


class Copy:
    def __init__(self, a):
        self.a = a
        self.mode = 'real' if a.real else 'rehearse' if a.rehearse else 'dry-run'
        base = dict(os.environ)
        for k in ('SBINST_ENV_UP', 'SB_TRANSPORT', 'ANDROID_SERIAL'):
            base.pop(k, None)                         # adb must be the real one, for the source only
        senv, tenv = dict(base), dict(base)
        if a.fake_src or a.fake_dst:
            if self.mode == 'real':
                raise Refused('--fake-src/--fake-dst and --real do not mix')
            if not (a.fake_src and a.fake_dst):
                raise Refused('a rehearsal needs both --fake-src and --fake-dst')
            fakes = str(HERE / 'fakes')
            for e, root in ((senv, a.fake_src), (tenv, a.fake_dst)):
                e['PATH'] = fakes + os.pathsep + e['PATH']
                e['FAKE_ROOT'] = str(Path(root).resolve())
            senv['FAKE_SERIAL'] = a.from_serial
            self.fake_usb = Path(a.fake_dst).resolve() / 'usb'
        elif self.mode == 'rehearse':
            raise Refused('--rehearse needs --fake-src and --fake-dst')
        else:
            self.fake_usb = None
        if a.from_serial == a.to_serial:
            raise Refused('--from-serial and --to-serial are the same phone')
        self.state_dir = Path(a.state_dir or default_state_dir()).resolve()
        self.src = Source(a.from_serial, senv)
        self.tgt = Target(a.to_serial, tenv, Path(os.environ.get('SBINST_STATE', default_state_dir())) / 'env')
        self.sspec, self.tspec = Spec(a.source, 'source'), Spec(a.target, 'target')
        self.pid = phone_id_of(a.to_serial)
        self.journal = Journal(self.state_dir / self.pid / 'journal.json')

    def say(self, s):
        print(s, flush=True)

    # ---------------------------------------------------------------- the two sides
    def check_phones(self):
        if self.fake_usb is not None:
            os.environ['SBINST_FAKE_USB'] = str(self.fake_usb)
        if DOWNLOAD_MODE_USB in usb_list():
            raise Refused('a phone is in Download mode: no copy while Heimdall could be in play (one phone at a time '
                          'in Download mode, and never two connected). Take it out of Download mode first')
        st = self.src.state()
        if st != 'recovery':
            raise Refused(f'the source {self.a.from_serial} is not in TWRP (adb -s says {st or "nothing"}): it must be '
                          'in TWRP, so that it is only read')
        if not self.tgt.reachable():
            raise Refused('the target\'s installer environment does not answer on 172.16.42.1 (start it with '
                          'starbite-install --bundle B --serial S2 --start-env)')
        got = self.tgt.sh('getprop ro.boot.serialno').stdout.strip()
        if got != self.a.to_serial:
            raise Refused(f'the environment on 172.16.42.1 is phone {got or "unknown"}, not --to-serial {self.a.to_serial}')
        self.say(f'ok   source {self.a.from_serial}: TWRP (read only); target {self.a.to_serial} ({self.pid}): '
                 'the installer environment')

    def resolve_source(self):
        s = self.sspec
        if s.kind == 'part':
            node = self.src.sh(f'readlink -f /dev/block/by-name/{s.name}').stdout.strip()
            if not re.fullmatch(r'/dev/block/sd[a-e]\d+', node):
                raise Refused(f'source: no partition {s.name} ({node or "none"})')
            size = int(self.src.sh(f'blockdev --getsize64 {node}').stdout.strip())
            off = 0
            for ln in self.src.sh('cat /proc/mounts').stdout.splitlines():
                b = ln.split()
                if len(b) >= 4 and b[0] in (node, f'/dev/block/by-name/{s.name}') and 'rw' in b[3].split(','):
                    raise Refused(f'source: {s.name} is mounted read-write at {b[1]} in TWRP: unmount it first '
                                  '(the copy would not be one consistent image)')
        elif s.kind == 'card':
            node, off, size = '/dev/block/mmcblk0', s.start * 512, s.sectors * 512
            card = int(self.src.sh('cat /sys/block/mmcblk0/size').stdout.strip() or 0)
            if s.start + s.sectors > card:
                raise Refused(f'source: card:lba:{s.start}+{s.sectors} is past the card\'s end ({card} sectors)')
        else:
            node, off = '/external_sd' + s.path, 0
            if self.src.sh(f'[ -e {q(node)} ] && echo yes', check=False).stdout.strip() != 'yes':
                raise Refused(f'source: no file {node} (TWRP mounts the card\'s p1 at /external_sd)')
            size = int(self.src.sh(f'wc -c < {q(node)}').stdout.strip())
        if off % MIB:
            raise Refused(f'source: offset {off} is not MiB aligned')
        if self.a.size:
            if self.a.size > size:
                raise Refused(f'--size {self.a.size} is larger than the source ({size})')
            size = self.a.size
        self.snode, self.soff, self.size = node, off, size

    def resolve_target(self):
        t, size = self.tspec, self.size
        if t.kind == 'part':
            if t.name not in TARGET_PARTS:
                raise Refused(f'target part:{t.name}: only {", ".join(TARGET_PARTS)} (never RECOVERY, BOOT, EFS, PARAM ...)')
            node = self.tgt.sh(f'readlink -f /dev/block/by-name/{t.name}').stdout.strip()
            if not re.fullmatch(r'/dev/block/sda\d+', node):
                raise Refused(f'target: no partition {t.name} on sda ({node or "none"})')
            cap = int(self.tgt.sh(f'blockdev --getsize64 {node}').stdout.strip())
            if cap < size:
                raise Refused(f'target {t.name} is {human(cap)}, the source is {human(size)}')
            mounts = self.tgt.sh('cat /proc/mounts').stdout
            if any(ln.split()[0] in (node, f'/dev/block/by-name/{t.name}') for ln in mounts.splitlines() if ln.split()):
                raise Refused(f'target {t.name} is mounted in the environment: unmount it first')
            self.tnode, self.toff, self.final = node, 0, None
        elif t.kind in ('card', 'root'):
            mbr = self.tgt.sh('dd if=/dev/block/mmcblk0 bs=512 count=1 2>/dev/null', binary=True).stdout
            sectors = int(self.tgt.sh('cat /sys/block/mmcblk0/size').stdout.strip() or 0)
            name = cardlayout.identify(mbr_parts(mbr) or [], sectors)
            if name is None:
                raise Refused('target card: not a Starbite card layout (run the core install, step A, first)')
            L = cardlayout.get(name)
            if t.kind == 'root':
                area = L.root(t.name)
                if area is None:
                    raise Refused(f'target card ({name}) has no root slot for {t.name}')
                start = area.start
            else:
                start = t.start
            area = L.area_at(start, (size + 511) // 512)
            if area is None or area.kind != 'root':
                raise Refused(f'target card:lba:{start} + {human(size)} is not inside one root slot of the {name} layout '
                              '(never p1, the store or the tail)')
            self.tnode, self.toff, self.final = '/dev/block/mmcblk0', start * 512, None
            self.say(f'ok   target card layout {name}: root slot {area.name} {area.start}+{area.sectors}')
        else:
            if self.mode != 'dry-run':
                self.tgt.sh('grep -q " /external_sd " /proc/mounts || { mkdir -p /external_sd; mount -t exfat '
                            '/dev/block/mmcblk0p1 /external_sd 2>/dev/null || exfat-fuse /dev/block/mmcblk0p1 /external_sd; }')
            self.final = '/external_sd' + t.path
            self.tnode, self.toff = self.final + '.part', 0
        if self.toff % MIB:
            raise Refused(f'target: offset {self.toff} is not MiB aligned')

    # ---------------------------------------------------------------- state
    def label(self):
        return re.sub(r'[^A-Za-z0-9_.-]+', '_', f'{self.a.from_serial[:4]}-{self.sspec.text}--{self.tspec.text}')

    def state_path(self):
        return self.state_dir / self.pid / 'stream' / f'{self.label()}.json'

    def ident(self):
        return {'source': self.sspec.text, 'source_phone': phone_id_of(self.a.from_serial), 'target': self.tspec.text,
                'size': self.size, 'snode': self.snode, 'tnode': self.tnode}

    def fingerprint(self):
        n = (self.size + CHUNK - 1) // CHUNK
        first = self.src.sh(range_cmd(self.snode, self.soff, min(CHUNK, self.size), True)).stdout.split()[0]
        lo = (n - 1) * CHUNK
        last = self.src.sh(range_cmd(self.snode, self.soff + lo, self.size - lo, True)).stdout.split()[0]
        return [first, last]

    # ---------------------------------------------------------------- run
    def plan(self):
        n = (self.size + CHUNK - 1) // CHUNK
        self.say(f'   source: {self.a.from_serial} {self.sspec.text} = {self.snode} at byte {self.soff}, {human(self.size)} '
                 f'({self.size} bytes)')
        self.say(f'   target: {self.a.to_serial} {self.tspec.text} = {self.tnode} at byte {self.toff}'
                 + (f' (then mv to {self.final})' if self.final else ''))
        self.say(f'   {n} chunks of 64 MiB, through this Mac\'s RAM only. Per chunk:')
        self.say(f'   $ adb -s {self.a.from_serial} shell "{range_cmd(self.snode, self.soff, CHUNK, True)}"   (source hash)')
        self.say(f'   $ adb -s {self.a.from_serial} exec-out "{range_cmd(self.snode, self.soff, CHUNK, False)}"   (the bytes, '
                 'unless the chunk is all zeros)')
        self.say('   $ ssh root@172.16.42.1 "cat > /tmp/sbsc-N.chunk; (trap \'\' HUP; dd if=/tmp/sbsc-N.chunk of=' + self.tnode +
                 f' bs=1048576 seek=<{self.toff // MIB}+64*i> conv=notrunc,fsync"   (zero chunks: head -c LEN /dev/zero | dd ...)')
        self.say(f'   $ ssh root@172.16.42.1 "{range_cmd(self.tnode, self.toff, CHUNK, True)}"   (read back)')
        self.say(f'   at the end: both phones hash all {human(self.size)}; the target journal records it')

    def run(self):
        self.say(f'== stream-copy ({self.mode.upper()}) ==')
        self.check_phones()
        self.resolve_source()
        self.resolve_target()
        self.plan()
        gate = 'stream:' + hashlib.sha256(json.dumps(self.ident(), sort_keys=True).encode()).hexdigest()
        if self.mode == 'dry-run':
            self.journal.record_dry_run(gate)
            self.say('ok   dry run: both sides resolved read only, nothing written. --real may now run this copy.')
            return 0
        if not self.journal.had_dry_run(gate):
            raise Refused('no dry run of this copy (same source, target and size): run it with --dry-run first')
        sp = self.state_path()
        st = json.loads(sp.read_text()) if sp.is_file() and not self.a.restart else None
        fp = self.fingerprint()
        if st is not None and (st.get('ident') != self.ident() or st.get('fingerprint') != fp):
            raise Refused(f'{sp.name}: the source changed since the copy started (or another copy has this name): run '
                          'again with --restart to start over')
        if st is None:
            st = {'ident': self.ident(), 'fingerprint': fp, 'chunks': {}}
        if self.mode == 'real':
            want = f'stream to {self.pid}'
            print(f'Write {human(self.size)} to {self.tspec.text} on {self.a.to_serial}? Type exactly: {want}')
            try:
                got = input('   > ').strip()
            except EOFError:
                got = ''
            if got != want:
                raise Refused('not confirmed: nothing written')
        elif not self.a.yes:
            raise Refused('a rehearsal needs --yes')
        if self.final and not st['chunks']:
            self.tgt.sh(f'mkdir -p {q(os.path.dirname(self.final))}; rm -f {q(self.tnode)}')
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(st))             # before the first write: a pull in chunk 0 still resumes (and is checked)
        self.tgt.wait_leftovers()
        self.journal.mark_step(f'stream:{self.tspec.text}', 'started', **self.ident())
        n = (self.size + CHUNK - 1) // CHUNK
        t0, moved = time.time(), 0
        for i in range(n):
            if str(i) in st['chunks']:
                continue
            ln = min(CHUNK, self.size - i * CHUNK)
            soff, toff = self.soff + i * CHUNK, self.toff + i * CHUNK
            want = self.src.sh(range_cmd(self.snode, soff, ln, True)).stdout.split()[0]
            seek = toff // MIB
            if want == zero_sha(ln):
                r = self.tgt.detached(f': sbsc-zero; head -c {ln} /dev/zero | dd of={q(self.tnode)} bs=1048576 seek={seek} '
                                      'conv=notrunc,fsync 2>/dev/null', f'sbsc-{seek}')
                if r != '0':
                    raise Stopped(f'chunk {i}: zero write failed on the target (dd exit {r}): a re-run resumes here')
            else:
                data = self.src.sh(range_cmd(self.snode, soff, ln, False), binary=True).stdout
                if len(data) != ln or hashlib.sha256(data).hexdigest() != want:
                    raise Stopped(f'chunk {i}: the bytes read ({len(data)}) do not match the source phone\'s hash: '
                                  'stopped (a re-run resumes here)')
                tmp = f'/tmp/sbsc-{seek}.chunk'
                self.tgt.sh(f'cat > {tmp}', data=data)
                r = self.tgt.detached(f'dd if={tmp} of={q(self.tnode)} bs=1048576 seek={seek} conv=notrunc,fsync 2>/dev/null',
                                      f'sbsc-{seek}')
                self.tgt.sh(f'rm -f {tmp}', check=False)
                if r != '0':
                    raise Stopped(f'chunk {i}: the write failed on the target (dd exit {r}): a re-run resumes here')
                moved += ln
            back = self.tgt.sh(range_cmd(self.tnode, toff, ln, True)).stdout.split()
            if not back or back[0] != want:
                raise Stopped(f'chunk {i}: the target read back {back[0][:16] if back else "nothing"}... != {want[:16]}...: '
                              'stopped (a re-run resumes here)')
            st['chunks'][str(i)] = want
            tmp = sp.with_suffix('.tmp')
            tmp.write_text(json.dumps(st))
            os.replace(tmp, sp)
            if (i + 1) % 8 == 0 or i + 1 == n:
                dt = max(time.time() - t0, 1)
                self.say(f'   {min((i + 1) * 64, max(self.size // MIB, 1))} / {max(self.size // MIB, 1)} MiB '
                         f'({moved / MIB / dt:.0f} MiB/s over USB, the rest zeros)')
        self.say('   both phones hash the whole range...')
        sh = self.src.sh(range_cmd(self.snode, self.soff, self.size, True)).stdout.split()[0]
        th = self.tgt.sh(range_cmd(self.tnode, self.toff, self.size, True)).stdout.split()[0]
        if sh != th:
            raise Stopped(f'whole range: source {sh[:16]}... != target {th[:16]}...: stopped. --restart copies it again')
        if self.final:
            self.tgt.sh(f'mv -f {q(self.tnode)} {q(self.final)}; sync')
            got = self.tgt.sh(f'sha256sum {q(self.final)}').stdout.split()[0]
            if got != sh:
                raise Stopped(f'{self.final}: {got[:16]}... after the move, expected {sh[:16]}...')
        self.journal.mark_step(f'stream:{self.tspec.text}', 'done', sha256=sh, **self.ident())
        sp.unlink()
        self.say(f'ok   {self.tspec.text} on {self.a.to_serial} = {self.sspec.text} of {self.a.from_serial}: '
                 f'{human(self.size)}, sha256 {sh[:16]}... (both phones agree)')
        return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--from-serial', required=True)
    ap.add_argument('--to-serial', required=True)
    ap.add_argument('--source', required=True)
    ap.add_argument('--target', required=True)
    ap.add_argument('--size', type=int)
    m = ap.add_mutually_exclusive_group()
    m.add_argument('--dry-run', action='store_true')
    m.add_argument('--real', action='store_true')
    m.add_argument('--rehearse', action='store_true')
    ap.add_argument('--restart', action='store_true', help='forget a finished-chunk state and copy from chunk 0')
    ap.add_argument('--state-dir')
    ap.add_argument('--fake-src')
    ap.add_argument('--fake-dst')
    ap.add_argument('--yes', action='store_true', help='rehearsal only')
    a = ap.parse_args(argv)
    if a.yes and a.real:
        print('REFUSED: --yes is not accepted with --real')
        return 2
    try:
        return Copy(a).run()
    except Refused as e:
        print(f'REFUSED: {e}\n   nothing was written by this step.')
        return 2
    except Stopped as e:
        print(f'STOPPED: {e}\n   run the same command again to resume from the last verified chunk.')
        return 3


if __name__ == '__main__':
    sys.exit(main())
