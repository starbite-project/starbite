#!/usr/bin/env python3
"""Find an app's data container(s) on iOS 26's card root by bundle ID and list its tree with owners and modes,
from TWRP over adb, read only (HFS_CARD=1 tools/overlay/hfs-read.py).
    tools/ios26-container.py com.apple.PosterBoard [DEPTH]      default depth 5
    tools/ios26-container.py --path /private/var/containers/Shared/SystemGroup [DEPTH]   any folder, with
                                 the owner and mode of every folder on the way down to it
Every container whose .com.apple.mobile_container_manager.metadata.plist names the bundle is listed: more
than one means containermanagerd made new ones (apps starting empty)."""
import importlib.util
import os
import plistlib
import sys
from pathlib import Path

os.environ['HFS_CARD'] = '1'
spec = importlib.util.spec_from_file_location('hfsread', Path(__file__).with_name('overlay') / 'hfs-read.py')
hr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hr)
BASES = ['/private/var/mobile/Containers/Data/Application', '/private/var/mobile/Containers/Data/PluginKitPlugin',
         '/private/var/mobile/Containers/Shared/AppGroup']
META = '.com.apple.mobile_container_manager.metadata.plist'


def tree(vol, path, depth, indent=''):
    try:
        ents = vol.listdir(path)
    except SystemExit as e:
        print(f'{indent}{e}')
        return
    perms = dict(vol.perms)
    for name, kind, size in ents:
        uid, gid, mode = perms.get(name, (0, 0, 0))
        print(f'{indent}{kind:4} {mode & 0o7777:04o} {uid}:{gid} {size:>10} {name}')
        if kind == 'dir' and depth > 1:
            tree(vol, f'{path}/{name}', depth - 1, indent + '    ')


def path_chain(vol, path):
    """owner/mode of each folder from / down to path (a non-searchable one blocks the user below it)"""
    parts = [p for p in path.split('/') if p]
    cur = ''
    for name in parts:
        try:
            ents = vol.listdir(cur or '/')
        except SystemExit as e:
            print(f'  {e}')
            return
        uid, gid, mode = vol.perms.get(name, (None, None, None))
        if uid is None:
            print(f'  {cur}/{name}: not found')
            return
        print(f'  {mode & 0o7777:04o} {uid}:{gid} {cur}/{name}')
        cur = f'{cur}/{name}'


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == '--path':
        vol = hr.Volume()
        print('the way down:')
        path_chain(vol, sys.argv[2])
        print('inside:')
        tree(vol, sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2, '  ')
        return
    want = sys.argv[1].lower()
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    vol = hr.Volume()
    found = []
    for base in BASES:
        try:
            uuids = [n for n, k, _ in vol.listdir(base) if k == 'dir']
        except SystemExit:
            continue
        for u in uuids:
            try:
                meta = plistlib.loads(vol.read_path(f'{base}/{u}/{META}'))
            except (SystemExit, Exception):
                continue
            ident = str(meta.get('MCMMetadataIdentifier', ''))
            if want in ident.lower():
                found.append((base, u, ident))
    print(f'{len(found)} container(s) for {sys.argv[1]}:')
    for base, u, ident in found:
        print(f'== {ident}: {base}/{u}')
        tree(vol, f'{base}/{u}', depth, '    ')


if __name__ == '__main__':
    main()
