#!/usr/bin/env python3
"""Read or edit MobileGestalt's answer cache in a mounted iPadOS 18 root.

  starlte-mg.py ROOT show            # decoded identity / screen answers
  starlte-mg.py ROOT clear           # delete the cache: rebuilt from the ADT at boot
  starlte-mg.py ROOT phone           # answer "iPhone": UIKit / SpringBoard phone idiom
  starlte-mg.py ROOT pad             # back to the iPad answers

ROOT is the volume mounted by `hdiutil attach` (no owners), e.g. /Volumes/IOSROOT,
of build/root-phone.img from tools/starlte-root-sync.py pull. The cache folder
is mode 000 + ACL (containermanagerd); it is opened for the edit and its mode
restored afterwards.

Cache format (libMobileGestalt, iPadOS 18.7.11 22H384): CacheData = 658 slots
x 8 bytes + 658 presence bytes; numbers live in their slot, objects (strings,
dictionaries, data) in CacheExtra under the obfuscated key
base64(md5("MGCopyAnswer" + name))[:22]. Slots from the library's perfect-hash
key table (entry +0x9a = slot, +0x9c = type): DeviceClassNumber 101 (sint32),
DeviceClass 100, ArtworkTraits 35, MainScreenCanvasSizes 428, ProductType 503.
UIKit's idiom = DeviceClassNumber (1 iPhone, 3 iPad).
"""
import base64
import hashlib
import os
import plistlib
import struct
import sys

SUB = 'private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library'
SLOTS = 658
SLOT = {'DeviceClassNumber': 101}


def ob(name):
    return base64.b64encode(hashlib.md5(b'MGCopyAnswer' + name.encode()).digest()).decode()[:22]


class Opened:
    """chmod 700 the two protected folders for the duration, then restore."""

    def __init__(self, root):
        self.dirs = [os.path.join(root, SUB), os.path.join(root, SUB, 'Caches')]
        self.modes = []

    def __enter__(self):
        for d in self.dirs:
            self.modes.append(os.stat(d).st_mode & 0o7777)
            os.chmod(d, 0o700)
        return os.path.join(self.dirs[1], 'com.apple.MobileGestalt.plist')

    def __exit__(self, *exc):
        for d, m in reversed(list(zip(self.dirs, self.modes))):
            os.chmod(d, m)


def show(d):
    data, extra = d['CacheData'], d['CacheExtra']
    print('CacheVersion', d.get('CacheVersion'), 'UUID', d.get('CacheUUID'))
    s = SLOT['DeviceClassNumber']
    val = struct.unpack_from('<i', data, s * 8)[0]
    print(f'DeviceClassNumber slot {s}: {val} (present {data[SLOTS * 8 + s]})')
    for name in ('DeviceClass', 'DeviceName', 'ProductType', 'ArtworkTraits', 'MainScreenCanvasSizes'):
        v = extra.get(ob(name))
        if isinstance(v, bytes) and len(v) % 8 == 0:
            v = struct.unpack(f'<{len(v) // 8}d', v)
        print(f'{name}: {v!r}'[:300])


def set_identity(d, phone):
    data = bytearray(d['CacheData'])
    if len(data) != SLOTS * 9:
        raise SystemExit(f'CacheData is {len(data)} bytes, expected {SLOTS * 9}: other iOS build?')
    s = SLOT['DeviceClassNumber']
    struct.pack_into('<q', data, s * 8, 1 if phone else 3)
    data[SLOTS * 8 + s] = 1
    d['CacheData'] = bytes(data)
    extra = d['CacheExtra']
    extra[ob('DeviceClass')] = 'iPhone' if phone else 'iPad'
    extra[ob('DeviceName')] = 'iPhone' if phone else 'iPad'
    traits = extra.get(ob('ArtworkTraits'))
    if isinstance(traits, dict):
        traits['ArtworkDeviceIdiom'] = 'phone' if phone else 'pad'


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ('show', 'clear', 'phone', 'pad'):
        raise SystemExit(__doc__)
    root, cmd = sys.argv[1], sys.argv[2]
    with Opened(root) as path:
        if not os.path.exists(path):
            print('no MobileGestalt cache (rebuilt at the next boot)')
            return
        if cmd == 'clear':
            os.remove(path)
            print('cache deleted')
            return
        with open(path, 'rb') as f:
            d = plistlib.load(f)
        show(d)
        if cmd in ('phone', 'pad'):
            set_identity(d, cmd == 'phone')
            with open(path, 'r+b') as f:          # same inode: owner, mode and ACL stay
                f.truncate(0)
                plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
            print('--- after')
            show(d)


if __name__ == '__main__':
    main()
