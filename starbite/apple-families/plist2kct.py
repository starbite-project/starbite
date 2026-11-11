#!/usr/bin/env python3
# SPDX-License-Identifier: APSL-2.0
"""Turn a family's Info.plist IOKitPersonalities into KernelConfigTables.cpp entries (XNU's OSUnserialize text).
    plist2kct.py Info.plist [--target osx|ios] [--skip NAME ...] [--only NAME ...] [--set NAME.KEY=VALUE ...]
Handles the "#if TARGET_OS_OSX" lines Apple's plists carry (Xcode preprocesses them with TargetConditionals.h).
Drops CFBundleIdentifier (a built-in personality belongs to the kernel). Prints C string lines ready to paste
inside gIOKernelConfigTables, one "{ ... }," per personality, and checks the text with a small OSUnserialize
parser (libkern/c++/OSUnserialize.y syntax: 'quoted' strings, NUMBER:bits, .true./.false., {k = v;}, (a, b))."""
import argparse
import plistlib
import re
import sys


def preprocess(text, target):
    defs = {'TARGET_OS_OSX': target == 'osx', 'TARGET_OS_IPHONE': target == 'ios', 'TARGET_OS_IOS': target == 'ios'}
    out, stack = [], []  # (parent active, this branch's condition)
    active = True
    for line in text.split('\n'):
        s = line.strip()
        m = re.match(r'#\s*if\s+(!?)\s*(\w+)', s)
        if m:
            v = defs.get(m.group(2), False)
            stack.append((active, not v if m.group(1) else v))
        elif re.match(r'#\s*else', s):
            parent, cond = stack.pop()
            stack.append((parent, not cond))
        elif re.match(r'#\s*endif', s):
            stack.pop()
        elif s.startswith('#'):
            raise ValueError('unsupported directive: ' + s)
        else:
            if active:
                out.append(line)
            continue
        active = all(p and c for p, c in stack) if stack else True
    return '\n'.join(out)


def q(s):
    return "'" + s.replace('\\', '\\\\').replace("'", "\\'").replace('"', '\\"') + "'"


def val(v):
    if isinstance(v, bool):
        return '.true.' if v else '.false.'
    if isinstance(v, int):
        return '%d:%d' % (v, 32 if 0 <= v < 2 ** 32 else 64)
    if isinstance(v, str):
        return q(v)
    if isinstance(v, dict):
        return '{ ' + ' '.join('%s = %s;' % (q(k), val(x)) for k, x in v.items()) + ' }'
    if isinstance(v, list):
        return '( ' + ', '.join(val(x) for x in v) + ' )'
    if isinstance(v, bytes):
        return '<' + v.hex() + '>'
    raise ValueError('unsupported plist value %r' % (v,))


# --- a small OSUnserialize checker (syntax only) ---
def check(text):
    toks = re.findall(r"\s*(?:'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"|\.true\.|\.false\.|0x[0-9a-f]+|\d+|[A-Za-z][A-Za-z0-9_.]*|<[0-9a-f\s]*>|[{}()=;:,])", text)
    if ''.join(toks).replace(' ', '').replace('\n', '') != re.sub(r'\s', '', text):
        raise ValueError('untokenizable text')
    toks = [t.strip() for t in toks]
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def take(x=None):
        t = peek()
        if x is not None and t != x:
            raise ValueError('expected %s got %s at token %d' % (x, t, pos[0]))
        pos[0] += 1
        return t

    def obj():
        t = peek()
        if t == '{':
            take('{')
            while peek() != '}':
                k = take()
                if not (k[0] in "'\"" or k[0].isalpha()):
                    raise ValueError('bad key ' + k)
                take('=')
                obj()
                take(';')
            take('}')
        elif t == '(':
            take('(')
            if peek() != ')':
                obj()
                while peek() == ',':
                    take(',')
                    obj()
            take(')')
        elif t and (t[0].isdigit()):
            take()
            if peek() == ':':
                take(':')
                take()
        elif t and (t[0] in "'\"<." or t[0].isalpha()):
            take()
        else:
            raise ValueError('unexpected %s' % t)
    obj()
    if pos[0] != len(toks):
        raise ValueError('trailing tokens')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('plist')
    ap.add_argument('--target', default='osx', choices=['osx', 'ios'])
    ap.add_argument('--skip', nargs='*', default=[])
    ap.add_argument('--only', nargs='*', default=None)
    a = ap.parse_args()
    d = plistlib.loads(preprocess(open(a.plist).read(), a.target).encode())
    entries = []
    for name, p in d['IOKitPersonalities'].items():
        if name in a.skip or (a.only is not None and name not in a.only):
            continue
        p = {k: v for k, v in p.items() if k != 'CFBundleIdentifier'}
        order = ['IOClass', 'IOProviderClass', 'IONameMatch', 'IOPropertyMatch', 'IOResourceMatch', 'IOMatchCategory',
                 'IOProbeScore']
        keys = [k for k in order if k in p] + sorted(k for k in p if k not in order)
        body = ['   {', '     /* %s */' % name] + ['     %s = %s;' % (q(k), val(p[k])) for k in keys] + ['   },']
        entries.append(body)
    text = '(' + '\n'.join(l for e in entries for l in e if not l.strip().startswith('/*')).rstrip(',') + ')'
    check(text)
    for e in entries:
        for l in e:
            if l.strip().startswith('/*'):
                print('    /* %s */' % l.strip()[3:-3].strip())
            else:
                print('    "%s"' % l.replace('\\', '\\\\').replace('"', '\\"'))
    print('    /* %d personalities, OSUnserialize syntax checked */' % len(entries))


if __name__ == '__main__':
    main()
