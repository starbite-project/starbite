#!/usr/bin/env python3
"""Locate a file's data clusters on an NTFS partition of the phone, over adb.

  ntfs-locate.py BLOCKDEV NAME SIZE [FILE_OFFSET]

Reads the boot sector and $MFT over `adb exec-out dd` (read-only), finds the
MFT record whose $FILE_NAME is NAME and whose non-resident $DATA has exactly
SIZE bytes, prints its runlist, and (with FILE_OFFSET) the partition byte
offset holding that file byte. Never writes.
"""
import struct
import subprocess
import sys


def dd(dev, off, n):
    bs = 4096
    assert off % bs == 0 and n % bs == 0
    return subprocess.run(['adb', 'exec-out', f'dd if={dev} bs={bs} skip={off // bs} count={n // bs} 2>/dev/null'],
                          capture_output=True, check=True).stdout


def runlist(attr, off):
    runs, lcn, p = [], 0, off
    while attr[p]:
        h = attr[p]; ls, os_ = h & 15, h >> 4; p += 1
        length = int.from_bytes(attr[p:p + ls], 'little'); p += ls
        delta = int.from_bytes(attr[p:p + os_], 'little', signed=True) if os_ else 0; p += os_
        lcn += delta
        runs.append((lcn if os_ else None, length))
    return runs


def fixup(rec, sector=512):
    usa_off, usa_n = struct.unpack_from('<HH', rec, 4)
    usn = rec[usa_off:usa_off + 2]
    rec = bytearray(rec)
    for i in range(1, usa_n):
        end = i * sector - 2
        if rec[end:end + 2] != usn:
            return None
        rec[end:end + 2] = rec[usa_off + 2 * i:usa_off + 2 * i + 2]
    return bytes(rec)


def attrs(rec):
    p = struct.unpack_from('<H', rec, 0x14)[0]
    while p + 8 <= len(rec):
        t, ln = struct.unpack_from('<II', rec, p)
        if t == 0xffffffff or ln == 0:
            break
        yield t, rec[p:p + ln]
        p += ln


def main():
    dev, name, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
    foff = int(sys.argv[4], 0) if len(sys.argv) > 4 else None
    bs = dd(dev, 0, 4096)
    assert bs[3:11] == b'NTFS    '
    bps, spc = struct.unpack_from('<HB', bs, 0x0b)
    cl = bps * spc
    mft_lcn, = struct.unpack_from('<Q', bs, 0x30)
    crec = struct.unpack_from('<b', bs, 0x40)[0]
    rsz = cl * crec if crec > 0 else 1 << -crec
    r0 = fixup(dd(dev, mft_lcn * cl, cl)[:rsz])
    mft_runs = next(runlist(a, struct.unpack_from('<H', a, 0x20)[0]) for t, a in attrs(r0) if t == 0x80)
    print(f'cluster {cl} B, record {rsz} B, $MFT runs {mft_runs}', file=sys.stderr)
    for lcn, length in mft_runs:
        chunk = 64 << 20
        for start in range(0, length * cl, chunk):
            data = dd(dev, lcn * cl + start, min(chunk, length * cl - start))
            for r in range(0, len(data), rsz):
                rec = data[r:r + rsz]
                if rec[:4] != b'FILE' or not (struct.unpack_from('<H', rec, 0x16)[0] & 1):
                    continue
                rec = fixup(rec)
                if rec is None:
                    continue
                names, data_attr = [], None
                for t, a in attrs(rec):
                    if t == 0x30 and a[8] == 0:
                        v = a[struct.unpack_from('<H', a, 0x14)[0]:]
                        names.append(v[0x42:0x42 + 2 * v[0x40]].decode('utf-16-le'))
                    if t == 0x80 and a[8] == 1 and a[9] == 0:
                        data_attr = a
                if name in names and data_attr is not None and struct.unpack_from('<Q', data_attr, 0x30)[0] == size:
                    runs = runlist(data_attr, struct.unpack_from('<H', data_attr, 0x20)[0])
                    print(f'record found: names {names} runs {runs}')
                    if foff is not None:
                        vcn, rem = divmod(foff, cl)
                        for lcn2, length2 in runs:
                            if vcn < length2:
                                assert lcn2 is not None, 'sparse'
                                print(f'file offset {foff:#x} -> partition byte {(lcn2 + vcn) * cl + rem:#x} '
                                      f'(cluster byte {(lcn2 + vcn) * cl:#x})')
                                break
                            vcn -= length2
                    return
    raise SystemExit('not found')


if __name__ == '__main__':
    main()
