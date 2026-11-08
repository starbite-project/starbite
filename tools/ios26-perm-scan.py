#!/usr/bin/env python3
"""Find folders/files on iOS 26's card root that no user can use: mode 0000 or owner 4294967294 (-2, left by
a Mac edit with owners off), two levels into every system group / app group / app data container. Read only,
TWRP + adb (HFS_CARD=1 hfs-read). Prints one line per hit; --ops prints sboverlay attr: lines that give
folders 0755 and files 0644 with the container's own owner (the owner of its .com.apple... metadata plist,
else 501:501)."""
import importlib.util
import os
import sys
from pathlib import Path

os.environ['HFS_CARD'] = '1'
spec = importlib.util.spec_from_file_location('hfsread', Path(__file__).with_name('overlay') / 'hfs-read.py')
hr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hr)
BASES = ['/private/var/containers/Shared/SystemGroup', '/private/var/mobile/Containers/Shared/AppGroup',
         '/private/var/mobile/Containers/Data/Application',
         # 2026-11-08 T6: lsd's database lives in a System data container; app extensions have their own
         '/private/var/containers/Data/System', '/private/var/containers/Data/InternalDaemon',
         '/private/var/mobile/Containers/Data/PluginKitPlugin', '/private/var/mobile/Containers/Data/InternalDaemon']
META = '.com.apple.mobile_container_manager.metadata.plist'
BAD_UID = 4294967294


def bad(uid, gid, mode, kind):
    return uid == BAD_UID or gid == BAD_UID or (kind == 'dir' and mode & 0o700 == 0)


def main():
    ops = '--ops' in sys.argv
    vol = hr.Volume()
    hits = 0
    for base in BASES:
        try:
            conts = [n for n, k, _ in vol.listdir(base) if k == 'dir']
        except SystemExit:
            continue
        for c in conts:
            root = f'{base}/{c}'
            try:
                ents = vol.listdir(root)
            except SystemExit:
                continue
            perms = dict(vol.perms)
            owner = perms.get(META, (501, 501, 0))[:2]
            if BAD_UID in owner:
                owner = (501, 501)
            todo = [(root, n, k) for n, k, _ in ents]
            for parent, name, kind in todo:
                uid, gid, mode = perms.get(name, (0, 0, 0o755))
                if bad(uid, gid, mode, kind):
                    hits += 1
                    path = f'{parent}/{name}'
                    if ops:
                        m = '755' if kind == 'dir' else '644'
                        print(f'attr:{path}:{m}:{owner[0]}:{owner[1]}')
                    else:
                        print(f'{mode & 0o7777:04o} {uid}:{gid} {kind} {path}')
            # one level more under Library
            if any(n == 'Library' and k == 'dir' for n, k, _ in ents):
                try:
                    sub = vol.listdir(f'{root}/Library')
                except SystemExit:
                    continue
                sp = dict(vol.perms)
                for n, k, _ in sub:
                    uid, gid, mode = sp.get(n, (0, 0, 0o755))
                    if bad(uid, gid, mode, k):
                        hits += 1
                        path = f'{root}/Library/{n}'
                        if ops:
                            print(f'attr:{path}:{"755" if k == "dir" else "644"}:{owner[0]}:{owner[1]}')
                        else:
                            print(f'{mode & 0o7777:04o} {uid}:{gid} {k} {path}')
    print(f'# {hits} hits', file=sys.stderr)


if __name__ == '__main__':
    main()
