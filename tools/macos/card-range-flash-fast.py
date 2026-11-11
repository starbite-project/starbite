#!/usr/bin/env python3
"""Write an HFS+ / HFSX root image into a sector range of the S9's microSD through TWRP, fast.

  card-range-flash-fast.py IMAGE START_SECTOR [--state FILE] [--check-only] [--all] [--verify] [--replace-ios26]

The same checks as card-range-flash.py before anything is written (TWRP, the 128 GB card,
the range inside p2 past the card store's used end or exactly the 64 MiB test range, never
p1, 1 MiB alignment), plus: the range must never overlap iOS 26's root on the card
(182888448+33554432, moved there 2026-11-04; macOS 26's old range is inside it).

Faster because (measured 2026-11-03 in TWRP: adb push 29.6 MB/s, card write 15.4 MB/s,
card read 17 MB/s; the old tool's gzip / gunzip and one-step-at-a-time loop gave ~3 MB/s):
  - chunks are pushed raw, and the next chunk is pushed while the phone writes this one
    (two 64 MiB slots in TWRP's /tmp, a semaphore so a slot is never refilled early);
  - chunks holding no allocated HFS block (from the volume's own allocation bitmap) are
    skipped: free-space contents do not matter to HFS. The first and last chunks are always
    written (volume header, alternate volume header). --all writes every chunk.
Each pushed chunk's SHA-256 is checked in the phone's RAM before it is written (catches a bad
transfer; a card write error fails dd). --verify also reads every written chunk back from the
card and compares (about twice the time). All-zero allocated chunks are written from /dev/zero. --state records finished
chunks (resume). adb exec-in is never used: on this TWRP it corrupts binary streams.
"""
import argparse, hashlib, os, queue, struct, subprocess, sys, tempfile, threading, time, zlib

DEV = '/dev/block/mmcblk0'
CARD_SECTORS = 249999360
TEST_RANGE = (249868288, 131072)
IOS26 = (182888448, 33554432)        # iOS 26's root on the card: never written by this tool
CHUNK = 64 << 20
MIB = 1 << 20
SLOTS = ('/tmp/sbfast0', '/tmp/sbfast1')


def adb(args, binary=False):
    r = subprocess.run(['adb', *args], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode(errors='replace').strip()


def read_sectors(first, count):
    return adb(['exec-out', f'dd if={DEV} bs=512 skip={first} count={count} 2>/dev/null'], True)


STOREDATA = 247900160                 # p2 block 8126720: the card store's data area (docs/STORE-ON-CARD.md)


def check(start, count, replace_ios26=False):
    """card-range-flash.py's checks, plus the Ventura range. Returns a description.
    replace_ios26: write exactly the old iOS 26 range (the ios26 row is dropped, 2026-11-11: macOS Sequoia's root
    goes there); it must lie inside p2, past the store head (p2 blocks 0..6) and below the store's data area."""
    if adb(['get-state']) != 'recovery':
        sys.exit('the phone is not in TWRP')
    sectors = int(adb(['shell', 'cat /sys/block/mmcblk0/size']))
    if sectors != CARD_SECTORS:
        sys.exit(f'mmcblk0 has {sectors} sectors, not the 128 GB card ({CARD_SECTORS}): refusing')
    mbr = read_sectors(0, 1)
    if len(mbr) != 512 or mbr[510:512] != b'\x55\xaa':
        sys.exit('no MBR on the card')
    parts = [struct.unpack_from('<4xB3xII', mbr, 446 + 16 * i) for i in range(4)]
    p1, p2 = parts[0], parts[1]
    s, e = start, start + count
    if s < p1[1] + p1[2] and p1[1] < e:
        sys.exit('the range overlaps p1 (the exFAT data partition): refusing')
    if replace_ios26:
        if (s, count) != IOS26:
            sys.exit(f'--replace-ios26 writes exactly {IOS26[0]}+{IOS26[1]}, not {s}+{count}: refusing')
        if p2[0] != 0xDA or s < p2[1] + 7 * 8 or e > STOREDATA or e > p2[1] + p2[2]:
            sys.exit('the old iOS 26 range is not inside p2 between the store head and its data area: refusing')
        return 'the old iOS 26 range (replaced, --replace-ios26)'
    if s < IOS26[0] + IOS26[1] and IOS26[0] < e:
        sys.exit('the range overlaps iOS 26\'s root on the card (182888448+33554432): refusing')
    if (s, count) == TEST_RANGE:
        return 'the 64 MiB test range'
    if p2[0] != 0xDA or s < p2[1] or e > p2[1] + p2[2]:
        sys.exit(f'the range {s}+{count} is not inside p2 (type 0x{p2[0]:02x} {p2[1]}+{p2[2]}): refusing')
    hdr = read_sectors(p2[1] + 8, 1)
    if hdr[:8] != b'SBSTORE2':
        sys.exit('p2 has no SBSTORE2 header: the store\'s used end is unknown, refusing')
    if zlib.crc32(hdr[:48]) != struct.unpack_from('<I', hdr, 48)[0]:
        sys.exit('p2 store header CRC bad: refusing')
    used, = struct.unpack_from('<Q', hdr, 24)
    if s < p2[1] + used * 8:
        sys.exit(f'the range starts below the card store\'s used end (sector {p2[1] + used * 8}): refusing')
    return f'p2 past the store\'s used end (block {used})'


def allocated_chunks(path, size):
    """Chunk indexes that hold at least one allocated HFS+ block, or None (not HFS / unsure)."""
    with open(path, 'rb') as f:
        f.seek(1024)
        vh = f.read(512)
        if vh[:2] not in (b'H+', b'HX'):
            return None
        bs, total = struct.unpack_from('>II', vh, 40)
        fork = vh[112:112 + 80]                          # allocationFile HFSPlusForkData
        logical, = struct.unpack_from('>Q', fork, 0)
        fork_blocks, = struct.unpack_from('>I', fork, 12)
        ext = [struct.unpack_from('>II', fork, 16 + 8 * i) for i in range(8)]
        if sum(n for _, n in ext) != fork_blocks:
            return None                                  # bitmap continues in the extents overflow file
        bitmap = bytearray()
        for sb, n in ext:
            if n:
                f.seek(sb * bs)
                bitmap += f.read(n * bs)
        bitmap = bitmap[:(total + 7) // 8]
    nchunks = (size + CHUNK - 1) // CHUNK
    per = CHUNK // bs
    need = {0, nchunks - 1}
    for k in range(nchunks):
        b0, b1 = k * per, min((k + 1) * per, total)
        if b0 >= total:
            break
        lo, hi = b0 // 8, (b1 + 7) // 8
        if any(bitmap[lo:hi]):
            need.add(k)
    return sorted(need)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('start', type=int)
    ap.add_argument('--state')
    ap.add_argument('--check-only', action='store_true')
    ap.add_argument('--replace-ios26', action='store_true',
                    help='write exactly the old iOS 26 range 182888448+33554432 (macOS Sequoia root, 2026-11-11)')
    ap.add_argument('--all', action='store_true', help='write every chunk, allocated or not')
    ap.add_argument('--verify', action='store_true', help='read every written chunk back from the card')
    a = ap.parse_args()
    size = os.path.getsize(a.image)
    count = size // 512
    if size % MIB or a.start % 2048:
        sys.exit('START and the image size must be 1 MiB multiples')
    where = check(a.start, count, a.replace_ios26)
    nchunks = (size + CHUNK - 1) // CHUNK
    todo = list(range(nchunks)) if a.all else allocated_chunks(a.image, size)
    how = 'all chunks'
    if todo is None:
        todo, how = list(range(nchunks)), 'all chunks (no HFS allocation bitmap read)'
    elif not a.all:
        how = f'{len(todo)} of {nchunks} chunks hold allocated HFS blocks'
    print(f'{a.image}: {size >> 20} MiB -> {DEV} sectors {a.start}..{a.start + count - 1}, inside {where}; {how}',
          flush=True)
    if a.check_only:
        return
    state = a.state or os.path.join(os.path.dirname(os.path.abspath(a.image)), f'card-fast-{a.start}.state')
    done = set()
    if os.path.exists(state):
        done = {int(x) for x in open(state).read().split()}
    todo = [k for k in todo if k not in done]
    seek1 = a.start // 2048                              # dd bs=1 MiB on the phone
    tmpdir = tempfile.mkdtemp(prefix='sbfast-')
    free = threading.Semaphore(2)
    q = queue.Queue(maxsize=2)
    err = []

    def producer():
        try:
            with open(a.image, 'rb') as f:
                for i, k in enumerate(todo):
                    f.seek(k * CHUNK)
                    data = f.read(CHUNK)
                    h = hashlib.sha256(data).hexdigest()
                    zero = not any(data)
                    free.acquire()
                    if err:
                        return
                    slot = i % 2
                    if not zero:
                        local = os.path.join(tmpdir, f'c{slot}')
                        with open(local, 'wb') as o:
                            o.write(data)
                        adb(['push', local, SLOTS[slot]])
                        os.unlink(local)
                    q.put((k, slot, len(data), h, zero))
        except Exception as ex:                           # noqa: BLE001
            err.append(f'producer: {ex}')
        q.put(None)

    th = threading.Thread(target=producer, daemon=True)
    th.start()
    t0, written = time.time(), 0
    with open(state, 'a') as st:
        while True:
            item = q.get()
            if item is None:
                break
            k, slot, n, h, zero = item
            n1 = n // MIB
            src = '/dev/zero' if zero else SLOTS[slot]
            at = seek1 + k * 64
            pre = '' if zero else f'[ "$(sha256sum {src} | cut -d" " -f1)" = {h} ] || {{ echo BADPUSH; exit 0; }}; '
            wr = f'dd if={src} of={DEV} bs=1048576 seek={at} count={n1} conv=fsync 2>/dev/null || {{ echo BADWRITE; exit 0; }}; '
            post = (f'dd if={DEV} bs=1048576 skip={at} count={n1} 2>/dev/null | sha256sum' if a.verify
                    else f'echo {h}')
            try:
                back = adb(['shell', pre + wr + post]).split()[0]
            except subprocess.CalledProcessError as ex:
                err.append(f'chunk {k}: adb failed ({ex})')
                break
            finally:
                free.release()
            if back != h:
                err.append(f'chunk {k}: read back {back[:16]}, image {h[:16]}')
                break
            st.write(f'{k}\n')
            st.flush()
            written += n
            el = time.time() - t0
            print(f'  chunk {k}: {n >> 20} MiB ok{" (zeros)" if zero else ""}, '
                  f'{written / MIB / el:.1f} MB/s', flush=True)
    for _ in range(2):
        free.release()
    th.join(timeout=60)
    adb(['shell', f'rm -f {SLOTS[0]} {SLOTS[1]}'])
    os.rmdir(tmpdir) if not os.listdir(tmpdir) else None
    if err:
        sys.exit('FAILED: ' + '; '.join(err))
    el = time.time() - t0
    print(f'done: {written >> 20} MiB written and read back in {el:.0f} s ({written / MIB / max(el, 1):.1f} MB/s)')


if __name__ == '__main__':
    main()
