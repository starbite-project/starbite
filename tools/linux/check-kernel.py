#!/usr/bin/env python3
"""Checks on the Starbite kernel outputs (pure Python: runs in the VM and on the Mac).

    check-kernel.py config .config FRAGMENT...   every value of the fragments survived olddefconfig
    check-kernel.py dtb exynos9810-starlte.dtb    /chosen for the loader, CMA against reserved-memory
    check-kernel.py dtb --legacy-cma --image Image linux-old.dtb
                                                  the rollback DTB: CMA between the Image and the initrd
    check-kernel.py image Image                   arm64 header, text_offset 0x80000, fits the loader window
    check-kernel.py pair Image modules-REL.img    the modules image was made for this very Image
                                                  (release in the name, /starbite-kernel inside)
    check-kernel.py id Image                      "STARBITE-KERNEL <release> <Image SHA256>", the line
                                                  build-kernel.sh puts in the modules image (/starbite-kernel)

Each prints what it checked and exits 1 on a failure.
"""
import re
import struct
import sys
from pathlib import Path

# The Starbite loader's Linux window (shim/starlte/xnu_boot.c, ufs.h)
RAM_BASE, OS_RD, EDIT_FDT, LINUX_DTB, UFS_DMA = 0x92000000, 0xa8000000, 0xad000000, 0xae000000, 0xaf000000
WINDOW_END = 0xaf800000          # rkp starts here
INITRD_MAX = EDIT_FDT - OS_RD    # 80 MiB
CMA_ALIGN = 4 << 20              # pageblock (4K pages, MAX_PAGE_ORDER 10)
REQUIRED_ARGS = ('maxcpus=8', 's9p_vendor_m3_defaults', 'console=ramoops-1', 'console=tty0',
                 's3c2410_wdt.tmr_atboot=0', 'panic=10', 'loglevel=4')
FORBIDDEN_ARGS = ('earlycon', 'keep_bootcon', 's9p_late_ramlog', 'ignore_loglevel')

fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL', msg)


def ok(msg):
    print('ok  ', msg)


# ------------------------------------------------------------------ config --
def parse_config(text):
    vals = {}
    for line in text.splitlines():
        m = re.match(r'^(CONFIG_\w+)=(.*)$', line)
        if m:
            vals[m.group(1)] = m.group(2)
            continue
        m = re.match(r'^# (CONFIG_\w+) is not set$', line)
        if m:
            vals[m.group(1)] = 'n'
    return vals


def check_config(cfg, frags):
    have = parse_config(Path(cfg).read_text())
    n = warn = 0
    for frag in frags:
        optional = False
        for line in Path(frag).read_text().splitlines():
            if line.startswith('#') and '(optional)' in line:
                optional = True
                continue
            m = re.match(r'^(CONFIG_\w+)=(.*)$', line) or re.match(r'^# (CONFIG_\w+) is not set$', line)
            if not m:
                if line.strip() and not line.startswith('#'):
                    fail(f'{frag}: cannot parse {line!r}')
                continue
            sym, want = m.group(1), (m.group(2) if '=' in line and not line.startswith('#') else 'n')
            got = have.get(sym, 'n')
            n += 1
            good = got == want or (want == 'm' and got == 'y')
            if not good:
                msg = f'{sym}: wanted {want}, got {got}'
                if optional:
                    warn += 1
                    print('warn', msg, '(optional)')
                else:
                    fail(msg)
            optional = False
    ok(f'{n} fragment values checked against {cfg} ({warn} optional ones missing)')
    for sym in ('CONFIG_CMDLINE', 'CONFIG_LOCALVERSION', 'CONFIG_INITRAMFS_SOURCE', 'CONFIG_CMA_SIZE_MBYTES'):
        print('     ', sym, '=', have.get(sym))
    mods = sum(1 for v in have.values() if v == 'm')
    print(f'      {mods} options =m')


# --------------------------------------------------------------------- dtb --
class Node:
    def __init__(self, name):
        self.name, self.props, self.kids = name, {}, []

    def find(self, path):
        n = self
        for part in [p for p in path.split('/') if p]:
            m = [k for k in n.kids if k.name == part or k.name.split('@')[0] == part]
            if not m:
                return None
            n = m[0]
        return n


def parse_dtb(b):
    magic, total, off_struct, off_strings = struct.unpack_from('>IIII', b, 0)
    if magic != 0xd00dfeed:
        raise SystemExit('not an FDT')
    strings = b[off_strings:]
    pos, stack, root = off_struct, [], None
    while True:
        tok = struct.unpack_from('>I', b, pos)[0]
        pos += 4
        if tok == 1:
            end = b.index(b'\0', pos)
            node = Node(b[pos:end].decode())
            pos = (end + 4) & ~3
            if stack:
                stack[-1].kids.append(node)
            else:
                root = node
            stack.append(node)
        elif tok == 2:
            stack.pop()
        elif tok == 3:
            ln, nameoff = struct.unpack_from('>II', b, pos)
            pos += 8
            name = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            stack[-1].props[name] = b[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            return root, total
        else:
            raise SystemExit(f'bad FDT token {tok}')


def cells(v, n):
    return [int.from_bytes(v[i:i + 4 * n], 'big') for i in range(0, len(v), 4 * n)]


def regs(node, ac, sc):
    raw = node.props.get('reg', b'')
    words = struct.unpack(f'>{len(raw) // 4}I', raw)
    out = []
    for i in range(0, len(words), ac + sc):
        a = 0
        for w in words[i:i + ac]:
            a = (a << 32) | w
        s = 0
        for w in words[i + ac:i + ac + sc]:
            s = (s << 32) | w
        out.append((a, a + s))
    return out


def overlap(a, b):
    return a[0] < b[1] and b[0] < a[1]


def h(r):
    return f'{r[0]:#x}-{r[1]:#x}'


def image_span(path):
    """(load address, end incl. BSS) of an Image at the loader's RAM_BASE"""
    text_offset, image_size = struct.unpack_from('<QQ', Path(path).read_bytes(), 8)
    start = RAM_BASE + text_offset
    return start, start + image_size


def check_dtb(path, legacy=False, image=None):
    b = Path(path).read_bytes()
    root, total = parse_dtb(b)
    ok(f'{path}: FDT, {total} bytes, model {root.props.get("model", b"").rstrip(bytes(1)).decode()!r}')
    if total > UFS_DMA - LINUX_DTB:
        fail(f'DTB {total} bytes does not fit LINUX_DTB..UFS_DMA')
    rac = int.from_bytes(root.props.get('#address-cells', b'\0\0\0\2'), 'big')
    rsc = int.from_bytes(root.props.get('#size-cells', b'\0\0\0\1'), 'big')
    ch = root.find('chosen')
    if not ch:
        return fail('no /chosen')
    args = ch.props.get('bootargs', b'').rstrip(b'\0').decode()
    print(f'      bootargs = "{args}"')
    for a in REQUIRED_ARGS:
        if a not in args.split():
            fail(f'bootargs lack {a}')
    for a in FORBIDDEN_ARGS:
        if any(w == a or w.startswith(a + '=') for w in args.split()):
            fail(f'bootargs still carry {a}')
    for p in ('linux,initrd-start', 'linux,initrd-end', 'starbite,rtc-epoch'):
        v = ch.props.get(p)
        if v is None:
            fail(f'/chosen/{p} missing (the loader can only overwrite, not add)')
        elif len(v) not in (4, 8):
            fail(f'/chosen/{p} is {len(v)} bytes')
        else:
            ok(f'/chosen/{p} = {int.from_bytes(v, "big"):#x} ({len(v)} bytes)')
    # memory banks
    banks = []
    for n in root.kids:
        if n.props.get('device_type', b'').rstrip(b'\0') == b'memory':
            banks += regs(n, rac, rsc)
    print('      memory:', ', '.join(h(r) for r in banks))
    rm = root.find('reserved-memory')
    if not rm:
        return fail('no /reserved-memory')
    ac = int.from_bytes(rm.props.get('#address-cells', b'\0\0\0\2'), 'big')
    sc = int.from_bytes(rm.props.get('#size-cells', b'\0\0\0\1'), 'big')
    res, cma = [], None
    for n in rm.kids:
        for r in regs(n, ac, sc):
            tag = 'reusable' if 'reusable' in n.props else 'no-map' if 'no-map' in n.props else ''
            print(f'      reserved {n.name:32s} {h(r)} {tag}')
            if 'linux,cma-default' in n.props:
                cma = r
            else:
                res.append((n.name, r))
    fb = [(k.name, r) for k in ch.kids if k.name.startswith('framebuffer') for r in regs(k, ac, sc)]
    if not cma:
        return fail('no linux,cma-default region')
    size = cma[1] - cma[0]
    print(f'      CMA {h(cma)} = {size >> 20} MiB')
    if cma[0] % CMA_ALIGN or size % CMA_ALIGN:
        fail('CMA not 4 MiB aligned')
    if cma[1] > 1 << 32:
        fail('CMA above 4 GB (DECON fetches 32-bit addresses)')
    if not any(b0 <= cma[0] and cma[1] <= b1 for b0, b1 in banks):
        fail('CMA not inside one memory bank')
    for name, r in res + fb:
        if overlap(cma, r):
            fail(f'CMA overlaps {name} {h(r)}')
    if legacy:
        # rollback layout: inside the window, but only between the Image's end
        # (with BSS) and the initrd at OS_RD
        if not image:
            fail('--legacy-cma needs --image (the Image + BSS must end below the CMA)')
        else:
            span = image_span(image)
            print(f'      Image {h(span)} (with BSS), initrd from {OS_RD:#x}')
            if overlap(cma, span):
                fail(f'CMA overlaps the Image {h(span)}')
        if overlap(cma, (OS_RD, WINDOW_END)):
            fail(f'CMA overlaps the initrd/DTB part of the loader window {h((OS_RD, WINDOW_END))}')
    elif overlap(cma, (RAM_BASE, WINDOW_END)):
        fail(f'CMA overlaps the loader window {h((RAM_BASE, WINDOW_END))} (Image, initrd at {OS_RD:#x}, DTB)')
    for name, r in res:
        if overlap(r, (RAM_BASE, UFS_DMA)):
            fail(f'{name} {h(r)} overlaps the loader window')
    if not [f for f in fails if 'CMA' in f]:
        ok('CMA clear of every reserved region, the boot framebuffer and the ' +
           ('Image, initrd and DTB (rollback layout)' if legacy else 'loader window'))
    # the GPU OPPs, for the 260 MHz question (plan 1.3.2)
    def walk(n):
        yield n
        for k in n.kids:
            yield from walk(k)
    for n in walk(root):
        if n.props.get('compatible', b'').startswith(b'operating-points-v2') and \
                any(k.name.startswith('opp-') and int.from_bytes(k.props.get('opp-hz', b'\0'), 'big') > 200e6 and
                    int.from_bytes(k.props.get('opp-hz', b'\0'), 'big') < 600e6 for k in n.kids):
            opps = [(int.from_bytes(k.props['opp-hz'], 'big') // 1000000,
                     int.from_bytes(k.props.get('opp-microvolt', b'\0\0\0\0')[:4], 'big') // 1000)
                    for k in n.kids if 'opp-hz' in k.props]
            print(f'      OPP table {n.name}: ' + ', '.join(f'{f} MHz@{v} mV' for f, v in opps))
            for f, v in opps:
                if v and v < 950:
                    fail(f'{n.name}: {f} MHz at {v} mV (< 950 mV)')


# ------------------------------------------------------------------- image --
def check_image(path):
    b = Path(path).read_bytes()
    text_offset, image_size, flags = struct.unpack_from('<QQQ', b, 8)
    magic = b[0x38:0x3c]
    if magic != b'ARM\x64':
        fail(f'no arm64 magic at 0x38 ({magic!r})')
    else:
        ok('arm64 Image header (magic ARM\\x64)')
    if text_offset != 0x80000:
        fail(f'text_offset {text_offset:#x}, sboot-era loaders expect 0x80000')
    else:
        ok('text_offset 0x80000')
    endian = 'LE' if not flags & 1 else 'BE'
    page = {0: '?', 1: '4K', 2: '16K', 3: '64K'}[(flags >> 1) & 3]
    print(f'      file {len(b)} bytes, image_size (incl. BSS) {image_size} = {image_size / 2**20:.1f} MiB, {endian}, {page} pages')
    room = OS_RD - (RAM_BASE + text_offset)
    if image_size > room:
        fail(f'image_size {image_size:#x} does not fit RAM_BASE+text_offset..OS_RD ({room:#x})')
    else:
        ok(f'fits the loader window ({room >> 20} MiB before the initrd at {OS_RD:#x})')
    m = re.search(rb'Linux version (\S+)', b)
    if m:
        print('      release', m.group(1).decode())


# -------------------------------------------------------------------- pair --
# The banner carries no build number or time here (reproducible build), so two
# builds with different configs can share it: the release carries a build id
# (build-kernel.sh) and the modules image a line naming the Image's SHA256.
BANNER = re.compile(rb'Linux version (\S+) ')


def kernel_id(image):
    import hashlib
    b = Path(image).read_bytes()
    m = BANNER.search(b)
    if not m:
        sys.exit(f'{image}: no Linux banner')
    return m.group(1).decode(), f'STARBITE-KERNEL {m.group(1).decode()} {hashlib.sha256(b).hexdigest()}'


def check_pair(image, modimg):
    """build-kernel.sh writes kernel_id() into the modules image as
    /starbite-kernel; the release is also in the file name. Both must match."""
    rel, kid = kernel_id(image)
    print(f'      {kid}')
    name = Path(modimg).name
    if name != f'modules-{rel}.img':
        fail(f'{name}: the Image is release {rel}, wants modules-{rel}.img')
    else:
        ok(f'file name matches release {rel}')
    import mmap
    with open(modimg, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        found = mm.find(kid.encode() + b'\n')
    if found < 0:
        fail(f'{name} has no /starbite-kernel line for this Image: built for another kernel')
    else:
        ok(f'{name} was built with this Image')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    what = sys.argv[1]
    if what == 'config' and len(sys.argv) >= 4:
        check_config(sys.argv[2], sys.argv[3:])
    elif what == 'dtb':
        rest, legacy, image = sys.argv[2:], False, None
        if '--legacy-cma' in rest:
            rest.remove('--legacy-cma')
            legacy = True
        if '--image' in rest:
            i = rest.index('--image')
            image = rest[i + 1]
            del rest[i:i + 2]
        if len(rest) != 1:
            sys.exit(__doc__)
        check_dtb(rest[0], legacy, image)
    elif what == 'id' and len(sys.argv) == 3:
        print(kernel_id(sys.argv[2])[1])
        return
    elif what == 'pair' and len(sys.argv) == 4:
        check_pair(sys.argv[2], sys.argv[3])
    elif what == 'image':
        check_image(sys.argv[2])
    else:
        sys.exit(__doc__)
    if fails:
        print(f'{len(fails)} check(s) failed')
        sys.exit(1)


main()
