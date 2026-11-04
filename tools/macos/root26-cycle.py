#!/usr/bin/env python3
"""One macOS 26.5 fix-and-boot cycle on the S9, unattended (the phone in TWRP).

  root26-cycle.py [--wrap PATH]... [--unwrap PATH]... [--fetch PATH]... [--link LINK=TARGET]...
                  [--rm PATH]... [--boot KEY] [--no-boot] [--wait S]

1. Edit the root image (attached on the Mac):
     --wrap PATH    PATH -> PATH.starbite-orig, Apple's /usr/bin/true in its place (a boot task or
                    daemon that cannot work on our kernel yet); --unwrap undoes it
     --fetch PATH   copy PATH (file or tree) from the 26.5 IPSW's system volume (ipsw-apfs.py,
                    staged on the USB drive, AppleDouble files dropped)
     --link L=T     symlink L -> T          --rm PATH   delete PATH
2. Owners (hfs-owners.py with the build's owners JSON, kept up to date here), fsck_hfs -fn.
3. Rewrite to the card only the 64 MiB chunks that changed since the last cycle, plus every
   chunk holding HFS metadata (volume headers, allocation / extents / catalog / attributes
   files), with card-range-flash-fast.py. macOS's own writes elsewhere are free space again.
4. boot-next.sh KEY (default mac26t1), then a summary of the boot log.
"""
import argparse, glob, hashlib, json, os, shutil, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = '/Volumes/Untitled/starbite-macos26'
IMG = os.environ.get('ROOTIMG', f'{W}/root-26t1.img')   # ROOTIMG: a copy on the internal SSD (the USB stick reads ~34 MB/s)
OWNERS = f'{W}/owners-t1b.json'
HASHES = f'{ROOT}/build/test-boots/26t1-chunks-current.txt'
DIRTY = f'{ROOT}/build/test-boots/26t1-dirty.txt'   # root chunks the phone wrote (boot-next.sh)
START = 182888448
URL = ('https://updates.cdn-apple.com/2026SpringFCS/fullrestores/122-58869/'
       'DFB1CEEF-5619-4591-9924-E20DB2C8FED0/UniversalMac_26.5_25F71_Restore.ipsw')
OS_DMG = '094-56453-087.dmg.aea'
CX_DMG = '094-56679-089.dmg.aea'        # the SystemOS cryptex (dylibs outside the cache, resources)
C = 64 << 20


def sh(*a, **k):
    return subprocess.run(a, check=True, text=True, capture_output=True, **k).stdout


def attached_elsewhere():
    info = subprocess.run(['hdiutil', 'info'], text=True, capture_output=True).stdout
    return f'image-path      : {IMG}' in info


def attach(nomount=False):
    for _ in range(30):                             # a detach can take a few seconds to clear
        if not attached_elsewhere():
            break
        __import__('time').sleep(1)
    else:
        sys.exit(f'{IMG} is already attached (another agent?): refusing to attach it again')
    a = ['hdiutil', 'attach', '-nobrowse', '-imagekey', 'diskimage-class=CRawDiskImage'] + \
        (['-nomount'] if nomount else []) + [IMG]
    out = sh(*a)
    if nomount:
        return out.split()[0]
    return [l.split('\t')[-1] for l in out.splitlines() if '/Volumes/' in l][0]


def replace_copy(src, dst):
    """copy2 that replaces an existing (possibly read-only) file or link instead of writing into it"""
    if os.path.lexists(dst) and not os.path.isdir(dst):
        os.unlink(dst)
    return shutil.copy2(src, dst, follow_symlinks=False)


def merge_tree(src, dst):
    """src's tree over dst: directories made, files and symlinks replaced, nothing else touched"""
    for d, dirs, files in os.walk(src):
        rel = os.path.relpath(d, src)
        t = dst if rel == '.' else os.path.join(dst, rel)
        if os.path.lexists(t) and not os.path.isdir(t):
            os.unlink(t)
        os.makedirs(t, exist_ok=True)
        for n in list(dirs):
            if os.path.islink(os.path.join(d, n)):       # os.walk lists dir symlinks as dirs
                dirs.remove(n)
                files.append(n)
        for n in files:
            if n.startswith('._'):
                continue                                  # exFAT AppleDouble sidecars
            s, o = os.path.join(d, n), os.path.join(t, n)
            if os.path.lexists(o) and not (os.path.isdir(o) and not os.path.islink(o)):
                os.unlink(o)
            elif os.path.isdir(o) and not os.path.islink(o):
                shutil.rmtree(o)
            if os.path.islink(s):
                os.symlink(os.readlink(s), o)
            else:
                shutil.copy2(s, o)


def chunk_hashes():
    hs = []
    with open(IMG, 'rb') as f:
        while True:
            b = f.read(C)
            if not b:
                return hs
            hs.append(hashlib.sha256(b).hexdigest())


def meta_chunks():
    with open(IMG, 'rb') as f:
        f.seek(1024)
        vh = f.read(512)
    bs, = struct.unpack_from('>I', vh, 40)
    out = {0, os.path.getsize(IMG) // C - 1}
    for off in (112, 192, 272, 352):
        for i in range(8):
            sb, n = struct.unpack_from('>II', vh, off + 16 + 8 * i)
            if n:
                out.update(range(sb * bs // C, ((sb + n) * bs - 1) // C + 1))
    return out


def changed_file_chunks(since):
    """Chunks holding the data of every file or symlink changed since the last card write (ctime newer
    than the hash list's mtime): the volume mounted read-only, `find -newer`, then each file's extents
    from F_LOG2PHYS_EXT (device offset == image offset: the image is a bare HFSX volume). Replaces
    hashing all 10 GiB over the ~34 MB/s USB stick; FULLHASH=1 goes back to hashing."""
    import fcntl
    mnt = attach_ro()
    out, n = set(), 0
    try:
        found = subprocess.run(['find', mnt, '-xdev', '-newer', since, '(', '-type', 'f', '-o', '-type', 'l', ')'],
                               text=True, capture_output=True).stdout.splitlines()
        for path in found:
            try:
                fd = os.open(path, os.O_RDONLY | (0x200000 if os.path.islink(path) else 0))   # O_SYMLINK
            except OSError:
                continue
            try:
                size = os.fstat(fd).st_size
                off = 0
                while off < size:
                    buf = struct.pack('<Iqq', 0, size - off, off)
                    _, contig, dev = struct.unpack('<Iqq', fcntl.fcntl(fd, 65, buf))   # F_LOG2PHYS_EXT
                    if contig <= 0:
                        break
                    out.update(range(dev // C, (dev + contig - 1) // C + 1))
                    off += contig
                n += 1
            finally:
                os.close(fd)
    finally:
        subprocess.run(['hdiutil', 'detach', mnt], capture_output=True)
    print(f'changed since the last write: {n} files in chunks {sorted(out)}')
    return out


def attach_ro():
    for _ in range(30):
        if not attached_elsewhere():
            break
        __import__('time').sleep(1)
    out = sh('hdiutil', 'attach', '-readonly', '-nobrowse', '-imagekey', 'diskimage-class=CRawDiskImage', IMG)
    return [l.split('\t')[-1] for l in out.splitlines() if '/Volumes/' in l][0]


def summary(log):
    t = open(log, errors='replace').read().splitlines()
    keys = ('panic(', 'boot task', 'exited --', 'CRASH ', '<Error>', 'SBMTLTEST', 'SBMAC', 'WindowServer',
            'loginwindow', 'mount:', 'Library not loaded', 'Symbol not found')
    seen, out = set(), []
    for l in t:
        if any(k in l for k in keys) and l not in seen:
            seen.add(l)
            out.append(l[:220])
    tasks = [l.split('(')[1].split(')')[0] for l in t if 'Doing boot task' in l and '(' in l]
    print('boot tasks run:', ' '.join(dict.fromkeys(tasks)))
    print('\n'.join(out[-40:]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wrap', action='append', default=[])
    ap.add_argument('--unwrap', action='append', default=[])
    ap.add_argument('--fetch', action='append', default=[])
    ap.add_argument('--fetch-list', help='a file of OS-volume paths to fetch (ipsw-apfs.py --list)')
    ap.add_argument('--fetch-cx', action='append', default=[], help='like --fetch, from the SystemOS cryptex')
    ap.add_argument('--link', action='append', default=[])
    ap.add_argument('--rm', action='append', default=[])
    ap.add_argument('--enable-job', action='append', default=[], help='LABEL: back into launchd.plist from launchd-disabled.plist')
    ap.add_argument('--disable-list', help='a list file of launchd labels to take out (starlte-disable-jobs.py)')
    ap.add_argument('--job-set', action='append', default=[], help='LABEL:KEY=JSON on a launchd.plist job')
    ap.add_argument('--job-unset', action='append', default=[], help='LABEL:KEY removed from a launchd.plist job (and its plist file)')
    ap.add_argument('--job-console', action='append', default=[], help='LABEL: its stdout/stderr to /dev/console, os_log to stderr')
    ap.add_argument('--fix-frameworks', action='store_true', help='add missing Versions/Current and top-level bundle links')
    ap.add_argument('--put', action='append', default=[], help='LOCAL=ROOTPATH: a file from the Mac (owner 0:0, mode kept)')
    ap.add_argument('--boot', default='mac26t1')
    ap.add_argument('--no-boot', action='store_true')
    ap.add_argument('--wait', type=int, default=600)
    a = ap.parse_args()
    os.chdir(ROOT)
    if not os.path.exists(HASHES):
        sys.exit(f'{HASHES} missing: hash the image as it is on the card first')
    before = open(HASHES).read().split()
    own = json.load(open(OWNERS))
    edits = a.wrap or a.unwrap or a.fetch or a.fetch_list or a.fetch_cx or a.link or a.rm or a.put or a.fix_frameworks or a.enable_job or a.job_console or a.disable_list or a.job_set or a.job_unset
    if edits:
        stage = os.environ.get('STAGE', f'{W}/stage-cycle')   # small files: an APFS folder beats exFAT's 128 KiB clusters
        if a.fetch or a.fetch_cx or a.fetch_list:
            shutil.rmtree(stage, ignore_errors=True)
            os.makedirs(stage)
        listed = []
        if a.fetch_list:
            listed = [l.split('#')[0].strip() for l in open(a.fetch_list)]
            listed = [l for l in listed if l]
        for member, paths, lst in ((OS_DMG, a.fetch, a.fetch_list), (CX_DMG, a.fetch_cx, None)):
            if not paths and not lst:
                continue
            ofs = f'{W}/owners-cycle.json'
            subprocess.run(['python3', '-I', '-B', 'tools/macos/ipsw-apfs.py', URL, 'get', member, stage, *paths,
                            *(['--list', lst] if lst else []),
                            '--keys', f'{W}/keys.txt', '--owners', ofs, '--thin'], check=True)
            if os.path.exists(ofs):
                own.update(json.load(open(ofs)))
        mnt = attach()
        try:
            for p in a.fetch + a.fetch_cx + listed:
                src, dst = stage + p, mnt + p
                if not os.path.lexists(src):
                    print('not in the IPSW, skipped:', p)
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.isdir(src) and not os.path.islink(src):
                    merge_tree(src, dst)
                elif os.path.lexists(dst):
                    os.unlink(dst)
                    shutil.copy2(src, dst, follow_symlinks=False)
                else:
                    shutil.copy2(src, dst, follow_symlinks=False)
                for junk in glob.glob(dst + '/**/._*', recursive=True) + glob.glob(os.path.dirname(dst) + '/._*'):
                    try:
                        os.unlink(junk)
                    except FileNotFoundError:
                        pass
                print('fetched', p)
            for pr in a.put:
                loc, rp = pr.split('=', 1)
                os.makedirs(os.path.dirname(mnt + rp), exist_ok=True)
                replace_copy(loc, mnt + rp)
                own[rp] = [0, 0, os.stat(loc).st_mode & 0o7777]
                print('put', rp)
            for p in a.wrap:
                t, o = mnt + p, mnt + p + '.starbite-orig'
                if os.path.exists(o):
                    print('already wrapped', p)
                    continue
                os.rename(t, o)
                own[p + '.starbite-orig'] = own.get(p, [0, 0, 0o755])
                shutil.copy2(mnt + '/usr/bin/true', t)
                own[p] = [0, 0, 0o755]
                print('wrapped', p)
            for p in a.unwrap:
                o = mnt + p + '.starbite-orig'
                if os.path.exists(o):
                    os.replace(o, mnt + p)
                    own[p] = own.pop(p + '.starbite-orig', [0, 0, 0o755])
                    print('unwrapped', p)
            for l in a.link:
                lk, tg = l.split('=', 1)
                os.makedirs(os.path.dirname(mnt + lk), exist_ok=True)
                if os.path.lexists(mnt + lk):
                    os.unlink(mnt + lk)
                os.symlink(tg, mnt + lk)
                print('linked', lk, '->', tg)
            for p in a.rm:
                q = mnt + p
                if os.path.isdir(q) and not os.path.islink(q):
                    shutil.rmtree(q)
                elif os.path.lexists(q):
                    os.unlink(q)
                own.pop(p, None)
                print('removed', p)
            if a.enable_job:
                import plistlib
                lp, kp = mnt + '/System/Library/xpc/launchd.plist', mnt + '/System/Library/xpc/launchd-disabled.plist'
                d, kept = plistlib.load(open(lp, 'rb')), plistlib.load(open(kp, 'rb'))
                for key, job in list(kept.items()):
                    if job.get('Label') in a.enable_job:
                        d['LaunchDaemons'][key] = kept.pop(key)
                        off = mnt + key + '.starbite-off'
                        if os.path.exists(off):
                            os.replace(off, mnt + key)
                        print('enabled', job['Label'], key)
                with open(lp, 'r+b') as f:
                    f.truncate(0)
                    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
                with open(kp, 'wb') as f:
                    plistlib.dump(kept, f, fmt=plistlib.FMT_BINARY)
            if a.disable_list:
                subprocess.run(['python3', '-I', 'tools/starlte-disable-jobs.py', mnt, a.disable_list], check=True)
                import plistlib
                want = {l.split('#')[0].strip() for l in open(a.disable_list)} - {''}
                for pl in glob.glob(mnt + '/System/Library/LaunchDaemons/*.plist') + glob.glob(mnt + '/System/Library/LaunchAgents/*.plist'):
                    try:
                        if plistlib.load(open(pl, 'rb')).get('Label') in want:
                            os.rename(pl, pl + '.starbite-off')
                            print('renamed', pl[len(mnt):])
                    except Exception:
                        pass
            if a.job_set:
                import plistlib
                lp = mnt + '/System/Library/xpc/launchd.plist'
                d = plistlib.load(open(lp, 'rb'))
                for spec in a.job_set:
                    label, kv = spec.split(':', 1)
                    k, v = kv.split('=', 1)
                    for key, job in d['LaunchDaemons'].items():
                        if job.get('Label') == label:
                            job[k] = json.loads(v)
                            print('set', label, k, '=', job[k])
                            if os.path.exists(mnt + key):                  # the on-disk plist too: launchd reads both
                                with open(mnt + key, 'wb') as f:
                                    plistlib.dump(job, f)
                with open(lp, 'r+b') as f:
                    f.truncate(0)
                    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
            if a.job_unset:
                import plistlib
                lp = mnt + '/System/Library/xpc/launchd.plist'
                d = plistlib.load(open(lp, 'rb'))
                for spec in a.job_unset:
                    label, k = spec.split(':', 1)
                    for sec in ('LaunchDaemons', 'LaunchAgents'):
                        for key, job in d.get(sec, {}).items():
                            if job.get('Label') == label and k in job:
                                job.pop(k)
                                print('unset', label, k)
                                if os.path.exists(mnt + key):
                                    with open(mnt + key, 'wb') as f:
                                        plistlib.dump(job, f)
                with open(lp, 'r+b') as f:
                    f.truncate(0)
                    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
            if a.job_console:
                import plistlib
                lp = mnt + '/System/Library/xpc/launchd.plist'
                d = plistlib.load(open(lp, 'rb'))
                for key, job in d['LaunchDaemons'].items():
                    if job.get('Label') in a.job_console:
                        job['StandardOutPath'] = job['StandardErrorPath'] = '/dev/console'
                        env = job.setdefault('EnvironmentVariables', {})
                        env['OS_ACTIVITY_DT_MODE'] = 'YES'
                        env['OS_ACTIVITY_MODE'] = 'debug'
                        print('console + os_log to stderr for', job['Label'])
                        if os.path.exists(mnt + key):                  # the on-disk plist too: launchd reads both
                            with open(mnt + key, 'wb') as f:
                                plistlib.dump(job, f)
                with open(lp, 'r+b') as f:
                    f.truncate(0)
                    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
            if a.fix_frameworks:
                n = 0
                for fw in glob.glob(mnt + '/System/Library/**/*.framework', recursive=True):
                    v = fw + '/Versions'
                    if not os.path.isdir(v) or os.path.islink(fw):
                        continue
                    vers = [d for d in os.listdir(v) if d != 'Current' and not d.startswith('.')]
                    if not os.path.lexists(v + '/Current') and len(vers) == 1:
                        os.symlink(vers[0], v + '/Current')
                        n += 1
                    if not os.path.isdir(v + '/Current'):
                        continue
                    for e in os.listdir(v + '/Current'):
                        if e.startswith('._') or e == '_CodeSignature':
                            continue
                        if not os.path.lexists(fw + '/' + e):
                            os.symlink('Versions/Current/' + e, fw + '/' + e)
                            n += 1
                print('framework links added:', n)
            shutil.rmtree(mnt + '/.fseventsd', ignore_errors=True)
        finally:
            subprocess.run(['sync'])
            for attempt in range(6):                # busy right after big edits: retry, then force
                if subprocess.run(['hdiutil', 'detach', mnt] + (['-force'] if attempt == 5 else []),
                                  capture_output=True).returncode == 0:
                    break
                __import__('time').sleep(3)
        json.dump(own, open(OWNERS, 'w'))
        print(sh('python3', '-I', 'tools/macos/hfs-owners.py', IMG, OWNERS).strip())
        dev = attach(nomount=True)
        try:
            fs = subprocess.run(['fsck_hfs', '-fn', dev.replace('disk', 'rdisk')], text=True, capture_output=True)
        finally:
            subprocess.run(['hdiutil', 'detach', dev], capture_output=True)
        print(fs.stdout.strip().splitlines()[-1])
        if fs.returncode:
            sys.exit('fsck_hfs failed: not writing')
    nchunks = os.path.getsize(IMG) // C
    if os.environ.get('FULLHASH') == '1':
        after = chunk_hashes()
        need = {k for k, h in enumerate(after) if k >= len(before) or before[k] != h} | meta_chunks()
    else:
        need = changed_file_chunks(HASHES) | meta_chunks()
    # what the phone wrote since the last card write (kernel 0384 bitmap via boot-next.sh): the card's
    # copy of those chunks is no longer the image's, whatever the image says is allocated there
    if os.path.exists(DIRTY):
        words = open(DIRTY).read().split()
        if 'ALL' in words:
            need |= set(range(nchunks))
            print('phone writes unknown for a boot: every chunk')
        else:
            need |= {int(w) for w in words if int(w) < nchunks}
    state = f'{ROOT}/build/test-boots/cycle.state'
    open(state, 'w').write('\n'.join(str(k) for k in range(nchunks) if k not in need) + '\n')
    print('card chunks:', sorted(need))
    subprocess.run(['python3', 'tools/macos/card-range-flash-fast.py', IMG, str(START), '--state', state],
                   check=True)
    if os.path.exists(DIRTY):
        os.unlink(DIRTY)
    if os.environ.get('FULLHASH') == '1':
        open(HASHES, 'w').write('\n'.join(after) + '\n')
    else:
        os.utime(HASHES)   # the reference time for the next cycle's `find -newer`
    if a.no_boot:
        return
    r = subprocess.run(['tools/store/boot-next.sh', a.boot, str(a.wait)], text=True, capture_output=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr)
    logs = sorted(glob.glob(f'build/test-boots/*-{a.boot}/pmsg.txt'), key=os.path.getmtime)
    if logs:
        print('log:', logs[-1])
        summary(logs[-1])


if __name__ == '__main__':
    main()
