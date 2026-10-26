#!/usr/bin/env python3
"""Swap modules inside the release's compressed DXE volume, then splice it with our SEC/PEI.

  dxe-replace.py OUR_FD RELEASE_FD OUT_FD --kill GUID [--kill GUID ...] [--add FILE.ffs ...]
                 [--extract GUID OUT.ffs]

The release DXE volume is the last file of the release FD's outer FV: one GUID-defined
LZMA section holding a 4 MiB inner FV. Every --kill file becomes an FFS pad file in place
(type 0xF0, header checksum fixed), so no other module moves and every alignment holds.
Every --add file is appended in the inner FV's free space at its required data alignment
(a pad file fills the gap). The sections are recompressed with edk2's LzmaCompress (lima
VM, same format the DXE core decodes), the outer FFS file is rebuilt, and the result is
spliced behind OUR_FD's SEC/PEI exactly like fv-splice.py.
"""
import argparse
import lzma
import os
import struct
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location('fv_splice', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fv-splice.py'))
fv_splice = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fv_splice)

LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf')
PAD = 0xF0


def ffs_files(fv, start):
    flen = struct.unpack_from('<Q', fv, start + 32)[0]
    hl = struct.unpack_from('<H', fv, start + 48)[0]
    ext = struct.unpack_from('<H', fv, start + 52)[0]
    off = start + hl
    if ext:
        off = (start + ext + struct.unpack_from('<I', fv, start + ext + 16)[0] + 7) & ~7
    out = []
    while off + 24 <= start + flen:
        name = fv[off:off + 16]
        if name == b'\xff' * 16:
            break
        a = fv[off + 19]
        sz = int.from_bytes(fv[off + 20:off + 23], 'little')
        h = 24
        if a & 1:
            sz = struct.unpack_from('<Q', fv, off + 24)[0]
            h = 32
        out.append(dict(off=off, size=sz, type=fv[off + 18], attr=a, guid=uuid.UUID(bytes_le=bytes(name)), hdr=h))
        off = (off + sz + 7) & ~7
    return out, start + flen, off


def header_checksum(buf, off, hdr):
    h = bytearray(buf[off:off + hdr])
    h[16] = 0
    h[17] = 0
    h[23] = 0
    return (-sum(h)) & 0xFF


def data_alignment(attr):
    # FFS_ATTRIB_DATA_ALIGNMENT bits 3..5 (+ FFS_ATTRIB_DATA_ALIGNMENT_2 bit 1 for >64K, unsupported here)
    assert not (attr & 0x02), 'DATA_ALIGNMENT_2 not supported'
    return [1, 16, 128, 512, 1024, 4096, 32768, 65536][(attr >> 3) & 7]


def pad_file(size, state):
    assert size >= 24 and size < 1 << 24
    f = bytearray(b'\0' * 16) + bytes([0, 0xAA, PAD, 0x00]) + size.to_bytes(3, 'little') + bytes([state])
    f[16] = header_checksum(f, 0, 24)
    return bytes(f) + b'\xff' * (size - 24)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('our_fd')
    ap.add_argument('release_fd')
    ap.add_argument('out_fd')
    ap.add_argument('--kill', action='append', default=[])
    ap.add_argument('--add', action='append', default=[])
    ap.add_argument('--extract', nargs=2, metavar=('GUID', 'OUT'))
    args = ap.parse_args()

    rel = open(args.release_fd, 'rb').read()
    outer, _, _ = ffs_files(rel, 0)
    dxe = outer[-1]
    assert dxe['type'] == 0x0B
    sec = dxe['off'] + dxe['hdr']
    ssz = int.from_bytes(rel[sec:sec + 3], 'little')
    assert rel[sec + 3] == 0x02 and uuid.UUID(bytes_le=rel[sec + 4:sec + 20]) == LZMA_GUID
    doff, gattr = struct.unpack_from('<HH', rel, sec + 20)
    raw = bytearray(lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(rel[sec + doff:sec + ssz]))
    # Find the FV image section.
    p = 0
    fvoff = None
    while p < len(raw):
        ln = int.from_bytes(raw[p:p + 3], 'little')
        if raw[p + 3] == 0x17:
            fvoff = p + 4
            fvsec = p
        p = (p + ln + 3) & ~3
    files, fvend, free = ffs_files(raw, fvoff)
    state = raw[files[0]['off'] + 23]

    if args.extract:
        g = uuid.UUID(args.extract[0])
        f = next(f for f in files if f['guid'] == g)
        open(args.extract[1], 'wb').write(raw[f['off']:f['off'] + f['size']])
        print(f'extracted {g} {f["size"]:#x} bytes, attr {f["attr"]:#x}')
        return

    for k in args.kill:
        g = uuid.UUID(k)
        f = next(f for f in files if f['guid'] == g and f['type'] != PAD)
        raw[f['off'] + 18] = PAD
        raw[f['off'] + 16] = header_checksum(raw, f['off'], f['hdr'])
        print(f'killed {g} at {f["off"] - fvoff:#x} ({f["size"]:#x} bytes) -> pad file')

    for path in args.add:
        new = open(path, 'rb').read()
        attr = new[19]
        hdr = 32 if attr & 1 else 24
        align = data_alignment(attr)
        pos = (free + 7) & ~7
        need = (-(pos - fvoff + hdr)) % align
        if need:
            if need < 24:
                need += align
            raw[pos:pos + need] = pad_file(need, state)
            pos += need
        assert pos + len(new) <= fvend, 'inner FV full'
        nb = bytearray(new)
        nb[23] = state
        raw[pos:pos + len(nb)] = nb
        print(f'added {uuid.UUID(bytes_le=new[:16])} at {pos - fvoff:#x} ({len(new):#x} bytes, align {align})')
        free = pos + len(nb)

    # Verify the rebuilt inner FV parses and every file header checksum holds.
    files2, _, _ = ffs_files(raw, fvoff)
    for f in files2:
        assert header_checksum(raw, f['off'], f['hdr']) == raw[f['off'] + 16], f
    print(f'inner FV: {len(files2)} files, ok')

    with tempfile.TemporaryDirectory(dir=os.path.expanduser('~/')) as td:
        src = os.path.join(td, 'sections.bin')
        dst = os.path.join(td, 'sections.lzma')
        open(src, 'wb').write(raw)
        # The lima VM's edk2 LzmaCompress; the VM sees this path (home is shared read-only for writes,
        # so the output goes back through stdout).
        res = subprocess.run(['limactl', 'shell', 'starbite', '--', 'bash', '-lc',
                              f'cd ~/work/edk2 && . ./edksetup.sh >/dev/null 2>&1; '
                              f'LzmaCompress -e "{src}" -o /tmp/dxe.lzma >/dev/null && cat /tmp/dxe.lzma'],
                             capture_output=True, check=True)
        comp = res.stdout
    assert lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(comp) == bytes(raw), 'recompression mismatch'
    gsec = bytearray((24 + len(comp)).to_bytes(3, 'little') + b'\x02' + LZMA_GUID.bytes_le +
                     struct.pack('<HH', 24, gattr)) + comp
    while len(gsec) % 4:
        gsec += b'\0'
    body = bytes(gsec)
    hdr = 24
    size = hdr + len(body)
    f = bytearray(rel[dxe['off']:dxe['off'] + 24])
    f[20:23] = size.to_bytes(3, 'little')
    f[16] = header_checksum(f, 0, 24)
    if f[19] & 0x40:  # FFS_ATTRIB_CHECKSUM
        f[17] = (-sum(body)) & 0xFF
    newdxe = bytes(f) + body

    ours = open(args.our_fd, 'rb').read()
    # Reuse fv-splice: build a stand-in release FD whose last file is the new DXE file.
    length = struct.unpack_from('<Q', rel, 32)[0]
    stand = bytearray(rel[:dxe['off']]) + newdxe
    if len(stand) > length:
        struct.pack_into('<Q', stand, 32, (len(stand) + 0xfff) & ~0xfff)
    stand += b'\xff' * (struct.unpack_from('<Q', stand, 32)[0] - len(stand))
    out = fv_splice.splice(ours, bytes(stand))
    open(args.out_fd, 'wb').write(out)
    print(f'DXE file {len(newdxe):#x} bytes (release {dxe["size"]:#x}); wrote {args.out_fd}')


if __name__ == '__main__':
    main()
