#!/usr/bin/env python3
"""The x86/x64 emulator fix for this phone: ScaleRdtsc = 0 in an OFFLINE SOFTWARE hive (docs/WIN11-24H2-A55.md,
"The emulator crash: proven").

  hive-scalerdtsc.py IN OUT set       ScaleRdtsc = 0 (REG_DWORD) for both emulators
  hive-scalerdtsc.py IN OUT remove    delete the two values again (the keys stay, empty: harmless, see below)
  hive-scalerdtsc.py IN show          what is there now

Why: xtajit64.dll (x64) and xtajit.dll (x86, WoW64) read CNTFRQ_EL0 themselves when they start
(xtajit64 +0x30C5C, xtajit +0x45A40) to scale the emulated TSC, and divide by it: on this phone the register is 0
(S-Boot never sets it; only EL3 could), so every emulated process dies with 0xC0000094 at the divide-by-zero trap
(brk #0xF004, xtajit64 +0x30C70 = the Application log's offset). Both read the setting "ScaleRdtsc" first, from
  HKLM\\SOFTWARE\\Microsoft\\Wow64\\amd64\\xtajit64[\\<app>]   (x64)
  HKLM\\SOFTWARE\\Microsoft\\Wow64\\x86\\xtajit[\\<app>]       (x86)
as a REG_DWORD (type 4, 4 bytes, NtQueryValueKey); 0 returns before the division (no TSC scaling: RDTSC then counts
the raw 26 MHz generic timer). Wow64\\amd64 doesn't exist on this install: ntdll reads that key only for the x64
emulator's DLL name (a named value, then the default value) and falls back to "xtajit64.dll" when either is missing,
so an amd64 key without values changes nothing there.

Writes through hivexsh in the lima VM like hive-service.py (same staging and checks); IN is never modified.
Only edit a hive from a cleanly shut-down Windows (both sequence numbers equal).
"""
import importlib.util
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(HERE, 'hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)

X86 = 'Microsoft\\Wow64\\x86\\xtajit'
X64 = 'Microsoft\\Wow64\\amd64\\xtajit64'


def show(h):
    for key in (X86, X64):
        if hs.exists(h, key):
            _, k = h.find(key)
            v = {n: (t, d) for n, t, d in h.values(k)}
            print(f'{key}: ' + (f'ScaleRdtsc type {v["ScaleRdtsc"][0]} = {v["ScaleRdtsc"][1].hex()}' if 'ScaleRdtsc' in v
                                else 'no ScaleRdtsc') + f' ({len(v)} values)')
        else:
            print(f'{key}: no key')


def ensure(h, path):
    """hivexsh lines creating every missing key along path (SOFTWARE-relative)."""
    s, cur = '', ''
    for part in path.split('\\'):
        nxt = f'{cur}\\{part}' if cur else part
        if not hs.exists(h, nxt):
            s += f'cd \\{cur}\nadd {part}\n'
        cur = nxt
    return s


def main(a):
    if len(a) == 2 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]))
        return
    if len(a) != 3 or a[2] not in ('set', 'remove'):
        raise SystemExit(__doc__)
    src, out, cmd = a
    hs.check(src)
    h = hs.Hive(src)
    if not hs.exists(h, 'Microsoft\\Wow64\\x86'):
        raise SystemExit('no Microsoft\\Wow64\\x86: is this a SOFTWARE hive of the ARM64 install?')
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SOFTWARE.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    s = ''
    for key in (X86, X64):
        have = hs.exists(h, key)
        if cmd == 'set':
            s += ensure(h, key) + hs.setvals(h if have else None, key, {'ScaleRdtsc': hs.dword(0)})
        elif have:
            _, k = h.find(key)
            rest = [(n, t, d) for n, t, d in h.values(k) if n != 'ScaleRdtsc']
            s += f'cd \\{key}\nsetval {len(rest)}\n' + ''.join(
                ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
                for n, t, d in rest)
    hs.hivexsh(work, s + 'commit\n')
    # second, independent reader: hivexsh lists the values
    for key in (X86, X64):
        open(work + '.ls', 'w').write(f'cd \\{key}\nlsval\n')
        r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'hivexsh', '-f', work + '.ls', work],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True)
        listed = 'scalerdtsc' in r.stdout.lower()
        if listed != (cmd == 'set'):
            raise SystemExit(f'hivexsh read-back of {key}: ScaleRdtsc {"listed" if listed else "missing"}; {out} not written')
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh', work + '.ls'):
        if os.path.exists(f):
            os.remove(f)
    hs.check(out)
    show(hs.Hive(out))
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
