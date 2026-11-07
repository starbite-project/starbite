#!/usr/bin/env python3
"""List the DXE volume of a UEFI FD, or compare two FDs file by file.

  dxe-diff.py A.fd            the files of A's DXE volume (GUID, type, size, UI name)
  dxe-diff.py A.fd B.fd       files only in A, only in B, and in both but different; also
                              whether the outer FVs (SEC/PEI) are byte-identical

The DXE volume is the last file of the outer FV (an LZMA section holding the inner FV), as
tools/woa/dxe-replace.py builds it. Exit 1 when B differs from A in anything but added files.
"""
import importlib.util
import lzma
import os
import struct
import sys

_spec = importlib.util.spec_from_file_location(
    'dxe_replace', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dxe-replace.py'))
dr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dr)


def load(path):
    fd = open(path, 'rb').read()
    outer, _, _ = dr.ffs_files(fd, 0)
    dxe = outer[-1]
    sec = dxe['off'] + dxe['hdr']
    ssz = int.from_bytes(fd[sec:sec + 3], 'little')
    doff, _ = struct.unpack_from('<HH', fd, sec + 20)
    raw = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(fd[sec + doff:sec + ssz])
    p = 0
    fvoff = None
    while p < len(raw):
        ln = int.from_bytes(raw[p:p + 3], 'little')
        if raw[p + 3] == 0x17:
            fvoff = p + 4
        p = (p + ln + 3) & ~3
    files, _, _ = dr.ffs_files(raw, fvoff)
    out = {}
    for f in files:
        if f['type'] == dr.PAD:
            continue
        body = raw[f['off'] + f['hdr']:f['off'] + f['size']]
        name, q = '', 0
        while q + 4 <= len(body):
            ln = int.from_bytes(body[q:q + 3], 'little')
            if ln < 4:
                break
            if body[q + 3] == 0x15:
                name = body[q + 4:q + ln].decode('utf-16le').rstrip('\0')
            q = (q + ln + 3) & ~3
        out[str(f['guid'])] = (f['type'], body, name)
    return fd[:dxe['off']], out


def main():
    oa, a = load(sys.argv[1])
    if len(sys.argv) == 2:
        for g, (t, body, name) in a.items():
            print(f'{g} type={t:#x} size={len(body):#x} {name}')
        return
    ob, b = load(sys.argv[2])
    bad = False
    if oa != ob:
        print('outer FV (SEC/PEI): DIFFERENT')
        bad = True
    else:
        print('outer FV (SEC/PEI): identical')
    for g in a:
        if g not in b:
            print(f'only in A: {g} {a[g][2]}')
            bad = True
        elif a[g][:2] != b[g][:2]:
            print(f'changed:   {g} {a[g][2]} ({len(a[g][1]):#x} -> {len(b[g][1]):#x} bytes)')
            bad = True
    for g in b:
        if g not in a:
            print(f'added:     {g} {b[g][2]} ({len(b[g][1]):#x} bytes)')
    print('B = A + added files' if not bad else 'B differs from A beyond added files')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
