#!/usr/bin/env python3
"""Read a compiled Starbite DTB back and check what the loader and the kernel
rely on (run by build-kernel.sh on linux.dtb and linux-old.dtb; exits non-zero
on any miss). No libfdt needed.

    check-dtb.py [--bootargs STR] [--image Image] [--legacy-cma] FILE.dtb

/chosen (the loader edits it in place, shim/starlte/xnu_boot.c boot_linux):
  - bootargs is there and equals --bootargs (mk-starlte-dts.py --print-bootargs),
    with maxcpus=8 and s9p_vendor_m3_defaults; with a 511-byte entry cmdline
    appended it still fits arm64's 2048-byte COMMAND_LINE_SIZE
  - linux,initrd-start, linux,initrd-end and starbite,rtc-epoch are 8 bytes
    each (the loader overwrites them, it cannot add or grow them)
  - every /chosen property comes before its first subnode (framebuffer@...);
    the loader's property lookup stops at the first subnode
CMA (reserved-memory, compatible shared-dma-pool + linux,cma-default):
  - inside one memory bank, below 4 GB (DECON's fetch address is 32 bits),
    4 MiB aligned
  - no overlap with any other reserved-memory node or /chosen framebuffer
  - no overlap with the loader's Linux window 0x92000000-0xaf800000 (Image from
    0x92080000, initrd at 0xa8000000, edit FDT 0xad000000, DTB 0xae000000,
    UFS DMA 0xaf000000). --legacy-cma (the rollback DTB) allows the Image part
    and instead needs --image: Image + BSS must end at or below the CMA base.
  - no reserved-memory node inside 0x92000000-0xaf000000 at all
The DTB must be under 2 MiB (the loader's limit).
"""
import argparse
import struct
import sys

FDT_MAGIC = 0xd00dfeed
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9
RAM_BASE, OS_RD, EDIT_FDT, LINUX_DTB, UFS_DMA, WINDOW_END = (
    0x92000000, 0xa8000000, 0xad000000, 0xae000000, 0xaf000000, 0xaf800000)
CMDLINE_SIZE, ENTRY_CMDLINE_MAX = 2048, 511
FOUR_GB, CMA_ALIGN = 1 << 32, 4 << 20


class Node:
    def __init__(self, name, parent):
        self.name, self.parent = name, parent
        self.props, self.children = {}, []
        self.prop_after_child = []

    def path(self):
        return '/' if not self.parent else (self.parent.path().rstrip('/') + '/' + self.name)

    def cells(self, which, default):
        v = self.props.get(which)
        return struct.unpack('>I', v)[0] if v and len(v) == 4 else default


def parse(blob):
    magic, total, off_struct, off_strings = struct.unpack_from('>IIII', blob, 0)
    if magic != FDT_MAGIC:
        sys.exit('check-dtb: not an FDT (bad magic)')
    if total != len(blob):
        sys.exit(f'check-dtb: header totalsize {total} != file size {len(blob)}')
    pos, root, cur = off_struct, None, None
    while True:
        tok = struct.unpack_from('>I', blob, pos)[0]
        pos += 4
        if tok == BEGIN_NODE:
            end = blob.index(b'\0', pos)
            node = Node(blob[pos:end].decode(), cur)
            pos = (end + 4) & ~3
            if cur is None:
                root = node
            else:
                cur.children.append(node)
            cur = node
        elif tok == END_NODE:
            cur = cur.parent
        elif tok == PROP:
            ln, nameoff = struct.unpack_from('>II', blob, pos)
            pos += 8
            name = blob[off_strings + nameoff:blob.index(b'\0', off_strings + nameoff)].decode()
            cur.props[name] = blob[pos:pos + ln]
            if cur.children:
                cur.prop_after_child.append(name)
            pos = (pos + ln + 3) & ~3
        elif tok == NOP:
            continue
        elif tok == END:
            return root
        else:
            sys.exit(f'check-dtb: bad token {tok:#x} at {pos - 4:#x}')


def regs(node):
    p = node.parent
    ac, sc = p.cells('#address-cells', 2), p.cells('#size-cells', 1)
    raw = node.props.get('reg', b'')
    step = 4 * (ac + sc)
    out = []
    for i in range(0, len(raw) - step + 1, step):
        w = struct.unpack_from(f'>{ac + sc}I', raw, i)
        a = s = 0
        for x in w[:ac]:
            a = a << 32 | x
        for x in w[ac:]:
            s = s << 32 | x
        out.append((a, s))
    return out


def walk(node):
    yield node
    for c in node.children:
        yield from walk(c)


def overlap(a, b):
    return a[0] < b[0] + b[1] and b[0] < a[0] + a[1]


def rng(r):
    return f'{r[0]:#x}-{r[0] + r[1]:#x}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bootargs')
    ap.add_argument('--image', help='the arm64 Image this DTB is booted with (header image_size)')
    ap.add_argument('--legacy-cma', action='store_true')
    ap.add_argument('dtb')
    a = ap.parse_args()
    blob = open(a.dtb, 'rb').read()
    root = parse(blob)
    bad, info = [], []

    if len(blob) >= 2 << 20:
        bad.append(f'DTB is {len(blob)} bytes, the loader takes under 2 MiB')

    chosen = next((c for c in root.children if c.name == 'chosen'), None)
    if not chosen:
        sys.exit('check-dtb: no /chosen')
    ba = chosen.props.get('bootargs')
    if ba is None:
        bad.append('/chosen/bootargs missing (the loader refuses the DTB)')
        cmd = ''
    else:
        cmd = ba.rstrip(b'\0').decode()
        info.append(f'bootargs ({len(cmd)} chars): {cmd}')
        for arg in ('maxcpus=8', 's9p_vendor_m3_defaults'):
            if arg not in cmd.split():
                bad.append(f'bootargs lack {arg}')
        if a.bootargs is not None and cmd != a.bootargs:
            bad.append(f'bootargs {cmd!r} != expected {a.bootargs!r}')
        if len(cmd) + 1 + ENTRY_CMDLINE_MAX >= CMDLINE_SIZE:
            bad.append(f'bootargs ({len(cmd)}) + a full entry cmdline overflow {CMDLINE_SIZE}')
    for name in ('linux,initrd-start', 'linux,initrd-end', 'starbite,rtc-epoch'):
        v = chosen.props.get(name)
        if v is None:
            bad.append(f'/chosen/{name} missing (the loader cannot add properties)')
        elif len(v) != 8:
            bad.append(f'/chosen/{name} is {len(v)} bytes, the loader writes a u64')
        else:
            info.append(f'{name} = {struct.unpack(">Q", v)[0]:#x} (8 bytes)')
    if chosen.prop_after_child:
        bad.append(f'/chosen properties after a subnode: {chosen.prop_after_child}')

    banks = []
    for n in root.children:
        if n.props.get('device_type', b'').rstrip(b'\0') == b'memory':
            banks += regs(n)
    if not banks:
        bad.append('no memory node')
    resv = next((c for c in root.children if c.name == 'reserved-memory'), None)
    if not resv:
        sys.exit('check-dtb: no /reserved-memory')
    nodes = [(c.name, r) for c in resv.children for r in regs(c)]
    for n in walk(chosen):
        if n is not chosen and n.name.startswith('framebuffer'):
            nodes += [(f'chosen/{n.name}', r) for r in regs(n)]
    cmas = [c for c in resv.children
            if b'shared-dma-pool' in c.props.get('compatible', b'') and 'linux,cma-default' in c.props]
    if len(cmas) != 1:
        sys.exit(f'check-dtb: {len(cmas)} default CMA nodes, expected 1')
    cma_node = cmas[0]
    (cma,) = regs(cma_node)
    info.append(f'CMA {cma_node.name}: {rng(cma)} ({cma[1] >> 20} MiB)')

    if not any(b[0] <= cma[0] and cma[0] + cma[1] <= b[0] + b[1] for b in banks):
        bad.append(f'CMA {rng(cma)} is not inside one memory bank ({", ".join(map(rng, banks))})')
    if cma[0] + cma[1] > FOUR_GB:
        bad.append(f'CMA {rng(cma)} reaches above 4 GB')
    if cma[0] % CMA_ALIGN or cma[1] % CMA_ALIGN:
        bad.append(f'CMA {rng(cma)} is not 4 MiB aligned')
    for name, r in nodes:
        if name != cma_node.name and overlap(cma, r):
            bad.append(f'CMA {rng(cma)} overlaps {name} {rng(r)}')
    window = (OS_RD, WINDOW_END - OS_RD) if a.legacy_cma else (RAM_BASE, WINDOW_END - RAM_BASE)
    if overlap(cma, window):
        bad.append(f'CMA {rng(cma)} overlaps the loader window {rng(window)}')
    for name, r in nodes:
        if name != cma_node.name and overlap(r, (RAM_BASE, UFS_DMA - RAM_BASE)):
            bad.append(f'{name} {rng(r)} lies in the loader window {RAM_BASE:#x}-{UFS_DMA:#x}')

    if a.image:
        hdr = open(a.image, 'rb').read(64)
        text_offset, image_size = struct.unpack_from('<QQ', hdr, 8)
        if hdr[56:60] != b'ARM\x64':
            bad.append(f'{a.image}: not an arm64 Image')
        end = RAM_BASE + text_offset + image_size
        info.append(f'Image {RAM_BASE + text_offset:#x}-{end:#x} (text_offset {text_offset:#x}, '
                    f'image_size {image_size >> 20} MiB incl. BSS)')
        if end > OS_RD:
            bad.append(f'Image + BSS end {end:#x} runs into the initrd at {OS_RD:#x}')
        if overlap(cma, (RAM_BASE, end - RAM_BASE)):
            bad.append(f'Image + BSS {RAM_BASE:#x}-{end:#x} overlap CMA {rng(cma)}')
    elif a.legacy_cma:
        bad.append('--legacy-cma needs --image (Image + BSS must end below the CMA)')

    for i in info:
        print(f'check-dtb: {i}')
    if bad:
        print(f'check-dtb: {a.dtb}: FAILED', file=sys.stderr)
        for b in bad:
            print('  ' + b, file=sys.stderr)
        sys.exit(1)
    print(f'check-dtb: {a.dtb}: OK ({len(blob)} bytes, {len(nodes)} reserved regions checked)')


main()
