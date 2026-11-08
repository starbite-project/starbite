#!/usr/bin/env python3
"""Find files/folders by name on iOS 26's card root (TWRP + adb, read only; HFS_CARD=1 hfs-read).
    tools/ios26-find.py PATTERN [ROOT ...] [--depth N]     fnmatch pattern, e.g. '*.csstore'
Default roots: /private/var/containers/Shared/SystemGroup /private/var/mobile/Library, depth 5."""
import fnmatch
import importlib.util
import os
import sys
from pathlib import Path

os.environ['HFS_CARD'] = '1'
spec = importlib.util.spec_from_file_location('hfsread', Path(__file__).with_name('overlay') / 'hfs-read.py')
hr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hr)


def walk(vol, path, pat, depth):
    try:
        ents = vol.listdir(path)
    except SystemExit:
        return
    perms = dict(vol.perms)
    for name, kind, size in ents:
        full = f'{path.rstrip("/")}/{name}'
        if fnmatch.fnmatch(name, pat):
            uid, gid, mode = perms.get(name, (0, 0, 0))
            print(f'{kind:4} {mode & 0o7777:04o} {uid}:{gid} {size:>12} {full}')
        if kind == 'dir' and depth > 1:
            walk(vol, full, pat, depth - 1)


def main():
    args = [a for a in sys.argv[1:]]
    depth = 5
    if '--depth' in args:
        i = args.index('--depth')
        depth = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    pat, roots = args[0], args[1:] or ['/private/var/containers/Shared/SystemGroup', '/private/var/mobile/Library']
    vol = hr.Volume()
    for r in roots:
        walk(vol, r, pat, depth)


if __name__ == '__main__':
    main()
