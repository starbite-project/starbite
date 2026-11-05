#!/usr/bin/env python3
"""block-patch.py PART ORIG PATCHED [ORIG PATCHED ...] [--write]

Patch a file inside a partition that TWRP cannot mount read-write (Google's GSI system image: ext4 with
features the 4.9 kernel refuses to write), at block level. For every 4 KiB block where ORIG and PATCHED
differ (same size, same layout: an in-place binary patch), the partition is streamed once over adb and
each original block's content must be found EXACTLY once (else REFUSED); with --write the patched block
is written there with dd and read back (SHA-256). Without --write: report only.
  PART  SYSTEM (the only partition allowed here)
"""
import hashlib, os, subprocess, sys, tempfile

BS = 4096
ALLOW = {'SYSTEM': 'sda18'}


def sh(cmd):
    return subprocess.run(['adb', 'shell', cmd], capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout.strip()


args = [a for a in sys.argv[1:] if a != '--write']
write = '--write' in sys.argv
part, pairs = args[0], args[1:]
if part not in ALLOW or len(pairs) % 2:
    sys.exit(__doc__)
node = sh(f'readlink -f /dev/block/by-name/{part}')
assert node == f'/dev/block/{ALLOW[part]}', node
want = {}                                  # sha256(orig block) -> (patched block, label)
for o, p in zip(pairs[::2], pairs[1::2]):
    a, b = open(o, 'rb').read(), open(p, 'rb').read()
    assert len(a) == len(b), f'{o} and {p} differ in size'
    for i in range(0, len(a), BS):
        if a[i:i + BS] != b[i:i + BS]:
            blk = a[i:i + BS]
            assert len(blk) == BS, f'{o}: patch in the partial last block (not supported)'
            want[hashlib.sha256(blk).hexdigest()] = (b[i:i + BS], f'{os.path.basename(o)} block {i // BS}')
print(f'{len(want)} block(s) to patch: ' + ', '.join(v[1] for v in want.values()))
size = int(sh(f'blockdev --getsize64 {node}'))
found = {h: [] for h in want}
proc = subprocess.Popen(['adb', 'exec-out', f'dd if={node} bs=1048576 2>/dev/null'], stdout=subprocess.PIPE,
                        stdin=subprocess.DEVNULL)
n = 0
while True:
    blk = proc.stdout.read(BS)
    if not blk:
        break
    if len(blk) == BS:
        h = hashlib.sha256(blk).hexdigest()
        if h in found:
            found[h].append(n)
    n += 1
proc.wait()
assert n * BS == size, f'read {n * BS} of {size} bytes: stream incomplete, refusing'
bad = [(want[h][1], v) for h, v in found.items() if len(v) != 1]
for h, v in found.items():
    print(f'  {want[h][1]}: partition block(s) {v}')
if bad:
    sys.exit(f'REFUSED: not exactly one match: {bad}')
if not write:
    print('report only (no --write): nothing written')
    sys.exit(0)
with tempfile.TemporaryDirectory() as t:
    for h, (new, label) in want.items():
        pb = found[h][0]
        f = os.path.join(t, 'b.bin')
        open(f, 'wb').write(new)
        subprocess.run(['adb', 'push', f, '/tmp/blk.bin'], capture_output=True, check=True, stdin=subprocess.DEVNULL)
        sh(f'dd if=/tmp/blk.bin of={node} bs={BS} seek={pb} count=1 conv=notrunc,fsync 2>/dev/null; sync')
        back = sh(f'dd if={node} bs={BS} skip={pb} count=1 2>/dev/null | sha256sum').split()[0]
        assert back == hashlib.sha256(new).hexdigest(), f'{label}: read back mismatch at block {pb}'
        print(f'  {label}: written at partition block {pb}, verified')
print('done')
