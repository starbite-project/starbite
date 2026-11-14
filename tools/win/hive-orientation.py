#!/usr/bin/env python3
"""The S9 panel's saved Windows display orientation, in an OFFLINE SYSTEM hive (docs/WIN11-GPU.md 6.81).

  hive-orientation.py IN show
  hive-orientation.py IN OUT set N       Rotation = N in every saved layout of our panel's monitor
                                         (Control\\GraphicsDrivers\\Configuration\\NOEDID_42525453_*\\<path>\\<target>)

N is D3DKMDT_VIDPN_PRESENT_PATH_ROTATION: 1 identity (portrait, the panel's own orientation), 2 = 90, 3 = 180,
4 = 270. On the user's phone 2 shows as "landscape (flipped)" and 4 as "landscape" (11-14, the user's words; 6.81).
Windows applies the saved layout at the next start; sbwddm offers all four rotations (Parameters\\Rotation = 1) and
Windows turns the touch digitizer's coordinates with the display. Other monitors' layouts (e.g. Microsoft Basic
Display's MSNIL* set) are left alone. Only Rotation changes; the primary surface size stays as saved.

Writes go through hivexsh in the lima VM with the main checkout's tools/win/hive-service.py helpers (staging,
independent read-back, refuses a dirty hive). IN is never modified. Nothing here touches the phone.
"""
import importlib.util
import os
import shutil
import struct
import sys

MAIN = os.environ.get('STARBITE_MAIN', '/Users/dev/project-starbite')
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)

PANEL = 'NOEDID_42525453_'          # sbwddm's target: no EDID, manufacturer/product from the STRB0072 path
NAMES = {1: 'portrait (identity)', 2: '90 (shown as landscape flipped)', 3: '180 (portrait flipped)',
         4: '270 (shown as landscape)'}


def targets(h):
    """[(key path, rotation)] for every saved path target of our panel."""
    c = hs.control_set(h)
    base = f'{c}\\Control\\GraphicsDrivers\\Configuration'
    out = []
    if not hs.exists(h, base):
        return out
    _, kb = h.find(base)
    kids = lambda k: [(sk['name'], sk) for _, sk in h.subkeys(k)]
    for n1, k1 in kids(kb):
        if not n1.startswith(PANEL):
            continue
        for n2, k2 in kids(k1):
            for n3, k3 in kids(k2):
                v = {n: (t, d) for n, t, d in h.values(k3)}
                if 'Rotation' in v and v['Rotation'][0] == 4:
                    out.append((f'{base}\\{n1}\\{n2}\\{n3}', struct.unpack('<I', v['Rotation'][1])[0]))
    return out


def show(h):
    t = targets(h)
    if not t:
        print('no saved layout for our panel (Windows starts in portrait, its default)')
    for k, r in t:
        print(f'{k.split(chr(92))[-3][:40]}...\\{k.split(chr(92))[-2]}\\{k.split(chr(92))[-1]}: Rotation {r} = '
              f'{NAMES.get(r, "?")}')


def main(a):
    if len(a) == 2 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]))
        return
    if len(a) != 4 or a[2] != 'set' or a[3] not in ('1', '2', '3', '4'):
        raise SystemExit(__doc__)
    src, out, _, n = a[0], a[1], a[2], int(a[3])
    hs.check(src)
    h = hs.Hive(src)
    t = targets(h)
    if not t:
        raise SystemExit('no saved layout for our panel: nothing to set (boot once with the GPU driver first)')
    s = ''
    for k, _ in t:
        s += hs.setvals(h, k, {'Rotation': hs.dword(n)})
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SYSTEM.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    hs.hivexsh(work, s + 'commit\n')
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh'):
        if os.path.exists(f):
            os.remove(f)
    hs.check(out)
    h2 = hs.Hive(out)
    t2 = targets(h2)
    if len(t2) != len(t) or any(r != n for _, r in t2):
        os.remove(out)
        raise SystemExit('own read-back disagrees; output removed')
    show(h2)
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
