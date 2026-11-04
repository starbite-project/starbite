#!/usr/bin/env python3
"""Libraries the given binaries (on a mounted root) load that are neither in the 26.5 shared cache
(build/test-boots/dsc265.map) nor on the root, recursing into the ones found on the root.
  missing-deps.py MOUNT /System/Library/CoreServices/Finder.app/Contents/MacOS/Finder ..."""
import os, subprocess, sys
mnt = sys.argv[1]
cache = {l.strip() for l in open('build/test-boots/dsc265.map') if l.startswith('/')}
missing, seen = {}, set()
def deps(path):
    out = subprocess.run(['otool', '-L', mnt + path], capture_output=True, text=True).stdout.splitlines()[1:]
    return [l.split(' (compatibility')[0].strip() for l in out if '(compatibility' in l]
def resolve(dep, frm):
    base = os.path.dirname(frm)
    if dep.startswith('@executable_path/') or dep.startswith('@loader_path/'):
        return os.path.normpath(os.path.join(base, dep.split('/', 1)[1]))
    if dep.startswith('@rpath/'):
        for r in ['../Frameworks', '../../Frameworks', '../../../Frameworks', '.']:
            p = os.path.normpath(os.path.join(base, r, dep[7:]))
            if os.path.exists(mnt + p):
                return p
        return None
    return dep
def walk(path, frm):
    if path in seen:
        return
    seen.add(path)
    for d in deps(path):
        p = resolve(d, path)
        if p is None:
            missing.setdefault(d, path); continue
        if p in cache:
            continue
        if os.path.exists(mnt + p):
            walk(p, path)
        else:
            missing.setdefault(p, path)
for b in sys.argv[2:]:
    if os.path.exists(mnt + b):
        walk(b, None)
    else:
        print('no binary', b)
for m, by in sorted(missing.items()):
    print(m, '<-', by)
