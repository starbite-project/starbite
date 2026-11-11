#!/usr/bin/env python3
"""ADT v2: today's ADT plus everything tools/board/starlte.board describes (docs/APPLE-S9-DRIVERS.md H1).
    tools/board/mkadt-board.py BASE.adt OUT.adt [--board tools/board/starlte.board] [--topology m1|amp]
    tools/board/mkadt-board.py --check OUT.adt [--board ...]          parse back, dump, verify
BASE is an ADT from tools/mkadt_starlte.py (e.g. out/starlte-iphone11-1792.adt, iOS 26 identity, 8 CPUs).
Kept as they are, because today's kernel reads them: /arm-io/gic, /arm-io/wdt, /arm-io/disp0, /herolte-ufs,
/pram, /a5xelte-fbcon, /buttons, /product, /chosen's identity. Added:
  - root compatible: the identity entries and samsung,a5xelte (today's SweetPlatformExpert IONameMatch) stay;
    AppleARM goes; the board name (starbite,s9) is LAST.
  - /arm-io/<block> for every [[block]] except gic: compatible, reg (relative to [soc].base), interrupts
    (INTIDs), interrupt-parent (the gic's phandle), starbite,driver. The gic node gains ARM,gicv2 and a phandle.
  - /cpus/cpuN: starbite,mpidr, starbite,psci-target, starbite,cluster, starbite,cluster-kind. With --topology
    amp (M2 onwards: MAX_CPU_CLUSTERS 2 kernels only) also cluster-id = the board cluster and cluster-type
    'E' / 'P'; the default m1 leaves cluster-id/-type 0, which today's kernel (one cluster) needs.
  - /psci (method smc, function ids), /chosen boot-uuid + starbite,board + starbite,page-size,
    /starbite/firmware (sbfw, pram, pmsg, loader-log, framebuffer ranges).
--check: the result parses with ref/ios-herolte/tools/adt.py and dumps; every block's register ranges are
distinct and do not overlap; every arm-io block node is present once; prints node and block counts."""
import argparse
import struct
import sys
import tomllib
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'ref/ios-herolte/tools'))
import adt     # noqa: E402

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'
cstrs = lambda l: b''.join(cstr(s) for s in l)


def mknode(name, props=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    return n


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for c in node.children:
        if c.name == name:
            return c
    return None


def phandles(node, acc):
    v = node.prop('AAPL,phandle')
    if v:
        acc.append(struct.unpack('<I', v[:4])[0])
    for c in node.children:
        phandles(c, acc)
    return acc


def ranges(block):
    r = block['reg']
    assert len(r) % 2 == 0 and r, f"{block['name']}: reg needs addr,size pairs"
    return [(r[i], r[i + 1]) for i in range(0, len(r), 2)]


def check_board(board):
    """Every block's ranges distinct and non-overlapping; names unique. Returns problems."""
    bad, seen = [], {}
    names = [b['name'] for b in board['block']]
    for n in {n for n in names if names.count(n) > 1}:
        bad.append(f'block name {n} twice')
    spans = []
    for b in board['block']:
        for a, s in ranges(b):
            if a in seen:
                bad.append(f"{hex(a)} in {seen[a]} and {b['name']}")
            seen[a] = b['name']
            spans.append((a, a + s, b['name']))
    spans.sort()
    for (a0, e0, n0), (a1, e1, n1) in zip(spans, spans[1:]):
        if a1 < e0:
            bad.append(f'{n0} [{a0:#x}+{e0 - a0:#x}] overlaps {n1} at {a1:#x}')
    soc = board['soc']
    for a, e, n in spans:
        if not (soc['base'] <= a and e <= soc['base'] + soc['size']):
            bad.append(f'{n} at {a:#x} outside arm-io ranges')
    return bad


def build(base, board, topology):
    root, _ = adt.parse(base)
    b = board['board']
    # root compatible
    comp = [c.decode() for c in root.prop('compatible').split(b'\0') if c]
    comp = [c for c in comp if c not in ('AppleARM', b['name'])] + [b['name']]
    set_prop(root, 'compatible', cstrs(comp))
    used = phandles(root, [])
    nextph = [max(used + [0]) + 1]

    def new_phandle(node):
        v = node.prop('AAPL,phandle')
        if v:
            return struct.unpack('<I', v[:4])[0]
        p = nextph[0]
        nextph[0] += 1
        set_prop(node, 'AAPL,phandle', u32(p))
        return p

    arm_io = child(root, 'arm-io')
    soc = board['soc']
    gic = child(arm_io, 'gic')
    gic_ph = new_phandle(gic)
    gicb = next(x for x in board['block'] if x['name'] == 'gic')
    set_prop(gic, 'compatible', cstrs(gicb['compatible']))
    set_prop(gic, 'starbite,driver', cstr(gicb['driver']))
    for blk in board['block']:
        if blk['name'] == 'gic':
            continue
        assert child(arm_io, blk['name']) is None, f"arm-io already has {blk['name']}"
        reg = b''.join(u64(a - soc['base'], s) for a, s in ranges(blk))
        props = [('compatible', cstrs(blk['compatible'])), ('reg', reg),
                 ('starbite,driver', cstr(blk['driver']))]
        for k, v in blk.get('u64', {}).items():
            props.append((k, u64(*(v if isinstance(v, list) else [v]))))
        for k, v in blk.get('u32', {}).items():
            props.append((k, u32(*(v if isinstance(v, list) else [v]))))
        if blk['spi']:
            props += [('interrupts', u32(*(s + 32 for s in blk['spi']))),
                      ('interrupt-parent', u32(gic_ph))]
        n = mknode(blk['name'], props)
        new_phandle(n)
        arm_io.children.append(n)
    # cpus
    cpus = child(root, 'cpus')
    kinds = {c['id']: c['kind'] for c in board['cluster']}
    for c in board['cpu']:
        n = child(cpus, f"cpu{c['logical']}")
        assert n is not None, f"base ADT has no cpu{c['logical']} (build it with mkadt_starlte.py --big)"
        set_prop(n, 'starbite,mpidr', u64(c['mpidr']))
        set_prop(n, 'starbite,psci-target', u64(c['psci']))
        set_prop(n, 'starbite,cluster', u32(c['cluster']))
        set_prop(n, 'starbite,cluster-kind', cstr(kinds[c['cluster']]))
        if topology == 'amp':
            set_prop(n, 'cluster-id', u32(c['cluster']))
            set_prop(n, 'cluster-type', u32(ord(kinds[c['cluster']])))
    # psci
    ps = board['psci']
    assert child(root, 'psci') is None
    root.children.append(mknode('psci', [
        ('compatible', cstrs(['starbite,psci', 'arm,psci'])), ('method', cstr(ps['method'])),
        ('cpu_suspend', u32(ps['cpu_suspend'])), ('cpu_off', u32(ps['cpu_off'])),
        ('cpu_on', u32(ps['cpu_on'])), ('version', cstr('0.1'))]))
    # chosen
    chosen = child(root, 'chosen')
    ident = root.prop('model').rstrip(b'\0').decode()
    set_prop(chosen, 'boot-uuid', cstr(str(uuid.uuid5(uuid.NAMESPACE_URL, f"{b['name']}/{ident}")).upper()))
    set_prop(chosen, 'starbite,board', cstr(b['name']))
    set_prop(chosen, 'starbite,page-size', u32(b['page_size']))
    set_prop(chosen, 'starbite,monitor', cstr(b['monitor']))
    # firmware services
    mem = board['memory']
    fw = mknode('firmware', [('compatible', cstr('starbite,firmware'))] +
                [(f'{k}-range', u64(*mem[k])) for k in ('sbfw', 'pram', 'pmsg', 'loader_log', 'framebuffer')])
    assert child(root, 'starbite') is None
    sb = mknode('starbite', [('compatible', cstr('starbite,services'))])
    sb.children.append(fw)
    root.children.append(sb)
    return root


def count(node):
    return 1 + sum(count(c) for c in node.children)


def check(path, board):
    blob = Path(path).read_bytes()
    root, end = adt.parse(blob)
    assert end == len(blob), f'{path}: {len(blob) - end} trailing bytes'
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()) as out:
        adt.dump(root)
    bad = check_board(board)
    arm_io = child(root, 'arm-io')
    names = [c.name for c in arm_io.children]
    for blk in board['block']:
        if names.count(blk['name']) != 1:
            bad.append(f"arm-io/{blk['name']}: {names.count(blk['name'])} nodes")
    comp = root.prop('compatible').split(b'\0')
    if comp[-2].decode() != board['board']['name'] or b'AppleARM' in comp:
        bad.append(f'root compatible ends {comp[-2]!r}')
    print(f"{path}: {len(blob)} bytes, {count(root)} nodes, {len(arm_io.children)} arm-io nodes, "
          f"{len(board['block'])} board blocks, dump {len(out.getvalue().splitlines())} lines")
    for x in bad:
        print('  PROBLEM:', x)
    print('check:', 'FAIL' if bad else 'PASS')
    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--board', default=str(ROOT / 'tools/board/starlte.board'))
    ap.add_argument('--topology', choices=('m1', 'amp'), default='m1')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    board = tomllib.loads(Path(a.board).read_text())
    if a.check:
        sys.exit(0 if all(check(p, board) for p in a.paths) else 1)
    if len(a.paths) != 2:
        ap.error('BASE.adt OUT.adt')
    bad = check_board(board)
    if bad:
        sys.exit('board: ' + '; '.join(bad))
    root = build(adt.load(a.paths[0]), board, a.topology)
    blob = adt.serialize(root)
    Path(a.paths[1]).write_bytes(blob)
    print(f'{a.paths[1]}: {len(blob)} bytes (topology {a.topology})')
    sys.exit(0 if check(a.paths[1], board) else 1)


if __name__ == '__main__':
    main()
