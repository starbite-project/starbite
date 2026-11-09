#!/usr/bin/env python3
"""List every HFS-compressed (decmpfs, UF_COMPRESSED) file on iOS 26's card root, read only (TWRP + adb).
    tools/ios26-compressed-scan.py OUT.tsv
Kernel logs show ~41 "decmpfs ... tried to access a compressed file of unregistered type 12" per boot: our XNU
has no decompressor (macOS/iOS get them from AppleFSCompression), so such files read as errors. This walks every
leaf node of the catalog B-tree once (read in 16 MiB slices), keeps folder names and compressed file records, and
writes: path, file ID, data fork size, resource fork logical size (type 4/8/12 keep their chunks there; 0 means
the data sits in the com.apple.decmpfs xattr), mode. Hard links are listed under each link path with the inode's
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
    with open(sys.argv[1], 'w') as f:
        f.write('path\tfile_id\tdata_size\trsrc_size\tmode\n')
        for p, fid, ds, rs, mode in rows:
            f.write(f'{p}\t{fid}\t{ds}\t{rs}\t{mode:o}\n')
    print(f'{files} files, {len(comp)} compressed ({sum(1 for r in rows if r[3])} with a resource fork, '
          f'{sum(r[3] for r in rows) >> 20} MiB of resource forks) -> {sys.argv[1]}')


if __name__ == '__main__':
    main()
