#!/usr/bin/env python3
"""Park a directory on the USB stick: tar it to DEST/NAME.tar, verify every file and symlink against the
original (content SHA-256, link targets; macOS ._ metadata entries skipped), and only then remove the
original, leaving NAME-PARKED.txt beside where it was.   park-dir.py DIR DEST"""
import hashlib, os, shutil, subprocess, sys, tarfile
src, dest = os.path.abspath(sys.argv[1]), sys.argv[2]
parent, name = os.path.dirname(src), os.path.basename(src)
tar = os.path.join(dest, name + '.tar')
subprocess.run(['tar', '-cf', tar, '-C', parent, name], check=True)
n = bad = 0
with tarfile.open(tar) as t:
    for m in t:
        if os.path.basename(m.name).startswith('._'):
            continue
        p = os.path.join(parent, m.name)
        if m.isfile():
            n += 1
            bad += hashlib.sha256(t.extractfile(m).read()).hexdigest() != hashlib.sha256(open(p, 'rb').read()).hexdigest()
        elif m.issym():
            n += 1
            bad += os.readlink(p) != m.linkname
want = sum(1 for d, ds, fs in os.walk(src) for f in fs + [x for x in ds if os.path.islink(os.path.join(d, x))]
           if not f.startswith('._'))
print(f'{name}: {n} entries checked, {bad} differ, {want} on disk')
if bad or n != want:
    sys.exit('NOT removed: the archive does not match')
shutil.rmtree(src)
open(os.path.join(parent, name + '-PARKED.txt'), 'w').write(f'{name} parked at {tar} (verified file by file)\n')
print('removed original; archive', tar)
