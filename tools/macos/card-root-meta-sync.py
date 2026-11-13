#!/usr/bin/env python3
"""Add files to the macOS root on the S9's microSD card WITHOUT a full reflash and without touching any other
file's data (2026-11-13, S2aa: the four MetalPerformanceShaders default.metallib files were missing from the card's
root, card-file-replace.py can only replace a file in place, and the card's root holds in-place fixes and runtime
state that no image on the Mac has).

A mounted HFS volume reads only its metadata (volume headers, allocation bitmap, extents overflow, catalog and
attributes B-trees, journal) to create files. So:

  1. pull IMG          (TWRP) the 64 MiB chunks of the card's root holding that metadata go into IMG, a sparse
                       image of the whole range (every other chunk stays a hole); each is checked against the
                       phone's own sha256. IMG.pull.json keeps their hashes and the allocation bitmap.
  2. add IMG SRC       (Mac) hfs-owners.py --dump (the card's owners); fsck_hfs -fn; mount; copy the tree SRC
                       (laid out like the volume root, new files only); detach by mount point; the dumped owners +
                       root:wheel 0755/0644 for what was added; fsck_hfs -fn again.
  3. plan IMG          (Mac, IMG detached) what changed: the pulled chunks whose bytes differ, and the blocks
                       allocated since the pull (the bitmap's new 1 bits: the new files' data). -> IMG.plan.json
  4. push IMG [--dry-run]   (TWRP) first every changed pulled chunk's card bytes must still hash to what was
                       pulled (the card did not change since); then the new blocks are written (block runs: only
                       those blocks, never the rest of their chunk), then the changed metadata chunks, the chunk
                       holding the volume header last. Each piece: pushed to /tmp, sha256 checked on the phone,
                       dd conv=fsync, read back and compared.

Data blocks of existing files are never written. Freed blocks are not written (free space). The range checks of the
other card tools apply: TWRP, the 128 GB card, every write inside [--start, --start + --count).

  tools/macos/card-root-meta-sync.py pull IMG [--start 182888448] [--count 33554432]
  tools/macos/card-root-meta-sync.py remove IMG LIST     (Mac: delete the listed paths, metadata only)
  tools/macos/card-root-meta-sync.py plan IMG
  tools/macos/card-root-meta-sync.py push IMG [--dry-run]
"""
import argparse
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CARD_SECTORS = 249999360
DEV = '/dev/block/mmcblk0'
C = 64 << 20                    # chunk bytes
SECT = 512


def adb(*args, binary=False):
    r = subprocess.run(['adb', *args], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode(errors='replace').strip()


def twrp_checks():
    if adb('get-state') != 'recovery':
        sys.exit('phone not in TWRP')
    size = adb('shell', 'cat /sys/block/mmcblk0/size')
    if size != str(CARD_SECTORS):
        sys.exit(f'card has {size} sectors, not the 128 GB card ({CARD_SECTORS})')


def phone_sha(off, length):
    """sha256 of card bytes [off, off + length) (off, length multiples of 4 KiB), hashed on the phone."""
    bs = 4096 if (off | length) % (1 << 20) else 1 << 20
    out = adb('shell', f'dd if={DEV} bs={bs} skip={off // bs} count={length // bs} 2>/dev/null | sha256sum')
    return out.split()[0]


def phone_read(off, length):
    bs = 4096 if (off | length) % (1 << 20) else 1 << 20
    data = adb('exec-out', f'dd if={DEV} bs={bs} skip={off // bs} count={length // bs} 2>/dev/null', binary=True)
    if len(data) != length:
        sys.exit(f'short read at card byte {off}: {len(data)} of {length}')
    return data


def volume_at(start):
    os.environ['HFS_CARD'] = '1'
    spec = importlib.util.spec_from_file_location('hfsread', ROOT / 'tools/overlay/hfs-read.py')
    hr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hr)
    hr.BASE = start * SECT
    return hr


def meta_extents(read):
    """[(byte offset, byte length)] of every metadata structure, from the volume header read by read(off, len)
    (relative to the volume start): the headers, the five special files' extents (with overflow extents through
    hfs-read's Volume when given), the journal info block and the journal."""
    vh = read(1024, 512)
    if vh[:2] not in (b'H+', b'HX'):
        sys.exit(f'no HFS+ volume header ({vh[:2]!r})')
    bs, total = struct.unpack_from('>II', vh, 40)
    out = [(0, 4096), ((total - 1) * bs, bs)]           # volume header block, alternate header block
    return vh, bs, total, out


def bitmap_of(read, vh, bs, total, fork_extents):
    exts = fork_extents(vh, 112, 6)
    data = b''.join(read(sb * bs, n * bs) for sb, n in exts)
    return data[:(total + 7) // 8]


def pull(a):
    twrp_checks()
    img = Path(a.image)
    if img.exists():
        sys.exit(f'{img} exists: pull writes a new image only')
    hr = volume_at(a.start)
    vol = hr.Volume()
    read = hr.dev_read
    vh, bs, total, spans = meta_extents(read)
    if bs != 4096:
        sys.exit(f'block size {bs}, not 4096')
    if total * bs > a.count * SECT:
        sys.exit(f'volume ({total * bs} bytes) larger than the range ({a.count * SECT})')

    def fork_extents(buf, off, fid):
        return [(sb, n) for sb, n in vol.fork(buf, off, fid)[1] if n]

    for off, fid in ((112, 6), (192, 3), (272, 4), (352, 8), (432, 7)):
        if struct.unpack_from('>Q', vh, off)[0]:
            spans += [(sb * bs, n * bs) for sb, n in fork_extents(vh, off, fid)]
    attrs = struct.unpack_from('>I', vh, 4)[0]
    jib = struct.unpack_from('>I', vh, 12)[0]
    if attrs & 0x2000:                                  # kHFSVolumeJournaledBit
        jb = read(jib * bs, 512)
        joff, jsize = struct.unpack_from('>QQ', jb, 36)
        spans += [(jib * bs, bs), (joff, jsize)]
    chunks = sorted({k for off, n in spans for k in range(off // C, (off + n - 1) // C + 1)})
    nchunks = (a.count * SECT) // C
    if chunks[-1] >= nchunks:
        sys.exit('metadata past the range end')
    bitmap = bitmap_of(read, vh, bs, total, fork_extents)
    print(f'volume: {total} blocks of {bs}; metadata in {len(chunks)} of {nchunks} chunks: {chunks}')
    with open(img, 'wb') as f:
        f.truncate(a.count * SECT)
    hashes = {}
    with open(img, 'r+b') as f:
        for i, k in enumerate(chunks):
            off = a.start * SECT + k * C
            data = phone_read(off, C)
            h = hashlib.sha256(data).hexdigest()
            if phone_sha(off, C) != h:
                sys.exit(f'chunk {k}: transfer hash differs from the card; nothing kept (delete {img})')
            f.seek(k * C)
            f.write(data)
            hashes[k] = h
            print(f'  chunk {k} pulled ({i + 1}/{len(chunks)})', flush=True)
    state = {'start': a.start, 'count': a.count, 'block': bs, 'blocks': total, 'chunks': hashes,
             'bitmap_sha256': hashlib.sha256(bitmap).hexdigest()}
    Path(str(img) + '.pull.json').write_text(json.dumps(state, indent=1))
    Path(str(img) + '.pull-bitmap').write_bytes(bitmap)
    print(f'{img}: {len(chunks)} chunks pulled; state in {img}.pull.json')


def image_reader(img):
    f = open(img, 'rb')

    def read(off, n):
        f.seek(off)
        b = f.read(n)
        if len(b) != n:
            sys.exit(f'short image read at {off}')
        return b
    return read


def image_fork_extents(read, bs):
    """Extents of a special file from the image's volume header (no overflow lookups: the allocation file's 8
    in-header extents are enough for a 16 GiB volume; anything else refuses)."""
    def fx(buf, off, fid):
        exts = [struct.unpack_from('>II', buf, off + 16 + 8 * i) for i in range(8)]
        blocks = struct.unpack_from('>I', buf, off + 12)[0]
        if sum(n for _, n in exts) != blocks:
            sys.exit(f'file {fid}: extents overflow (not handled for the bitmap)')
        return [(sb, n) for sb, n in exts if n]
    return fx


def runs(bits_new, bits_old, total):
    """[(first block, count)] of blocks set in bits_new and clear in bits_old."""
    out, run = [], None
    for byte in range(len(bits_new)):
        d = bits_new[byte] & ~bits_old[byte] & 0xff
        if not d:
            if run:
                out.append(run)
                run = None
            continue
        for bit in range(8):
            blk = byte * 8 + bit
            if blk >= total:
                break
            if d & (0x80 >> bit):
                if run and run[0] + run[1] == blk:
                    run = (run[0], run[1] + 1)
                else:
                    if run:
                        out.append(run)
                    run = (blk, 1)
            elif run:
                out.append(run)
                run = None
    if run:
        out.append(run)
    return out


def plan(a):
    img = Path(a.image)
    st = json.loads(Path(str(img) + '.pull.json').read_text())
    old_bitmap = Path(str(img) + '.pull-bitmap').read_bytes()
    read = image_reader(img)
    vh = read(1024, 512)
    bs, total = struct.unpack_from('>II', vh, 40)
    if bs != st['block'] or total != st['blocks']:
        sys.exit('block size / count changed since the pull')
    new_bitmap = bitmap_of(read, vh, bs, total, image_fork_extents(read, bs))
    changed = []
    for k, h in sorted(st['chunks'].items(), key=lambda kv: int(kv[0])):
        if hashlib.sha256(read(int(k) * C, C)).hexdigest() != h:
            changed.append(int(k))
    pulled = {int(k) for k in st['chunks']}
    new_runs = []
    for b0, n in runs(new_bitmap, old_bitmap, total):
        # split at chunk edges; blocks inside a pulled chunk travel with that chunk if it changed
        b = b0
        while b < b0 + n:
            k = b * bs // C
            end = min(b0 + n, (k + 1) * C // bs)
            if k not in pulled:
                new_runs.append((b, end - b))
            elif k not in changed:
                sys.exit(f'blocks {b}+{end - b} newly allocated in pulled chunk {k}, but the chunk did not change')
            b = end
    # every byte the Mac wrote must be in a pulled chunk or a new run: the image is sparse, so any data region
    # elsewhere (e.g. a journal replay into a block that is not metadata) would be lost on the card -> refuse
    allowed = [(k * C, (k + 1) * C) for k in pulled] + [(b * bs, (b + n) * bs) for b, n in new_runs]
    allowed.sort()

    def bit(bm, blk):
        return (bm[blk // 8] >> (7 - blk % 8)) & 1

    # Journal replay (S3z 2026-11-13): when the card's last boot ended with transactions still in the journal, the
    # Mac's mount replays them, and HFS+ journals symlink DATA blocks too ("../../../../Keychains" etc. from
    # containermanagerd). Those land in blocks of existing files, outside the pulled chunks. The push carries the
    # journal (now empty) in its chunk, so it must carry these blocks as well, or the card's symlinks lose their
    # targets. A non-zero 4 KiB block there that was allocated at the pull and still is = a replayed block; zero
    # blocks next to it are the Mac's page-sized writes and are left alone; non-zero data in a free block refuses.
    replay = []
    fd = os.open(img, os.O_RDONLY)
    try:
        pos, size = 0, os.fstat(fd).st_size
        while pos < size:
            try:
                d0 = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                break
            d1 = os.lseek(fd, d0, os.SEEK_HOLE)
            x = d0
            for lo, hi in allowed:
                if lo <= x < hi:
                    x = hi
            while x < d1:                               # walk the region through the allowed spans
                inside = [hi for lo, hi in allowed if lo <= x < hi]
                if not inside:
                    nxt = min([lo for lo, hi in allowed if lo > x] + [d1])
                    if x % bs:
                        sys.exit(f'data region at byte {x} not block aligned; stopping')
                    end = min(nxt, d1)
                    while x < end:
                        n = min(bs, end - x)
                        blk = os.pread(fd, n, x)
                        b = x // bs
                        if any(blk):
                            if n != bs or not (bit(old_bitmap, b) and bit(new_bitmap, b)):
                                sys.exit(f'the image has data at byte {x} (block {b}) outside the pulled chunks '
                                         f'and the new blocks, in a block that is not allocated: stopping')
                            if replay and replay[-1][0] + replay[-1][1] == b:
                                replay[-1] = (replay[-1][0], replay[-1][1] + 1)
                            else:
                                replay.append((b, 1))
                        x += n
                else:
                    x = max(inside)
            pos = d1
    finally:
        os.close(fd)
    for b, n in replay:
        print(f'  journal-replayed blocks {b}+{n} (allocated before and after): carried by the push')
    freed = sum(bin(o & ~n & 0xff).count('1') for o, n in zip(old_bitmap, new_bitmap))
    if replay and not changed:
        sys.exit('journal-replayed blocks but no changed metadata chunk: unexpected; stopping')
    p = {'changed_chunks': changed, 'new_runs': new_runs, 'freed_blocks': freed,
         'image_chunk_sha256': {k: hashlib.sha256(read(k * C, C)).hexdigest() for k in changed},
         'run_sha256': [hashlib.sha256(read(b * bs, n * bs)).hexdigest() for b, n in new_runs],
         'replay_runs': replay,
         'replay_sha256': [hashlib.sha256(read(b * bs, n * bs)).hexdigest() for b, n in replay]}
    Path(str(img) + '.plan.json').write_text(json.dumps(p, indent=1))
    nb = sum(n for _, n in new_runs)
    print(f'changed metadata chunks: {changed}')
    print(f'new data outside them: {len(new_runs)} run(s), {nb} blocks ({nb * bs / 1048576:.1f} MiB); freed blocks: {freed}')
    print(f'plan in {img}.plan.json')


def push_piece(card_off, data, what, dry):
    """Write data at card byte card_off (4 KiB aligned), checked on the phone, then read back."""
    h = hashlib.sha256(data).hexdigest()
    if dry:
        print(f'  would write {what}: {len(data)} bytes at card byte {card_off}')
        return
    bs = 1 << 20 if (card_off | len(data)) % (1 << 20) == 0 else 4096
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
    try:
        adb('push', t.name, '/tmp/sbmeta')
    finally:
        os.unlink(t.name)
    if adb('shell', 'sha256sum /tmp/sbmeta').split()[0] != h:
        sys.exit(f'{what}: pushed copy differs; nothing written for it')
    adb('shell', f'dd if=/tmp/sbmeta of={DEV} bs={bs} seek={card_off // bs} count={len(data) // bs} conv=fsync,notrunc 2>&1; rm -f /tmp/sbmeta')
    if phone_sha(card_off, len(data)) != h:
        sys.exit(f'{what}: READ-BACK DIFFERS after the write (card at byte {card_off})')
    print(f'  wrote {what} ({len(data)} bytes), read back OK', flush=True)


def push(a):
    twrp_checks()
    img = Path(a.image)
    st = json.loads(Path(str(img) + '.pull.json').read_text())
    p = json.loads(Path(str(img) + '.plan.json').read_text())
    start, bs = st['start'], st['block']
    base = start * SECT
    read = image_reader(img)
    lo, hi = base, base + st['count'] * SECT
    for k in p['changed_chunks']:
        if hashlib.sha256(read(k * C, C)).hexdigest() != p['image_chunk_sha256'][str(k)]:
            sys.exit(f'image chunk {k} changed after plan: run plan again')
        if not (lo <= base + k * C and base + (k + 1) * C <= hi):
            sys.exit(f'chunk {k} outside the range')
    replay = [tuple(r) for r in p.get('replay_runs', [])]
    for (b, n), h in list(zip(p['new_runs'], p['run_sha256'])) + list(zip(replay, p.get('replay_sha256', []))):
        if hashlib.sha256(read(b * bs, n * bs)).hexdigest() != h:
            sys.exit(f'image blocks {b}+{n} changed after plan: run plan again')
        if not (lo <= base + b * bs and base + (b + n) * bs <= hi):
            sys.exit(f'blocks {b}+{n} outside the range')
    print('checking that the card still holds what was pulled ...', flush=True)
    for k in p['changed_chunks']:
        if phone_sha(base + k * C, C) != st['chunks'][str(k)]:
            sys.exit(f'card chunk {k} changed since the pull (a boot ran?): nothing written; pull again')
    for b, n in p['new_runs']:
        # data for the new files: up to 16 MiB per piece
        step = 4096
        for b1 in range(b, b + n, step):
            m = min(step, b + n - b1)
            push_piece(base + b1 * bs, read(b1 * bs, m * bs), f'new blocks {b1}+{m}', a.dry_run)
    for b, n in replay:
        # blocks the Mac's journal replay wrote home (symlink data): before the chunk with the emptied journal
        push_piece(base + b * bs, read(b * bs, n * bs), f'journal-replayed blocks {b}+{n}', a.dry_run)
    order = sorted(p['changed_chunks'], key=lambda k: (k == 0, k))    # the volume header's chunk last
    for k in order:
        push_piece(base + k * C, read(k * C, C), f'metadata chunk {k}', a.dry_run)
    print('dry run: nothing written' if a.dry_run else 'done: card root updated')


def own_device(img):
    """/dev/diskN of IMG's attachment, from hdiutil info -plist (by image path), or None."""
    import plistlib
    info = plistlib.loads(subprocess.run(['hdiutil', 'info', '-plist'], capture_output=True, check=True).stdout)
    for im in info.get('images', []):
        if os.path.realpath(im.get('image-path', '')) == os.path.realpath(img):
            for e in im.get('system-entities', []):
                if e.get('dev-entry'):
                    return e['dev-entry']
    return None


def fsck_image(img):
    if own_device(img):
        sys.exit(f'{img} is already attached: refusing')
    out = subprocess.run(['hdiutil', 'attach', '-nobrowse', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', img],
                         capture_output=True, text=True, check=True).stdout.split()
    dev = own_device(img)
    if not dev or dev != out[0]:
        sys.exit(f'attachment of {img} not found in hdiutil info')
    r = subprocess.run(['/sbin/fsck_hfs', '-fn', '/dev/r' + dev[len('/dev/'):]], capture_output=True, text=True)
    if own_device(img) == dev:                          # detach only what hdiutil info says is ours
        subprocess.run(['hdiutil', 'detach', dev], capture_output=True, check=True)
    tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
    print('  fsck_hfs -fn: ' + ' / '.join(tail))
    if r.returncode != 0:
        sys.exit('fsck_hfs found problems: stopping (nothing on the card was touched)')


def add(a):
    """Mac side: copy the tree SRC (paths relative to the volume root) into IMG; owners root:wheel, dirs 0755, files
    0644; every other record keeps the card's owners (dumped first)."""
    img, src = Path(a.image), Path(a.src)
    st = json.loads(Path(str(img) + '.pull.json').read_text())
    own_path = Path(str(img) + '.owners.json')
    if not own_path.exists():
        subprocess.run(['python3', '-I', str(ROOT / 'tools/macos/hfs-owners.py'), str(img), str(own_path), '--dump'],
                       check=True)
    own = json.loads(own_path.read_text())
    fsck_image(img)
    if own_device(img):
        sys.exit(f'{img} is already attached: refusing')
    out = subprocess.run(['hdiutil', 'attach', '-nobrowse', '-imagekey', 'diskimage-class=CRawDiskImage', img],
                         capture_output=True, text=True, check=True).stdout
    mnts = [l.split('\t')[-1].strip() for l in out.splitlines() if '/Volumes/' in l]
    if len(mnts) != 1:
        sys.exit(f'no single mount point for {img}: {out}')
    mnt = mnts[0]
    added = []
    try:
        for d, dirs, files in os.walk(src):
            rel = os.path.relpath(d, src)
            tdir = os.path.join(mnt, rel) if rel != '.' else mnt
            if not os.path.isdir(tdir):
                os.makedirs(tdir)
                added.append(('/' + rel, 0o755))
            for name in files:
                if name.startswith('._'):
                    continue
                s, t = os.path.join(d, name), os.path.join(tdir, name)
                if os.path.lexists(t):
                    sys.exit(f'{t} exists on the card root: add only adds new files')
                subprocess.run(['cp', '-X', s, t], check=True)
                added.append(('/' + os.path.normpath(os.path.join(rel, name)), 0o644))
        subprocess.run(['sync'], check=True)
    finally:
        subprocess.run(['hdiutil', 'detach', mnt], capture_output=True)     # by mount point
    if own_device(img):
        sys.exit(f'{img} still attached after detaching {mnt}: stopping')
    for p, mode in added:
        own[p.replace('/./', '/')] = [0, 0, mode]
    new_own = Path(str(img) + '.owners-new.json')
    new_own.write_text(json.dumps(own))
    subprocess.run(['python3', '-I', str(ROOT / 'tools/macos/hfs-owners.py'), str(img), str(new_own)], check=True)
    fsck_image(img)
    for p, mode in added:
        print(f'  added {p} ({oct(mode)})')
    print(f'{len(added)} item(s) added to {img}; next: plan')


def remove(a):
    """Mac side: delete the paths listed in LIST (one per line, relative to the volume root, '#' comments) from IMG,
    to free space on the card root (2026-11-13, S3w: 114 MiB free, under HFS's root-only reserve, so non-root
    programs got ENOSPC). Only metadata changes (catalog, bitmap, journal): freed blocks are never written. Every
    other record keeps the card's owners (dumped first, applied again after)."""
    img = Path(a.image)
    paths = []
    for line in Path(a.src).read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        p = os.path.normpath('/' + line.lstrip('/'))
        if p in ('/', '/System', '/Library', '/private', '/usr', '/Users') or '..' in p.split('/'):
            sys.exit(f'refusing to remove {p}')
        paths.append(p)
    own_path = Path(str(img) + '.owners.json')
    if not own_path.exists():
        subprocess.run(['python3', '-I', str(ROOT / 'tools/macos/hfs-owners.py'), str(img), str(own_path), '--dump'],
                       check=True)
    own = json.loads(own_path.read_text())
    fsck_image(img)
    if own_device(img):
        sys.exit(f'{img} is already attached: refusing')
    out = subprocess.run(['hdiutil', 'attach', '-nobrowse', '-imagekey', 'diskimage-class=CRawDiskImage', img],
                         capture_output=True, text=True, check=True).stdout
    mnts = [l.split('\t')[-1].strip() for l in out.splitlines() if '/Volumes/' in l]
    if len(mnts) != 1:
        sys.exit(f'no single mount point for {img}: {out}')
    mnt = mnts[0]
    gone = []
    try:
        before = os.statvfs(mnt)
        for p in paths:
            t = mnt + p
            if not os.path.lexists(t):
                print(f'  not there: {p}')
                continue
            kb = int(subprocess.run(['du', '-skx', t], capture_output=True, text=True).stdout.split()[0] or 0)
            subprocess.run(['rm', '-rf', t], check=True)
            gone.append((p, kb))
        subprocess.run(['sync'], check=True)
        after = os.statvfs(mnt)
        print(f'  free: {before.f_bavail * before.f_frsize >> 20} MiB -> {after.f_bavail * after.f_frsize >> 20} MiB')
    finally:
        subprocess.run(['hdiutil', 'detach', mnt], capture_output=True)     # by mount point
    if own_device(img):
        sys.exit(f'{img} still attached after detaching {mnt}: stopping')
    prefixes = tuple(p + '/' for p, _ in gone)
    own = {k: v for k, v in own.items() if k not in dict(gone) and not k.startswith(prefixes)}
    new_own = Path(str(img) + '.owners-new.json')
    new_own.write_text(json.dumps(own))
    subprocess.run(['python3', '-I', str(ROOT / 'tools/macos/hfs-owners.py'), str(img), str(new_own)], check=True)
    fsck_image(img)
    for p, kb in gone:
        print(f'  removed {p} ({kb >> 10} MiB)')
    print(f'{len(gone)} item(s) removed from {img}; next: plan')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['pull', 'add', 'remove', 'plan', 'push'])
    ap.add_argument('image')
    ap.add_argument('src', nargs='?', help='add: a directory tree laid out like the volume root; '
                    'remove: a file listing paths to delete')
    ap.add_argument('--start', type=int, default=182888448)
    ap.add_argument('--count', type=int, default=33554432)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    if a.cmd in ('add', 'remove') and not a.src:
        sys.exit(f'{a.cmd} needs SRC')
    {'pull': pull, 'add': add, 'remove': remove, 'plan': plan, 'push': push}[a.cmd](a)


if __name__ == '__main__':
    main()
