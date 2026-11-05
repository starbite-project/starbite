"""Talking to the phone: a command runner that honours the mode, adb/fastboot wrappers, phone
state detection and the read-only fact sheet (preflight)."""
import gzip
import hashlib
import os
import platform
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, field

from .common import CHUNK, DOWNLOAD_MODE_USB, MIB, REPO, STARBITE_FASTBOOT_USB, Refused, Stopped


class Result:
    def __init__(self, returncode=0, stdout='', stderr=''):
        self.returncode, self.stdout, self.stderr = returncode, stdout, stderr


class Runner:
    """Runs commands. mode: dry-run (writes are printed, not run), rehearse (fakes only), real.
    Every command and its outcome goes to the session log; `writes` counts executed writes."""

    def __init__(self, ui, mode, env=None):
        self.ui, self.mode, self.env = ui, mode, env or dict(os.environ)
        self.writes = 0
        self.planned = []        # dry run: the write commands that would run

    def show(self, argv):
        """The command as text; paths inside the checkout are shown relative to it (run from the repo root)."""
        root = str(REPO) + '/'
        argv = ['python3' if a == sys.executable else a for a in argv]
        text = ' '.join(shlex.quote(str(a).replace(root, '')) for a in argv)
        return text.split('\n')[0] + " ...'" if '\n' in text else text

    def run(self, argv, write=False, binary=False, check=True, timeout=None, env_extra=None, quiet=False,
            stream=False):
        argv = [str(a) for a in argv]
        if write and self.mode == 'dry-run':
            self.planned.append(self.show(argv))
            if not quiet:
                self.ui.cmd(self.show(argv) + '    [dry run: not run]')
            return Result(0, b'' if binary else '', '')
        if self.ui.log:
            self.ui.log.write(f'   $ {self.show(argv)}\n')
        env = dict(self.env, **(env_extra or {}))
        try:
            if stream:          # an external writer: its progress lines go to the screen and the log
                p = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, text=True)
                out = []
                for line in p.stdout:
                    out.append(line)
                    self.ui.info('| ' + line.rstrip())
                p.wait(timeout=timeout)
                r = Result(p.returncode, ''.join(out), '')
            else:
                p = subprocess.run(argv, env=env, capture_output=True, stdin=subprocess.DEVNULL,
                                   timeout=timeout, text=not binary)
                r = Result(p.returncode, p.stdout, p.stderr)
        except FileNotFoundError:
            r = Result(127, b'' if binary else '', f'{argv[0]}: not found')
        if write:
            self.writes += 1
        if self.ui.log and not binary and not stream:
            if r.stdout.strip():
                self.ui.log.write('     ' + r.stdout.strip().replace('\n', '\n     ')[:4000] + '\n')
            if r.stderr.strip():
                self.ui.log.write('     [stderr] ' + r.stderr.strip()[:2000] + '\n')
        if check and r.returncode:
            msg = (r.stderr if isinstance(r.stderr, str) else r.stderr.decode(errors='replace')).strip()
            out = r.stdout if isinstance(r.stdout, str) else ''
            raise Stopped(f'command failed ({r.returncode}): {self.show(argv)}\n{out.strip()[-1500:]}\n{msg[-800:]}')
        return r


@dataclass
class State:
    kind: str                  # recovery | android | unauthorized | fastboot-starbite | fastboot-other | download | none
    serial: str = ''
    detail: str = ''


def usb_ids(runner=None):
    """vid:pid pairs on the host's USB (for Download mode, which neither adb nor fastboot sees)."""
    fake = os.environ.get('SBINST_FAKE_USB')
    if fake:
        try:
            return {tuple(l.split(':')) for l in open(fake).read().split()}
        except OSError:
            return set()
    ids = set()
    if platform.system() == 'Darwin':
        try:
            out = subprocess.run(['ioreg', '-p', 'IOUSB', '-l', '-w0'], capture_output=True, text=True).stdout
        except FileNotFoundError:
            return ids
        vid = None
        for line in out.splitlines():
            m = re.search(r'"idVendor" = (\d+)', line)
            if m:
                vid = int(m.group(1))
            m = re.search(r'"idProduct" = (\d+)', line)
            if m and vid is not None:
                ids.add((f'{vid:04x}', f'{int(m.group(1)):04x}'))
        return ids
    base = '/sys/bus/usb/devices'
    if os.path.isdir(base):
        for d in os.listdir(base):
            try:
                v = open(f'{base}/{d}/idVendor').read().strip()
                p = open(f'{base}/{d}/idProduct').read().strip()
                ids.add((v, p))
            except OSError:
                pass
    return ids


class Phone:
    def __init__(self, runner):
        self.r = runner

    # -- transport -----------------------------------------------------------------------------
    def adb(self, *args, write=False, binary=False, check=True, quiet=False):
        for attempt in range(4):
            res = self.r.run(['adb', *args], write=write, binary=binary, check=False, quiet=quiet)
            err = res.stderr if isinstance(res.stderr, str) else res.stderr.decode(errors='replace')
            if not (res.returncode and any(s in err for s in ('device offline', 'no devices', 'cannot connect'))):
                break
            self.r.ui.warn(f'adb: {err.strip()[:100]}; retrying ({attempt + 1})')
            time.sleep(3)
        if check and res.returncode:
            out = res.stdout if isinstance(res.stdout, str) else ''
            raise Stopped(f'adb {" ".join(args)[:200]} failed: {out.strip()[-500:]} {err.strip()[-500:]}')
        return res

    def sh(self, cmd, write=False, check=True, quiet=False):
        return self.adb('shell', cmd, write=write, check=check, quiet=quiet).stdout.replace('\r', '').strip()

    def exec_out(self, cmd):
        return self.adb('exec-out', cmd, binary=True).stdout

    def push(self, local, remote):
        """To the phone's /tmp (RAM): not a storage write, but still skipped in a dry run."""
        self.adb('push', local, remote, write=True, quiet=True)

    def fastboot(self, *args, write=False, check=False):
        return self.r.run(['fastboot', *args], write=write, check=check)

    # -- state ---------------------------------------------------------------------------------
    def detect(self):
        found = []
        res = self.r.run(['adb', 'devices'], check=False)
        for line in (res.stdout or '').splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 2:
                st = {'recovery': 'recovery', 'device': 'android', 'unauthorized': 'unauthorized',
                      'sideload': 'recovery'}.get(parts[1], parts[1])
                found.append(State(st, parts[0], 'adb'))
        res = self.r.run(['fastboot', 'devices'], check=False)
        for line in (res.stdout or '').splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1] == 'fastboot':
                v = self.fastboot('getvar', 'version-bootloader')
                text = (v.stdout or '') + (v.stderr or '')
                m = re.search(r'version-bootloader:\s*(\S+)', text)
                vb = m.group(1) if m else ''
                found.append(State('fastboot-starbite' if vb.startswith('starbite') else 'fastboot-other',
                                   parts[0], vb))
        ids = usb_ids()
        if DOWNLOAD_MODE_USB in ids:
            found.append(State('download', '', 'Samsung Download mode (04e8:685d)'))
        if not found and STARBITE_FASTBOOT_USB in ids:
            found.append(State('none', '', 'a Starbite fastboot device is on USB but fastboot does not list it'))
        if len(found) > 1:
            raise Refused('more than one phone (or mode) is connected: ' +
                          ', '.join(f'{f.kind} {f.serial}' for f in found) + '. Connect only the S9.')
        return found[0] if found else State('none')

    def wait_for(self, kind, seconds, poll=3):
        t0 = time.time()
        while time.time() - t0 < seconds:
            st = self.detect()
            if st.kind == kind:
                return st
            time.sleep(poll)
        return self.detect()

    # -- facts ---------------------------------------------------------------------------------
    FACTS = r'''
for p in ro.product.model ro.product.device ro.boot.bootloader ro.boot.serialno ro.boot.flash.locked \
         ro.boot.verifiedbootstate ro.boot.warranty_bit ro.twrp.version; do echo "prop=$p=$(getprop $p)"; done
echo "battery=$(cat /sys/class/power_supply/battery/capacity 2>/dev/null)"
echo "sda=$(cat /sys/block/sda/size 2>/dev/null)"
echo "card=$(cat /sys/block/mmcblk0/size 2>/dev/null)"
echo "card_name=$(cat /sys/block/mmcblk0/device/name 2>/dev/null)"
for l in /dev/block/by-name/*; do b=$(readlink -f $l); b=${b##*/}
  echo "part=${l##*/} $b $(cat /sys/class/block/$b/start 2>/dev/null) $(cat /sys/class/block/$b/size 2>/dev/null)"; done
for t in mkfs.exfat mkexfatfs mke2fs e2fsck exfat-fuse sha256sum gzip blockdev; do
  command -v $t >/dev/null 2>&1 && echo "tool=$t"; done
cat /proc/mounts | while read d m t r; do echo "mount=$d $m $t"; done
df -k /external_sd 2>/dev/null | tail -1 | while read a b c d e f; do echo "sdfree=$d"; done
'''

    def facts(self):
        out = self.sh(self.FACTS)
        f = Facts()
        for line in out.splitlines():
            k, _, v = line.partition('=')
            if k == 'prop':
                pk, _, pv = v.partition('=')
                f.props[pk] = pv
            elif k == 'part':
                bits = v.split()
                if len(bits) == 4 and bits[2].isdigit() and bits[3].isdigit():
                    f.parts[bits[0]] = (bits[1], int(bits[2]), int(bits[3]))
            elif k == 'tool':
                f.tools.add(v)
            elif k == 'mount':
                d, m, t = (v.split() + ['', '', ''])[:3]
                f.mounts.append((d, m, t))
            elif k in ('battery', 'sda', 'card', 'sdfree'):
                setattr(f, k, int(v) if v.strip().isdigit() else None)
            elif k == 'card_name':
                f.card_name = v
        return f

    # -- reading ranges ------------------------------------------------------------------------
    def range_sha(self, node, offset, size):
        """SHA-256 of `size` bytes at `offset` of a node, computed on the phone."""
        if offset % MIB == 0:
            cmd = (f'dd if={node} bs=1048576 skip={offset // MIB} count={(size + MIB - 1) // MIB} 2>/dev/null'
                   f' | head -c {size} | sha256sum')
        else:
            cmd = f'dd if={node} bs=4096 skip={offset // 4096} count={(size + 4095) // 4096} 2>/dev/null | head -c {size} | sha256sum'
        out = self.sh(cmd).split()
        return out[0] if out else ''

    def is_blank(self, node, offset, size):
        """First and last MiB of the range read as zeros (a never-used target)."""
        n = min(size, MIB)
        zero = hashlib.sha256(bytes(n)).hexdigest()
        if self.range_sha(node, offset, n) != zero:
            return False
        return self.range_sha(node, offset + max(0, size - MIB) // MIB * MIB, n) == zero


@dataclass
class Facts:
    props: dict = field(default_factory=dict)
    parts: dict = field(default_factory=dict)        # by-name -> (node, start sectors, size sectors)
    tools: set = field(default_factory=set)
    mounts: list = field(default_factory=list)       # (device, mount point, type)
    battery: int = None
    sda: int = None                                  # 512-byte sectors
    card: int = None                                 # 512-byte sectors
    card_name: str = ''
    sdfree: int = None                               # KiB free on /external_sd

    def node(self, name):
        p = self.parts.get(name)
        return f'/dev/block/{p[0]}' if p else None

    def size(self, name):
        p = self.parts.get(name)
        return p[2] * 512 if p else None

    def mounted(self, node_or_name):
        n = node_or_name if node_or_name.startswith('/') else self.node(node_or_name)
        alias = f'/dev/block/by-name/{node_or_name}' if not node_or_name.startswith('/') else None
        return [m for m in self.mounts if m[0] in (n, alias)]

    def mount_of(self, mountpoint):
        return next((m for m in self.mounts if m[1] == mountpoint), None)


def open_text(p):
    with open(p) as f:
        return f.read()


def chunk_write(phone, ui, src, size, node, offset, state_path, label, file_mode=False):
    """The installer's own verified writer (the algorithm of tools/android/pixel/part-write.py and
    tools/macos/card-range-flash.py, for the targets those tools do not take: BOOT, RECOVERY,
    WINESP, WINDOWS and files on the card's exFAT). 64 MiB chunks: zero chunks are written from
    /dev/zero on the phone, others are gzip'd, pushed to /tmp and dd'd at their offset; every
    chunk is read back and compared by SHA-256 on the phone; finished chunks go to state_path, so
    an interrupted write resumes. The caller verifies the whole range afterwards."""
    import tempfile
    if offset % MIB:
        raise Stopped(f'{label}: offset {offset} is not MiB aligned')
    os.makedirs(os.path.dirname(state_path), exist_ok=True)
    done = set(open_text(state_path).split()) if os.path.exists(state_path) else set()
    n = (size + CHUNK - 1) // CHUNK
    dry = phone.r.mode == 'dry-run'
    if dry:
        ui.cmd(f'adb push <chunk i>.gz /tmp/sbinst-chunk.gz; adb shell "gzip -dc /tmp/sbinst-chunk.gz | '
               f'dd of={node} bs=1048576 seek=<{offset // MIB}+64*i> conv=notrunc,fsync"   (i = 0..{n - 1}; zero chunks: '
               f'head -c <len> /dev/zero | dd ...)    [dry run: not run]')
        ui.cmd(f'adb shell "dd if={node} bs=1048576 skip=<{offset // MIB}+64*i> count=<64> | head -c <len> | sha256sum"'
               '   (read back every chunk)')
        return
    zero = bytes(CHUNK)
    t0 = time.time()
    with open(src, 'rb') as f, tempfile.TemporaryDirectory() as tmp:
        for i in range(n):
            data = f.read(CHUNK)
            if str(i) in done:
                continue
            seek = offset // MIB + i * (CHUNK // MIB)
            if data == zero[:len(data)]:
                phone.sh(f'head -c {len(data)} /dev/zero | dd of={node} bs=1048576 seek={seek} conv=notrunc,fsync 2>/dev/null',
                         write=True, quiet=True)
            else:
                gz = os.path.join(tmp, 'chunk.gz')
                with gzip.open(gz, 'wb', compresslevel=1) as g:
                    g.write(data)
                phone.push(gz, '/tmp/sbinst-chunk.gz')
                phone.sh(f'gzip -dc /tmp/sbinst-chunk.gz | dd of={node} bs=1048576 seek={seek} conv=notrunc,fsync 2>/dev/null;'
                         ' rm -f /tmp/sbinst-chunk.gz', write=True, quiet=True)
            back = phone.sh(f'dd if={node} bs=1048576 skip={seek} count={(len(data) + MIB - 1) // MIB} 2>/dev/null'
                            f' | head -c {len(data)} | sha256sum', quiet=True).split()
            want = hashlib.sha256(data).hexdigest()
            if not back or back[0] != want:
                raise Stopped(f'{label}: chunk {i} read back {back[0][:16] if back else "nothing"}... != {want[:16]}...: '
                              'stopped (a re-run resumes from this chunk)')
            with open(state_path, 'a') as st:
                st.write(f'{i}\n')
            if (i + 1) % 8 == 0 or i + 1 == n:
                ui.info(f'{label}: {min((i + 1) * 64, size // MIB or 1)} / {max(size // MIB, 1)} MiB'
                        f' ({(i + 1) * 64 / max(time.time() - t0, 1):.0f} MiB/s)')
