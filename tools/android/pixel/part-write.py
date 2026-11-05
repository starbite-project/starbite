#!/usr/bin/env python3
"""part-write.py NAME IMAGE [--dry-run] [--state FILE] [--size BYTES]

Write one Android image into ONE of the partitions this plan may write, from TWRP over adb, the way
tools/starlte-flash-root.py does it (TWRP's adbd ignores exec-in): 64 MiB chunks, each gzip'd, pushed to
/tmp, dd'd at its offset and read back (SHA-256 compared on the phone); all-zero chunks are written on the
phone from /dev/zero and read back too; at the end the whole written range is hashed on the phone and
compared with the image's hash. --state makes an interrupted run resume.

  NAME   SYSTEM (sda18) | VENDOR (sda19) | A16DATA (sda28) | ODM (sda20)   - nothing else, ever
  IMAGE  raw image, IMAGE.gz, or - (raw on stdin; then --size is required)

--dry-run: every read-only check (adb state, by-name -> sdaN, partition size, nothing mounted on it,
image size and hash, chunk plan) and the exact commands, and no write.
Never touches EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB, DTBO, BOOT, RECOVERY, the
GPT or the boot LUNs: the allowlist below is the only way in."""
import argparse, gzip, hashlib, os, subprocess, sys, tempfile, time

ALLOW = {'SYSTEM': 'sda18', 'VENDOR': 'sda19', 'ODM': 'sda20', 'A16DATA': 'sda28'}
CHUNK, MIB = 64 << 20, 1 << 20


def adb(*args, check=True):
    for attempt in range(6):
        # stdin=DEVNULL: adb shell reads stdin, and with IMAGE '-' that is the image stream
        p = subprocess.run(['adb', *args], capture_output=True, text=True, stdin=subprocess.DEVNULL)
        transient = any(s in p.stderr for s in ('daemon', 'device offline', 'no devices', 'cannot connect'))
        if not (p.returncode and transient):
            break
        print(f'adb transient error, retry {attempt + 1}: {p.stderr.strip()[:120]}', flush=True)
        time.sleep(5)
        subprocess.run(['adb', 'wait-for-recovery'], capture_output=True, timeout=120, stdin=subprocess.DEVNULL)
    if check and p.returncode:
        sys.exit(f'adb {" ".join(args)} failed: {p.stdout} {p.stderr}')
    return p.stdout.replace('\r', '')


def sh(cmd):
    return adb('shell', cmd).strip()


def open_image(path):
    if path == '-':
        return sys.stdin.buffer
    return gzip.open(path, 'rb') if path.endswith('.gz') else open(path, 'rb')


def read_full(f, n):
    """Exactly n bytes, or fewer only at the end of the image: a pipe's read() may return less than
    asked (stdin from unzip -p): a short read would misplace every following chunk."""
    out = bytearray()
    while len(out) < n:
        c = f.read(n - len(out))
        if not c:
            break
        out += c
    return bytes(out)


def image_size(path, given):
    if given:
        return given
    if path == '-':
        sys.exit('--size is required for stdin')
    if path.endswith('.gz'):
        n = 0
        with gzip.open(path, 'rb') as f:
            while True:
                c = f.read(CHUNK)
                if not c:
                    return n
                n += len(c)
    return os.path.getsize(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name'); ap.add_argument('image')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--state')
    ap.add_argument('--size', type=int)
    a = ap.parse_args()
    if a.name not in ALLOW:
        sys.exit(f'REFUSED: {a.name} is not one of {sorted(ALLOW)}')
    size = image_size(a.image, a.size)
    if size % 4096:
        sys.exit(f'REFUSED: image size {size} is not a multiple of 4096')
    if adb('get-state', check=False).strip() != 'recovery':
        sys.exit('phone not in TWRP (adb get-state != recovery)')
    node = sh(f'readlink -f /dev/block/by-name/{a.name}')
    if node != f'/dev/block/{ALLOW[a.name]}':
        sys.exit(f'REFUSED: by-name/{a.name} -> {node!r}, expected /dev/block/{ALLOW[a.name]}: re-read the GPT')
    psize = int(sh(f'blockdev --getsize64 {node}'))
    if size > psize:
        sys.exit(f'REFUSED: image {size} B > {a.name} {psize} B')
    mounted = sh(f"grep -E '^({node}|/dev/block/by-name/{a.name}|/dev/block/platform/11120000.ufs/by-name/{a.name}) ' /proc/mounts || true")
    if mounted:
        sys.exit(f'REFUSED: {a.name} is mounted in TWRP; unmount first:\n{mounted}')
    n_chunks = (size + CHUNK - 1) // CHUNK
    print(f'{a.name} = {node}, {psize} B; image {a.image} {size} B = {n_chunks} chunks of 64 MiB'
          f' (partition tail {psize - size} B left as it is)')
    if a.dry_run:
        h, zeros, f = hashlib.sha256(), 0, open_image(a.image)
        for i in range(n_chunks):
            d = read_full(f, CHUNK); h.update(d)
            zeros += d.count(0) == len(d)
        print(f'image sha256 {h.hexdigest()}; {zeros} all-zero chunks (written from /dev/zero), '
              f'{n_chunks - zeros} pushed')
        print(f'per chunk: adb push chunk.gz /tmp; gzip -dc | dd of={node} bs=1048576 seek=<64*i> conv=notrunc,fsync; '
              f'read back + sha256')
        print(f'end: head -c {size} {node} | sha256sum == image sha256')
        print('dry run: nothing written')
        return
    state = a.state or f'build/android/pixel/part-write-{a.name}.state'
    os.makedirs(os.path.dirname(state) or '.', exist_ok=True)
    done = {int(x) for x in open(state).read().split()} if os.path.exists(state) else set()
    st = open(state, 'a')
    f, total, start = open_image(a.image), hashlib.sha256(), time.time()
    zero = bytes(CHUNK)
    with tempfile.TemporaryDirectory() as tmp:
        local = os.path.join(tmp, 'chunk.gz')
        for i in range(n_chunks):
            data = read_full(f, CHUNK)
            total.update(data)
            seek, count = i * CHUNK // MIB, (len(data) + MIB - 1) // MIB
            want = hashlib.sha256(data).hexdigest()
            if i not in done:
                if data == zero[:len(data)]:
                    sh(f'dd if=/dev/zero of={node} bs=1048576 seek={seek} count={count} conv=notrunc,fsync 2>/dev/null')
                else:
                    with gzip.open(local, 'wb', compresslevel=1) as g:
                        g.write(data)
                    adb('push', local, '/tmp/chunk.gz')
                    sh(f'gzip -dc /tmp/chunk.gz | dd of={node} bs=1048576 seek={seek} conv=notrunc,fsync 2>/dev/null; rm -f /tmp/chunk.gz')
                back = sh(f'dd if={node} bs=1048576 skip={seek} count={count} 2>/dev/null | head -c {len(data)} | sha256sum').split()[0]
                if back != want:
                    sys.exit(f'chunk {i} read back {back} != {want}: stopping (rerun resumes; rollback in docs/ANDROID-PIXEL.md)')
                st.write(f'{i}\n'); st.flush()
            if (i + 1) % 8 == 0 or i + 1 == n_chunks:
                print(f'  {(i + 1) * 64} / {n_chunks * 64} MiB, {(i + 1) * 64 / max(time.time() - start, 1):.0f} MiB/s', flush=True)
    sh('sync')
    whole = sh(f'head -c {size} {node} | sha256sum').split()[0]
    if whole != total.hexdigest():
        sys.exit(f'WHOLE-IMAGE CHECK FAILED: phone {whole} != image {total.hexdigest()}')
    os.unlink(state)
    print(f'{a.name}: {size} B written and verified, sha256 {whole}')


if __name__ == '__main__':
    main()
