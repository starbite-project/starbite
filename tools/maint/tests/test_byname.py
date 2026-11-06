"""SBMAINT's block paths (tools/maint/env/sb-byname), read-only flags (sb-protect) and reboots (sb-reboot),
run with /bin/sh against a fake sysfs built from S9 #1's real GPT backup (backup/20261102-185306/
sda-head-4M.img: read only; in this checkout or the main one, or $SBINST_GPT_BACKUP; skipped without it).
The by-name tree must give exactly the nodes the existing tools check (part-write.py's ALLOW,
preflight.sh, metadata-reset.sh), as TWRP does."""
import ast
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.realpath(__file__))
MAINT = os.path.dirname(HERE)
ENV = os.path.join(MAINT, 'env')
REPO = os.path.realpath(os.path.join(MAINT, '..', '..'))
MAIN = REPO.split(os.sep + '.claude' + os.sep + 'worktrees' + os.sep)[0]
sys.path.insert(0, os.path.join(REPO, 'tools', 'installer'))
from sbinst import ufslayout  # noqa: E402


def backup_head():
    for d in (os.environ.get('SBINST_GPT_BACKUP'), os.path.join(REPO, 'backup', '20261102-185306'),
              os.path.join(MAIN, 'backup', '20261102-185306')):
        if d and os.path.isfile(os.path.join(d, 'sda-head-4M.img')):
            return os.path.join(d, 'sda-head-4M.img')
    return None


def gpt_entries(path):
    """[(index, name, first, last)] from a 4 KiB-block GPT (an independent reader, not sbinst/gpt.py)."""
    with open(path, 'rb') as f:
        head = f.read(4 << 20)
    h = head[4096:4096 + 92]
    assert h[:8] == b'EFI PART'
    elba, n, esz = struct.unpack_from('<QII', h, 72)
    out = []
    for i in range(n):
        e = head[elba * 4096 + i * esz:elba * 4096 + (i + 1) * esz]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', e, 32)
        name = e[56:128].decode('utf-16le').split('\0')[0]
        out.append((i + 1, name, first, last))
    return out


HEAD = backup_head()


def sh(script_name, *args, env=None, check=True):
    p = subprocess.run(['/bin/sh', os.path.join(ENV, script_name), *args], capture_output=True, text=True,
                       env=dict(os.environ, **(env or {})))
    if check and p.returncode:
        raise AssertionError(f'{script_name} {args}: {p.returncode}\n{p.stdout}\n{p.stderr}')
    return p


class FakeSys:
    """R/sys/class/block/<name> -> R/sys/devices/... with dev, uevent, ro (like the kernel's)."""
    def __init__(self, root):
        self.r = root
        os.makedirs(os.path.join(root, 'sys', 'class', 'block'))

    def add(self, name, dev, parent_path, partname=None, partn=None):
        d = os.path.join(self.r, 'sys', 'devices', parent_path, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'dev'), 'w') as f:
            f.write(dev + '\n')
        with open(os.path.join(d, 'ro'), 'w') as f:
            f.write('0\n')
        ue = [f'MAJOR={dev.split(":")[0]}', f'MINOR={dev.split(":")[1]}', f'DEVNAME={name}',
              f'DEVTYPE={"partition" if partn else "disk"}']
        if partn:
            ue.append(f'PARTN={partn}')
        if partname:
            ue.append(f'PARTNAME={partname}')
        with open(os.path.join(d, 'uevent'), 'w') as f:
            f.write('\n'.join(ue) + '\n')
        link = os.path.join(self.r, 'sys', 'class', 'block', name)
        if os.path.lexists(link):
            os.unlink(link)
        os.symlink(os.path.relpath(d, os.path.dirname(link)), link)

    def remove(self, name):
        os.unlink(os.path.join(self.r, 'sys', 'class', 'block', name))


UFS = 'platform/11120000.ufs/host0/target0:0:0'
USB = 'platform/10c00000.usb/xhci-hcd.0.auto/usb1/1-1/1-1:1.0/host1/target1:0:0'


@unittest.skipIf(HEAD is None, 'S9 #1 GPT backup (backup/20261102-185306) not on this computer')
class ByName(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp(prefix='sbm-byname-')
        self.fs = FakeSys(self.t)
        self.parts = gpt_entries(HEAD)
        self.fs.add('sda', '8:0', UFS + '/0:0:0:0/block')
        for i, name, _, _ in self.parts:
            # sd: 16 minors per disk; partitions past 15 get the extended dev_t (blkext, major 259)
            self.fs.add(f'sda{i}', f'8:{i}' if i < 16 else f'259:{i - 16}', UFS + '/0:0:0:0/block/sda', name, i)
        self.fs.add('sdb', '8:16', UFS.replace('target0:0:0', 'target0:0:1') + '/0:0:0:1/block')
        self.fs.add('sdb1', '8:17', UFS.replace('target0:0:0', 'target0:0:1') + '/0:0:0:1/block/sdb', 'SBOOT_X', 1)
        self.fs.add('mmcblk0', '179:0', 'platform/11500000.mmc/mmc_host/mmc0/mmc0:aaaa/block')
        self.fs.add('mmcblk0p1', '179:1', 'platform/11500000.mmc/mmc_host/mmc0/mmc0:aaaa/block/mmcblk0', None, 1)
        self.fs.add('sdz', '65:144', USB + '/1:0:0:0/block')
        self.fs.add('sdz1', '65:145', USB + '/1:0:0:0/block/sdz', 'EFS', 1)      # a USB disk with a clashing name
        self.env = {'SB_SYSROOT': self.t, 'SB_FAKE_MKNOD': '1', 'SB_FAKE_BLOCKDEV': os.path.join(self.t, 'bd.log')}
        sh('sb-byname', env=self.env)

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def resolve(self, name, d=('by-name',)):
        """What `readlink -f /dev/block/by-name/NAME` gives on the phone (the links are relative, so they
        resolve the same under the fake root); it must be a node sb-byname made."""
        root = os.path.realpath(self.t)
        p = os.path.realpath(os.path.join(self.t, 'dev', 'block', *d, name))
        self.assertTrue(p.startswith(root + os.sep) and os.path.isfile(p), f'{name}: {p} is not a node')
        return p[len(root):]

    def node(self, n):
        with open(os.path.join(self.t, 'dev', 'block', n)) as f:
            return f.read().split()[1]

    def test_every_gpt_partition(self):
        self.assertGreaterEqual(len(self.parts), 28)
        for i, name, _, _ in self.parts:
            self.assertEqual(self.resolve(name), f'/dev/block/sda{i}', name)
            self.assertEqual(self.node(f'sda{i}'), f'8:{i}' if i < 16 else f'259:{i - 16}')
        self.assertEqual(self.resolve('EFS'), '/dev/block/sda3')               # the UFS one, not the USB disk's
        self.assertEqual(self.node('sda'), '8:0')
        self.assertEqual(self.node('mmcblk0p1'), '179:1')
        self.assertEqual(self.node('sdz1'), '65:145')
        alias = os.path.join(self.t, 'dev', 'block', 'platform', '11120000.ufs', 'by-name')
        self.assertEqual(os.readlink(alias), '../../by-name')               # TWRP's path (collect-logs.sh)
        self.assertEqual(self.resolve('MISC', ('platform', '11120000.ufs', 'by-name')), '/dev/block/sda15')

    def test_matches_the_layout_table(self):
        L = ufslayout.get('starbite-a')
        self.assertEqual([(i, n) for i, n, _, _ in self.parts], [(i, p.name) for i, p in enumerate(L.parts, 1)])

    def test_what_the_host_tools_check(self):
        src = open(os.path.join(REPO, 'tools', 'android', 'pixel', 'part-write.py')).read()
        allow = ast.literal_eval(re.search(r'^ALLOW = (\{.*\})$', src, re.M).group(1))
        for name, node in allow.items():
            self.assertEqual(self.resolve(name), f'/dev/block/{node}', f'part-write.py {name}')
        pre = open(os.path.join(REPO, 'tools', 'android', 'pixel', 'preflight.sh')).read()
        pairs = re.findall(r'([A-Z0-9_]+):(sda\d+)', re.search(r'for p in (SYSTEM:sda18.*?); do', pre).group(1))
        self.assertGreater(len(pairs), 8)
        for name, node in pairs:
            self.assertEqual(self.resolve(name), f'/dev/block/{node}', f'preflight.sh {name}')
        self.assertEqual(self.resolve('ODM'), '/dev/block/sda20')               # metadata-reset.sh

    def test_reread(self):
        last = self.parts[-1]
        self.fs.remove(f'sda{last[0]}')                                      # the partition is gone
        self.fs.add('sda18', '8:99', UFS + '/0:0:0:0/block/sda', 'SYSTEM', 18)   # another dev number
        sh('sb-byname', env=self.env)
        self.assertFalse(os.path.lexists(os.path.join(self.t, 'dev', 'block', f'sda{last[0]}')))
        self.assertFalse(os.path.lexists(os.path.join(self.t, 'dev', 'block', 'by-name', last[1])))
        self.assertEqual(self.node('sda18'), '8:99')
        self.assertEqual(self.resolve('SYSTEM'), '/dev/block/sda18')

    def protect_log(self):
        p = os.path.join(self.t, 'bd.log')
        with open(p) as f:
            out = f.read().split('\n')
        os.unlink(p)
        return [tuple(x.split()) for x in out if x]

    def test_protect(self):
        sh('sb-protect', 'apply', env=self.env)
        ro = {n for op, n in self.protect_log() if op == 'ro'}
        names = {name: f'sda{i}' for i, name, _, _ in self.parts}
        for p in ('EFS', 'PARAM', 'KEYSTORAGE', 'PERSISTENT', 'STEADY', 'RECOVERY', 'BOTA0', 'DTB', 'RADIO', 'MISC'):
            self.assertIn(p, ro, p)                     # by-name paths are logged by their link name
        for p in ('BOOT', 'SYSTEM', 'VENDOR', 'ODM', 'USERDATA', 'WINDOWS', 'A16DATA'):
            self.assertNotIn(p, ro, p)
            self.assertNotIn(names[p], ro, p)
        self.assertIn('sdb', ro)                        # the other UFS LUNs: S-Boot's
        self.assertIn('sdb1', ro)
        for n in ('sda', 'sdz', 'sdz1', 'mmcblk0', 'mmcblk0p1'):
            self.assertNotIn(n, ro, n)
        p = sh('sb-protect', 'allow', 'RECOVERY', env=self.env, check=False)
        self.assertEqual(p.returncode, 1)
        self.assertIn('REFUSED', p.stderr)
        sh('sb-protect', 'allow', 'EFS', env=self.env)
        self.assertEqual(self.protect_log(), [('rw', 'EFS')])
        sh('sb-protect', 'apply', env=self.env)
        self.assertNotIn('EFS', {n for op, n in self.protect_log() if op == 'ro'})

    def test_protect_all(self):
        sh('sb-protect', 'apply-all', env=self.env)
        ro = {n for op, n in self.protect_log() if op == 'ro'}
        self.assertTrue({'sda', 'sda18', 'sda28', 'sdb', 'sdb1'} <= ro)
        self.assertFalse({'sdz', 'sdz1'} & ro)

    def test_layout_proposal_on_the_real_gpt(self):
        """The SBMAINT proposal (ufslayout.PROPOSALS) planned by the installer's GPT engine on S9 #1's GPT:
        SBMAINT becomes sda29, nothing before it is renumbered, the protected rows stay byte-identical."""
        from sbinst import gpt
        d = os.path.dirname(HEAD)
        with open(HEAD, 'rb') as f:
            head = f.read()
        with open(os.path.join(d, 'sda-tail-gpt.img'), 'rb') as f:
            tail = f.read()
        p = gpt.plan(gpt.read(head, tail, 15616000), ufslayout.PROPOSALS['proposal-sbmaint'])
        new = {e.name: e for e in p.entries}
        sb = new['SBMAINT']
        self.assertEqual((sb.index, sb.first, sb.last, sb.attrs), (29, 15550208, 15615743, 1))
        self.assertEqual(sb.type, ufslayout.SBMAINT_TYPE)
        self.assertEqual(sb.blocks * 4096, 256 << 20)
        for i, name, _, _ in self.parts:
            self.assertEqual(new[name].index, i, name)
        self.assertEqual(sorted(p.fresh), ['A16DATA', 'SBMAINT', 'WINDOWS'])
        self.assertNotIn('proposal-sbmaint', ufslayout.TARGETS)
        self.assertNotIn('proposal-sbmaint', ufslayout.LAYOUTS)


class Reboot(unittest.TestCase):
    def writes(self, key):
        p = sh('sb-reboot', '-n', key)
        return [tuple(l.upper().replace('0X', '0x').split()[1::2]) for l in p.stdout.splitlines()
                if l.startswith('devmem')]

    def test_menu_clears_the_mailbox(self):
        self.assertEqual(self.writes('menu'), [('0xFED17FE0', '0'), ('0xFED17FE4', '0'), ('0x1406080C', '0x12345670')])

    def test_keys(self):
        w = self.writes('fastboot')
        self.assertEqual(w[0], ('0xFED17FE8', '0x74736166'))          # "fast"
        self.assertEqual(w[1], ('0xFED17FEC', '0x746F6F62'))          # "boot"
        self.assertEqual(w[2], ('0xFED17FF0', '0x00000000'))          # the key's NUL
        self.assertEqual(w[-3:], [('0xFED17FE0', '0x454E4253'), ('0xFED17FE4', '0x21325458'),   # "SBNEXT2!" last
                                  ('0x1406080C', '0x12345670')])
        w = self.writes('maint')
        self.assertEqual(w[0], ('0xFED17FE8', '0x6E69616D'))          # "main"
        self.assertEqual(w[1], ('0xFED17FEC', '0x00000074'))          # "t"
        self.assertEqual(self.writes('download')[-1], ('0x1406080C', '0x12345671'))

    def test_never_recovery(self):
        for k in ('recovery', 'twrp'):
            p = sh('sb-reboot', '-n', k, check=False)
            self.assertEqual(p.returncode, 1)
            self.assertIn('REFUSED', p.stderr)
        self.assertNotEqual(sh('sb-reboot', '-n', 'bad key!', check=False).returncode, 0)


class Scripts(unittest.TestCase):
    def test_posix_sh_syntax(self):
        for n in ('init', 'sb-byname', 'sb-protect', 'sb-reboot', 'sb-usb', 'sb-status', 'sb-info', 'sb-job', 'getprop'):
            p = subprocess.run(['/bin/sh', '-n', os.path.join(ENV, n)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, f'{n}: {p.stderr}')
        p = subprocess.run(['/bin/sh', '-n', os.path.join(REPO, 'tools', 'rescue', 'init')], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_never_recovery_anywhere(self):
        """No SBMAINT script writes INFORM3 0x12345674 (recovery) or names RECOVERY as a write target."""
        for n in os.listdir(ENV):
            text = open(os.path.join(ENV, n), errors='replace').read()
            self.assertNotIn('0x12345674', text, n)


if __name__ == '__main__':
    unittest.main()
