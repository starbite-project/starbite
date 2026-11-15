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
from pathlib import Path

from .common import CHUNK, DOWNLOAD_MODE_USB, INSTALLER_DIR, MIB, REPO, STARBITE_FASTBOOT_USB, Refused, Stopped


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


def usb_list(runner=None):
    """vid:pid pairs on the host's USB, one per device (two phones in Download mode give two
    ('04e8', '685d')): Download mode is seen neither by adb nor by fastboot, and heimdall has no
    device selector. Rehearsals: SBINST_FAKE_USB (a file of vvvv:pppp words) plus SBINST_FAKE_USB_EXTRA
    (more words: a second fake phone)."""
    fake = os.environ.get('SBINST_FAKE_USB')
    if fake:
        try:
            words = open(fake).read().split()
        except OSError:
            words = []
        words += os.environ.get('SBINST_FAKE_USB_EXTRA', '').split()
        return [tuple(w.split(':')) for w in words if ':' in w]
    ids = []
    if platform.system() == 'Darwin':
        try:
            out = subprocess.run(['ioreg', '-p', 'IOUSB', '-l', '-w0'], capture_output=True, text=True).stdout
        except FileNotFoundError:
            return ids
        vid = None
        for line in out.splitlines():
            if '+-o ' in line:                        # a new device node: forget the last one's vendor
                vid = None
            m = re.search(r'"idVendor" = (\d+)', line)
            if m:
                vid = int(m.group(1))
            m = re.search(r'"idProduct" = (\d+)', line)
            if m and vid is not None:
                ids.append((f'{vid:04x}', f'{int(m.group(1)):04x}'))
                vid = None
        return ids
    base = '/sys/bus/usb/devices'
    if not os.path.isdir(base):                       # no sysfs: lsusb's "ID vvvv:pppp"
        try:
            out = subprocess.run(['lsusb'], capture_output=True, text=True).stdout
        except FileNotFoundError:
            return ids
        return [tuple(m.split(':')) for m in re.findall(r'ID ([0-9a-f]{4}:[0-9a-f]{4})', out)]
    for d in os.listdir(base):
        if ':' in d:                                  # interfaces (1-1:1.0), not devices
            continue
        try:
            v = open(f'{base}/{d}/idVendor').read().strip()
            p = open(f'{base}/{d}/idProduct').read().strip()
            ids.append((v, p))
        except OSError:
            pass
    return ids


def usb_ids(runner=None):
    """The set of vid:pid pairs on the host's USB."""
    return set(usb_list(runner))


# USB vendors a phone shows up under: Samsung (Android, MTP, Download mode) and Google's 18d1 (adb,
# fastboot, the Starbite loader's fastboot 18d1:4ee0).
PHONE_VENDORS = ('04e8', '18d1')


def target_serial():
    """The phone this run is for (--serial, which main() puts in ANDROID_SERIAL; adb and fastboot honour
    it too). '' = not given: then exactly one phone may be connected."""
    return os.environ.get('SBINST_SERIAL') or os.environ.get('ANDROID_SERIAL') or ''


class Phone:
    def __init__(self, runner, env=None, serial=None):
        self.r = runner
        self.env = env              # InstallerEnv: detect() looks for it first
        self.serial = target_serial() if serial is None else serial    # every adb/fastboot call names it
        self.ignored = []           # other phones seen by detect() (not this run's serial)

    def _s(self):
        return ['-s', self.serial] if self.serial else []

    # -- transport -----------------------------------------------------------------------------
    def adb(self, *args, write=False, binary=False, check=True, quiet=False):
        for attempt in range(4):
            res = self.r.run(['adb', *self._s(), *args], write=write, binary=binary, check=False, quiet=quiet)
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
        return self.r.run(['fastboot', *self._s(), *args], write=write, check=check)

    # -- state ---------------------------------------------------------------------------------
    def detect(self):
        """The phone's state. With a serial (--serial), phones with another serial are ignored (and listed
        in self.ignored); without one, more than one phone refuses. Download mode has no serial on USB:
        with a serial it is taken as this phone only when this serial is not visible elsewhere and only
        one phone is in Download mode (heimdall cannot pick one: Phone.download_alone() before a flash)."""
        self.ignored = []
        if self.env is not None and self.env.reachable():
            es = self.env.remote_serial() if self.serial else ''
            if self.serial and es != self.serial:
                raise Refused(f'an installer environment answers on {self.env.HOST}, but it is not this phone\'s '
                              f'(its serial {es or "unknown"}, this run is for {self.serial}). Another phone is in '
                              'Starbite Maintenance or the installer environment: unplug it, then run again')
            return State('env', self.serial or self.env.serial, 'Starbite installer environment (ssh root@172.16.42.1)')
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
                v = self.r.run(['fastboot', '-s', parts[0], 'getvar', 'version-bootloader'], check=False)
                text = (v.stdout or '') + (v.stderr or '')
                m = re.search(r'version-bootloader:\s*(\S+)', text)
                vb = m.group(1) if m else ''
                found.append(State('fastboot-starbite' if vb.startswith('starbite') else 'fastboot-other',
                                   parts[0], vb))
        ids = usb_list()
        n_dl = ids.count(DOWNLOAD_MODE_USB)
        for _ in range(n_dl):
            found.append(State('download', '', 'Samsung Download mode (04e8:685d)'))
        if not found and STARBITE_FASTBOOT_USB in ids:
            found.append(State('none', '', 'a Starbite fastboot device is on USB but fastboot does not list it'))
        if self.serial:
            mine = [f for f in found if f.serial == self.serial]
            self.ignored = [f for f in found if f.serial and f.serial != self.serial]
            dl = [f for f in found if f.kind == 'download']
            if len(mine) > 1:
                raise Refused(f'phone {self.serial} shows up twice: ' + ', '.join(f.kind for f in mine))
            if mine:
                if dl:
                    raise Refused(f'phone {self.serial} is in {mine[0].kind}, and another phone is in Download mode: '
                                  'take that one out of Download mode (Volume Down + Power) or unplug it')
                return mine[0]
            if len(dl) > 1:
                raise Refused(f'{len(dl)} phones are in Download mode, and heimdall cannot tell them apart: unplug '
                              'all but the one to flash')
            if dl:
                return State('download', self.serial, 'Samsung Download mode (04e8:685d): taken as ' + self.serial +
                             ' (its serial is not visible in Download mode; nothing else shows that serial)')
            return State('none')
        if len(found) > 1:
            raise Refused('more than one phone (or mode) is connected: ' +
                          ', '.join(f'{f.kind} {f.serial}' for f in found) + '. Connect only the S9, or name the '
                          'phone with --serial SERIAL (adb devices / fastboot devices show it)')
        return found[0] if found else State('none')

    def download_alone(self):
        """Right before heimdall writes: heimdall flashes the first Samsung Download-mode device it finds,
        so exactly one may be on USB, and no other phone at all (no adb or fastboot device, no other
        Samsung or 18d1 USB device). Returns '' when that holds, else why not."""
        ids = usb_list()
        if ids.count(DOWNLOAD_MODE_USB) != 1:
            return f'{ids.count(DOWNLOAD_MODE_USB)} phones in Download mode on USB (exactly 1 needed)'
        others = [f'{v}:{p}' for v, p in ids if v in PHONE_VENDORS and (v, p) != DOWNLOAD_MODE_USB]
        lines = [l.split()[0] for c in (['adb', 'devices'], ['fastboot', 'devices'])
                 for l in (self.r.run(c, check=False).stdout or '').splitlines()
                 if l.strip() and not l.startswith('List of') and len(l.split()) >= 2]
        if others or lines:
            return ('another phone is connected (' + ', '.join(others + [f'serial {x}' for x in lines]) +
                    '): unplug every other phone before a Download-mode flash')
        return ''

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
echo "battery=$(cat /sys/class/power_supply/battery/capacity 2>/dev/null || cat /sys/class/power_supply/*/capacity 2>/dev/null | head -1)"
echo "sda=$(cat /sys/block/sda/size 2>/dev/null)"
echo "lbs=$(cat /sys/block/sda/queue/logical_block_size 2>/dev/null)"
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
            elif k in ('battery', 'sda', 'card', 'sdfree', 'lbs'):
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


class Heimdall:
    """heimdall with Samsung's session rule: after a command that ends with --no-reboot, the next one
    must pass --resume (and one with --resume fails when no session is open). The installer always
    passes --no-reboot; whether a session is open is remembered in `marker` (the phone may also have
    been rebooted since), so on a handshake/session failure the same command is tried once the other
    way."""
    FAIL = ('handshake', 'Protocol initialisation failed', 'Failed to begin session', 'Failed to send')

    def __init__(self, runner, marker):
        self.r, self.marker = runner, marker

    @staticmethod
    def exe():
        """The project's own Heimdall build (v2.2.2, build/heimdall-src/build/bin/heimdall, in this checkout
        or the main one), else SBINST_HEIMDALL, else `heimdall` on PATH. Rehearsals (FAKE_ROOT set) always
        take the fake on PATH."""
        if os.environ.get('FAKE_ROOT'):
            return 'heimdall'
        if os.environ.get('SBINST_HEIMDALL'):
            return os.environ['SBINST_HEIMDALL']
        main = Path(str(REPO).split(os.sep + '.claude' + os.sep + 'worktrees' + os.sep)[0])
        for root in (REPO, main):
            p = root / 'build' / 'heimdall-src' / 'build' / 'bin' / 'heimdall'
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
        return 'heimdall'

    def run(self, args, write=False):
        if args[0] == 'flash' and (len(args) < 3 or args[1] != '--BOOT' or any(a.startswith('--') and a not in (
                '--BOOT', '--no-reboot') for a in args[1:])):
            raise Refused(f'heimdall flash {" ".join(args[1:2])}: the installer writes only BOOT in Download mode '
                          '(never RECOVERY or anything else)')
        resume = os.path.exists(self.marker)
        for attempt in (resume, not resume):
            argv = [self.exe(), args[0], *(['--resume'] if attempt else []), *args[1:]]
            res = self.r.run(argv, write=write, check=False, stream=False)
            if self.r.mode == 'dry-run' and write:
                return res
            text = f'{res.stdout}{res.stderr}'
            if res.returncode == 0:
                if '--no-reboot' in args:
                    os.makedirs(os.path.dirname(self.marker), exist_ok=True)
                    open(self.marker, 'w').write('open\n')
                return res
            if not any(f in text for f in self.FAIL):
                break
        raise Stopped(f'heimdall {" ".join(args[:1])} failed ({res.returncode}): {text.strip()[-600:]}')

    def closed(self):
        try:
            os.unlink(self.marker)
        except OSError:
            pass


ANDROID_PROPS = ('ro.product.model', 'ro.product.device', 'ro.boot.bootloader', 'ro.build.PDA',
                 'ro.build.version.oneui', 'ro.boot.flash.locked', 'ro.boot.verifiedbootstate', 'ro.lineage.version')


def android_props(phone):
    """getprop in a running Android (one call per name: plain `getprop NAME` works unrooted)."""
    return {k: phone.sh(f'getprop {k}', check=False) for k in ANDROID_PROPS}


def is_stock_samsung(props):
    """Samsung's own firmware (One UI) rather than Starbite's Android: ro.build.PDA / One UI version."""
    return bool(props.get('ro.build.PDA') or props.get('ro.build.version.oneui')) and not props.get('ro.lineage.version')


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
    lbs: int = None                                  # sda's logical block size (4096 on this UFS)

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


class InstallerEnv:
    """The Starbite installer environment (docs/INSTALLER.md 13): a Linux kernel + initramfs
    (tools/installer/env/) that the Starbite loader's fastboot starts from RAM (`fastboot boot KERNEL
    INITRD --dtb DTB --header-version 2 --cmdline ...`: the loader's Linux path, star_boot_ram). It
    brings up USB networking (CDC-ECM, the phone 172.16.42.1, the computer gets 172.16.42.2 by DHCP)
    and dropbear, which takes only the key this computer made: an ed25519 key under <state>/env/,
    whose public half goes into the initrd as an appended newc cpio (root/.ssh/authorized_keys), so
    the environment needs no password and nobody else's key works. Once it answers, the transport
    `adb` (tools/installer/transport/adb: adb's CLI on ssh) goes first on PATH, and every existing
    writer runs against it unchanged."""
    HOST = '172.16.42.1'
    TRANSPORT = INSTALLER_DIR / 'transport'

    def __init__(self, runner, d):
        self.r, self.dir = runner, Path(d)
        self.key = self.dir / 'id_ed25519'
        self.serial = ''

    def ssh_argv(self, cmd, timeout=5):
        return ['ssh', '-i', str(self.key), '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', '-o',
                'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', '-o', f'ConnectTimeout={timeout}',
                f'root@{self.HOST}', cmd]

    def reachable(self):
        if not self.key.is_file():
            return False
        try:
            res = self.r.run(self.ssh_argv('echo sbinst-env-ok', 3), check=False, quiet=True, timeout=20)
        except subprocess.TimeoutExpired:
            return False
        return res.returncode == 0 and 'sbinst-env-ok' in (res.stdout or '')

    def ensure_key(self):
        if self.key.is_file() and Path(str(self.key) + '.pub').is_file():
            return
        self.dir.mkdir(parents=True, exist_ok=True)
        res = self.r.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', 'starbite-installer', '-f',
                          str(self.key)], check=False)
        if res.returncode:
            raise Refused(f'ssh-keygen failed: {res.stderr.strip()[:200]} (it comes with macOS and every Linux)')

    @staticmethod
    def newc(entries):
        """A newc cpio archive: [(name, mode, data)] + TRAILER!!!."""
        out = bytearray()
        for ino, (name, mode, data) in enumerate(list(entries) + [('TRAILER!!!', 0, b'')], 1):
            nb = name.encode() + b'\0'
            hdr = '070701' + ''.join(f'{v:08x}' for v in (ino if name != 'TRAILER!!!' else 0, mode, 0, 0,
                                                          2 if mode & 0o040000 else 1, 0, len(data), 0, 0, 0, 0,
                                                          len(nb), 0))
            out += hdr.encode() + nb
            out += bytes(-len(out) % 4)
            out += data
            out += bytes(-len(out) % 4)
        return bytes(out)

    def initrd_with_key(self, initrd):
        """The bundle's initrd + (4-byte padding) + a cpio with this computer's public key. The kernel
        unpacks concatenated archives in order, so the key lands in the environment's /root/.ssh."""
        pub = Path(str(self.key) + '.pub').read_bytes()
        data = Path(initrd).read_bytes()
        data += bytes(-len(data) % 4)
        data += self.newc([('root', 0o040700, b''), ('root/.ssh', 0o040700, b''),
                           ('root/.ssh/authorized_keys', 0o100600, pub)])
        out = self.dir / f'initrd-with-key-{hashlib.sha256(data).hexdigest()[:12]}.img'
        out.write_bytes(data)
        return out

    def remote_serial(self):
        """The serial the environment was started with (sb.serial= on its command line, which the getprop
        stand-in reports as ro.boot.serialno)."""
        res = self.r.run(self.ssh_argv('getprop ro.boot.serialno', 3), check=False, quiet=True, timeout=20)
        return (res.stdout or '').strip() if res.returncode == 0 else ''

    def _fs(self):
        s = target_serial()
        return ['-s', s] if s else []

    def fastboot_info(self):
        vals = {}
        for v in ('serialno', 'product', 'version-bootloader'):
            res = self.r.run(['fastboot', *self._fs(), 'getvar', v], check=False)
            m = re.search(rf'{v}:\s*(\S*)', f'{res.stdout}{res.stderr}')
            vals[v] = m.group(1) if m else ''
        if not vals['version-bootloader'].startswith('starbite'):
            raise Refused(f'the fastboot device is not the Starbite loader ({vals["version-bootloader"] or "?"})')
        want = target_serial()
        if want and vals['serialno'] != want:
            raise Refused(f'the Starbite loader\'s fastboot reports serial {vals["serialno"] or "?"}, this run is for '
                          f'{want}: not the right phone')
        self.serial = vals['serialno']
        return vals['serialno'], vals['product']

    def boot(self, kernel, initrd, dtb, cmdline, serial, wait=180):
        self.ensure_key()
        ird = self.initrd_with_key(initrd)
        size = sum(Path(f).stat().st_size for f in (kernel, ird, dtb)) + 3 * 2048
        if size > 0x04ff0000:
            raise Refused(f'the installer environment is {size} bytes; the loader\'s download buffer holds 0x04ff0000')
        # a reboot of the environment (or a crash) comes back to the loader's fastboot, not the menu
        self.r.run(['fastboot', *self._fs(), 'oem', 'boot-next', 'fastboot'], check=False)
        self.r.run(['fastboot', *self._fs(), 'boot', str(kernel), str(ird), '--dtb', str(dtb), '--header-version', '2',
                    '--cmdline', f'{cmdline} sb.serial={serial or "unknown"}'])
        t0 = time.time()
        while time.time() - t0 < wait:
            if self.reachable():
                got = self.remote_serial()
                if serial and got != serial:
                    raise Refused(f'the environment that answers on {self.HOST} reports serial {got or "unknown"}, '
                                  f'not {serial} (the phone just started): another phone answers there. Unplug it, '
                                  'then run again')
                self.activate()
                return
            time.sleep(2)
        raise Refused('the installer environment did not answer on 172.16.42.1 within 3 minutes. Check that the '
                      'computer shows a new USB network interface (macOS: System Settings > Network; Linux: '
                      'ip link) with 172.16.42.2, then run again (the phone stays in the environment)')

    def activate(self):
        """From now on `adb` is the transport shim (ssh), for the installer and every tool it runs."""
        env = {'SBINST_ENV_UP': '1', 'SBINST_ENV_KEY': str(self.key), 'SBINST_ENV_HOST': self.HOST,
               'SBINST_ENV_SERIAL': self.serial or 'SBINST-ENV'}
        path = os.environ['PATH']
        if not path.startswith(str(self.TRANSPORT) + os.pathsep):
            path = f'{self.TRANSPORT}{os.pathsep}{path}'
        env['PATH'] = path
        os.environ.update(env)
        self.r.env.update(env)
