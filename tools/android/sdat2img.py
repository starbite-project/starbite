#!/usr/bin/env python3
"""Rebuild a raw partition image from an Android block-based OTA:
    sdat2img.py X.transfer.list X.new.dat OUT.img
Only the "new" and "zero"/"erase" commands of transfer list versions 1-4.
"""
import sys


def ranges(s):
    v = [int(x) for x in s.split(',')]
    assert v[0] == len(v) - 1
    return [(v[i], v[i + 1]) for i in range(1, len(v), 2)]


def main():
    tl, dat, out = sys.argv[1:4]
    lines = open(tl).read().splitlines()
    version, total = int(lines[0]), int(lines[1])
    cmds = lines[4:] if version >= 2 else lines[2:]
    bs = 4096
    with open(dat, 'rb') as src, open(out, 'wb') as dst:
        dst.truncate(total * bs)
        for c in cmds:
            op, _, arg = c.partition(' ')
            if op == 'new':
                for a, b in ranges(arg):
                    dst.seek(a * bs)
                    n = (b - a) * bs
                    while n:
                        chunk = src.read(min(n, 1 << 24))
                        assert chunk, 'new.dat ended early'
                        dst.write(chunk)
                        n -= len(chunk)
            elif op in ('zero', 'erase'):
                pass                     # truncate() already left zeros
            else:
                sys.exit(f'unsupported command {op}')
    print(f'{out}: {total} blocks ({total * bs / 2**20:.0f} MiB)')


if __name__ == '__main__':
    main()
