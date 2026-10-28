#!/usr/bin/env python3
"""Edit the S9's live iOS root on the Mac without resetting it.

starlte-flash-root.py --diff writes a whole image back: every 64 MiB chunk the
phone changed at runtime (settings, caches, app data) reverts to the image's.
This tool goes the other way first:

  starlte-root-sync.py pull BASE OUT   # OUT = BASE + the chunks that differ on the phone
  (attach OUT on the Mac, edit files, detach)
  starlte-root-sync.py push OUT        # write back only the chunks edited on the Mac

pull hashes every chunk of the root range on the phone in one adb shell (8 in
parallel), clones BASE to OUT (APFS clone, no copy), and pulls only the chunks
whose hash differs, checking each against the phone's hash. The phone's hashes
are kept in OUT.hashes. push re-hashes OUT, and for each chunk that changed on
the Mac checks that the phone still holds the pulled contents (it was not
booted since), writes it like starlte-flash-root.py (gzip, push, dd, read back).
TWRP only; /data must not be mounted.
"""
import argparse
import gzip
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

DEV = '/dev/block/by-name/USERDATA'
CHUNK = 64 << 20
MIB = 1 << 20
ADB = os.environ.get('ADB', 'adb')


def adb(args, check=True, binary=False):
    for attempt in range(6):
        p = subprocess.run([ADB, *args], capture_output=True, text=not binary)
        err = p.stderr if not binary else p.stderr.decode(errors='replace')
        transient = any(s in err for s in ('daemon', 'offline', 'no devices', 'cannot connect'))
        if not (p.returncode and transient):
            break
        print(f'adb transient error, retry {attempt + 1}: {err.strip()[:120]}', flush=True)
        time.sleep(5)
        subprocess.run([ADB, 'kill-server'], capture_output=True)
        subprocess.run([ADB, 'start-server'], capture_output=True)
        subprocess.run([ADB, 'wait-for-recovery'], capture_output=True, timeout=180)
    if check and p.returncode:
        raise SystemExit(f'adb {" ".join(args)[:200]} failed: {err[:300]}')
    return p.stdout


def preflight():
    if adb(['get-state'], check=False).strip() != 'recovery':
        raise SystemExit('phone is not in TWRP')
    if adb(['shell', "mount | grep -c ' /data ' || true"]).strip() != '0':
        adb(['shell', 'umount /sdcard 2>/dev/null; umount /data 2>/dev/null; true'])
        if adb(['shell', "mount | grep -c ' /data ' || true"]).strip() != '0':
            raise SystemExit('/data is mounted; unmount it in TWRP first')


def phone_hashes(nchunks):
    """sha256 of chunks [0, nchunks) on the phone, 8 dd|sha256sum at a time."""
    script = (f'i=0; while [ $i -lt {nchunks} ]; do j=0; '
              f'while [ $j -lt 8 ] && [ $i -lt {nchunks} ]; do '
              f'(h=$(dd if={DEV} bs=1048576 skip=$((i*64)) count=64 2>/dev/null | sha256sum); '
              f'echo "$i ${{h%% *}}") & i=$((i+1)); j=$((j+1)); done; wait; done')
    t = time.time()
    out = adb(['shell', script])
    hashes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0].isdigit():
            hashes[int(parts[0])] = parts[1]
    if len(hashes) != nchunks:
        raise SystemExit(f'phone hashes: got {len(hashes)} of {nchunks}')
    print(f'phone: {nchunks} chunks hashed in {time.time() - t:.0f} s', flush=True)
    return hashes


def local_hashes(path):
    out = {}
    with open(path, 'rb') as f:
        i = 0
        while True:
            data = f.read(CHUNK)
            if not data:
                break
            if len(data) != CHUNK:
                raise SystemExit(f'{path}: size is not a multiple of 64 MiB')
            out[i] = hashlib.sha256(data).hexdigest()
            i += 1
    return out


def read_chunk(i):
    data = adb(['exec-out', f'dd if={DEV} bs=1048576 skip={i * 64} count=64 2>/dev/null'], binary=True)
    if len(data) != CHUNK:
        raise SystemExit(f'chunk {i}: pulled {len(data)} bytes')
    return data


def write_chunk(i, data, tmp):
    local = os.path.join(tmp, 'chunk.gz')
    with gzip.open(local, 'wb', compresslevel=1) as f:
        f.write(data)
    adb(['push', local, '/tmp/chunk.gz'])
    adb(['shell', f'gzip -dc /tmp/chunk.gz | dd of={DEV} bs=1048576 seek={i * 64} conv=fsync '
         f'2>/dev/null; rm -f /tmp/chunk.gz'])
    back = adb(['shell', f'dd if={DEV} bs=1048576 skip={i * 64} count=64 2>/dev/null | sha256sum']).split()[0]
    if back != hashlib.sha256(data).hexdigest():
        raise SystemExit(f'chunk {i}: read-back mismatch after write')
    return back


def pull(base, out):
    preflight()
    size = os.path.getsize(base)
    n = size // CHUNK
    phone = phone_hashes(n)
    host = local_hashes(base)
    diff = [i for i in range(n) if phone[i] != host[i]]
    print(f'{len(diff)} of {n} chunks differ from {base}: {diff}', flush=True)
    if os.path.exists(out):
        os.remove(out)
    subprocess.run(['cp', '-c', base, out], check=True)
    with open(out, 'r+b') as f:
        for i in diff:
            data = read_chunk(i)
            if hashlib.sha256(data).hexdigest() != phone[i]:
                raise SystemExit(f'chunk {i}: pulled data does not match the phone hash')
            f.seek(i * CHUNK)
            f.write(data)
    with open(out + '.hashes', 'w') as f:
        json.dump({'chunks': n, 'phone': phone}, f)
    print(f'{out}: phone root, {len(diff)} chunks pulled; edit it, then push', flush=True)


def push(img):
    preflight()
    meta = json.load(open(img + '.hashes'))
    phone = {int(k): v for k, v in meta['phone'].items()}
    now = local_hashes(img)
    changed = [i for i in range(meta['chunks']) if now[i] != phone[i]]
    print(f'{len(changed)} chunks edited on the Mac: {changed}', flush=True)
    with open(img, 'rb') as f, tempfile.TemporaryDirectory() as tmp:
        for i in changed:
            cur = adb(['shell', f'dd if={DEV} bs=1048576 skip={i * 64} count=64 2>/dev/null | sha256sum']).split()[0]
            if cur != phone[i]:
                raise SystemExit(f'chunk {i} changed on the phone since the pull (booted?): pull again')
            f.seek(i * CHUNK)
            phone[i] = write_chunk(i, f.read(CHUNK), tmp)
    adb(['shell', 'sync'])
    with open(img + '.hashes', 'w') as f:
        json.dump({'chunks': meta['chunks'], 'phone': phone}, f)
    print('push done', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pull')
    p.add_argument('base')
    p.add_argument('out')
    q = sub.add_parser('push')
    q.add_argument('img')
    a = ap.parse_args()
    if a.cmd == 'pull':
        pull(a.base, a.out)
    else:
        push(a.img)


if __name__ == '__main__':
    main()
