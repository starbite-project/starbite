#!/usr/bin/env python3
"""The UFS partition tables (GPT of LUN 0, /dev/block/sda) the installer can lay out: ONE table per
layout, in this file only. Everything else (the GPT engine sbinst/gpt.py, the installer's
repartition step, the fake phone, the tests, docs/INSTALLER.md 9) reads it from here.

  python3 tools/installer/sbinst/ufslayout.py show [LAYOUT] [--blocks N]   print a layout for a UFS size
  python3 tools/installer/sbinst/ufslayout.py check                       validate every layout

HOW TO CHANGE A LAYOUT (e.g. the One UI 8 redesign): copy `_starbite_a()` into a new function,
edit the rows after SYSTEM (never the PROTECTED rows), register it in LAYOUTS, and point ACTIVE at
it (or name it in a bundle: `[ufs] layout = "NAME"` in installer-manifest.toml). Run `check`, then
`python3 -m unittest discover -s tools/installer/tests`. The rules every layout obeys (enforced by
`validate()` and again by the engine):

  - Rows 1-17 are Samsung's partitions before SYSTEM (BOTA0 .. DQMDBG): PROTECTED. Their index,
    name, start, size, type GUID, unique GUID and attributes must equal the phone's byte for byte,
    or the installer refuses. They are never moved. They come from `SAMSUNG_HEAD` below and every
    layout starts with them unchanged.
  - Row 18 is SYSTEM. From SYSTEM on, rows may be moved, resized, added or removed.
  - The row order is the GPT entry order, so row N is /dev/block/sdaN (tools use by-name links,
    but some docs and phase2.sh name sda27/sda28).
  - `start` None = right after the previous row, rounded up to 1 MiB (256 blocks). `blocks` FILL =
    up to the last usable block of the disk (only the last row; it absorbs UFS size differences).
  - `content` says what moving the row means (see CONTENT). A KEEP row that the plan would move
    or resize is a refusal: its contents would be lost and nothing rewrites them.
  - `guid` None = derived from the name (`derived_guid`): the same on every phone, so a bundle made
    from the main phone (Windows BCD, root=PARTUUID=...) holds on every Starbite phone. The rows
    that exist on S9 #1 carry S9 #1's GUIDs, so a phone laid out with `starbite-a` has the very
    same GPT bytes as S9 #1 (tests/test_gpt.py checks this against the real backup).

Units: 4096-byte blocks, the UFS logical block size (sda's GPT header is at byte 4096, i.e. LBA 1
of 4 KiB; /sys/block/sda/queue/logical_block_size = 4096), the same units as /sys/class/block/*/
start divided by 8."""
import hashlib
import sys
import uuid
from dataclasses import dataclass, field, replace

BLOCK = 4096
ALIGN = 256                                   # 1 MiB in 4 KiB blocks
FILL = 'fill'

# Partition type GUIDs
MS_BASIC = 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7'      # Samsung uses it for every partition
EFI_SYSTEM = 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
LINUX_DATA = '0fc63daf-8483-4772-8e79-3d69d8477de4'

# Samsung's disk GUID on every UFS phone: the bytes "ANDROID MMC DISK".
SAMSUNG_DISK_GUID = str(uuid.UUID(bytes_le=b'ANDROID MMC DISK'))

# What moving / resizing a row means
PROTECTED = 'protected'    # Samsung's, before SYSTEM: never moved, must match the phone exactly
KEEP = 'keep'              # Samsung's, after SYSTEM, not rewritten by the installer: moving it is refused
INSTALLER = 'installer'    # written (or made fresh) by the installer from the bundle: may move
SCRATCH = 'scratch'        # nothing on it is needed: may move, left as it is
CONTENT = (PROTECTED, KEEP, INSTALLER, SCRATCH)


def samsung_guid(name):
    """Samsung's unique GUID for its own partitions: "ANDROID " + the name, NUL padded (16 bytes)."""
    return str(uuid.UUID(bytes_le=(b'ANDROID ' + name.encode())[:16].ljust(16, b'\0')))


def derived_guid(name):
    """A fixed random-looking GUID (version 4 bits) for a Starbite row without a pinned GUID."""
    h = bytearray(hashlib.sha256(b'starbite-ufslayout/' + name.encode()).digest()[:16])
    h[6] = (h[6] & 0x0F) | 0x40
    h[8] = (h[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(h)))


@dataclass(frozen=True)
class Part:
    name: str
    start: object             # first 4 KiB block, or None (after the previous row, 1 MiB aligned)
    blocks: object            # 4 KiB blocks, or FILL
    content: str
    role: str                 # what it holds, who reads or writes it (shown by `show`)
    type: str = MS_BASIC
    guid: str = None          # None: samsung_guid() for PROTECTED rows, derived_guid() otherwise
    attrs: int = 0

    def unique_guid(self):
        if self.guid:
            return self.guid
        return samsung_guid(self.name) if self.content == PROTECTED else derived_guid(self.name)


@dataclass(frozen=True)
class Layout:
    name: str
    title: str
    parts: tuple
    min_blocks: int            # smallest UFS LU0 (4 KiB blocks) this layout fits
    notes: tuple = field(default_factory=tuple)

    def part(self, name):
        return next((p for p in self.parts if p.name == name), None)

    def index(self, name):
        return next((i for i, p in enumerate(self.parts, 1) if p.name == name), None)

    def resolve(self, first_usable, last_usable):
        """[(index, Part, first, last)] for a disk whose GPT header says first/last usable block."""
        out, nxt = [], first_usable
        for i, p in enumerate(self.parts, 1):
            s = p.start if p.start is not None else -(-nxt // ALIGN) * ALIGN
            e = last_usable if p.blocks == FILL else s + p.blocks - 1
            out.append((i, p, s, e))
            nxt = e + 1
        return out


# ---------------------------------------------------------------------------------------------
# Samsung's partitions before SYSTEM, as the SM-G960F's PIT lays them out (S9 #1's GPT backup
# backup/20261102-185306/sda-head-4M.img, entries 1-17; identical on a stock phone: they come from
# the PIT, and Starbite never changed them). Index = sdaN.
SAMSUNG_HEAD = (
    Part('BOTA0', 1024, 1024, PROTECTED, 'Samsung bootloader A/B tag area 0 (read by S-Boot)'),
    Part('BOTA1', 2048, 2048, PROTECTED, 'Samsung bootloader tag area 1'),
    Part('EFS', 4096, 5120, PROTECTED, 'IMEI, radio calibration, Wi-Fi MAC: PER PHONE, irreplaceable'),
    Part('PARAM', 9216, 2048, PROTECTED, 'S-Boot parameters (boot flags, Download-mode counters): per phone'),
    Part('UP_PARAM', 11264, 2048, PROTECTED, 'boot splash images (Starbite splash is a later, separate step)'),
    Part('KEYSTORAGE', 13312, 2, PROTECTED, 'hardware-backed key blobs: per phone'),
    Part('BOTA2', 13314, 2048, PROTECTED, 'Samsung bootloader tag area 2'),
    Part('DTB', 15362, 2048, PROTECTED, 'device tree blob for the stock kernel'),
    Part('DTBO', 17410, 2048, PROTECTED, 'device tree overlays'),
    Part('BOOT', 19458, 14080, PROTECTED, 'BOOT image: the Starbite loader + menu (the installer writes its '
         'contents, never its place)'),
    Part('RECOVERY', 33538, 16638, PROTECTED, 'RECOVERY image: Starbite TWRP (heimdall/installer write its '
         'contents, never its place)'),
    Part('RADIO', 50176, 12800, PROTECTED, 'modem firmware (CP)'),
    Part('NAD_REFER', 62976, 256, PROTECTED, 'Samsung factory test (NAD) reference data'),
    Part('PERSISTENT', 63232, 128, PROTECTED, 'FRP / OEM-unlock state: per phone'),
    Part('MISC', 63360, 256, PROTECTED, 'bootloader message (recovery commands)'),
    Part('STEADY', 63616, 1024, PROTECTED, 'Samsung steady-state data: per phone'),
    Part('DQMDBG', 64640, 4096, PROTECTED, 'Samsung DQM debug log'),
)
assert len(SAMSUNG_HEAD) == 17

# Samsung's partitions from SYSTEM to CP_DEBUG (entries 18-24) as on a stock phone and on S9 #1.
# The GUIDs are S9 #1's (a stock phone has random ones; the plan replaces them).
SYSTEM_ROW = Part('SYSTEM', 68736, 1245184, INSTALLER, 'Android system (/system): the installer writes the '
                  'bundle\'s system.img (part-write.py)', guid='a54b222a-ef0d-455a-ab7a-f69933179685')
SAMSUNG_MID = (
    SYSTEM_ROW,
    Part('VENDOR', 1313920, 185600, INSTALLER, 'Android /vendor (Starbite LineageOS vendor; part-write.py)',
         guid='370857a3-62f6-4fbd-9dec-a6785ca5b09a'),
    Part('ODM', 1499520, 179200, INSTALLER, 'Android /metadata (FBE keys of A16DATA): made fresh by '
         'metadata-reset.sh', guid='43a88b6d-4253-4b33-933b-2bd7998849e9'),
    Part('CACHE', 1678720, 3072, KEEP, 'Samsung /cache (recovery logs, OTA staging); kept where it is',
         guid='a6ba7bd0-027d-4df9-af2d-42f00be2c5ea'),
    Part('HIDDEN', 1681792, 2560, KEEP, 'Samsung preload / HIDDEN partition; kept where it is',
         guid='d10a13a1-9c40-4eaf-861b-193c377d7ce9'),
    Part('OMR', 1684352, 12800, KEEP, 'Samsung OMC resources (carrier customisation); backed up, kept',
         guid='0b42542d-c44c-4020-b1dc-aa145bb18bda'),
    Part('CP_DEBUG', 1697152, 1280, KEEP, 'modem (CP) crash dumps; kept where it is',
         guid='2f1dfd30-485b-4122-9885-d46175ab3686'),
)
USERDATA_GUID = '3502f564-bda2-4073-945b-db9e35bd3fdd'
WINESP_GUID = 'b3babc53-99be-42c7-b0c1-c1df6350e566'
WINDOWS_GUID = '923ecef4-5f78-46b6-b695-2aede847e27c'
A16DATA_GUID = 'bafcb27c-5898-4a6b-a79f-b8bbc1fba781'


def _stock():
    """Samsung's stock SM-G960F 64 GB table: USERDATA takes everything after CP_DEBUG. Here as the
    reference a stock phone is checked against (and for the fake stock phone); never a target."""
    return Layout('stock', 'Samsung stock SM-G960F (USERDATA to the end of the disk)',
                  SAMSUNG_HEAD + SAMSUNG_MID + (
                      Part('USERDATA', 1698432, FILL, SCRATCH, 'stock Android /data (the user\'s stock data: '
                           'erased by a Starbite install)', guid=USERDATA_GUID),),
                  min_blocks=15616000)


def _stock_s9b():
    """S9 #2's stock table (2026-11-15, another stock firmware than S9 #1's original): rows 1-17 as everywhere, then
    SYSTEM 4.3 GiB, VENDOR, ODM, a 600 MiB CACHE, HIDDEN, OMR, CP_DEBUG at other places, and a USERDATA that stops
    short of the last usable block. Read from S9 #2's step-A backup (positions only). A reference for the fakes and
    the tests, never a target."""
    at = {'SYSTEM': (68736, 1126400), 'VENDOR': (1195136, 166400), 'ODM': (1361536, 165376), 'CACHE': (1526912, 153600),
          'HIDDEN': (1680512, 2560), 'OMR': (1683072, 12800), 'CP_DEBUG': (1695872, 1280)}
    mid = tuple(replace(p, start=at[p.name][0], blocks=at[p.name][1]) for p in SAMSUNG_MID)
    return Layout('stock-s9b', "Samsung stock SM-G960F as on S9 #2 (another firmware's table)",
                  SAMSUNG_HEAD + mid + (
                      Part('USERDATA', 1697152, 13917184, SCRATCH, 'stock Android /data (S9 #2)', guid=USERDATA_GUID),),
                  min_blocks=15616000)


def _starbite_a():
    """Layout A: S9 #1 since 2026-10-25 (tools/woa/gpt-split.sh). docs/ANDROID-PIXEL.md 2.1."""
    return Layout('starbite-a', 'Starbite layout A (S9 #1 today): USERDATA 18.75 GiB, WINESP, WINDOWS 25.84 GiB, '
                  'A16DATA 8 GiB', SAMSUNG_HEAD + SAMSUNG_MID + (
                      Part('USERDATA', 1698432, 4915328, INSTALLER, 'bytes 0-16 GiB: the iOS 16.5 (an earlier port) slot '
                           '(slot-write.py); 16 GiB-end: the Starbite OS store (store-flash.sh) and the iOS herolog '
                           'ring in its last 1025 blocks', guid=USERDATA_GUID),
                      Part('WINESP', 6613760, 131072, INSTALLER, 'Windows 11 EFI system partition (FAT32, 4K sectors)',
                           type=EFI_SYSTEM, guid=WINESP_GUID),
                      Part('WINDOWS', 6744832, 6773760, INSTALLER, 'Windows 11 (NTFS). The Windows UFS driver\'s write '
                           'fence is hard-coded to blocks 6613760..13518591 (master plan 2.7)', guid=WINDOWS_GUID),
                      Part('A16DATA', 13518592, FILL, INSTALLER, 'Android /data (f2fs; empty from make-a16data.sh)',
                           guid=A16DATA_GUID)),
                  min_blocks=15616000,
                  notes=('A16DATA ends at the last usable block: 2097403 blocks (8.001 GiB) on a 15616000-block UFS.',))


def _starbite_a16g():
    """Phase 2 of docs/ANDROID-PIXEL.md 2.3: A16DATA 16 GiB, WINDOWS shrunk by 8 GiB (after ntfsresize),
    A16DATA typed Linux data. Prepared, not applied to S9 #1."""
    a = _starbite_a()
    rows = list(a.parts)
    rows[26] = replace(rows[26], blocks=11421440 - 6744832)
    rows[27] = replace(rows[27], start=11421440, type=LINUX_DATA)
    return Layout('starbite-a16g', 'Starbite layout A, Phase 2: WINDOWS 17.84 GiB, A16DATA 16 GiB (Linux data type)',
                  tuple(rows), min_blocks=15616000,
                  notes=('WINDOWS must be shrunk with ntfsresize BEFORE this table is written (phase2.sh); the '
                         'installer writes WINDOWS fresh from the bundle anyway.',))


ONEUI_SYSTEM = 1609984        # SYSTEM grows over the old VENDOR + ODM: 68736..1678719 (6.14 GiB; up to CACHE)
ONEUI_VENDOR = 262144         # 1 GiB  (set from the built image: tools/android/oneui, docs/ANDROID-ONEUI.md 4)
ONEUI_ODM = 16384             # 64 MiB
ONEUI_WINDOWS = 22 << 18      # 22 GiB = 5767168 blocks (Windows 11 uses 17.2 GB: about 6.4 GB free; 18 GiB hung at login)


def _starbite_oneui():
    """One UI 8 as the main Android (docs/ANDROID-ONEUI.md, 2026-11-07). One UI's EROFS system image is 5.98 GiB:
    SYSTEM takes the whole region in front of CACHE (where SYSTEM, VENDOR and ODM were), and VENDOR and ODM move
    behind WINDOWS, keeping their names (the first-stage fstab finds them by name) and their entry numbers
    (sda19, sda20: no other partition is renumbered). WINDOWS exactly 18 GiB (its NTFS was shrunk with
    ntfsresize to 18 GiB - 64 MiB on 2026-11-06, which left Windows 1.4 GB free and hanging at the Welcome
    screen; on 2026-11-07 WINDOWS grew to 22 GiB and the NTFS was grown to fill it), then VENDOR, ODM, and A16DATA
    to the last usable block (Linux data, ext4)."""
    a = _starbite_a()
    rows = list(a.parts)
    win_blocks = ONEUI_WINDOWS
    back = 6744832 + win_blocks                        # 12512000, right after WINDOWS
    rows[17] = replace(rows[17], blocks=ONEUI_SYSTEM)
    rows[18] = replace(rows[18], start=back, blocks=ONEUI_VENDOR, role='One UI /vendor (EROFS)')
    rows[19] = replace(rows[19], start=back + ONEUI_VENDOR, blocks=ONEUI_ODM, role='One UI /odm (EROFS)')
    rows[26] = replace(rows[26], blocks=win_blocks)
    rows[27] = replace(rows[27], start=back + ONEUI_VENDOR + ONEUI_ODM, type=LINUX_DATA,
                       role='One UI /data (ext4, the fs ends 5 blocks early: length=-20480)')
    return Layout('starbite-oneui', 'Starbite layout for One UI 8: SYSTEM 6.14 GiB in front, WINDOWS 22 GiB, '
                  'VENDOR + ODM behind it, A16DATA ~10.8 GiB (Linux data)',
                  tuple(rows), min_blocks=15616000,
                  notes=('WINDOWS must hold its NTFS: shrink it with ntfsresize BEFORE this table is written if it is '
                         'larger than 22 GiB; grow it with ntfsresize afterwards to use the whole partition.',
                         'VENDOR (sda19) and ODM (sda20) lie after WINDOWS: the GPT entries are not in block order.',
                         'The Windows UFS driver write fence still spans 6613760..13518591 (old WINDOWS end): NTFS '
                         'only writes inside its 22 GiB volume, so VENDOR, ODM and A16DATA from 12512000 are safe '
                         'while Windows does not grow its volume past the partition.'))


LAYOUTS = {L.name: L for L in (_stock(), _stock_s9b(), _starbite_a(), _starbite_a16g(), _starbite_oneui())}
ACTIVE = 'starbite-a'          # the table a Starbite install lays out unless the bundle names another
TARGETS = ('starbite-a', 'starbite-a16g', 'starbite-oneui')      # layouts a bundle may name (stock is a reference only)


# ---------------------------------------------------------------------------------------------
# PROPOSAL, NOT A TARGET (docs/MAINTENANCE.md 4.1; the coordinator decides the One UI 8 rows):
# SBMAINT, Starbite Maintenance's own partition, which replaces TWRP without RECOVERY. 256 MiB = two
# 128 MiB slots for "SBMAINT1" containers (tools/maint/sbmaint.py: A at +0, B at +128 MiB; today's
# container is 35 MiB, the loader stages at most 80 MiB). Its own type GUID (the bytes "STARBITE MAINT01",
# Samsung-style), so Windows gives it no drive letter and tools can tell it from a data partition;
# attribute bit 0 ("required by the platform": partition tools should not delete it). The loader finds it
# by NAME (gpt_find("SBMAINT")).
# Placement here: the LAST row, after an A16DATA of fixed size (15.75 GiB instead of 16 GiB), so no
# existing row is renumbered (part-write.py, preflight.sh, metadata-reset.sh name sda18/19/20/28) and
# the slot B offset is the same on every phone. The other choice (SBMAINT before a FILL A16DATA, which
# renumbers A16DATA to sda29) is in docs/MAINTENANCE.md 4.1. Not in LAYOUTS/TARGETS: no bundle can name
# it; `ufslayout.py show proposal-sbmaint` prints it and `check` validates it.
SBMAINT_TYPE = str(uuid.UUID(bytes_le=b'STARBITE MAINT01'))     # 52415453-4942-4554-204d-41494e543031
SBMAINT_BLOCKS = 65536                                          # 256 MiB
SBMAINT_ROW = Part('SBMAINT', None, SBMAINT_BLOCKS, INSTALLER, 'Starbite Maintenance: two SBMAINT1 container '
                   'slots (kernel + DTBs + initramfs), started by the loader (Bixby > Starbite Maintenance, '
                   'boot-next "maint"); written by `sbm update`', type=SBMAINT_TYPE, attrs=1)


def _proposal_sbmaint():
    """starbite-a16g (Windows 17.84 GiB, Android data 16 GiB: the One UI 8 sizes the user asked for) with
    A16DATA fixed at 15.75 GiB and SBMAINT after it."""
    rows = list(_starbite_a16g().parts)
    rows[27] = replace(rows[27], blocks=4128768)                # A16DATA 15.75 GiB, no longer FILL
    return Layout('proposal-sbmaint', 'PROPOSAL: starbite-a16g + SBMAINT 256 MiB as the last row (no renumbering)',
                  tuple(rows) + (SBMAINT_ROW,), min_blocks=15616000,
                  notes=('A proposal for the One UI 8 layout, not a target (docs/MAINTENANCE.md 4.1).',
                         'SBMAINT ends 251 blocks before the last usable block on a 15616000-block UFS.'))


PROPOSALS = {L.name: L for L in (_proposal_sbmaint(),)}


class LayoutError(ValueError):
    pass


def get(name=None):
    name = name or ACTIVE
    if name not in LAYOUTS:
        raise LayoutError(f'no UFS layout {name!r} (known: {", ".join(LAYOUTS)})')
    return LAYOUTS[name]


def validate(L, first_usable=6, last_usable=None):
    """The rules in the module docstring. Returns the resolved rows; raises LayoutError."""
    last_usable = last_usable if last_usable is not None else L.min_blocks - 6
    names = [p.name for p in L.parts]
    if len(set(names)) != len(names):
        raise LayoutError(f'{L.name}: duplicate names')
    if len(L.parts) > 128:
        raise LayoutError(f'{L.name}: more than 128 rows')
    if L.parts[:17] != SAMSUNG_HEAD:
        raise LayoutError(f'{L.name}: rows 1-17 must be SAMSUNG_HEAD unchanged (the protected partitions)')
    if L.parts[17].name != 'SYSTEM':
        raise LayoutError(f'{L.name}: row 18 must be SYSTEM')
    for p in L.parts:
        if p.content not in CONTENT:
            raise LayoutError(f'{L.name}: {p.name}: content {p.content!r}')
        if p.content == PROTECTED and p not in SAMSUNG_HEAD:
            raise LayoutError(f'{L.name}: {p.name}: only Samsung\'s rows before SYSTEM are protected')
        if p.blocks == FILL and p is not L.parts[-1]:
            raise LayoutError(f'{L.name}: {p.name}: only the last row may fill the disk')
        if not p.name or len(p.name) > 36 or not p.name.isascii():
            raise LayoutError(f'{L.name}: {p.name!r}: a GPT name is at most 36 ASCII characters')
        uuid.UUID(p.type)
        uuid.UUID(p.unique_guid())
    guids = [p.unique_guid() for p in L.parts]
    if len(set(guids)) != len(guids):
        raise LayoutError(f'{L.name}: two rows have the same unique GUID')
    rows = L.resolve(first_usable, last_usable)
    for i, p, s, e in rows:
        if s < first_usable or e < s or e > last_usable:
            raise LayoutError(f'{L.name}: {p.name} {s}..{e} is outside the usable blocks {first_usable}..{last_usable}')
    # A GPT may list its entries in any block order (the index is the sdaN number, which other OSes rely on,
    # so moving a partition must not renumber the rest). A row out of block order needs an explicit start.
    prev_e, prev_n = first_usable - 1, None
    for i, p, s, e in sorted(rows, key=lambda r: r[2]):
        if s <= prev_e:
            raise LayoutError(f'{L.name}: {p.name} starts at {s}, inside {prev_n or "the GPT"} (ends {prev_e})')
        prev_e, prev_n = e, p.name
    starts = [s for _, _, s, _ in rows]
    for k, (i, p, s, e) in enumerate(rows):
        if p.start is None and k and s < max(starts[:k]):
            raise LayoutError(f'{L.name}: {p.name} is out of block order and has no explicit start')
    return rows


def show(L, total_blocks=15616000, out=sys.stdout):
    first, last = 6, total_blocks - 6
    rows = validate(L, first, last)
    print(f'{L.name}: {L.title}', file=out)
    print(f'  UFS LU0 {total_blocks} blocks of 4 KiB ({total_blocks * BLOCK / 2**30:.2f} GiB); usable {first}..{last}',
          file=out)
    print(f'  {"#":>3} {"name":<11} {"first":>9} {"last":>9} {"size":>10}  {"content":<9} {"type":<10} role', file=out)
    tn = {MS_BASIC: 'basic', EFI_SYSTEM: 'EFI', LINUX_DATA: 'linux', SBMAINT_TYPE: 'sbmaint'}
    for i, p, s, e in rows:
        n = (e - s + 1) * BLOCK
        size = f'{n / 2**30:.2f} GiB' if n >= 2**30 else f'{n / 2**20:.1f} MiB' if n >= 2**20 else f'{n // 1024} KiB'
        print(f'  {i:>3} {p.name:<11} {s:>9} {e:>9} {size:>10}  {p.content:<9} {tn.get(p.type, p.type[:8]):<10} {p.role}',
              file=out)
    for n in L.notes:
        print(f'  note: {n}', file=out)


def main(argv):
    if not argv or argv[0] not in ('show', 'check'):
        sys.exit(__doc__)
    if argv[0] == 'check':
        for L in LAYOUTS.values():
            validate(L)
            print(f'ok  {L.name}: {len(L.parts)} rows')
        for L in PROPOSALS.values():
            validate(L)
            print(f'ok  {L.name}: {len(L.parts)} rows (proposal, not a target)')
        print(f'active: {ACTIVE}')
        return
    blocks = int(argv[argv.index('--blocks') + 1]) if '--blocks' in argv else 15616000
    names = [a for a in argv[1:] if not a.startswith('--') and not a.isdigit()]
    for n in names or [ACTIVE]:
        show(PROPOSALS[n] if n in PROPOSALS else get(n), blocks)


if __name__ == '__main__':
    main(sys.argv[1:])
