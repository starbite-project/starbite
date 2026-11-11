#!/usr/bin/env python3
"""Mark a SWEETXNU payload as version 2: its kernel takes the 1024-byte boot_args (xnu-11417, xnu-12377).
    tools/payload-v2.py out/xnu-NAME.xnu [--check]
Version 2 has the same header and segments as version 1 (ref/ios-herolte/tools/mkpayload.py); only the
version word at +8 changes. A loader with XNU_LINE_MAX (shim/starlte/xnu_boot.c, 2026-11-11) then writes
boot_args with CommandLine[1024], bootFlags at +1136 and memSizeActual at +1144, and lets the boot-args it
builds (payload line + catalog line + log fragments + sbwdt=) grow to 1023 bytes. Older loaders refuse a
version-2 payload ("BAD SWEETXNU PAYLOAD"): flash the loader first. --check only prints the version."""
import struct
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    path = args[0]
    with open(path, 'r+b') as f:
        head = f.read(16)
        if head[:8] != b'SWEETXNU':
            sys.exit(f'{path}: not a SWEETXNU payload')
        version, nsegs = struct.unpack_from('<II', head, 8)
        if '--check' in sys.argv:
            print(f'{path}: version {version}, {nsegs} segments')
            return
        if version == 2:
            print(f'{path}: already version 2')
            return
        if version != 1:
            sys.exit(f'{path}: version {version}, expected 1')
        f.seek(8)
        f.write(struct.pack('<I', 2))
    print(f'{path}: version 1 -> 2 (1024-byte boot_args)')


if __name__ == '__main__':
    main()
