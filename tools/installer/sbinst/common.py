"""Constants shared by the Starbite installer (docs/INSTALLER.md)."""
import os
from pathlib import Path

INSTALLER_DIR = Path(__file__).resolve().parent.parent          # tools/installer
REPO = INSTALLER_DIR.parent.parent                               # the checkout
TOOLS = REPO / 'tools'

MIB = 1 << 20
GIB = 1 << 30
CHUNK = 64 * MIB

# A card "sold as 64 GB" holds 61.9-64.1 GB (SanDisk: 124,735,488 sectors = 63.86 GB), so the
# user's "at least 64 GB" rule is checked as "at least 60,000,000,000 bytes". A 32 GB card has ~31 GB.
MIN_CARD_BYTES = 60_000_000_000
# Rescue stick: refuse anything larger. A stick sold as 256 GB has ~256.06e9 bytes, so the limit
# is 256 GiB (274.9e9): every 256 GB stick passes, every 512 GB drive and every SSD above fails.
MAX_RESCUE_DISK_BYTES = 256 * GIB

# Never written by the installer, in any mode (task rules; docs/STARBITE-MASTER-PLAN.md 5.8).
PROTECTED = ('EFS', 'CPEFS', 'PARAM', 'UP_PARAM', 'KEYSTORAGE', 'PERSISTENT', 'STEADY',
             'BOTA0', 'BOTA1', 'BOTA2', 'DTB', 'DTBO')
# Whole-disk regions that are protected too: the GPT (sda head + tail) and the boot LUNs.
PROTECTED_RAW = ('sda', 'sdb', 'sdc', 'sdd', 'sde')

# Partitions the installer may write, and the writer each one goes through.
#   part-write.py (tools/android/pixel/): SYSTEM VENDOR A16DATA (ODM through metadata-reset.sh)
#   the installer's own chunk writer: BOOT RECOVERY WINESP WINDOWS
PART_WRITE_PY = ('SYSTEM', 'VENDOR', 'A16DATA')
NATIVE_PARTS = ('BOOT', 'RECOVERY', 'WINESP', 'WINDOWS')
WRITABLE_PARTS = PART_WRITE_PY + NATIVE_PARTS + ('ODM',)

# Backed up (read only) before anything is written: the protected partitions, plus the small ones
# the install overwrites, plus what the master plan lists (RADIO, OMR, MISC). Missing optional
# ones are reported, missing required ones stop the install.
BACKUP_REQUIRED = PROTECTED + ('BOOT', 'RECOVERY')
BACKUP_OPTIONAL = ('MISC', 'RADIO', 'OMR', 'ODM')
# Raw regions: (name, device, how). 'head' = first 4 MiB (protective MBR, primary GPT, PIT area),
# 'tail' = last 5 blocks of 4 KiB (backup GPT), 'whole' = the whole LUN.
BACKUP_RAW = (('sda-head-4M', 'sda', 'head'), ('sda-tail-gpt', 'sda', 'tail'),
              ('sdb', 'sdb', 'whole'), ('sdc', 'sdc', 'whole'), ('sdd', 'sdd', 'whole'),
              ('sde', 'sde', 'whole'))

MODELS = ('SM-G960F',)            # Exynos S9; a Snapdragon S9 is another platform
DEVICES = ('starlte',)

DOWNLOAD_MODE_USB = ('04e8', '685d')   # Samsung Download mode (Odin protocol)
STARBITE_FASTBOOT_USB = ('18d1', '4ee0')


class Refused(Exception):
    """A clean refusal: shown as REFUSED, exit status 2, nothing written."""


class Stopped(Exception):
    """A step failed after writing started: shown as STOPPED, exit status 3; a re-run resumes."""


def default_state_dir():
    return Path(os.environ.get('SBINST_STATE', REPO / 'build' / 'installer'))


def human(n):
    n = float(n)
    for unit in ('B', 'KiB', 'MiB', 'GiB', 'TiB'):
        if n < 1024 or unit == 'TiB':
            return f'{n:.0f} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024
