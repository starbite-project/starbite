#!/usr/bin/env python3
"""List every HFS-compressed (decmpfs, UF_COMPRESSED) file on iOS 26's card root, read only (TWRP + adb).
    tools/ios26-compressed-scan.py OUT.tsv
Kernel logs show ~41 "decmpfs ... tried to access a compressed file of unregistered type 12" per boot: our XNU
has no decompressor (macOS/iOS get them from AppleFSCompression), so such files read as errors. This walks every
leaf node of the catalog B-tree once (read in 16 MiB slices), keeps folder names and compressed file records, and
writes: path, file ID, data fork size, resource fork logical size (type 4/8/12 keep their chunks there; 0 means
the data sits in the com.apple.decmpfs xattr), mode, decmpfs type and unpacked size (from the com.apple.decmpfs
headers, collected in one pass over the Attributes B-tree's leaves). Hard links are listed under each link path with the inode's
fork sizes. HFS_IMAGE=FILE scans a local image instead (tools/overlay/hfs-read.py)."""
import importlib.util
import os
import struct
import sys
from pathlib import Path

if not os.environ.get('HFS_IMAGE'):
    os.environ['HFS_CARD'] = '1'
_spec = importlib.util.spec_from_file_location('hfsread', Path(__file__).with_name('overlay') / 'hfs-read.py')
hr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hr)

UF_COMPRESSED = 0x20
SLICE = 16 << 20


def walk_leaves(vol, fork, node_size, fn):
    size = fork[0]
    for base in range(0, size, SLICE):
        buf = vol.fork_read(fork, base, min(SLICE, size - base))
        hr.cache.clear()
        for off in range(0, len(buf) - node_size + 1, node_size):
            node = buf[off:off + node_size]
            if struct.unpack_from('>b', node, 8)[0] == -1:
                for r in vol.records(node, node_size):
                    fn(r)
        yield min(base + SLICE, size), size


def decmpfs_headers(vol):
    """file ID -> (decmpfs type, unpacked size) from every inline com.apple.decmpfs attribute."""
    out = {}
    if not vol.attributes:
        return out
    hdr = vol.fork_read(vol.attributes, 0, 512)
    ns = struct.unpack_from('>H', hdr, 32)[0]
    want = 'com.apple.decmpfs'.encode('utf-16-be')

    def rec(r):
        klen, fid, _, nlen = struct.unpack_from('>HxxIIH', r, 0)
        if r[14:14 + 2 * nlen] != want:
            return
        d = r[2 + klen:]
        if struct.unpack_from('>I', d, 0)[0] == 0x10 and d[16:20] == b'fpmc':
            out[fid] = (struct.unpack_from('<I', d, 20)[0], struct.unpack_from('<Q', d, 24)[0])
    for done, size in walk_leaves(vol, vol.attributes, ns, rec):
        print(f'{done >> 20} of {size >> 20} MiB of attributes: {len(out)} decmpfs headers', file=sys.stderr)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    vol = hr.Volume()
    size = vol.catalog[0]
    ns = vol.node_size
    folders = {}            # folder ID -> (parent ID, name)
    comp = {}               # file ID -> (parent, name, data size, rsrc size, mode)
    links = []              # (parent, name, inode number) of hard-link records
    files = 0
    for base in range(0, size, SLICE):
        buf = vol.fork_read(vol.catalog, base, min(SLICE, size - base))
        hr.cache.clear()
        for off in range(0, len(buf) - ns + 1, ns):
            node = buf[off:off + ns]
            if struct.unpack_from('>b', node, 8)[0] != -1:
                continue
            for r in vol.records(node, ns):
                klen, (parent, name) = vol.key(r)
                d = r[2 + klen + ((2 + klen) & 1):]
                rtype = struct.unpack_from('>h', d, 0)[0]
                nm = name.decode('utf-16-be', 'replace')
                if rtype == 1:
                    folders[struct.unpack_from('>I', d, 8)[0]] = (parent, nm)
                elif rtype == 2:
                    files += 1
                    fid = struct.unpack_from('>I', d, 8)[0]
                    if d[48:56] == b'hlnkhfs+':
                        links.append((parent, nm, struct.unpack_from('>I', d, 44)[0]))
                    if d[41] & UF_COMPRESSED:
                        comp[fid] = (parent, nm, struct.unpack_from('>Q', d, 88)[0],
                                     struct.unpack_from('>Q', d, 168)[0], struct.unpack_from('>H', d, 42)[0])
        print(f'{min(base + SLICE, size) >> 20} of {size >> 20} MiB of catalog: {files} files, '
              f'{len(comp)} compressed', file=sys.stderr)

    def path(parent, name):
        parts = [name]
        seen = 0
        while parent != 1 and parent in folders and seen < 64:
            parent, n = folders[parent]
            if parent != 1:
                parts.append(n)
            seen += 1
        return '/' + '/'.join(reversed(parts))

    rows = []
    linked = set()
    for parent, nm, inode in links:
        if inode in comp:
            _, _, ds, rs, mode = comp[inode]
            rows.append((path(parent, nm), inode, ds, rs, mode))
            linked.add(inode)
    for fid, (parent, nm, ds, rs, mode) in comp.items():
        if fid not in linked:
            rows.append((path(parent, nm), fid, ds, rs, mode))
    rows.sort()
    heads = decmpfs_headers(vol)
    types = {}
    with open(sys.argv[1], 'w') as f:
        f.write('path\tfile_id\tdata_size\trsrc_size\tmode\tdecmpfs_type\tunpacked_size\n')
        for p, fid, ds, rs, mode in rows:
            t, us = heads.get(fid, ('?', '?'))
            types[t] = types.get(t, 0) + 1
            f.write(f'{p}\t{fid}\t{ds}\t{rs}\t{mode:o}\t{t}\t{us}\n')
    unpacked = sum(heads[r[1]][1] for r in rows if r[1] in heads)
    print(f'{files} files, {len(rows)} compressed paths ({sum(1 for r in rows if r[3])} with a resource fork, '
          f'{sum(r[3] for r in rows) >> 20} MiB of resource forks, {unpacked >> 20} MiB unpacked); types '
          + ' '.join(f'{t}:{n}' for t, n in sorted(types.items(), key=str)) + f' -> {sys.argv[1]}')


if __name__ == '__main__':
    main()
