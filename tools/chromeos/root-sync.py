#!/usr/bin/env python3
"""Carry edits of chromeos.img's ROOT-A (p3) from the VM copy to the card copy, block by block.

ROOT-A edits that change sizes or allocate (installing Mesa) need a read-write mount, which the VM can do and
TWRP can't; re-sending 6.8 GB would also replace the card's live STATE (p1). So: fingerprint p3's 4 KiB
blocks, edit, and ship only the blocks that changed, in runs. Before each run is written, the card's current
bytes must hash to the VM's old bytes (the copies were identical there), and the written run is read back.

  VM:  root-sync.py snapshot IMG START LEN STATE        (p3 byte offset and length; STATE: old block hashes)
       ... mount p3 read-write, edit, unmount ...
  VM:  root-sync.py diff IMG STATE OUTDIR                (OUTDIR/runs.txt + run-N.bin, old bytes in old-N.sha)
  Mac: root-sync.py apply OUTDIR CARD_IMG               (adb; CARD_IMG e.g. /external_sd/distros/chromeos.img)
"""
import hashlib, os, struct, subprocess, sys

BS = 4096
RUN_MAX = 1024  # blocks per run (4 MiB)


def block_hashes(f, start, length):
    f.seek(start)
    out = bytearray()
    for _ in range(length // BS):
        out += hashlib.blake2b(f.read(BS), digest_size=8).digest()
    return bytes(out)


def snapshot(img, start, length, state):
    start, length = int(start, 0), int(length, 0)
    assert start % BS == 0 and length % BS == 0
    with open(img, 'rb') as f:
        h = block_hashes(f, start, length)
    with open(state, 'wb') as s:
        s.write(struct.pack('<QQ', start, length) + h)
    print(f'snapshot: {length // BS} blocks at {start}')


def diff(img, state, outdir):
    os.makedirs(outdir, exist_ok=True)
    with open(state, 'rb') as s:
        start, length = struct.unpack('<QQ', s.read(16))
        old = s.read()
    with open(img, 'rb') as f:
        new = block_hashes(f, start, length)
        changed = [i for i in range(length // BS) if old[i * 8:i * 8 + 8] != new[i * 8:i * 8 + 8]]
        runs, i = [], 0
        while i < len(changed):
            j = i
            while j + 1 < len(changed) and changed[j + 1] == changed[j] + 1 and j + 1 - i < RUN_MAX:
                j += 1
            runs.append((changed[i], changed[j] - changed[i] + 1))
            i = j + 1
        with open(os.path.join(outdir, 'runs.txt'), 'w') as r:
            for n, (b, cnt) in enumerate(runs):
                f.seek(start + b * BS)
                data = f.read(cnt * BS)
                open(os.path.join(outdir, f'run-{n}.bin'), 'wb').write(data)
                oldh = old[b * 8:(b + cnt) * 8].hex()
                r.write(f'{n} {start + b * BS} {cnt} {hashlib.sha256(data).hexdigest()} {oldh}\n')
    print(f'diff: {len(changed)} changed blocks in {len(runs)} runs ({len(changed) * BS / 1048576:.1f} MiB)')


def adb(cmd, binary=False):
    r = subprocess.run(['adb'] + cmd, capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode()


def apply(outdir, card):
    runs = [l.split() for l in open(os.path.join(outdir, 'runs.txt'))]
    # check every run's old bytes on the card first, so nothing is written unless all of them match
    for n, off, cnt, _, oldh in runs:
        off, cnt = int(off), int(cnt)
        cur = adb(['exec-out', f'dd if={card} bs={BS} skip={off // BS} count={cnt} 2>/dev/null'], binary=True)
        got = b''.join(hashlib.blake2b(cur[k * BS:(k + 1) * BS], digest_size=8).digest() for k in range(cnt)).hex()
        if len(cur) != cnt * BS or got != oldh:
            sys.exit(f'run {n} at {off}: card bytes differ from the VM copy before the edit: refusing (nothing written)')
    print(f'apply: all {len(runs)} runs match the old state; writing')
    for n, off, cnt, newsha, _ in runs:
        off, cnt = int(off), int(cnt)
        adb(['push', os.path.join(outdir, f'run-{n}.bin'), '/tmp/rs.bin'])
        adb(['shell', f'dd if=/tmp/rs.bin of={card} bs={BS} seek={off // BS} count={cnt} conv=notrunc 2>/dev/null'])
        back = adb(['shell', f'dd if={card} bs={BS} skip={off // BS} count={cnt} 2>/dev/null | sha256sum']).split()[0]
        if back != newsha:
            sys.exit(f'run {n} at {off}: readback mismatch')
    adb(['shell', 'sync; rm -f /tmp/rs.bin'])
    print(f'apply: {len(runs)} runs written and verified')


if __name__ == '__main__':
    cmd, args = sys.argv[1], sys.argv[2:]
    {'snapshot': snapshot, 'diff': diff, 'apply': apply}[cmd](*args)
