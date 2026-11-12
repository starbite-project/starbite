#!/usr/bin/env python3
"""Copy one file OUT of the macOS root on the S9's microSD card, from TWRP over adb, read-only (2026-11-13: the
companion of card-file-replace.py; the root's /System/Library/xpc/launchd.plist had to be edited in place while the
Mac had no copy of the card's root).

  tools/macos/card-file-get.py ROOT_PATH OUT [--start 182888448]

Same checks as card-file-replace.py (TWRP, the 128 GB card, an HFS+/HFSX volume with 4 KiB blocks at --start, the
file's catalog record, not HFS-compressed). Writes OUT and prints its size and sha256, the --expect-old value for a
later card-file-replace.py of the same path. Nothing on the phone is written.
"""
import argparse
import hashlib
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CARD_SECTORS = 249999360


def adb(*args, **kw):
    return subprocess.run(['adb', *args], capture_output=True, check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path')
    ap.add_argument('out')
    ap.add_argument('--start', type=int, default=182888448)
    a = ap.parse_args()

    if adb('get-state').stdout.decode().strip() != 'recovery':
        sys.exit('phone not in TWRP')
    size = adb('shell', 'cat /sys/block/mmcblk0/size').stdout.decode().strip()
    if size != str(CARD_SECTORS):
        sys.exit(f'card has {size} sectors, not the 128 GB card ({CARD_SECTORS})')

    os.environ['HFS_CARD'] = '1'
    spec = importlib.util.spec_from_file_location('hfsread', ROOT / 'tools/overlay/hfs-read.py')
    hr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hr)
    if hr.BASE != a.start * 512:
        hr.BASE = a.start * 512
    vol = hr.Volume()
    if vol.block != 4096:
        sys.exit(f'volume block size {vol.block}, not 4096')
    rec = vol.file_record(a.path)
    fid = int.from_bytes(rec[8:12], 'big')
    if rec[41] & 0x20:
        sys.exit(f'{a.path}: HFS-compressed: not readable (or replaceable) this way')
    logical, extents = vol.fork(rec, 88, fid)
    data = vol.fork_read((logical, extents), 0, logical)
    Path(a.out).write_bytes(data)
    print(f'{a.path}: file id {fid}, {logical} bytes in {len(extents)} extent(s) -> {a.out}')
    print(f'sha256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
