"""The user's rule (2026-11-06): the installer never writes RECOVERY (the phone keeps Samsung's stock
recovery) and never boots it. Every way a write could reach RECOVERY is checked here."""
import os
import re
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import common, manifest as mf, steps  # noqa: E402
from sbinst.common import INSTALLER_DIR, Refused  # noqa: E402
from sbinst.device import Heimdall  # noqa: E402


class NoRecovery(unittest.TestCase):
    def test_destination_refused(self):
        for d in ('part:RECOVERY', 'part:recovery'):
            with self.assertRaisesRegex(Refused, 'never writes RECOVERY|protected'):
                mf.parse_dest(d)
        self.assertNotIn('RECOVERY', common.WRITABLE_PARTS)
        self.assertNotIn('RECOVERY', common.NATIVE_PARTS)
        self.assertIn('RECOVERY', common.PROTECTED)

    def test_manifest_naming_recovery_refused(self):
        with tempfile.TemporaryDirectory() as t:
            Path(t, mf.MANIFEST_NAME).write_text(
                'format = 1\n[[item]]\nkey = "twrp"\nfile = "t.img"\nsize = 4096\nsha256 = "%s"\n'
                'dest = "part:RECOVERY"\n' % ('0' * 64))
            with self.assertRaises(Refused):
                mf.load(t)

    def test_no_planned_write_targets_recovery(self):
        """The example manifest and the rehearsal bundle's: no item goes to RECOVERY, and the plan's
        order (rank) has no place for it."""
        with tempfile.TemporaryDirectory() as t:
            Path(t, mf.MANIFEST_NAME).write_text((INSTALLER_DIR / 'installer-manifest.example.toml').read_text())
            m = mf.load(t)
        for it in m.items:
            self.assertNotEqual(str(it.dest).upper(), 'PART:RECOVERY', it.key)
            if it.dest.kind != 'env':
                steps.rank(it)
        with self.assertRaises(KeyError):
            steps.rank(types.SimpleNamespace(dest=mf.Dest('part', 'RECOVERY')))

    def test_heimdall_writes_only_boot(self):
        h = Heimdall(types.SimpleNamespace(mode='rehearse'), '/nonexistent/marker')
        for args in (['flash', '--RECOVERY', 'x.img', '--no-reboot'], ['flash', '--BOOT', 'x', '--RECOVERY', 'y'],
                     ['flash', '--SYSTEM', 'x']):
            with self.assertRaises(Refused):
                h.run(args, write=True)

    def test_transport_and_fakes_refuse_recovery(self):
        env = dict(os.environ, SBINST_ENV_UP='1', SBINST_ENV_KEY='/nonexistent')
        r = subprocess.run([sys.executable, str(INSTALLER_DIR / 'transport' / 'adb'), 'reboot', 'recovery'],
                           capture_output=True, text=True, env=env)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('never boots RECOVERY', r.stderr)

    def test_no_code_path_flashes_or_boots_recovery(self):
        """No `heimdall ... --RECOVERY`, `fastboot reboot recovery` or `flash:recovery` in the installer's
        code; `adb reboot recovery` only on the legacy TWRP transport (S9 #1's maintenance)."""
        for p in (INSTALLER_DIR / 'sbinst').glob('*.py'):
            text = p.read_text()
            self.assertNotRegex(text, r"\['flash', '--RECOVERY'", p.name)
            self.assertNotRegex(text, r"'fastboot', 'reboot', 'recovery'", p.name)
            for m in re.finditer(r"'reboot', 'recovery'", text):
                ctx = text[max(0, m.start() - 600):m.start()]
                self.assertTrue('legacy' in ctx or 'twrp' in ctx.lower(), f'{p.name}: reboot recovery outside the legacy path')


if __name__ == '__main__':
    unittest.main()
