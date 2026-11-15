#!/usr/bin/env python3
"""The GPT engine of the installer: pure Python, bytes in and bytes out, no device access.

  python3 tools/installer/sbinst/gpt.py show HEAD TAIL [--blocks N]
      parse and check a GPT backup (HEAD = the first 6+ blocks of sda, e.g. backup/<date>/sda-head-4M.img;
      TAIL = the last 5 blocks, sda-tail-gpt.img) and print the table
  python3 tools/installer/sbinst/gpt.py plan HEAD TAIL [LAYOUT] [--out DIR]
      the table LAYOUT (tools/installer/sbinst/ufslayout.py) would give this disk: old and new side by
      side; --out writes DIR/primary.bin (LBA 1..5) and DIR/backup.bin (the last 5 blocks)

What it does (used by the installer's repartition step, docs/INSTALLER.md 9):
  read()   both copies: protective MBR, header signature and CRC (over the header's own size: Samsung
           writes 512), entry-array CRC, the LBA cross references (primary at 1 and its alternate at
           the last block, backup the other way round), the same disk GUID / usable range / entry
           geometry in both, and the two entry arrays byte-identical.
  plan()   a new table from a ufslayout.Layout: the protected rows (Samsung's before SYSTEM) must
           equal the phone's entries exactly (index, name, start, size, type GUID, unique GUID,
           attributes) or it refuses; a KEEP row may not move; a partition on the phone that the
           layout does not know is refused. The headers keep every field (revision, header size,
           disk GUID, usable range, entry geometry) and only get new CRCs; the protective MBR is
           never written. Then every byte of the new copies is compared with the old ones: only the
           CRC fields and the entries from SYSTEM on may differ.
  classify() a copy on the phone is `orig` (the verified backup), `new` (the plan), `invalid` (fails
           its checks) or `other`; next_action() says what to write from that pair.

Write order (the installer, sbinst/steps.py): the BACKUP copy first, read back, then the PRIMARY copy,
read back. Linux (TWRP's 4.9 kernel, block/partitions/efi.c) uses the primary whenever it is valid
and never looks at a valid backup's contents; it only uses the backup with gpt=force. So until the
primary is written, every reader still sees the old table, and the one-block-group primary write is
the commit point. An interruption between the two copies (cable pull) leaves (orig primary, new
backup): the phone boots as before and a re-run writes only the primary. Each copy is written by a
single on-phone `dd` started detached (a cable pull cannot kill it half way); only a power loss in the
milliseconds of that dd could tear a copy, and next_action() repairs a torn copy from the other."""
import struct
import sys
import uuid
import zlib
from dataclasses import dataclass, field

try:
    from . import ufslayout
except ImportError:                                  # run as a script
    import ufslayout

SIG = b'EFI PART'
HDR_FMT = '<8sIIIIQQQQ16sQIII'                       # the first 92 bytes of a GPT header
CRC_FIELDS = ((16, 20), (88, 92))                    # header CRC, entry-array CRC


class GPTError(Exception):
    """A GPT that fails a check, or a plan the rules refuse."""


@dataclass(frozen=True)
class Entry:
    index: int                # 1-based: /dev/block/sdaN
    name: str
    first: int
    last: int
    type: str
    guid: str
    attrs: int = 0

    @property
    def blocks(self):
        return self.last - self.first + 1

    def key(self):
        return (self.index, self.name, self.first, self.last, self.type, self.guid, self.attrs)


@dataclass
class Header:
    block: bytes              # the whole header block as read (lba bytes)
    rev: int
    hsz: int
    crc: int
    cur: int
    alt: int
    first: int                # first usable LBA
    last: int                 # last usable LBA
    disk: str
    elba: int
    n: int
    esz: int
    ecrc: int


@dataclass
class Copy:
    header: Header
    array: bytes              # n * esz bytes
    entries: list


@dataclass
class Disk:
    lba: int
    total: int                # disk size in LBAs
    head: bytes               # LBA 0 .. 1 + entry blocks (at least)
    tail: bytes               # the last entry blocks + 1 LBAs
    pmbr_ok: bool = False
    primary: Copy = None
    backup: Copy = None
    perr: str = ''
    berr: str = ''
    problems: list = field(default_factory=list)

    @property
    def valid(self):
        return self.primary is not None and self.backup is not None and not self.problems

    @property
    def primary_only(self):
        """A valid primary and a backup region that was never written (all zeros), and nothing else wrong. S9 #2's
        stock GPT (2026-11-15): Samsung's firmware wrote no backup copy, though the primary names LBA total-1.
        The engine plans from the primary and writes both copies (the backup first, as always)."""
        return (self.primary is not None and self.backup is None and self.tail and not any(self.tail)
                and bool(self.berr) and all(p == self.berr for p in self.problems))

    @property
    def usable(self):
        return self.valid or self.primary_only

    @property
    def table(self):
        return (self.primary or self.backup).entries

    def primary_bytes(self):
        """LBA 1 .. 1 + entry blocks as on the disk (what a primary write replaces)."""
        eb = entry_blocks_of(self)
        return self.head[self.lba:(2 + eb) * self.lba]


def entry_blocks(n, esz, lba):
    return -(-(n * esz) // lba)


def entry_blocks_of(disk):
    c = disk.primary or disk.backup
    return entry_blocks(c.header.n, c.header.esz, disk.lba) if c else 4 * 4096 // disk.lba


def _g(b):
    return str(uuid.UUID(bytes_le=bytes(b)))


def parse_header(blk, where):
    if len(blk) < 92 or blk[:8] != SIG:
        raise GPTError(f'{where}: no "EFI PART" signature')
    f = struct.unpack_from(HDR_FMT, blk, 0)
    hsz = f[2]
    if not 92 <= hsz <= len(blk):
        raise GPTError(f'{where}: header size {hsz} is not in 92..{len(blk)}')
    h = bytearray(blk[:hsz])
    h[16:20] = bytes(4)
    if zlib.crc32(h) != f[3]:
        raise GPTError(f'{where}: header CRC wrong')
    if f[11] < 1 or f[11] > 1024 or f[12] < 128 or f[12] % 8:
        raise GPTError(f'{where}: {f[11]} entries of {f[12]} bytes: not a GPT this engine writes')
    return Header(bytes(blk), f[1], hsz, f[3], f[5], f[6], f[7], f[8], _g(f[9]), f[10], f[11], f[12], f[13])


def parse_entries(arr, n, esz):
    out = []
    for i in range(n):
        e = arr[i * esz:(i + 1) * esz]
        t, u, first, last, attrs = struct.unpack_from('<16s16sQQQ', e, 0)
        if t == bytes(16):
            continue
        name = e[56:128].decode('utf-16le', errors='replace').split('\0')[0]
        out.append(Entry(i + 1, name, first, last, _g(t), _g(u), attrs))
    return out


def parse_copy(hblock, array, where):
    h = parse_header(hblock, where)
    arr = bytes(array[:h.n * h.esz])
    if len(arr) < h.n * h.esz:
        raise GPTError(f'{where}: entry array cut short')
    if zlib.crc32(arr) != h.ecrc:
        raise GPTError(f'{where}: entry array CRC wrong')
    return Copy(h, arr, parse_entries(arr, h.n, h.esz))


def detect_lba(head):
    """The logical block size: where the primary header is (LBA 1)."""
    for lba in (4096, 512):
        if head[lba:lba + 8] == SIG:
            return lba
    raise GPTError('no GPT header at byte 512 or 4096 (not a GPT disk)')


def read(head, tail, total, lba=None):
    """Parse and cross-check both copies. `head` = LBA 0 onwards (6+ blocks at 4 KiB), `tail` = the
    last blocks of the disk (at least the backup header + entry blocks), `total` = disk size in LBAs.
    Never raises for a bad copy: the Disk says what is wrong (problems, perr, berr)."""
    lba = lba or detect_lba(head)
    d = Disk(lba, total, bytes(head), bytes(tail))
    mbr = head[:512]
    d.pmbr_ok = mbr[510:512] == b'\x55\xaa' and any(mbr[446 + 16 * i + 4] == 0xEE for i in range(4))
    if not d.pmbr_ok:
        d.problems.append('block 0 is not a protective MBR (type 0xEE)')
    try:
        h = parse_header(head[lba:2 * lba], 'primary')
        eb = entry_blocks(h.n, h.esz, lba)
        d.primary = parse_copy(head[lba:2 * lba], head[h.elba * lba:(h.elba + eb) * lba], 'primary')
        if (h.cur, h.alt, h.elba) != (1, total - 1, 2):
            d.problems.append(f'primary header LBAs {h.cur}/{h.alt}/{h.elba}, expected 1/{total - 1}/2 for a '
                              f'{total}-block disk')
    except GPTError as e:
        d.perr = str(e)
        d.problems.append(d.perr)
    try:
        if len(tail) < lba or len(tail) % lba:
            raise GPTError('backup: tail is not whole blocks')
        hb = tail[-lba:]
        h = parse_header(hb, 'backup')
        eb = entry_blocks(h.n, h.esz, lba)
        tail_first = total - len(tail) // lba               # LBA of tail[0]
        off = (h.elba - tail_first) * lba
        if off < 0 or off + eb * lba > len(tail) - lba:
            raise GPTError(f'backup: entries at LBA {h.elba} are outside the tail read ({tail_first}..{total - 1})')
        d.backup = parse_copy(hb, tail[off:off + eb * lba], 'backup')
        if (h.cur, h.alt, h.elba) != (total - 1, 1, total - 1 - eb):
            d.problems.append(f'backup header LBAs {h.cur}/{h.alt}/{h.elba}, expected {total - 1}/1/{total - 1 - eb}')
    except GPTError as e:
        d.berr = str(e)
        d.problems.append(d.berr)
    if d.primary and d.backup:
        p, b = d.primary.header, d.backup.header
        for k in ('first', 'last', 'disk', 'n', 'esz', 'rev', 'hsz'):
            if getattr(p, k) != getattr(b, k):
                d.problems.append(f'primary and backup headers differ in {k}: {getattr(p, k)} / {getattr(b, k)}')
        if d.primary.array != d.backup.array:
            d.problems.append('primary and backup entry arrays differ')
    c = d.primary or d.backup
    if c:
        h, eb = c.header, entry_blocks(c.header.n, c.header.esz, lba)
        if h.first < 2 + eb or h.last > total - 2 - eb or h.first > h.last:
            d.problems.append(f'usable range {h.first}..{h.last} overlaps the GPT copies')
        prev = None
        for e in sorted(c.entries, key=lambda e: e.first):
            if e.first > e.last or e.first < h.first or e.last > h.last:
                d.problems.append(f'entry {e.index} {e.name} {e.first}..{e.last} is outside {h.first}..{h.last}')
            if prev and e.first <= prev.last:
                d.problems.append(f'entries {prev.index} {prev.name} and {e.index} {e.name} overlap')
            prev = e
    return d


# ---------------------------------------------------------------------------------------------- build

def _entry_bytes(e, esz):
    b = bytearray(esz)
    struct.pack_into('<16s16sQQQ', b, 0, uuid.UUID(e.type).bytes_le, uuid.UUID(e.guid).bytes_le, e.first, e.last,
                     e.attrs)
    nm = e.name.encode('utf-16le')
    if len(nm) > 72:
        raise GPTError(f'{e.name}: name longer than 36 characters')
    b[56:56 + len(nm)] = nm
    return bytes(b)


def build_array(entries, n, esz, template=None):
    """The entry array: entries at their index; slots no entry takes keep the template's bytes when
    the template had nothing there either (zeros on Samsung)."""
    arr = bytearray(template[:n * esz]) if template else bytearray(n * esz)
    used = {e.index for e in entries}
    if template:
        for i in range(n):                       # an index the new table leaves empty is zeroed
            if i + 1 not in used and arr[i * esz:i * esz + 16] != bytes(16):
                arr[i * esz:(i + 1) * esz] = bytes(esz)
    for e in entries:
        if not 1 <= e.index <= n:
            raise GPTError(f'{e.name}: index {e.index} outside 1..{n}')
        arr[(e.index - 1) * esz:e.index * esz] = _entry_bytes(e, esz)
    return bytes(arr)


def _header_block(h, cur, alt, elba, ecrc):
    b = bytearray(h.block)
    struct.pack_into('<QQ', b, 24, cur, alt)
    struct.pack_into('<Q', b, 72, elba)
    struct.pack_into('<I', b, 88, ecrc)
    b[16:20] = bytes(4)
    struct.pack_into('<I', b, 16, zlib.crc32(bytes(b[:h.hsz])))
    return bytes(b)


def build_copies(disk, entries):
    """(primary, backup) bytes for `entries` on this disk, every header field kept but the LBAs and
    CRCs. primary = LBA 1 .. 1+eb (header + entries), backup = the last eb+1 LBAs (entries + header)."""
    c = disk.primary or disk.backup
    if c is None:
        raise GPTError('neither GPT copy is readable: nothing to build a new table on')
    h, lba, total = c.header, disk.lba, disk.total
    eb = entry_blocks(h.n, h.esz, lba)
    arr = build_array(entries, h.n, h.esz, c.array)
    ecrc = zlib.crc32(arr)
    pad = arr + bytes(eb * lba - len(arr))
    ph = _header_block(disk.primary.header if disk.primary else h, 1, total - 1, 2, ecrc)
    bh = _header_block(disk.backup.header if disk.backup else h, total - 1, 1, total - 1 - eb, ecrc)
    return ph + pad, pad + bh


# ---------------------------------------------------------------------------------------------- plan

@dataclass
class Row:
    index: int
    name: str
    old: Entry
    new: Entry
    status: str               # same | moved | resized | new | removed | changed (type/GUID only)
    content: str
    note: str = ''


@dataclass
class Plan:
    layout: str
    disk: Disk
    entries: list             # new entries
    rows: list
    primary: bytes            # new LBA 1..1+eb
    backup: bytes             # new last eb+1 LBAs
    fresh: list               # names whose contents are no longer valid (new place or size)
    lost_keep: list = field(default_factory=list)

    @property
    def changed(self):
        return any(r.status != 'same' for r in self.rows)

    @property
    def primary_offset(self):
        return self.disk.lba

    @property
    def backup_offset(self):
        return (self.disk.total - len(self.backup) // self.disk.lba) * self.disk.lba


def plan(disk, layout, move_keep=()):
    """The new table for `disk` from `layout` (a ufslayout.Layout). Raises GPTError on any refusal. `move_keep`: keep rows
    the caller is allowed to move on this phone (common.MOVE_KEEP_ALLOWED; their contents are the caller's job)."""
    if not disk.usable:
        raise GPTError('the GPT on the phone is not valid in both copies: ' + '; '.join(disk.problems))
    if disk.lba != ufslayout.BLOCK:
        raise GPTError(f'the disk has {disk.lba}-byte blocks; the UFS layouts are in {ufslayout.BLOCK}-byte blocks')
    if disk.total < layout.min_blocks:
        raise GPTError(f'the disk has {disk.total} blocks; layout {layout.name} needs at least {layout.min_blocks}')
    h = disk.primary.header
    try:
        rows_l = ufslayout.validate(layout, h.first, h.last)
    except ufslayout.LayoutError as e:
        raise GPTError(f'layout {layout.name} does not fit this disk: {e}') from None
    if len(layout.parts) > h.n:
        raise GPTError(f'layout {layout.name} has {len(layout.parts)} rows; this GPT holds {h.n} entries')
    old = {e.name: e for e in disk.primary.entries}
    old_by_index = {e.index: e for e in disk.primary.entries}
    new = [Entry(i, p.name, s, e, p.type, p.unique_guid(), p.attrs) for i, p, s, e in rows_l]
    # 1. protected rows: exactly the phone's
    bad = []
    for (i, p, s, e), ne in zip(rows_l, new):
        if p.content != ufslayout.PROTECTED:
            continue
        oe = old_by_index.get(i)
        if oe is None or oe.key() != ne.key():
            got = (f'{oe.name} {oe.first}..{oe.last} type {oe.type} guid {oe.guid} attrs {oe.attrs:#x}'
                   if oe else 'no entry')
            bad.append(f'entry {i}: the phone has {got}; the table says {p.name} {s}..{e} type {p.type} '
                       f'guid {ne.guid} attrs {p.attrs:#x}')
    if bad:
        raise GPTError('the protected partitions (Samsung\'s, before SYSTEM) are not exactly what the layout table '
                       'says, so this is not an SM-G960F laid out the way the installer knows: ' + '; '.join(bad))
    # 2. nothing on the phone the layout does not know
    names = {p.name for p in layout.parts}
    unknown = [e for e in disk.primary.entries if e.name not in names]
    if unknown:
        raise GPTError('the phone has partitions the layout table does not know (refusing to drop them): ' +
                       ', '.join(f'{e.index} {e.name}' for e in unknown))
    # 3. rows, KEEP rows that would move
    rows, fresh, lost = [], [], []
    for (i, p, s, e), ne in zip(rows_l, new):
        oe = old.get(p.name)
        if oe is None:
            st, note = 'new', 'new partition: written fresh by the installer'
        elif (oe.first, oe.last) == (ne.first, ne.last):
            st = 'same' if oe.key() == ne.key() else 'changed'
            note = '' if st == 'same' else ', '.join(
                w for w, a, b in (('index', oe.index, ne.index), ('type', oe.type, ne.type),
                                  ('unique GUID', oe.guid, ne.guid), ('attributes', oe.attrs, ne.attrs)) if a != b) + \
                ' changes; contents stay'
        elif oe.first == ne.first:
            st, note = 'resized', f'{oe.blocks} -> {ne.blocks} blocks'
        else:
            st, note = 'moved', f'{oe.first}..{oe.last} -> {ne.first}..{ne.last}'
        if st in ('new', 'moved', 'resized'):
            fresh.append(p.name)
            if p.content == ufslayout.KEEP and oe is not None:
                if p.name in move_keep:
                    note += ' (moves by the allowance: saved first, written back)'
                else:
                    lost.append(p.name)
        rows.append(Row(i, p.name, oe, ne, st, p.content, note))
    for e in disk.primary.entries:
        if e.name not in names:
            rows.append(Row(e.index, e.name, e, None, 'removed', '', 'removed'))
    if lost:
        raise GPTError(f'layout {layout.name} moves or resizes {", ".join(lost)}, whose contents nothing rewrites '
                       '(content = KEEP in ufslayout.py): refusing. Change the row\'s content on purpose if they '
                       'may be lost.')
    prim, back = build_copies(disk, new)
    p = Plan(layout.name, disk, new, rows, prim, back, fresh)
    check_preserved(p)
    return p


def check_preserved(p):
    """Every byte of the new copies outside the CRC fields and the entries from SYSTEM on equals the
    old copy's; the new copies parse, cross-check and give exactly the planned entries."""
    d = p.disk
    lba = d.lba
    h = d.primary.header
    keep_until = 17 * h.esz                   # entries 1..17 (Samsung's, before SYSTEM)
    old_p, old_b = d.primary_bytes(), d.tail[-len(p.backup):]
    if len(old_b) != len(p.backup):
        raise GPTError('the tail read is shorter than the backup copy')
    pairs = [('primary', old_p, p.primary, 0, lba)]
    if not d.primary_only:                    # a never-written backup has no old bytes to keep: it is made new
        pairs.append(('backup', old_b, p.backup, len(p.backup) - lba, 0))
    for name, old_c, new_c, hoff, aoff in pairs:
        allowed = set()
        for a, b in CRC_FIELDS:
            allowed |= set(range(hoff + a, hoff + b))
        diff = [i for i in range(hoff, hoff + lba) if old_c[i] != new_c[i] and i not in allowed]
        if diff:
            raise GPTError(f'{name} header: bytes outside the CRC fields would change ({diff[:6]})')
        if old_c[aoff:aoff + keep_until] != new_c[aoff:aoff + keep_until]:
            raise GPTError(f'{name}: the entries before SYSTEM would change')
    nd = read(d.head[:lba] + p.primary + d.head[lba + len(p.primary):], p.backup, d.total, lba)
    if not nd.valid:
        raise GPTError('the planned GPT does not pass its own checks: ' + '; '.join(nd.problems))
    if [e.key() for e in nd.primary.entries] != [e.key() for e in p.entries]:
        raise GPTError('the planned GPT does not read back as the planned entries')


# ---------------------------------------------------------------------------------------------- state

def classify(copy_bytes, orig, new, which, total, lba):
    """`orig` | `new` | `invalid` | `other` for one copy as read from the phone."""
    if copy_bytes == new:
        return 'new'
    if copy_bytes == orig:
        return 'orig'
    try:
        if which == 'primary':
            h = parse_header(copy_bytes[:lba], 'primary')
            parse_copy(copy_bytes[:lba], copy_bytes[lba:], 'primary')
        else:
            parse_copy(copy_bytes[-lba:], copy_bytes[:-lba], 'backup')
            h = parse_header(copy_bytes[-lba:], 'backup')
        del h
    except GPTError:
        return 'invalid'
    return 'other'


ACTIONS = {
    ('orig', 'orig'): 'write-both',          # nothing written yet
    ('orig', 'invalid'): 'write-both',       # the backup write was torn: write it again, then the primary
    ('orig', 'new'): 'write-primary',        # interrupted between the copies (the cable-pull case)
    ('new', 'new'): 'done',
    ('invalid', 'new'): 'write-primary',     # the primary write was torn (power loss): the backup holds the plan
    ('new', 'orig'): 'write-backup',         # not this engine's order, but the plan is clear
    ('new', 'invalid'): 'write-backup',
}


def next_action(p_state, b_state):
    """What to write next, or GPTError when the phone holds a table that is neither the verified
    backup nor the plan (someone else changed it, or both copies are damaged)."""
    a = ACTIONS.get((p_state, b_state))
    if a is None:
        raise GPTError(f'the phone\'s GPT is primary={p_state}, backup={b_state}: neither the verified original nor '
                       'the planned table. Nothing is written; restore with starbite-install --restore-gpt (the '
                       'backup on this computer) or Odin/heimdall with the stock PIT')
    return a


# ---------------------------------------------------------------------------------------------- text

def human_blocks(n, lba=4096):
    b = n * lba
    return f'{b / 2**30:.2f} GiB' if b >= 2**30 else f'{b / 2**20:.1f} MiB' if b >= 2**20 else f'{b // 1024} KiB'


def side_by_side(p):
    """Lines: the phone's table and the planned one, row by row (rows before SYSTEM folded)."""
    w = 42
    out = [f'{"on the phone now":<{w}} | {"after (layout " + p.layout + ")":<{w}} | change',
           f'{"-" * w}-+-{"-" * w}-+-------']
    same_head = [r for r in p.rows if r.content == ufslayout.PROTECTED and r.status == 'same']
    if same_head:
        a, b = same_head[0], same_head[-1]
        txt = f'{a.index:>2}-{b.index:<2} {a.name}..{b.name} (Samsung, protected)'
        out.append(f'{txt:<{w}} | {"identical (byte for byte)":<{w}} | same')

    def cell(e):
        if e is None:
            return '-'
        return f'{e.index:>2} {e.name:<10} {e.first:>8}..{e.last:<8} {human_blocks(e.blocks):>9}'
    for r in p.rows:
        if r in same_head:
            continue
        out.append(f'{cell(r.old):<{w}} | {cell(r.new):<{w}} | {r.status}{": " + r.note if r.note else ""}')
    return out


def table_lines(entries, lba=4096):
    return [f'  {e.index:>3} {e.name:<11} {e.first:>9}..{e.last:<9} {human_blocks(e.blocks, lba):>10}  {e.type}  {e.guid}'
            for e in entries]


def main(argv):
    if len(argv) < 3 or argv[0] not in ('show', 'plan'):
        sys.exit(__doc__)
    head = open(argv[1], 'rb').read()
    tail = open(argv[2], 'rb').read()
    total = int(argv[argv.index('--blocks') + 1]) if '--blocks' in argv else None
    if total is None:                                   # the backup header knows where it is
        total = parse_header(tail[-detect_lba(head):], 'backup').cur + 1
    d = read(head, tail, total)
    print(f'{argv[1]}: {d.lba}-byte blocks, {d.total} blocks, disk {(d.primary or d.backup).header.disk}, '
          f'{"both copies valid" if d.valid else "PROBLEMS: " + "; ".join(d.problems)}')
    if argv[0] == 'show':
        print('\n'.join(table_lines(d.table, d.lba)))
        return 0 if d.valid else 1
    rest = [a for a in argv[3:] if not a.startswith('--')]
    L = ufslayout.get(rest[0] if rest and rest[0] != (argv[argv.index('--out') + 1] if '--out' in argv else None)
                      else None)
    p = plan(d, L)
    print('\n'.join(side_by_side(p)))
    print(f'fresh after the change: {", ".join(p.fresh) or "nothing"}')
    if '--out' in argv:
        import os
        o = argv[argv.index('--out') + 1]
        os.makedirs(o, exist_ok=True)
        open(os.path.join(o, 'primary.bin'), 'wb').write(p.primary)
        open(os.path.join(o, 'backup.bin'), 'wb').write(p.backup)
        print(f'{o}/primary.bin ({len(p.primary)} B, at byte {p.primary_offset}), {o}/backup.bin ({len(p.backup)} B, '
              f'at byte {p.backup_offset})')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
