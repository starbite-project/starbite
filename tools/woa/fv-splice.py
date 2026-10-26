#!/usr/bin/env python3
"""Inspect or splice the outer firmware volume of a Star2Lte UEFI FD.

  fv-splice.py info FD
  fv-splice.py splice OUR_FD RELEASE_FD OUT_FD

splice keeps OUR_FD's volume (bootshim-less FD: our SEC/PEI = PeilessSec file,
pads, PCDs) and replaces its last file, the LZMA-compressed DXE volume
(EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE), with the release FD's, then pads the
volume to its length with 0xFF and fixes the FV header checksum. This is the
src32 control build: our SEC/PEI + the release's entire DXE module set.
"""
import struct
import sys

FV_IMAGE = 0x0B


def fv_header(fd):
    (length, sig, attrs, hdr_len, checksum, ext_off) = struct.unpack_from('<Q4sIHHH', fd, 32)
    assert sig == b'_FVH', 'no _FVH signature'
    return length, attrs, hdr_len, checksum, ext_off


def files(fd):
    length, attrs, hdr_len, _, ext_off = fv_header(fd)
    off = hdr_len
    if ext_off:
        ext_size = struct.unpack_from('<I', fd, ext_off + 16)[0]
        off = (ext_off + ext_size + 7) & ~7
    out = []
    while off + 24 <= length:
        name = fd[off:off + 16]
        if name == b'\xff' * 16:
            break
        ftype, fattr = fd[off + 18], fd[off + 19]
        size = int.from_bytes(fd[off + 20:off + 23], 'little')
        hdr = 24
        if fattr & 0x01:
            size = struct.unpack_from('<Q', fd, off + 24)[0]
            hdr = 32
        out.append((off, size, ftype, fattr, name, hdr))
        off = (off + size + 7) & ~7
    return out


def info(fd):
    length, attrs, hdr_len, checksum, ext_off = fv_header(fd)
    print(f'FV length 0x{length:x} attrs 0x{attrs:08x} hdr {hdr_len} checksum 0x{checksum:04x} ext {ext_off}')
    for off, size, ftype, fattr, name, hdr in files(fd):
        print(f'  @0x{off:06x} size 0x{size:06x} type 0x{ftype:02x} attr 0x{fattr:02x} {name.hex()}')


def checksum16(data):
    return (-sum(struct.unpack_from('<%dH' % (len(data) // 2), data))) & 0xFFFF


def splice(ours, rel):
    length, _, hdr_len, _, _ = fv_header(ours)
    ofiles, rfiles = files(ours), files(rel)
    assert ofiles[-1][2] == FV_IMAGE and rfiles[-1][2] == FV_IMAGE, 'last file is not the DXE volume'
    o_off, o_size = ofiles[-1][0], ofiles[-1][1]
    r_off, r_size = rfiles[-1][0], rfiles[-1][1]
    assert ofiles[-1][3] == rfiles[-1][3], 'DXE volume file attributes differ'
    out = bytearray(ours[:o_off]) + rel[r_off:r_off + r_size]
    assert len(out) <= length, f'release DXE volume does not fit: {len(out):#x} > {length:#x}'
    out += b'\xff' * (length - len(out))
    out += ours[length:]   # the rest of the FD (0xFF) keeps the flashed image size
    # Header checksum covers the header only; the file bodies carry their own.
    struct.pack_into('<H', out, 50, 0)
    struct.pack_into('<H', out, 50, checksum16(out[:hdr_len]))
    assert checksum16(out[:hdr_len]) == 0
    print(f'spliced: our files up to 0x{o_off:x} (DXE volume was 0x{o_size:x}) + release DXE volume 0x{r_size:x}')
    return bytes(out)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'info':
        info(open(sys.argv[2], 'rb').read())
    elif len(sys.argv) == 5 and sys.argv[1] == 'splice':
        out = splice(open(sys.argv[2], 'rb').read(), open(sys.argv[3], 'rb').read())
        open(sys.argv[4], 'wb').write(out)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
