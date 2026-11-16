#!/usr/bin/env python3
"""The Starbite display adapter's OpenGL installable client driver (ICD), in an OFFLINE SYSTEM hive
(build/win/patchwork/glsys-report.md).

  hive-opengl-icd.py IN show
  hive-opengl-icd.py IN OUT set [--name sbglicd.dll] [--flags 3] [--version 1] [--type multi|sz]
  hive-opengl-icd.py IN OUT clear      the way back: removes exactly the three values "set" writes

"set" writes three values into the display class key of our adapter
(Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\NNNN whose MatchingDeviceId is acpi\\strb0072):

  OpenGLDriverName   the ICD's file name (System32). The system opengl32.dll gets it from dxgkrnl
                     (D3DKMTQueryAdapterInfo, KMTQAITYPE_UMOPENGLINFO) and loads it for windows on this adapter.
                     Microsoft documents REG_SZ; installed vendor drivers have REG_MULTI_SZ (as our own
                     UserModeDriverName is). Default here: REG_MULTI_SZ with the one name; --type sz for REG_SZ.
  OpenGLVersion      REG_DWORD, handed to the ICD's DrvValidateVersion (Mesa accepts any). Default 1.
  OpenGLFlags        REG_DWORD bit mask. Bit 0 "is set for compatibility"; bit 1: the OpenGL runtime does not call
                     the ICD's finish before its swap-buffers (Mesa flushes the frame itself in DrvSwapBuffers,
                     and the game kit, which goes through Mesa's own opengl32.dll, never had that finish).
                     Default 3; --flags 1 keeps the runtime's finish.
  (source for the names and meanings: Microsoft, "Loading an OpenGL Installable Client Driver",
   learn.microsoft.com/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver)

Not written: OpenGLDriverNameWow / OpenGLVersionWow / OpenGLFlagsWow (32-bit programs: there is no 32-bit build;
they keep Microsoft's OpenGL). x64 programs on ARM64 are not WOW64 and use OpenGLDriverName, which is why the
named file must be ARM64X.

Safety: exactly one such adapter key must exist. "set" refuses when an OpenGLDriverName is there that this tool
does not know (it never overwrites someone else's ICD); "clear" refuses unless OpenGLDriverName is one this tool
writes, and then removes OpenGLDriverName, OpenGLVersion and OpenGLFlags and nothing else. After either, the
tool reads the output back itself: the three values are as intended (or gone) and every other value of the key
is byte for byte what it was. So set followed by clear gives back the key's values exactly.

Writes go through hivexsh in the lima VM with the main checkout's tools/win/hive-service.py helpers (staging,
refuses a dirty hive), the same way as tools/win/hive-umdname.py. IN is never modified. Nothing here touches a
phone.
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_GIT = subprocess.run(['git', '-C', _HERE, 'rev-parse', '--git-common-dir'], capture_output=True, text=True,
                      check=True).stdout.strip()
MAIN = os.environ.get('STARBITE_MAIN') or os.path.realpath(os.path.join(_HERE, _GIT, '..'))   # the main checkout
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)

CLASS = '{4d36e968-e325-11ce-bfc1-08002be10318}'
DEVICE = 'acpi\\strb0072'
NAMES = ('sbglicd.dll',)                                  # the ICD names this tool writes and may remove
OURS = ('OpenGLDriverName', 'OpenGLVersion', 'OpenGLFlags')
SEEN = OURS + ('OpenGLDriverNameWow', 'OpenGLVersionWow', 'OpenGLFlagsWow', 'UserModeDriverName')
TYPES = {1: 'REG_SZ', 4: 'REG_DWORD', 7: 'REG_MULTI_SZ'}


def text(d):
    return [s for s in d.decode('utf-16-le', 'replace').split('\0') if s]


def name_value(name, kind):
    if kind == 'sz':
        return (1, (name + '\0').encode('utf-16-le'))
    return (7, (name + '\0\0').encode('utf-16-le'))


def adapters(h):
    """[(key path, {value name: (type, data)})] for every display class instance of our adapter."""
    c = hs.control_set(h)
    base = f'{c}\\Control\\Class\\{CLASS}'
    out = []
    _, kb = h.find(base)
    for _, sk in h.subkeys(kb):
        v = {n: (t, d) for n, t, d in h.values(sk)}
        dev = v.get('MatchingDeviceId')
        if dev and text(dev[1])[:1] == [DEVICE]:
            out.append((f'{base}\\{sk["name"]}', v))
    return out


def icd_name(v):
    """The ICD file name in a key's values, None if there is no OpenGLDriverName, '?' if it is not a string."""
    if 'OpenGLDriverName' not in v:
        return None
    t, d = v['OpenGLDriverName']
    s = text(d) if t in (1, 7) else []
    return s[0].lower() if len(s) == 1 else '?'


def show(h):
    a = adapters(h)
    if not a:
        print('no Starbite display adapter (MatchingDeviceId acpi\\strb0072) in this hive')
    for k, v in a:
        print(k.split('\\', 1)[1] + ':')
        for n in SEEN:
            if n not in v:
                print(f'  {n:20} (not set)')
                continue
            t, d = v[n]
            shown = f'0x{struct.unpack("<I", d)[0]:08x}' if t == 4 and len(d) == 4 else repr(text(d))
            print(f'  {n:20} {TYPES.get(t, t)} {shown}')
        name = icd_name(v)
        print('  = ' + ('no OpenGL ICD: every OpenGL program gets Microsoft\'s software OpenGL 1.1' if name is None
                        else 'the Starbite ICD, for ARM64 and x64 programs' if name in NAMES
                        else 'an OpenGL ICD this tool does not know'))
    return a


def write(src, out, script):
    """Run a hivexsh script on a staged copy of src; the result becomes out. Returns the reread hive."""
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SYSTEM.{os.getpid()}.{os.urandom(4).hex()}')
    try:
        shutil.copyfile(src, work)
        if hs.vm_sha(work) != hs.sha(work):
            raise SystemExit('the VM does not see the staged hive as written; nothing changed')
        hs.hivexsh(work, script + 'commit\n')
        if hs.vm_sha(work) != hs.sha(work):
            raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
        shutil.copyfile(work, out)
    finally:
        for f in (work, work + '.hivexsh'):
            if os.path.exists(f):
                os.remove(f)
    hs.check(out)
    return hs.Hive(out)


def setval_script(key, vals):
    """hivexsh "setval" replaces every value of a key: emit all of them, byte for byte."""
    s = f'cd \\{key}\nsetval {len(vals)}\n'
    for n, t, d in vals:
        s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    return s


def main(a):
    if len(a) == 2 and a[1] == 'show':
        try:
            hs.check(a[0])
        except SystemExit as e:           # read-only: a dirty hive is still shown, with the warning
            print(f'({e})')
        show(hs.Hive(a[0]))
        return
    if len(a) < 3 or a[2] not in ('set', 'clear') or len(a) % 2 == 0:
        raise SystemExit(__doc__)
    src, out, cmd = a[0], a[1], a[2]
    opts = dict(zip(a[3::2], a[4::2]))
    if set(opts) - {'--name', '--flags', '--version', '--type'} or (cmd == 'clear' and opts):
        raise SystemExit(__doc__)
    if os.path.realpath(src) == os.path.realpath(out):
        raise SystemExit('IN and OUT are the same file: IN is never modified')
    name = opts.get('--name', NAMES[0]).lower()
    kind = opts.get('--type', 'multi')
    flags, version = int(opts.get('--flags', '3'), 0), int(opts.get('--version', '1'), 0)
    if name not in NAMES or kind not in ('multi', 'sz') or flags not in (1, 3) or not 0 <= version <= 0xffffffff:
        raise SystemExit(f'--name must be one of {NAMES}, --type multi or sz, --flags 1 or 3')

    hs.check(src)
    h = hs.Hive(src)
    t = adapters(h)
    if len(t) != 1:
        raise SystemExit(f'expected exactly one Starbite display adapter key, found {len(t)}: nothing written')
    key, v = t[0]
    have = icd_name(v)
    _, k1 = h.find(key)
    old = list(h.values(k1))                                  # [(name, type, data)] in the key's order
    rest = [(n, ty, d) for n, ty, d in old if n not in OURS]

    if cmd == 'set':
        if have is not None and have not in NAMES:
            raise SystemExit(f'the adapter already names an OpenGL ICD this tool does not know ({have}): nothing written')
        if have is None and any(n in v for n in OURS):
            raise SystemExit('OpenGLVersion / OpenGLFlags exist without an OpenGLDriverName: not ours, nothing written')
        want = {'OpenGLDriverName': name_value(name, kind), 'OpenGLVersion': hs.dword(version),
                'OpenGLFlags': hs.dword(flags)}
        new = rest + [(n, ty, d) for n, (ty, d) in want.items()]
    else:
        if have is None and not any(n in v for n in OURS):
            print('no OpenGL ICD values in this hive: nothing to clear, no output written')
            return
        if have not in NAMES:
            raise SystemExit(f'OpenGLDriverName is not one this tool writes ({have}): nothing removed')
        want = {}
        new = rest

    h2 = write(src, out, setval_script(key, new))
    # own read-back: our three values are as intended (or gone); every other value is byte for byte what it was
    t2 = adapters(h2)
    _, k2 = h2.find(key)
    got = list(h2.values(k2))
    ours2 = {n: (ty, d) for n, ty, d in got if n in OURS}
    rest2 = sorted((n, ty, d) for n, ty, d in got if n not in OURS)
    if len(t2) != 1 or t2[0][0] != key or ours2 != want or rest2 != sorted(rest) or len(got) != len(new):
        os.remove(out)
        raise SystemExit('own read-back disagrees; output removed')
    show(h2)
    print(f'{len(rest)} other values of the key unchanged; '
          + ('3 values written' if cmd == 'set' else f'{len(old) - len(rest)} values removed'))
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
