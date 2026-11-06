"""The adb transport (tools/maint/transport/adb -> tools/installer/transport/adb) against a fake ssh: a
fake phone root where every command runs with the phone's paths redirected (as the installer's fakes do).
Covers: transparent hand-off to the real adb by default, shell/exec-out (exit codes, 8-bit clean data,
stdin), push/pull of files and directories, get-state, the maintenance-environment guard, the pinned host
key, the job line, reboots (never recovery), and two unchanged host tools (part-write.py, part-backup.sh)
writing and backing up a partition through SB_TRANSPORT=maint."""
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.realpath(__file__))
MAINT = os.path.dirname(HERE)
REPO = os.path.realpath(os.path.join(MAINT, '..', '..'))
SHIM = os.path.join(MAINT, 'transport', 'adb')

FAKE_SSH = r'''#!/usr/bin/env python3
import json, os, re, subprocess, sys
R = os.environ['FAKE_PHONE']
a = sys.argv[1:]
with open(os.path.join(R, 'ssh.log'), 'a') as f:
    f.write(json.dumps(a) + '\n')
if os.path.exists(os.path.join(R, 'DOWN')):
    sys.stderr.write('ssh: connect to host 172.16.42.1 port 22: Connection refused\n')
    sys.exit(255)
i = 0
while i < len(a) and a[i].startswith('-'):
    i += 2 if a[i] in ('-i', '-o', '-p', '-l', '-F') else 1
host, cmd = a[i], ' '.join(a[i + 1:])
assert host == 'root@172.16.42.1', host
FUNCS = """
sb-job() { echo "$*" >> "$R/job.log"; }
sb-reboot() { echo "$*" >> "$R/reboot.log"; }
blockdev() { case $1 in --getsize64) wc -c < "$2" | tr -d ' ' ;; esac; }
"""
tok = '\x01R\x01'
s = cmd.replace(R, tok)
s = re.sub(r'(?<![\w./-])(/dev/block/|/tmp/|/run/|/proc/mounts|/etc/sbmaint-release|/external_sd)',
           lambda m: tok + m.group(1), s).replace(tok, R)
p = subprocess.run(['/bin/bash', '-c', FUNCS + s], env=dict(os.environ, R=R), stdin=sys.stdin,
                   capture_output=True)
sys.stdout.buffer.write(p.stdout.replace(R.encode(), b''))
sys.stderr.buffer.write(p.stderr.replace(R.encode(), b''))
sys.exit(p.returncode)
'''
REAL_ADB = '#!/bin/sh\necho "REAL-ADB $*"\n'


class Transport(unittest.TestCase):
    def setUp(self):
        self.t = os.path.realpath(tempfile.mkdtemp(prefix='sbm-tr-'))      # macOS: /var -> /private/var
        self.R = os.path.join(self.t, 'phone')
        for d in ('dev/block/by-name', 'tmp', 'run/sbmaint', 'etc', 'proc'):
            os.makedirs(os.path.join(self.R, d))
        open(os.path.join(self.R, 'etc', 'sbmaint-release'), 'w').write('sbmaint test\n')
        open(os.path.join(self.R, 'proc', 'mounts'), 'w').close()
        self.bin = os.path.join(self.t, 'bin')
        self.real = os.path.join(self.t, 'real')
        os.makedirs(self.bin)
        os.makedirs(self.real)
        for path, text in ((os.path.join(self.bin, 'ssh'), FAKE_SSH), (os.path.join(self.real, 'adb'), REAL_ADB)):
            with open(path, 'w') as f:
                f.write(text)
            os.chmod(path, 0o755)
        self.base = dict(os.environ, FAKE_PHONE=self.R, SB_MAINT_KEY=os.path.join(self.t, 'nokey'),
                         SB_MAINT_KNOWN_HOSTS=os.path.join(self.t, 'no-known-hosts'),
                         PATH=os.pathsep.join([self.bin, self.real, '/usr/bin', '/bin', '/usr/sbin', '/sbin']))
        self.base.pop('SB_TRANSPORT', None)
        self.base.pop('SBINST_ENV_UP', None)

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def adb(self, *args, maint=True, input=None, env=None, text=True):
        e = dict(self.base, **(env or {}))
        if maint:
            e['SB_TRANSPORT'] = 'maint'
        return subprocess.run([SHIM, *args], env=e, capture_output=True, input=input, text=text)

    def ssh_calls(self):
        with open(os.path.join(self.R, 'ssh.log')) as f:
            return [json.loads(l) for l in f]

    def part(self, name, node, size, fill=b'\xff'):
        p = os.path.join(self.R, 'dev', 'block', node)
        with open(p, 'wb') as f:
            f.write(fill * size)
        os.symlink(f'../{node}', os.path.join(self.R, 'dev', 'block', 'by-name', name))
        return p

    # ---- the mode switch ---------------------------------------------------------------------------
    def test_default_is_the_real_adb(self):
        p = self.adb('devices', maint=False)
        self.assertEqual(p.stdout.strip(), 'REAL-ADB devices')
        # the shim on PATH twice and through the tools/installer path: still no loop
        e = {'PATH': os.pathsep.join([os.path.dirname(SHIM), os.path.join(REPO, 'tools', 'installer', 'transport'),
                                      os.path.dirname(SHIM), self.base['PATH']])}
        p = subprocess.run(['adb', 'shell', 'id'], env=dict(self.base, **e), capture_output=True, text=True, timeout=20)
        self.assertEqual(p.stdout.strip(), 'REAL-ADB shell id')
        p = self.adb('get-state', maint=False, env={'SB_TRANSPORT': 'twrp'})
        self.assertEqual(p.stdout.strip(), 'REAL-ADB get-state')

    # ---- shell -------------------------------------------------------------------------------------
    def test_shell(self):
        p = self.adb('shell', 'echo', 'hello;', 'echo', '$((6*7))')
        self.assertEqual(p.stdout, 'hello\n42\n')
        self.assertEqual(self.adb('shell', 'exit 3').returncode, 3)
        p = self.adb('shell', 'cat', input='from the computer')
        self.assertEqual(p.stdout, 'from the computer')
        data = bytes(range(256)) * 300
        with open(os.path.join(self.R, 'tmp', 'bin'), 'wb') as f:
            f.write(data)
        p = self.adb('exec-out', 'cat /tmp/bin', text=False)
        self.assertEqual(p.stdout, data)
        p = self.adb('exec-in', 'cat > /tmp/in.bin', input=data, text=False)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(open(os.path.join(self.R, 'tmp', 'in.bin'), 'rb').read(), data)

    def test_guard_and_job(self):
        self.adb('shell', 'true', env={'SB_JOB': 'store-flash x.img'})
        with open(os.path.join(self.R, 'job.log')) as f:
            self.assertEqual(f.read(), '-q store-flash x.img: true\n')      # quiet: the screen, not the log
        os.unlink(os.path.join(self.R, 'etc', 'sbmaint-release'))       # e.g. a distro on 172.16.42.1
        p = self.adb('shell', f'touch /tmp/written')
        self.assertEqual(p.returncode, 97)
        self.assertIn('not the Starbite maintenance environment', p.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.R, 'tmp', 'written')))

    def test_host_key_pin(self):
        kh = os.path.join(self.t, 'known_hosts')
        open(kh, 'w').write('sbmaint,172.16.42.1 ssh-ed25519 AAAAtest\n')
        self.adb('shell', 'true', env={'SB_MAINT_KNOWN_HOSTS': kh})
        argv = self.ssh_calls()[-1]
        self.assertIn('StrictHostKeyChecking=yes', argv)
        self.assertIn(f'UserKnownHostsFile={kh}', argv)
        self.assertIn('HostKeyAlias=sbmaint', argv)
        self.adb('shell', 'true')                                         # no pin file: not checked
        self.assertIn('StrictHostKeyChecking=no', self.ssh_calls()[-1])

    # ---- files ---------------------------------------------------------------------------------------
    def test_push_pull(self):
        src = os.path.join(self.t, 'f.bin')
        data = os.urandom(200000)
        open(src, 'wb').write(data)
        self.assertEqual(self.adb('push', src, '/tmp/f.bin').returncode, 0)
        self.assertEqual(open(os.path.join(self.R, 'tmp', 'f.bin'), 'rb').read(), data)
        self.assertEqual(self.adb('push', src, src, '/tmp/d/').returncode, 0)
        self.assertTrue(os.path.isfile(os.path.join(self.R, 'tmp', 'd', 'f.bin')))
        out = os.path.join(self.t, 'back.bin')
        self.assertEqual(self.adb('pull', '/tmp/f.bin', out).returncode, 0)
        self.assertEqual(open(out, 'rb').read(), data)
        self.assertNotEqual(self.adb('pull', '/tmp/missing', out + '2').returncode, 0)
        self.assertFalse(os.path.exists(out + '2'))
        # directories, both ways
        tree = os.path.join(self.t, 'tree')
        os.makedirs(os.path.join(tree, 'sub'))
        open(os.path.join(tree, 'sub', 'a.txt'), 'w').write('A')
        self.assertEqual(self.adb('push', tree, '/run/sbmaint/tree').returncode, 0)
        self.assertEqual(open(os.path.join(self.R, 'run', 'sbmaint', 'tree', 'sub', 'a.txt')).read(), 'A')
        got = os.path.join(self.t, 'got')
        p = self.adb('pull', '/run/sbmaint/tree', got)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(open(os.path.join(got, 'sub', 'a.txt')).read(), 'A')

    # ---- state and reboots ----------------------------------------------------------------------------
    def test_state(self):
        self.assertEqual(self.adb('get-state').stdout.strip(), 'recovery')
        self.assertIn('\trecovery', self.adb('devices').stdout)
        open(os.path.join(self.R, 'DOWN'), 'w').close()
        p = self.adb('get-state')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('no devices', p.stderr)
        self.assertNotIn('recovery', self.adb('devices').stdout)

    def test_reboots(self):
        for args, want in ((('reboot',), 'menu'), (('reboot', 'bootloader'), 'fastboot'), (('reboot', 'download'), 'download')):
            self.assertEqual(self.adb(*args).returncode, 0)
            with open(os.path.join(self.R, 'reboot.log')) as f:
                self.assertEqual(f.read().split()[-1], want)
        for env in ({}, {'SBINST_ENV_UP': '1', 'SBINST_ENV_KEY': '/nonexistent'}):
            p = self.adb('reboot', 'recovery', maint=not env, env=env)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn('never boots RECOVERY', p.stderr)

    # ---- unchanged host tools through SB_TRANSPORT=maint ----------------------------------------------
    def tool_env(self):
        e = dict(self.base, SB_TRANSPORT='maint', BACKUP_LOG=os.path.join(self.t, 'backups.txt'))
        return e                                   # no shim on PATH: the tools' own preamble selects it

    def test_part_write_and_backup(self):
        node = self.part('SYSTEM', 'sda18', 72 << 20)
        img = os.path.join(self.t, 'system.img')
        payload = bytes(64 << 20) + os.urandom(4 << 20)     # chunk 0 all zeros (written from /dev/zero), chunk 1 pushed
        open(img, 'wb').write(payload)
        state = os.path.join(self.t, 'pw.state')
        tool = os.path.join(REPO, 'tools', 'android', 'pixel', 'part-write.py')
        p = subprocess.run([sys.executable, tool, 'SYSTEM', img, '--dry-run'], env=self.tool_env(),
                           capture_output=True, text=True, cwd=self.t)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('SYSTEM = /dev/block/sda18', p.stdout)
        self.assertIn('dry run: nothing written', p.stdout)
        p = subprocess.run([sys.executable, tool, 'SYSTEM', img, '--state', state], env=self.tool_env(),
                           capture_output=True, text=True, cwd=self.t)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('written and verified', p.stdout)
        part = open(node, 'rb').read()
        self.assertEqual(part[:len(payload)], payload)
        self.assertEqual(part[len(payload):], b'\xff' * (4 << 20))   # the tail left as it was
        # the job line named the tool
        self.assertIn('part-write.py: ', open(os.path.join(self.R, 'job.log')).read())
        # part-backup.sh mac: the partition back on the computer, verified by the tool itself
        out = os.path.join(self.t, 'bk')
        p = subprocess.run(['bash', os.path.join(REPO, 'tools', 'android', 'pixel', 'part-backup.sh'), 'mac', 'SYSTEM', out],
                           env=self.tool_env(), capture_output=True, text=True, cwd=self.t)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        with gzip.open(os.path.join(out, 'SYSTEM.img.gz')) as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), hashlib.sha256(part).hexdigest())
        self.assertIn('SYSTEM.img (raw; stored gzip\'d, mac:', open(os.path.join(self.t, 'backups.txt')).read())

    def test_part_write_refuses_a_wrong_node(self):
        self.part('SYSTEM', 'sda17', 1 << 20)
        p = subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'android', 'pixel', 'part-write.py'), 'SYSTEM',
                            os.devnull, '--size', '4096', '--dry-run'], env=self.tool_env(), capture_output=True,
                           text=True, cwd=self.t)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('expected /dev/block/sda18', p.stdout + p.stderr)


if __name__ == '__main__':
    unittest.main()
