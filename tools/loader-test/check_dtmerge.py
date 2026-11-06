#!/usr/bin/env python3
"""Check what shim/starlte/dtmerge.c made (host_test's merged-*.dtb) against its inputs, with an
independent FDT parser:

  check_dtmerge.py OUTDIR SBOOT_FDT   (OUTDIR: base-TAG.dtb, merged-TAG.dtb from host_test)

For each merge: /chosen holds exactly S-Boot's properties (bootargs = S-Boot's + " " + the extra
host_test passed, when there was one); the memory nodes are exactly S-Boot's; root properties
S-Boot has and the base lacks are there; every other node and property is the base's, byte for
byte, in the base's order; the header is consistent. Also the patched Linux DTB: only INFORM3's
value changed.
"""
import struct
import sys
from pathlib import Path

EXTRA = {'un1ca': 'androidboot.selinux=permissive sb.test=1', 'un1ca-noextra': '',
         'lineage': 'androidboot.selinux=permissive'}
fails = 0
checks = 0


def check(c, what):
    global fails, checks
    checks += 1
    if not c:
        fails += 1
        print('FAIL', what)


def parse(b):
    magic, total, ostruct, ostr, orsv, ver, lcv, cpu, sstr, sstruct = struct.unpack_from('>10I', b, 0)
    assert magic == 0xd00dfeed and total == len(b), 'header'
    nodes, order, path, p = {}, [], [], ostruct
    end = ostruct + sstruct
    while p < end:
        tok, = struct.unpack_from('>I', b, p)
        p += 4
        if tok == 1:
            e = b.index(b'\0', p)
            path.append(b[p:e].decode())
            p = (e + 4) & ~3
            key = '/'.join(path) or '/'
            nodes[key] = []
            order.append(key)
        elif tok == 2:
            path.pop()
        elif tok == 3:
            ln, no = struct.unpack_from('>II', b, p)
            e = b.index(b'\0', ostr + no)
            nodes['/'.join(path) or '/'].append((b[ostr + no:e].decode(), b[p + 8:p + 8 + ln]))
            p = (p + 8 + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
        else:
            raise ValueError(f'token {tok:#x}')
    assert p == end, 'struct size'
    rsv, q = [], orsv
    while True:
        a, s = struct.unpack_from('>QQ', b, q)
        q += 16
        if not a and not s:
            break
        rsv.append((a, s))
    return {'nodes': nodes, 'order': order, 'rsv': rsv, 'cpu': cpu, 'ver': ver, 'lcv': lcv}


def is_mem(n):
    top = n.split('/')
    return len(top) == 2 and (top[1] == 'memory' or top[1].startswith('memory@'))


def main():
    out, sbp = Path(sys.argv[1]), sys.argv[2]
    sb = parse(Path(sbp).read_bytes())
    tags = [t for t in EXTRA if (out / f'merged-{t}.dtb').exists()]
    check('un1ca' in tags, 'merged-un1ca.dtb exists')
    for t in tags:
        base, m = parse((out / f'base-{t}.dtb').read_bytes()), parse((out / f'merged-{t}.dtb').read_bytes())
        check(m['ver'] == 17 and m['lcv'] == 16 and m['cpu'] == base['cpu'] and m['rsv'] == base['rsv'],
              f'{t}: header and reservations')
        # /chosen = S-Boot's
        want = list(sb['nodes']['/chosen'])
        if EXTRA[t]:
            want = [(n, v[:-1] + b' ' + EXTRA[t].encode() + b'\0' if n == 'bootargs' else v) for n, v in want]
        check(m['nodes']['/chosen'] == want, f'{t}: /chosen is S-Boot\'s (+ the extra bootargs)')
        # memory = S-Boot's
        sm = {n: v for n, v in sb['nodes'].items() if is_mem(n)}
        mm = {n: v for n, v in m['nodes'].items() if is_mem(n)}
        check(sm == mm and len(mm) == 4, f'{t}: the 4 memory nodes are S-Boot\'s ({sorted(mm)})')
        check(not any(is_mem(n) for n in base['nodes']), f'{t}: base had no memory nodes')
        # root: base's properties, in order, then S-Boot's that the base lacks
        bnames = {n for n, _ in base['nodes']['/']}
        added = [(n, v) for n, v in sb['nodes']['/'] if n not in bnames]
        check(m['nodes']['/'] == base['nodes']['/'] + added, f'{t}: root = base + {[n for n, _ in added]}')
        check(('model_info-system_rev', b'026\0') in added, f'{t}: model_info-system_rev carried over')
        # everything else: identical, same order
        rest_b = [n for n in base['order'] if n not in ('/', '/chosen') and not is_mem(n)]
        rest_m = [n for n in m['order'] if n not in ('/', '/chosen') and not is_mem(n)]
        check(rest_b == rest_m, f'{t}: same nodes in the same order ({len(rest_b)})')
        diff = [n for n in rest_b if base['nodes'][n] != m['nodes'].get(n)]
        check(not diff, f'{t}: every other node byte for byte ({diff[:3]})')
        check(m['order'].index('/chosen') == base['order'].index('/chosen'), f'{t}: /chosen where the base had it')
        print(f'{t}: {len(m["order"])} nodes, chosen {len(want)} properties, bootargs '
              f'{len(dict(want)["bootargs"])} bytes')
    lx = out / 'linux-patched.dtb'
    if lx.exists():
        p = parse(lx.read_bytes())
        regs = dict(p['nodes']['/clock-gates'])['s9p,set-registers']
        cells = struct.unpack(f'>{len(regs) // 4}I', regs)
        check(cells[:4] == (0x14060808, 0x12345678, 0x1406080c, 0x12345670), 'Linux DTB: INFORM3 now normal')
    print(f'check_dtmerge: {checks} checks, {fails} failed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
