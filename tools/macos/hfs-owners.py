#!/usr/bin/env python3
"""Set owner, group and mode bits inside a raw HFS+/HFSX volume image, offline.

  hfs-owners.py IMAGE OWNERS.json [--default UID:GID] [--check]
  hfs-owners.py IMAGE OUT.json --dump      (read only: write the image's current owners as OWNERS.json)

A Mac user without sudo can create and fill an HFSX image (hdiutil + cp), but every
file then belongs to that user. macOS cares (launchd refuses LaunchDaemons plists not
owned by root, setuid tools need root), so this rewrites the BSD info of every catalog
record instead: paths listed in OWNERS.json ({"/path": [uid, gid, mode]}, written by
ipsw-apfs.py --owners) get exactly those values; every other record (the volume root,
directories we made, hard-link inodes) gets --default (0:0) and keeps its mode.
The image must not be attached. Only the catalog B-tree's leaf records change.
--check only reports what differs. --dump writes every record's current [uid, gid, mode & 07777] to a new
OUT.json (it refuses to overwrite), so a later run of this tool with that file changes nothing.
"""
import argparse, json, struct, sys

BLOCK_HDR = 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('owners')
    ap.add_argument('--default', default='0:0')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--dump', action='store_true')
    a = ap.parse_args()
    du, dg = (int(x) for x in a.default.split(':'))
    if a.dump:
        import os
        if os.path.exists(a.owners):
            sys.exit(f'{a.owners} exists: --dump writes a new file only')
        owners = {}
    else:
        owners = json.load(open(a.owners))
    f = open(a.image, 'rb' if a.check or a.dump else 'r+b')
    f.seek(BLOCK_HDR)
    vh = f.read(512)
    sig = vh[:2]
    if sig not in (b'H+', b'HX'):
        sys.exit(f'{a.image}: no HFS+ volume header')
    bsize, = struct.unpack_from('>I', vh, 40)
    # catalog fork data at 272: logicalSize, clumpSize, totalBlocks, 8 extents
    clen, _, ctot = struct.unpack_from('>QII', vh, 272)
    exts = [struct.unpack_from('>II', vh, 288 + 8 * i) for i in range(8)]
    if sum(c for _, c in exts) != ctot:
        sys.exit('catalog file has overflow extents: not handled')
    def cat_off(off):               # byte offset in the catalog file -> image offset
        for start, cnt in exts:
            if off < cnt * bsize:
                return start * bsize + off
            off -= cnt * bsize
        raise ValueError('offset past the catalog')
    f.seek(cat_off(0))
    hdr = f.read(512)
    first_leaf, = struct.unpack_from('>I', hdr, 14 + 10)
    nsize, = struct.unpack_from('>H', hdr, 14 + 18)
    folders = {1: ('', 0)}          # folderID -> (name, parentID); 1 = the root's parent
    recs = []                       # (image offset of record data, type, parentID, name, cnid)
    node = first_leaf
    while node:
        off = cat_off(node * nsize)
        f.seek(off)
        nd = f.read(nsize)
        flink, _, kind, _, nrec = struct.unpack_from('>IIbBH', nd, 0)
        if kind != -1:
            sys.exit(f'node {node}: not a leaf ({kind})')
        for i in range(nrec):
            ro, = struct.unpack_from('>H', nd, nsize - 2 * (i + 1))
            klen, parent, nlen = struct.unpack_from('>HIH', nd, ro)
            name = nd[ro + 8:ro + 8 + 2 * nlen].decode('utf-16-be')
            do = ro + 2 + klen
            do += do & 1
            rtype, = struct.unpack_from('>H', nd, do)
            if rtype in (1, 2):     # folder / file record
                cnid, = struct.unpack_from('>I', nd, do + 8)
                if rtype == 1:
                    folders[cnid] = (name, parent)
                recs.append((off + do, rtype, parent, name, cnid))
        node = flink
    def path_of(parent, name):
        parts = [name]
        while parent > 2:
            n, parent = folders[parent]
            parts.append(n)
        return '/' + '/'.join(reversed(parts)) if parent == 2 or name else '/'
    changed = 0
    dump = {}
    for at, rtype, parent, name, cnid in recs:
        p = '/' if cnid == 2 else path_of(parent, name)
        f.seek(at + 32)
        uid, gid, af, of, mode = struct.unpack('>IIBBH', f.read(12))
        if a.dump:
            dump[p] = [uid, gid, mode & 0o7777]
            continue
        if p in owners:
            nu, ng, m = owners[p]
            nmode = (mode & 0o170000) | m
        else:
            nu, ng, nmode = du, dg, mode
        if (uid, gid, mode) != (nu, ng, nmode):
            changed += 1
            if a.check:
                print(f'{p}: {uid}:{gid} {mode:o} -> {nu}:{ng} {nmode:o}')
            else:
                f.seek(at + 32)
                f.write(struct.pack('>IIBBH', nu, ng, af, of, nmode))
    if a.dump:
        with open(a.owners, 'x') as out:
            json.dump(dump, out, indent=0, sort_keys=True)
        print(f'{a.image}: {len(recs)} records, owners of {len(dump)} paths written to {a.owners}')
        return
    print(f'{a.image}: {len(recs)} records, {changed} {"would change" if a.check else "changed"}')


if __name__ == '__main__':
    main()
