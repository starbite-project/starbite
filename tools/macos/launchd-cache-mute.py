#!/usr/bin/env python3
"""Mute launchd jobs in a binary launchd cache (/System/Library/xpc/launchd.plist) WITHOUT changing its size, so the
card's copy can be replaced in place (card-file-replace.py): every Mach service name of the named jobs gets its last
character changed ('X', or 'Z' if it already ends in X). A client's lookup of such a service then fails at once,
instead of waiting for a job launchd can never run (2026-11-13, S2w: /usr/libexec/remindd is missing from the root,
launchd still registered com.apple.remindd, ReminderKit's synchronous XPC to it never returned, so
UserAccountUpdater never exited and loginwindow never launched Dock, Finder or SystemUIServer).

  tools/macos/launchd-cache-mute.py IN OUT --label com.apple.remindd [--label com.apple.bluetoothd ...]

A binary plist stores each distinct string once, so the edit renames every use of a service name in the whole cache.
The result is parsed again and compared with the original: if any value changes outside the named jobs, nothing is
written. Prints both sha256s and every renamed name.
"""
import argparse
import hashlib
import plistlib
import struct
import sys


def objects(data):
    """(offset of each object, ref size, top) from a bplist00 trailer."""
    if data[:8] != b'bplist00':
        sys.exit('not a binary plist')
    off_size, ref_size, n, top, table = struct.unpack('>6xBBQQQ', data[-32:])
    offs = [int.from_bytes(data[table + i * off_size:table + (i + 1) * off_size], 'big') for i in range(n)]
    return offs, ref_size, top


def ascii_string(data, o):
    """(payload offset, length) of an ASCII string object at o, else None."""
    m = data[o]
    if m >> 4 != 5:
        return None
    n, p = m & 0xf, o + 1
    if n == 0xf:
        im = data[p]
        if im >> 4 != 1:
            return None
        w = 1 << (im & 0xf)
        n, p = int.from_bytes(data[p + 1:p + 1 + w], 'big'), p + 1 + w
    return p, n


def diff(a, b, path, out):
    if type(a) is not type(b):
        out.append(path)
    elif isinstance(a, dict):
        if len(a) != len(b):
            out.append(path)
            return
        for (ka, va), (kb, vb) in zip(sorted(a.items()), sorted(b.items())):
            if ka != kb:
                out.append(path + (ka,))
            diff(va, vb, path + (kb,), out)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append(path)
            return
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, path + (i,), out)
    elif a != b:
        out.append(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inp')
    ap.add_argument('out')
    ap.add_argument('--label', action='append', required=True)
    a = ap.parse_args()
    data = bytearray(open(a.inp, 'rb').read())
    old = plistlib.loads(bytes(data))
    jobs = {}
    for section, entries in old.items():
        if isinstance(entries, dict):
            for key, job in entries.items():
                if isinstance(job, dict) and job.get('Label') in a.label:
                    jobs[(section, key)] = job
    found = {j['Label'] for j in jobs.values()}
    if found != set(a.label):
        sys.exit(f'not in this cache: {" ".join(sorted(set(a.label) - found))}')
    names = {s for j in jobs.values() for s in (j.get('MachServices') or {})}
    if not names:
        sys.exit('the named jobs have no MachServices')
    offs, _, _ = objects(data)
    renamed = {}
    for o in offs:
        s = ascii_string(data, o)
        if not s:
            continue
        p, n = s
        text = data[p:p + n].decode('ascii', 'replace')
        if text in names:
            last = ord('Z') if data[p + n - 1] == ord('X') else ord('X')
            data[p + n - 1] = last
            renamed[text] = data[p:p + n].decode()
    if set(renamed) != names:
        sys.exit(f'service names not stored as ASCII strings: {" ".join(sorted(names - set(renamed)))}')
    new = plistlib.loads(bytes(data))
    changes = []
    diff(old, new, (), changes)
    inside = [c for c in changes if len(c) >= 2 and (c[0], c[1]) in jobs]
    outside = [c for c in changes if c not in inside]
    for (section, key), job in jobs.items():
        ms = new[section][key].get('MachServices') or {}
        if len(ms) != len(job['MachServices']) or set(ms) & names:
            sys.exit(f'{job["Label"]}: renamed services collide')
    if outside:
        sys.exit('values outside the named jobs would change too, refusing:\n  ' + '\n  '.join(map(str, outside[:20])))
    open(a.out, 'wb').write(data)
    print(f'{len(jobs)} job(s): ' + ', '.join(f'{j["Label"]} ({k[1]})' for k, j in jobs.items()))
    for t in sorted(renamed):
        print(f'  {t} -> {renamed[t]}')
    print(f'{len(changes)} value(s) changed, all inside those jobs; size {len(data)} (unchanged)')
    print(f'in  sha256 {hashlib.sha256(open(a.inp, "rb").read()).hexdigest()}')
    print(f'out sha256 {hashlib.sha256(bytes(data)).hexdigest()}')


if __name__ == '__main__':
    main()
