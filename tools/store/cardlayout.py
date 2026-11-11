#!/usr/bin/env python3
"""The microSD card layouts: the one place where the card's geometry is written down
(docs/INSTALLER.md section 11). Used by tools/store/sbstore2.py (--medium sd / sd64),
tools/macos/card-range-flash.py (--layout), the installer (tools/installer/sbinst) and its fakes.

  sd128  S9 #1's 128 GB card (exactly 249,999,360 sectors), as it is today and never moved:
         p1 exFAT 8192.., p2 0xDA 182886400..249999359 holding the card store's header (1 MiB), the iOS 26
         root, the macOS Ventura root, the card store's data (961 MiB) and the 64 MiB tail.
  sd64   any card of at least 60,000,000,000 bytes (a card sold as 64 GB; 117,187,500 sectors):
         p2 0xDA FIRST at sector 8192 with a fixed size (64 MiB card store, the macOS Ventura root,
         the 64 MiB tail), then p1 exFAT from sector 31727616 to the card's last whole MiB. The
         root sits at the same sector on every card, so one set of payloads serves every card size;
         a bigger card only gives p1 more room. No iOS 26 slot: the parked edge pair needs sd128.

Rules every layout keeps (tools/installer/tests/test_cardlayout.py checks them): 1 MiB alignment;
MBR entry 1 = p1 (type 0x07, exFAT "STARBITE"), entry 2 = p2 (type 0xDA, raw); the areas tile p2
exactly; the store area starts at p2's first sector; the last 64 MiB of p2 is the tail (the XNU
microSD test range and the sdlog ring, starlte_sd.c; its last sector holds the SBTIME01 clock).

  cardlayout.py show [LAYOUT] [--sectors N]          the table in absolute sectors
  cardlayout.py for-card SECTORS                     which layout a card of SECTORS sectors gets
  cardlayout.py manifest LAYOUT                      [card] lines for installer-manifest.toml
  cardlayout.py cmdline LAYOUT KEY FILE|-            a boot-args line with KEY's sdroot= for LAYOUT
  cardlayout.py payload LAYOUT KEY IN.xnu OUT.xnu    a SWEETXNU payload with its packed sdroot= retargeted
  cardlayout.py payload-root IN.xnu                  the sdroot= packed in a payload
  cardlayout.py catalog LAYOUT IN.toml OUT.toml      catalog roots for LAYOUT (entries without a root hidden)

Python 3 standard library only (3.8+): the installer imports this module on any Python it runs on.
"""
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

SECTOR = 512
MIB_SECTORS = 2048                    # 1 MiB
BLOCK_SECTORS = 8                     # a 4 KiB store block
TAIL_SECTORS = 131072                 # 64 MiB: XNU test range + sdlog ring; its last sector the clock
MIN_CARD_BYTES = 60_000_000_000       # "at least 64 GB" (a card sold as 64 GB holds 61.9-64.1 GB)
MIN_CARD_SECTORS = MIN_CARD_BYTES // SECTOR   # 117187500
EXFAT, RAW_STORE = 0x07, 0xDA

PAYLOAD_MAGIC = b'SWEETXNU'           # ref/ios-herolte/tools/mkpayload.py, shim/starlte/xnu_boot.c struct payload
PAYLOAD_CMDLINE = 0x48
PAYLOAD_CMDLINE_LEN = 608             # BOOT_LINE_LENGTH: 607 characters + NUL
_SDROOT = re.compile(r'(?<!\S)sdroot=(\S*)')
_SDROOT_LBA = re.compile(r'^lba:(\d+),(\d+)$')


class LayoutError(ValueError):
    """A card or a request that does not fit a layout."""


@dataclass(frozen=True)
class Area:
    name: str                 # store | tail | free | a root name
    kind: str                 # store | root | free | tail
    start: int                # absolute card sector
    sectors: int
    keys: tuple = ()          # catalog keys whose root this is
    what: str = ''

    @property
    def end(self):
        return self.start + self.sectors

    def sdroot(self, sectors=None):
        return f'sdroot=lba:{self.start},{self.sectors if sectors is None else sectors}'


@dataclass(frozen=True)
class Layout:
    name: str
    title: str
    exact: int                # the card's sector count this layout is only for (0: any card >= min_sectors)
    min_sectors: int
    p1_start: int
    p1_sectors: int           # 0: to the card's last whole MiB
    p2_start: int
    p2_sectors: int
    areas: tuple
    p1_label: str = 'STARBITE'
    pending: tuple = field(default=())   # what still needs a rebuild or the phone (shown on the plan)

    # -- the card
    def check_card(self, sectors):
        if self.exact and sectors != self.exact:
            raise LayoutError(f'the {self.name} layout is only for the {self.exact}-sector card '
                              f'({self.exact * SECTOR / 1e9:.1f} GB); this card has {sectors} sectors '
                              f'({sectors * SECTOR / 1e9:.1f} GB)')
        if sectors < self.min_sectors:
            raise LayoutError(f'the {self.name} layout needs a card of at least {self.min_sectors} sectors '
                              f'({self.min_sectors * SECTOR / 1e9:.1f} GB); this card has {sectors} '
                              f'({sectors * SECTOR / 1e9:.1f} GB)')

    def p1_size(self, sectors):
        if self.p1_sectors:
            return self.p1_sectors
        return sectors // MIB_SECTORS * MIB_SECTORS - self.p1_start

    def parts(self, sectors):
        """[(type, start, sectors) of p1, of p2] for a card of `sectors` (LayoutError if it does not fit)."""
        self.check_card(sectors)
        return [(EXFAT, self.p1_start, self.p1_size(sectors)), (RAW_STORE, self.p2_start, self.p2_sectors)]

    def parts_dicts(self, sectors):
        (t1, s1, c1), (t2, s2, c2) = self.parts(sectors)
        return [{'type': t1, 'start': s1, 'sectors': c1, 'label': self.p1_label},
                {'type': t2, 'start': s2, 'sectors': c2}]

    # -- the areas inside p2
    def area(self, name):
        return next((a for a in self.areas if a.name == name), None)

    @property
    def store(self):
        return self.area('store')

    @property
    def tail(self):
        return self.area('tail')

    @property
    def store_data(self):
        """Where a home card store keeps its data when its 1 MiB store area is too small (or None)."""
        return self.area('storedata')

    def roots(self):
        return [a for a in self.areas if a.kind == 'root']

    def root(self, key):
        """The root area of a catalog key or root name, or None."""
        return next((a for a in self.roots() if key == a.name or key in a.keys), None)

    def area_at(self, start, sectors):
        """The one area that holds [start, start + sectors), or None."""
        return next((a for a in self.areas if a.start <= start and start + sectors <= a.end), None)

    @property
    def store_blocks(self):
        """Capacity of a card store for this layout (sbstore2.py --capacity): p2 in 4 KiB blocks."""
        return self.p2_sectors // BLOCK_SECTORS

    @property
    def store_area_blocks(self):
        """Blocks of p2 the store may use before the first root."""
        return self.store.sectors // BLOCK_SECTORS

    @property
    def clock_sector(self):
        return self.tail.end - 1


def _sd128():
    p2 = (182886400, 67112960)
    return Layout(
        name='sd128', title="S9 #1's 128 GB card, unchanged", exact=249999360, min_sectors=249999360,
        p1_start=8192, p1_sectors=182878208, p2_start=p2[0], p2_sectors=p2[1],
        areas=(Area('store', 'store', 182886400, 2048, (), 'card OS store (1 MiB: iOS 26 follows)'),
               Area('ios26', 'root', 182888448, 33554432, ('ios26', 'ios26sdt'), 'iOS 26 root, HFSX 16 GiB (parked edge pair)'),
               Area('mac13', 'root', 216442880, 31457280, ('mac13',), 'macOS Ventura root, HFSX 15 GiB'),
               Area('storedata', 'store', 247900160, 1968128, (), 'card OS store data, 961 MiB (the home store since '
                    '2026-11-11: sbstore2.py --home --data-at, tools/store/card-store.sh; docs/STORE-ON-CARD.md)'),
               Area('tail', 'tail', 249868288, TAIL_SECTORS, (), 'tail: XNU test range + sdlog ring; last sector SBTIME01 clock')))


def _sd64():
    store, mac, start = 64 * MIB_SECTORS, 31457280, 8192
    p2 = store + mac + TAIL_SECTORS
    return Layout(
        name='sd64', title='any card of at least 64 GB: roots first, p1 exFAT takes the rest',
        exact=0, min_sectors=MIN_CARD_SECTORS,
        p1_start=start + p2, p1_sectors=0, p2_start=start, p2_sectors=p2,
        areas=(Area('store', 'store', start, store, (), 'card OS store (64 MiB)'),
               Area('mac13', 'root', start + store, mac, ('mac13',), 'macOS Ventura root, HFSX 15 GiB'),
               Area('tail', 'tail', start + store + mac, TAIL_SECTORS, (), 'tail: XNU test range + sdlog ring; last sector SBTIME01 clock')),
        pending=(
            'XNU kernel: the SBTIME01 clock and the sdlog ring are read/written only on the 249999360-sector '
            'card (patches 0370, 0377, 0393, 0395): on sd64 macOS Ventura starts with no clock from the card '
            '(calendar at 1970) and leaves no kernel log until a kernel that takes the tail from the 0xDA '
            "partition's end is built (docs/INSTALLER.md 11.5)",
            'tools/store/boot-next.sh writes the clock and reads the sdlog ring only on the 128 GB card (sd128)',
            'iOS 26 (parked edge pair) has no root slot on sd64: its menu entry is hidden and its payload is left out'))


LAYOUTS = {L.name: L for L in (_sd128(), _sd64())}


def names():
    return sorted(LAYOUTS)


def get(name):
    if name not in LAYOUTS:
        raise LayoutError(f'unknown card layout {name!r} (known: {", ".join(names())})')
    return LAYOUTS[name]


def for_card(sectors):
    """The layout a card of `sectors` sectors gets: sd128 for S9 #1's card, sd64 for any other card
    of at least 64 GB, None for a smaller card."""
    if sectors == LAYOUTS['sd128'].exact:
        return 'sd128'
    if sectors >= LAYOUTS['sd64'].min_sectors:
        return 'sd64'
    return None


def identify(parts, sectors):
    """Which layout an MBR's first two entries [(type, start, sectors), ...] are, on a card of
    `sectors` sectors; None if neither."""
    got = [tuple(p) for p in (parts or [])[:2]]
    for L in LAYOUTS.values():
        try:
            if got == L.parts(sectors):
                return L.name
        except LayoutError:
            continue
    return None


# ------------------------------------------------------------------ boot-args and payloads
def sdroot_of(line):
    """(start, sectors) of the one sdroot=lba:S,C in a boot-args line; None without sdroot=;
    LayoutError for several or another form (sdroot=pN)."""
    found = _SDROOT.findall(line)
    if not found:
        return None
    if len(found) > 1:
        raise LayoutError(f'{len(found)} sdroot= arguments in one command line')
    m = _SDROOT_LBA.match(found[0])
    if not m:
        raise LayoutError(f'sdroot={found[0]} is not of the form lba:START,COUNT')
    return int(m.group(1)), int(m.group(2))


def retarget_cmdline(line, layout, key):
    """`line` with its sdroot=lba:S,C moved to KEY's root on LAYOUT (the sector count, the image's
    size, is kept and must fit the root slot). Exactly one sdroot= must be there."""
    L = get(layout) if isinstance(layout, str) else layout
    area = L.root(key)
    if area is None:
        raise LayoutError(f'{key} has no root on the {L.name} card layout')
    cur = sdroot_of(line)
    if cur is None:
        raise LayoutError('the command line has no sdroot=')
    if cur[1] > area.sectors:
        raise LayoutError(f'sdroot count {cur[1]} is larger than the {L.name} {area.name} slot ({area.sectors} sectors)')
    out = _SDROOT.sub(area.sdroot(cur[1]), line, count=1)
    if len(out.encode()) > PAYLOAD_CMDLINE_LEN - 1:
        raise LayoutError(f'the command line would be {len(out.encode())} bytes (at most {PAYLOAD_CMDLINE_LEN - 1})')
    return out


def payload_cmdline(blob):
    """The boot-args packed in a SWEETXNU payload, or None for anything else."""
    if len(blob) < PAYLOAD_CMDLINE + PAYLOAD_CMDLINE_LEN or blob[:8] != PAYLOAD_MAGIC or \
            int.from_bytes(blob[8:12], 'little') != 1:
        return None
    return bytes(blob[PAYLOAD_CMDLINE:PAYLOAD_CMDLINE + PAYLOAD_CMDLINE_LEN]).split(b'\0')[0].decode(errors='replace')


def retarget_payload(blob, layout, key):
    """A copy of a SWEETXNU payload whose packed cmdline has KEY's sdroot= for LAYOUT. Only the
    608-byte cmdline field changes (no checksum covers it; the store's CRC is made when the
    store is built). Read back before it is returned."""
    line = payload_cmdline(blob)
    if line is None:
        raise LayoutError('not a SWEETXNU payload (version 1)')
    new = retarget_cmdline(line, layout, key)
    fld = new.encode().ljust(PAYLOAD_CMDLINE_LEN, b'\0')
    out = bytes(blob[:PAYLOAD_CMDLINE]) + fld + bytes(blob[PAYLOAD_CMDLINE + PAYLOAD_CMDLINE_LEN:])
    if payload_cmdline(out) != new or len(out) != len(blob):
        raise LayoutError('the retargeted payload does not read back')
    return out


# ------------------------------------------------------------------ the catalog
_ROOT_LBA = re.compile(r'^(root\s*=\s*")sd:lba (\d+)\+(\d+)(.*)$')


def catalog_text(text, layout):
    """catalog.toml text with every `root = "sd:lba S+C ..."` for LAYOUT: entries whose key has a root
    slot get its start sector; entries without one are hidden (flags) and their root says so.
    Line by line, so comments, order and every other entry stay byte-identical; on sd128 the
    result is the input."""
    L = get(layout) if isinstance(layout, str) else layout
    blocks, cur = [], []
    for ln in text.splitlines(keepends=True):
        if ln.strip() == '[[entry]]' and cur:
            blocks.append(cur)
            cur = []
        cur.append(ln)
    blocks.append(cur)
    out = []
    for b in blocks:
        key = next((re.match(r'^key\s*=\s*"([^"]*)"', ln).group(1) for ln in b if re.match(r'^key\s*=\s*"', ln)), None)
        hide = False
        for i, ln in enumerate(b):
            m = _ROOT_LBA.match(ln.rstrip('\n'))
            if not m or key is None:
                continue
            area = L.root(key)
            count = int(m.group(3))
            if area is None:
                b[i] = f'{m.group(1)}none on the {L.name} card layout (128 GB card only)"\n'
                hide = True
            else:
                if count > area.sectors:
                    raise LayoutError(f'catalog {key}: root count {count} is larger than the {L.name} slot ({area.sectors})')
                b[i] = f'{m.group(1)}sd:lba {area.start}+{count}{m.group(4)}\n'
        if hide:
            fl = next((i for i, ln in enumerate(b) if re.match(r'^flags\s*=', ln)), None)
            if fl is None:
                last = max(i for i, ln in enumerate(b) if ln.strip() and not ln.lstrip().startswith('#'))
                b.insert(last + 1, 'flags = ["hidden"]\n')
            elif '"hidden"' not in b[fl]:
                b[fl] = re.sub(r'\[\s*', '["hidden", ', b[fl], count=1).replace(', ]', ']')
        out.append(''.join(b))
    return ''.join(out)


def manifest_lines(layout):
    """The [card] table of an installer manifest for LAYOUT."""
    L = get(layout)
    lines = ['[card]', f'layout = "{L.name}"                       # tools/store/cardlayout.py: p1/p2 and the root sectors come from it']
    if L.exact:
        lines.append(f'sectors = {L.exact}')
    return '\n'.join(lines) + '\n'


# ------------------------------------------------------------------ command line
def _show(L, sectors):
    card = sectors or L.exact or L.min_sectors
    print(f'{L.name}: {L.title}')
    print(f'  card: {"exactly" if L.exact else "at least"} {L.exact or L.min_sectors} sectors'
          f' ({(L.exact or L.min_sectors) * SECTOR / 1e9:.1f} GB); shown for {card} sectors')
    try:
        (t1, s1, c1), (t2, s2, c2) = L.parts(card)
    except LayoutError as e:
        print(f'  {e}')
        return 1
    rows = [('p1', f'0x{t1:02x}', s1, c1, f'exFAT "{L.p1_label}"' + ('' if L.p1_sectors else ' (to the last whole MiB)')),
            ('p2', f'0x{t2:02x}', s2, c2, f'raw: store + roots + tail ({c2 // BLOCK_SECTORS} store blocks)')]
    rows += [(f'  {a.name}', a.kind, a.start, a.sectors, a.what + (f'; menu keys {", ".join(a.keys)}' if a.keys else ''))
             for a in L.areas]
    for name, typ, s, c, what in sorted(rows, key=lambda r: (r[2], not r[0].startswith('p'))):
        print(f'  {name:<8} {typ:<6} {s:>10}..{s + c - 1:<10} {c:>10} sectors {c * SECTOR / 2**30:7.2f} GiB  {what}')
    print(f'  clock sector (SBTIME01): {L.clock_sector}; store area: {L.store_area_blocks} blocks')
    for p in L.pending:
        print(f'  PENDING: {p}')
    return 0


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0 if argv else 2
    cmd, a = argv[0], argv[1:]
    try:
        if cmd == 'show':
            sectors = int(a[a.index('--sectors') + 1]) if '--sectors' in a else 0
            pos = [x for i, x in enumerate(a) if x != '--sectors' and (i == 0 or a[i - 1] != '--sectors')]
            return max(_show(get(n), sectors) for n in (pos or names()))
        if cmd == 'for-card':
            name = for_card(int(a[0]))
            print(name or f'none: {a[0]} sectors is smaller than {MIN_CARD_SECTORS} (64 GB)')
            return 0 if name else 1
        if cmd == 'manifest':
            sys.stdout.write(manifest_lines(a[0]))
            return 0
        if cmd == 'cmdline':
            line = (sys.stdin.read() if a[2] == '-' else Path(a[2]).read_text()).strip('\n')
            if '\n' in line:
                raise LayoutError('the command line is more than one line')
            print(retarget_cmdline(line, a[0], a[1]))
            return 0
        if cmd == 'payload':
            blob = Path(a[2]).read_bytes()
            out = retarget_payload(blob, a[0], a[1])
            Path(a[3]).write_bytes(out)
            print(f'{a[3]}: {sdroot_arg(out)} ({"unchanged" if out == blob else "retargeted"} for {a[0]} {a[1]})')
            return 0
        if cmd == 'payload-root':
            print(sdroot_arg(Path(a[0]).read_bytes()))
            return 0
        if cmd == 'catalog':
            Path(a[2]).write_text(catalog_text(Path(a[1]).read_text(), a[0]))
            print(f'{a[2]}: catalog roots for {a[0]}')
            return 0
    except (LayoutError, IndexError, ValueError) as e:
        print(f'cardlayout: {e}', file=sys.stderr)
        return 2
    print(__doc__, file=sys.stderr)
    return 2


def sdroot_arg(blob):
    line = payload_cmdline(blob)
    if line is None:
        return 'not a SWEETXNU payload'
    r = sdroot_of(line)
    return f'sdroot=lba:{r[0]},{r[1]}' if r else 'no sdroot='


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
