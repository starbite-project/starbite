#!/usr/bin/env python3
"""Merge keys into a copy of an iOS preferences plist pulled from the card root (or start an empty one).
    ios26-prefs-tune.py IN.plist|- OUT.plist KEY=VALUE ...      VALUE: true/false, an integer, or text
Writes a binary plist; prints old -> new for every key. Used by tools/ios26-root-tune.sh for
com.apple.Accessibility (Reduce Motion, Reduce Transparency: no Liquid Glass blur / motion on the Mali path)."""
import plistlib
import sys


def value(v):
    if v.lower() in ('true', 'yes'):
        return True
    if v.lower() in ('false', 'no'):
        return False
    try:
        return int(v)
    except ValueError:
        return v


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    d = plistlib.load(open(src, 'rb')) if src != '-' else {}
    for kv in sys.argv[3:]:
        k, _, v = kv.partition('=')
        old = d.get(k, '(unset)')
        d[k] = value(v)
        print(f'{k}: {old!r} -> {d[k]!r}')
    with open(dst, 'wb') as f:
        plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)


if __name__ == '__main__':
    main()
