#!/usr/bin/env python3
"""Pull Face Unlock's test frames from the phone (TWRP, read only) and unpack them.

    python3 tools/face/facedbg-pull.py [OUTDIR]          (default build/face/facedbg/<time>)
    python3 tools/face/facedbg-pull.py --from ENTRY.bin OUTDIR   (unpack an entry already pulled)

The face test image (starbite-next-*-facetest.img) saves its frames in the home store's volatile entry
"sbos-facedbg": kern/photos.c's format (32 slots of 2 MiB: a header block "SBPHOTO1", then 24-bit pixels). Per
capture it saves the raw frame's top half, its bottom half (1632 x 612 16-bit samples each, packed as 1632 x 408
pixels of three bytes) and the preview the phone showed (360 x 480). Out:
  NN-raw.bin        1632 x 1224 little-endian uint16 (RAW10, Bayer GRBG, black 64): the whole sensor frame
  NN-preview.ppm    the phone's preview (v2 pipeline, 360 x 480, as the face code saw it: not mirrored)
  frames.txt        slot, seq, sizes, the time
Finds the store on the card (p2) or UFS (USERDATA + 16 GiB) by its header. Needs adb in TWRP. Pulls only the
entry (64 MiB); the user's photos ("sbos-photos") are never read.
"""
import os, struct, subprocess, sys, time, zlib

BLOCK = 4096
DIR_BLOCK, DIR_BLOCKS = 3, 4
NAME = b'sbos-facedbg'
SLOTS, SLOT = 32, 2 << 20


def adb_dd(dev, skip, count):
    return subprocess.run(['adb', 'exec-out', f'dd if={dev} bs=4096 skip={skip} count={count} 2>/dev/null'],
                          check=True, capture_output=True).stdout


def find_entry():
    state = subprocess.run(['adb', 'get-state'], capture_output=True, text=True).stdout.strip()
    if state != 'recovery':
        sys.exit('the phone is not in TWRP (adb get-state: %r)' % state)
    cands = [('/dev/block/mmcblk0p2', 0)]
    ud = subprocess.run(['adb', 'shell', 'readlink -f /dev/block/by-name/USERDATA'], capture_output=True,
                        text=True).stdout.strip()
    if ud:
        cands.append((ud, 4194304))
    for dev, skip in cands:
        hdr = adb_dd(dev, skip + 1, 1)
        if hdr[:8] != b'SBSTORE2':
            continue
        n = struct.unpack_from('<I', hdr, 12)[0]
        d = adb_dd(dev, skip + DIR_BLOCK, DIR_BLOCKS)
        for i in range(n):
            name, blk, size, crc, typ, fl = struct.unpack_from('<32sQQIHH', d, 64 * i)
            if name.rstrip(b'\0') == NAME:
                return dev, skip + blk, size
        print('store on %s: no "%s" entry' % (dev, NAME.decode()))
    sys.exit('no store with a "sbos-facedbg" entry (make it: tools/store/store-update.sh --volatile sbos-facedbg=67108864)')


def unpack(data, out):
    os.makedirs(out, exist_ok=True)
    slots = []
    for k in range(min(SLOTS, len(data) // SLOT)):
        h = data[k * SLOT:k * SLOT + 64]
        if h[:8] != b'SBPHOTO1':
            continue
        ver, = struct.unpack_from('<I', h, 8)
        seq, w, hh, fmt, utc, dcrc, n = struct.unpack_from('<QIIIqII', h, 16)
        hc = bytearray(h); hc[12:16] = b'\0\0\0\0'
        if ver != 1 or seq == 0 or fmt != 1 or zlib.crc32(bytes(hc)) != struct.unpack_from('<I', h, 12)[0]:
            continue
        px = data[k * SLOT + BLOCK:k * SLOT + BLOCK + n]
        ok = zlib.crc32(px) == dcrc
        slots.append((seq, k, w, hh, utc, ok, px))
    slots.sort()
    lines, cap, half = [], 0, None
    for seq, k, w, h, utc, ok, px in slots:
        lines.append('slot %2d seq %d %dx%d utc %d %s' % (k, seq, w, h, utc, 'ok' if ok else 'CRC BAD'))
        if not ok:
            continue
        if (w, h) == (1632, 408):                      # a raw half
            if half is None:
                half = px
            else:
                with open(os.path.join(out, '%02d-raw.bin' % cap), 'wb') as f:
                    f.write(half + px)
                half = None
        elif (w, h) == (360, 480):
            with open(os.path.join(out, '%02d-preview.ppm' % cap), 'wb') as f:
                f.write(b'P6\n360 480\n255\n' + px)
            cap += 1
    with open(os.path.join(out, 'frames.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    print('%d captures unpacked into %s' % (cap, out))


def main():
    args = sys.argv[1:]
    if args[:1] == ['--from']:
        unpack(open(args[1], 'rb').read(), args[2])
        return
    out = args[0] if args else os.path.join('build', 'face', 'facedbg', time.strftime('%Y%m%d-%H%M%S'))
    dev, blk, size = find_entry()
    print('sbos-facedbg: %s block %d, %d MiB' % (dev, blk, size >> 20))
    data = adb_dd(dev, blk, size // BLOCK)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'entry.bin'), 'wb') as f:
        f.write(data)
    unpack(data, out)


if __name__ == '__main__':
    main()
