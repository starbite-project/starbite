"""Samsung PIT (partition information table), as `heimdall download-pit` saves it, and the check that
the phone in Download mode is an SM-G960F laid out the way tools/installer/sbinst/ufslayout.py says.

Format (heimdall libpit, PitData::Unpack): a 28-byte header (u32 magic 0x12349876, u32 entry count,
then 20 bytes: on recent phones the strings "COM_TAR2" and the chip/project tag), then entries of
132 bytes: u32 binary type, device type, identifier, attributes, update attributes, block size or
offset (the start), block count, file offset, file size; char partition name[32], flash file name[32],
FOTA file name[32]. All little-endian.

The check: every protected row of the layout (Samsung's partitions before SYSTEM) must be in the PIT
under its name with the same start and size, in 4 KiB blocks (UFS) or, if this PIT counts 512-byte
sectors, eight times that. An S9+ (SM-G965F) or any other phone has another PIT (other sizes or
names), so it is refused before anything is flashed. NOT YET SEEN on a real SM-G960F PIT: which unit
the UFS PIT uses (both are accepted and the one found is reported)."""
import struct
from dataclasses import dataclass

from . import ufslayout

MAGIC = 0x12349876
HDR = 28
ENT = 132


class PitError(Exception):
    pass


@dataclass(frozen=True)
class PitEntry:
    binary_type: int
    device_type: int
    identifier: int
    attributes: int
    update_attributes: int
    start: int
    blocks: int
    file_offset: int
    file_size: int
    name: str
    flash_name: str
    fota_name: str


def _s(b):
    return b.split(b'\0')[0].decode('ascii', errors='replace')


def parse(data):
    if len(data) < HDR:
        raise PitError('PIT shorter than its header')
    magic, n = struct.unpack_from('<II', data, 0)
    if magic != MAGIC:
        raise PitError(f'not a PIT (magic {magic:#x}, expected {MAGIC:#x})')
    if n < 1 or n > 512 or HDR + n * ENT > len(data):
        raise PitError(f'PIT says {n} entries; {len(data)} bytes hold {(len(data) - HDR) // ENT}')
    tags = [_s(data[8 + 8 * i:16 + 8 * i]) for i in range(2)]
    out = []
    for i in range(n):
        o = HDR + i * ENT
        f = struct.unpack_from('<9I', data, o)
        out.append(PitEntry(*f, _s(data[o + 36:o + 68]), _s(data[o + 68:o + 100]), _s(data[o + 100:o + 132])))
    return tags, out


def build(entries, tags=('COM_TAR2', 'EXYNOS98')):
    """A PIT from PitEntry rows (the fake phone's, and the tests')."""
    b = bytearray(HDR + ENT * len(entries))
    struct.pack_into('<II', b, 0, MAGIC, len(entries))
    for i, t in enumerate(tags[:2]):
        b[8 + 8 * i:16 + 8 * i] = t.encode()[:8].ljust(8, b'\0')
    for i, e in enumerate(entries):
        o = HDR + i * ENT
        struct.pack_into('<9I', b, o, e.binary_type, e.device_type, e.identifier, e.attributes, e.update_attributes,
                         e.start, e.blocks, e.file_offset, e.file_size)
        for k, s in enumerate((e.name, e.flash_name, e.fota_name)):
            b[o + 36 + 32 * k:o + 68 + 32 * k] = s.encode()[:31].ljust(32, b'\0')
    return bytes(b)


def check(data, layout=None):
    """(ok, unit, lines): does this PIT describe the layout's protected partitions exactly?"""
    L = layout or ufslayout.get('stock')
    tags, ents = parse(data)
    by = {e.name: e for e in ents}
    lines = [f'PIT: {len(ents)} entries, tags {" / ".join(t for t in tags if t) or "-"}']
    bad, unit = [], None
    for p in L.parts:
        if p.content != ufslayout.PROTECTED:
            continue
        e = by.get(p.name)
        if e is None:
            bad.append(f'{p.name}: not in the PIT')
            continue
        for u, mul in ((4096, 1), (512, 8)):
            if (e.start, e.blocks) == (p.start * mul, p.blocks * mul) and unit in (None, u):
                unit = u
                break
        else:
            bad.append(f'{p.name}: PIT {e.start}+{e.blocks}, SM-G960F {p.start}+{p.blocks} (4 KiB blocks)')
    if 'RECOVERY' not in by:
        bad.append('no RECOVERY entry: heimdall --RECOVERY would have nowhere to go')
    lines += [f'  {b}' for b in bad]
    if not bad:
        lines.append(f'  the {sum(p.content == ufslayout.PROTECTED for p in L.parts)} protected partitions match '
                     f'the SM-G960F table ({unit}-byte units)')
    return not bad, unit, lines


def stock_entries(model='SM-G960F'):
    """PIT rows of a stock phone from the stock layout (fake phones and tests). model 'SM-G965F'
    gives an S9+-like PIT: same names, other sizes (its BOOT/RECOVERY/SYSTEM differ)."""
    L = ufslayout.get('stock')
    rows, ident = [], 1
    for i, p, s, e in L.resolve(6, 15616000 - 6):
        blocks = e - s + 1
        if model != 'SM-G960F' and p.name in ('BOOT', 'RECOVERY', 'SYSTEM'):
            blocks += 2048
        rows.append(PitEntry(0, 8, ident, 5, 1, s, blocks if p.blocks != ufslayout.FILL else 0, 0, 0, p.name,
                             f'{p.name.lower()}.img', ''))
        ident += 1
    return rows
