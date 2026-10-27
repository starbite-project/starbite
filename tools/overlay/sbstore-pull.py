#!/usr/bin/env python3
"""Read the Starbite OS store back from the phone (TWRP, USERDATA +16 GiB)
into DIR/<entry name>, each checked against its CRC32, so the store can be
repacked with tools/linux/sbstore.py without the original inputs.
    sbstore-pull.py DIR
"""
import struct
import subprocess
import sys
import zlib
from pathlib import Path

STORE = 4194304         # 16 GiB in 4 KiB blocks


def rd(dev, block, count):
    return subprocess.run(['adb', 'exec-out', f'dd if={dev} bs=4096 skip={block} count={count} 2>/dev/null'],
                          capture_output=True, check=True).stdout


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    dev = subprocess.run(['adb', 'shell', 'readlink -f /dev/block/by-name/USERDATA'],
                         capture_output=True, text=True, check=True).stdout.strip()
    hdr = rd(dev, STORE, 1)
    magic, ver, count, size = struct.unpack_from('<8sIIQ', hdr, 0)
    if magic != b'SBSTORE1':
        sys.exit('no store on the phone')
    print(f'store: {count} entries, {size} blocks')
    for i in range(count):
        name, off, length, crc = struct.unpack_from('<24sQQI', hdr, 64 + 64 * i)
        name = name.rstrip(b'\0').decode()
        blob = rd(dev, STORE + off // 4096, (length + 4095) // 4096)[:length]
        ok = zlib.crc32(blob) == crc
        (out / name).write_bytes(blob)
        print(f'  {name}: {length} bytes crc {crc:08x} {"ok" if ok else "MISMATCH"}')
        if not ok:
            sys.exit(1)


if __name__ == '__main__':
    main()
