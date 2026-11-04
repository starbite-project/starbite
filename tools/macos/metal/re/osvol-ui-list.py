#!/usr/bin/env python3
"""Make tools/macos/macos26-ui.list from the 26.5 OS volume itself (one metadata open, one walk):
the framework bundle contents macOS's login UI needs that only the OS volume has (the cryptex holds
the code). For every framework under /System/Library/{Frameworks,PrivateFrameworks}, nested ones
included:
  1. every *.xpc under its XPCServices,
  2. its Resources if under LIMIT bytes (plus the always-wanted big ones in KEEP; ML/voice ones skipped),
  3. its PlugIns,
and 4. /System/Library/CoreServices/Software Update.app.

  osvol-ui-list.py OUT.list [--limit MB] [--budget MB]
Prints every chosen path with its size, and the total.
"""
import importlib.util
import os
import stat
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
spec = importlib.util.spec_from_file_location('ipswapfs', os.path.join(ROOT, 'tools/macos/ipsw-apfs.py'))
ia = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ia)

URL = ('https://updates.cdn-apple.com/2026SpringFCS/fullrestores/122-58869/'
       'DFB1CEEF-5619-4591-9924-E20DB2C8FED0/UniversalMac_26.5_25F71_Restore.ipsw')
OS_DMG = '094-56453-087.dmg.aea'
KEYS = '/Volumes/Untitled/starbite-macos26/keys.txt'

KEEP = ('/Frameworks/QuartzCore.framework/', '/HIToolbox.framework/', '/AppKit.framework/', '/LoginUIKit.framework/',
        '/SkyLight.framework/', '/AvatarKit.framework/', '/Security.framework/', '/CoreText.framework/',
        '/CoreUI.framework/', '/LocalAuthentication', '/ViewBridge.framework/', '/UIFoundation.framework/')


def main():
    out = sys.argv[1]
    limit = 5 << 20
    budget = 1400 << 20
    if '--limit' in sys.argv:
        limit = int(sys.argv[sys.argv.index('--limit') + 1]) << 20
    if '--budget' in sys.argv:
        budget = int(sys.argv[sys.argv.index('--budget') + 1]) << 20
    rem = ia.Remote(URL)
    fs, m, u = ia.open_member(rem, OS_DMG, None, KEYS)
    print(f'volume "{fs.vol_name}"', file=sys.stderr)

    def kids(oid):
        try:
            return fs.children(oid)
        except Exception:
            return {}

    def mode(cid):
        return fs.inode(cid)['mode']

    def size(path):
        try:
            return ia.Plan(fs, [path]).logical()
        except Exception as e:
            print('  size failed', path, e, file=sys.stderr)
            return 0

    chosen = []    # (kind, path, size)
    skipped = []

    def framework(path, oid, depth=0):
        ks = kids(oid)
        # versioned layout: Versions/<X>/...; flat layout: the bundle dir itself
        roots = []
        if 'Versions' in ks:
            for v, vid in kids(ks['Versions']).items():
                if v != 'Current' and stat.S_ISDIR(mode(vid)):
                    roots.append((f'{path}/Versions/{v}', vid))
        roots.append((path, oid))
        for rpath, rid in roots:
            rk = kids(rid)
            for name, kind in (('XPCServices', 'xpc'), ('Resources', 'res'), ('PlugIns', 'plugins')):
                cid = rk.get(name)
                if cid is None or not stat.S_ISDIR(mode(cid)):
                    continue
                p = f'{rpath}/{name}'
                if kind == 'xpc':
                    for x, xid in kids(cid).items():
                        if x.endswith('.xpc') and stat.S_ISDIR(mode(xid)):
                            chosen.append(('xpc', f'{p}/{x}', size(f'{p}/{x}')))
                    continue
                s = size(p)
                if kind == 'res' and s >= limit and not any(k in p + '/' for k in KEEP):
                    skipped.append((p, s))
                    continue
                chosen.append((kind, p, s))
            sub = rk.get('Frameworks')
            if sub is not None and stat.S_ISDIR(mode(sub)) and depth < 3:
                for f, fid in kids(sub).items():
                    if f.endswith('.framework') and stat.S_ISDIR(mode(fid)):
                        framework(f'{rpath}/Frameworks/{f}', fid, depth + 1)

    for top in ('/System/Library/Frameworks', '/System/Library/PrivateFrameworks'):
        tid = fs.lookup(top)
        names = sorted(kids(tid).items())
        for i, (f, fid) in enumerate(names):
            if f.endswith('.framework') and stat.S_ISDIR(mode(fid)):
                framework(f'{top}/{f}', fid)
            if i % 200 == 0:
                print(f'  {top} {i}/{len(names)}, {len(chosen)} chosen', file=sys.stderr, flush=True)
    su = '/System/Library/CoreServices/Software Update.app'
    chosen.append(('app', su, size(su)))

    # within budget: XPC services and plug-ins first, then Resources smallest first
    chosen.sort(key=lambda c: (c[0] == 'res', c[2]))
    total = 0
    kept = []
    for c in chosen:
        if total + c[2] > budget:
            skipped.append((c[1], c[2]))
            continue
        total += c[2]
        kept.append(c)
    with open(out, 'w') as f:
        f.write('# macOS 26.5 (25F71) OS volume 094-56453-087: framework bundle contents the login UI needs that the\n'
                '# root lacks (XPC services, Resources < %d MB + named ones, PlugIns, Software Update.app).\n'
                '# Made by tools/macos/metal/re/osvol-ui-list.py; %d paths, %.1f MiB logical.\n'
                % (limit >> 20, len(kept), total / 2**20))
        for kind, p, s in sorted(kept, key=lambda c: c[1]):
            f.write(f'{p}\n')
    by = {}
    for kind, p, s in kept:
        by[kind] = by.get(kind, 0) + s
    print(f'kept {len(kept)} paths, {total / 2**20:.1f} MiB: ' + ', '.join(f'{k} {v / 2**20:.1f}' for k, v in by.items()))
    print(f'skipped {len(skipped)}: ' + ', '.join(f'{os.path.basename(os.path.dirname(p))} {s / 2**20:.0f}M'
                                                  for p, s in sorted(skipped, key=lambda x: -x[1])[:25]))


if __name__ == '__main__':
    main()
