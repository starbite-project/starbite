#!/usr/bin/env python3
"""Starbite Maintenance (SBMAINT): the "SBMAINT1" container and the reference of what the loader does
with it (docs/MAINTENANCE.md 3 and 4). Python 3 standard library only; used by tools/maint/pack.py,
tools/maint/sbm and the tests. Nothing here touches a phone.

THE CONTAINER (one slot). Little endian. A 4096-byte header block, then the sections, each at a
4096-aligned offset (from the slot's first byte), in this order, padded with zeros to 4096:

  off   size   field
  0     8      magic "SBMAINT1"
  8     4      version = 1
  12    4      header_size = 4096
  16    4      seq: generation, >= 1. Of two valid slots the loader starts the one with the higher seq
  20    4      flags = 0 (reserved; readers ignore unknown bits)
  24    8      total: bytes from the slot's start to the end of the last section (a 4096 multiple)
  32    8      build_time: Unix seconds (shown only)
  40    24     reserved, 0
  64    4x32   sections[4] = { u64 offset; u64 size; u32 crc32; u32 kind; u64 reserved }
               [0] kernel (kind 1, an arm64 Image), [1] dtb (kind 2, USB device: maintenance),
               [2] initrd (kind 3), [3] dtb-rescue (kind 4, USB host: rescue mode; size 0 = none)
  192   256    cmdline: NUL-terminated ASCII, appended to the DTB's /chosen/bootargs
  448   64     version string: NUL-terminated ASCII ("sbmaint 2026-11-06 f1b3d9a"), shown on screens
  512   4x32   SHA-256 of each section (hosts check them; the loader does not need to)
  640   ...    zeros
  4092  4      header_crc32: zlib CRC-32 of bytes 0..4091

CRC-32 is zlib.crc32 (IEEE 802.3), the same as the OS store and the SBRESCUE bundle.

THE PARTITION "SBMAINT" holds two slots: A at byte 0 and B at slot_b_offset(size) (half the partition,
rounded down to 1 MiB). A container must fit its slot and STAGE_MAX (the loader stages it whole in its
80 MiB download buffer and checks every CRC before it places anything)."""
import hashlib
import struct
import zlib

MAGIC = b'SBMAINT1'
VERSION = 1
HDR = 4096
ALIGN = 4096
STAGE_MAX = 0x04FF0000           # the loader's download buffer (docs/USB-FASTBOOT.md 3): 80 MiB - 64 KiB
KINDS = ('kernel', 'dtb', 'initrd', 'dtb-rescue')
REQUIRED = ('kernel', 'dtb', 'initrd')
LIMITS = {'kernel': 64 << 20, 'dtb': 2 << 20, 'initrd': 80 << 20, 'dtb-rescue': 2 << 20}
SEC_OFF, SEC_SIZE = 64, 32
CMDLINE_OFF, CMDLINE_LEN = 192, 256
VERSION_OFF, VERSION_LEN = 448, 64
SHA_OFF = 512
CRC_OFF = 4092
MIB = 1 << 20


class ContainerError(ValueError):
    pass


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def _pad(b):
    return b + bytes(-len(b) % ALIGN)


def _cstr(b):
    return b.split(b'\0', 1)[0].decode('ascii', 'replace')


# ---- a minimal FDT reader (enough to check what the loader and the environment rely on) -----------
def fdt_nodes(dtb):
    """{'/path': {prop: bytes}} of a flattened device tree; raises ContainerError if malformed."""
    if len(dtb) < 40 or dtb[:4] != b'\xd0\x0d\xfe\xed':
        raise ContainerError('not a flattened device tree (magic)')
    total, off_struct, off_strings = struct.unpack_from('>III', dtb, 4)
    size_strings, size_struct = struct.unpack_from('>II', dtb, 32)
    if total > len(dtb) or off_struct + size_struct > total or off_strings + size_strings > total:
        raise ContainerError('FDT header sizes are outside the blob')
    nodes, path, p, end = {}, [], off_struct, off_struct + size_struct
    while p < end:
        tok, = struct.unpack_from('>I', dtb, p)
        p += 4
        if tok == 1:                                   # FDT_BEGIN_NODE
            e = dtb.index(b'\0', p)
            path.append(dtb[p:e].decode('ascii', 'replace'))
            p = (e + 4) & ~3
            nodes.setdefault('/' + '/'.join(x for x in path if x), {})
        elif tok == 2:                                 # FDT_END_NODE
            if not path:
                raise ContainerError('FDT: unbalanced END_NODE')
            path.pop()
        elif tok == 3:                                 # FDT_PROP
            ln, nameoff = struct.unpack_from('>II', dtb, p)
            p += 8
            ne = dtb.index(b'\0', off_strings + nameoff)
            name = dtb[off_strings + nameoff:ne].decode('ascii', 'replace')
            nodes['/' + '/'.join(x for x in path if x)][name] = dtb[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == 4:                                 # FDT_NOP
            continue
        elif tok == 9:                                 # FDT_END
            break
        else:
            raise ContainerError(f'FDT: bad token {tok:#x}')
    return nodes


def dtb_facts(dtb):
    """What the loader and the environment need from a DTB: bootargs, the initrd placeholders, the
    USB controller's dr_mode, whether Linux's ramoops driver would bind (it overwrites the previous
    boot's log rings and the boot-next mailbox)."""
    n = fdt_nodes(dtb)
    ch = n.get('/chosen', {})
    usb = [v for k, v in n.items() if v.get('compatible', b'').startswith(b'snps,dwc3')]
    ramoops = [k for k, v in n.items() if v.get('compatible', b'') == b'ramoops\0']
    return {
        'bootargs': _cstr(ch['bootargs']) if 'bootargs' in ch else None,
        'initrd_props': 'linux,initrd-start' in ch and 'linux,initrd-end' in ch,
        'rtc_epoch': 'starbite,rtc-epoch' in ch,
        'dr_mode': _cstr(usb[0].get('dr_mode', b'')) if usb else None,
        'role_switch': bool(usb and 'usb-role-switch' in usb[0]),
        'ramoops_bound': bool(ramoops),
        'model': _cstr(n.get('/', {}).get('model', b'')),
    }


# ---- pack / parse -------------------------------------------------------------------------------
def pack(kernel, dtb, initrd, dtb_rescue=b'', cmdline='', version='', seq=1, build_time=0):
    """The container bytes. Checks what the loader would refuse (sizes, magics)."""
    blobs = {'kernel': kernel, 'dtb': dtb, 'initrd': initrd, 'dtb-rescue': dtb_rescue or b''}
    for k in REQUIRED:
        if not blobs[k]:
            raise ContainerError(f'{k} is empty')
    for k, b in blobs.items():
        if len(b) > LIMITS[k]:
            raise ContainerError(f'{k} is {len(b)} bytes, over {LIMITS[k]}')
    if kernel[56:60] != b'ARMd':
        raise ContainerError('kernel is not an arm64 Image ("ARMd" at +56)')
    for k in ('dtb', 'dtb-rescue'):
        if blobs[k] and blobs[k][:4] != b'\xd0\x0d\xfe\xed':
            raise ContainerError(f'{k} is not a flattened device tree')
    cmd = cmdline.encode('ascii')
    ver = version.encode('ascii')
    if len(cmd) >= CMDLINE_LEN or b'\0' in cmd:
        raise ContainerError(f'cmdline must be under {CMDLINE_LEN} bytes')
    if len(ver) >= VERSION_LEN or b'\0' in ver:
        raise ContainerError(f'version must be under {VERSION_LEN} bytes')
    if not 1 <= seq <= 0xFFFFFFFF:
        raise ContainerError('seq must be 1..2^32-1')
    h = bytearray(HDR)
    body, off = bytearray(), HDR
    for i, k in enumerate(KINDS):
        b = blobs[k]
        o = off if b else 0
        struct.pack_into('<QQIIQ', h, SEC_OFF + i * SEC_SIZE, o, len(b), crc32(b) if b else 0, i + 1, 0)
        h[SHA_OFF + i * 32:SHA_OFF + (i + 1) * 32] = hashlib.sha256(b).digest() if b else bytes(32)
        if b:
            body += _pad(b)
            off += len(_pad(b))
    total = HDR + len(body)
    if total > STAGE_MAX:
        raise ContainerError(f'container is {total} bytes, over the loader\'s staging buffer {STAGE_MAX:#x}')
    struct.pack_into('<8sIIIIQQ', h, 0, MAGIC, VERSION, HDR, seq, 0, total, build_time)
    h[CMDLINE_OFF:CMDLINE_OFF + len(cmd)] = cmd
    h[VERSION_OFF:VERSION_OFF + len(ver)] = ver
    struct.pack_into('<I', h, CRC_OFF, crc32(bytes(h[:CRC_OFF])))
    return bytes(h) + bytes(body)


def parse_header(h, limit=STAGE_MAX):
    """The header's fields, with the checks the loader makes BEFORE it reads the body (it reads `total`
    bytes next). `limit`: the slot size (and never more than STAGE_MAX)."""
    if len(h) < HDR:
        raise ContainerError('short header')
    magic, ver, hsz, seq, flags, total, btime = struct.unpack_from('<8sIIIIQQ', h, 0)
    if magic != MAGIC:
        raise ContainerError('no SBMAINT1 magic')
    if ver != VERSION or hsz != HDR:
        raise ContainerError(f'version {ver} / header size {hsz} unknown')
    want, = struct.unpack_from('<I', h, CRC_OFF)
    if crc32(bytes(h[:CRC_OFF])) != want:
        raise ContainerError('header CRC mismatch')
    if seq == 0:
        raise ContainerError('seq 0')
    if total % ALIGN or total < HDR or total > min(limit, STAGE_MAX):
        raise ContainerError(f'total {total} is not a 4096 multiple inside the slot ({min(limit, STAGE_MAX)})')
    secs, prev_end = {}, HDR
    for i, k in enumerate(KINDS):
        o, n, c, kind, _ = struct.unpack_from('<QQIIQ', h, SEC_OFF + i * SEC_SIZE)
        if kind != i + 1:
            raise ContainerError(f'section {i} kind {kind}, expected {i + 1}')
        if n == 0:
            if k in REQUIRED:
                raise ContainerError(f'{k} missing')
            if o or c:
                raise ContainerError(f'{k}: size 0 with offset/CRC set')
            secs[k] = (0, 0, 0)
            continue
        if n > LIMITS[k]:
            raise ContainerError(f'{k} {n} bytes over {LIMITS[k]}')
        if o % ALIGN or o < prev_end or o + n > total:
            raise ContainerError(f'{k} at {o}+{n} is not aligned, overlaps or leaves the container')
        prev_end = o + n
        secs[k] = (o, n, c)
    return {'seq': seq, 'flags': flags, 'total': total, 'build_time': btime, 'sections': secs,
            'cmdline': _cstr(h[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN]),
            'version': _cstr(h[VERSION_OFF:VERSION_OFF + VERSION_LEN]),
            'sha256': {k: bytes(h[SHA_OFF + i * 32:SHA_OFF + (i + 1) * 32]).hex() for i, k in enumerate(KINDS)}}


def verify(c, limit=STAGE_MAX, deep=True):
    """parse_header + every section's CRC-32 (what the loader checks) and, with deep, SHA-256, the
    kernel/DTB magics, and the DTB facts the loader and the environment need. Returns the header dict
    with 'blobs' and 'dtb' facts added; raises ContainerError."""
    hd = parse_header(c, limit)
    if len(c) < hd['total']:
        raise ContainerError(f'container is {len(c)} bytes, header says {hd["total"]}')
    blobs = {}
    for k, (o, n, crc) in hd['sections'].items():
        b = bytes(c[o:o + n])
        if n and crc32(b) != crc:
            raise ContainerError(f'{k} CRC mismatch')
        if deep and n and hashlib.sha256(b).hexdigest() != hd['sha256'][k]:
            raise ContainerError(f'{k} SHA-256 mismatch')
        blobs[k] = b
    if deep:
        if blobs['kernel'][56:60] != b'ARMd':
            raise ContainerError('kernel is not an arm64 Image')
        facts = {}
        for k in ('dtb', 'dtb-rescue'):
            if not blobs[k]:
                continue
            f = dtb_facts(blobs[k])
            if not f['bootargs']:
                raise ContainerError(f'{k} has no /chosen/bootargs (the loader refuses it)')
            if not f['initrd_props']:
                raise ContainerError(f'{k} has no /chosen linux,initrd-start/-end placeholders (the loader refuses it)')
            facts[k] = f
        if facts['dtb']['dr_mode'] not in ('peripheral', 'otg'):
            raise ContainerError(f'dtb: the USB controller is dr_mode "{facts["dtb"]["dr_mode"]}"; maintenance '
                                 'needs a USB device (peripheral)')
        if 'dtb-rescue' in facts and facts['dtb-rescue']['dr_mode'] not in ('host', 'otg'):
            raise ContainerError('dtb-rescue: the USB controller is not host/otg (rescue mode needs USB host)')
        hd['dtb'] = facts
    hd['blobs'] = blobs
    return hd


def restamp(c, seq):
    """The same container with another seq (header CRC renewed)."""
    h = bytearray(c[:HDR])
    struct.pack_into('<I', h, 16, seq)
    struct.pack_into('<I', h, CRC_OFF, crc32(bytes(h[:CRC_OFF])))
    return bytes(h) + bytes(c[HDR:])


# ---- the partition: two slots, and the loader's choice (the reference for shim/starlte) ---------
def slot_b_offset(part_size):
    """Byte offset of slot B: half the partition, rounded down to 1 MiB."""
    return (part_size // 2) // MIB * MIB


def slots(part_size):
    b = slot_b_offset(part_size)
    return {'a': (0, b), 'b': (b, part_size - b)}


def read_slot(read, off, size, deep=False):
    """(header dict incl. blobs, None) or (None, why) for the slot at off; read(off, n) -> bytes."""
    try:
        hd = parse_header(read(off, HDR), min(size, STAGE_MAX))
        return verify(read(off, hd['total']), min(size, STAGE_MAX), deep=deep), None
    except ContainerError as e:
        return None, str(e)


def choose(read, part_size, prev=False):
    """What the loader starts from the SBMAINT partition: [(slot, header)] in the order it tries them
    (valid slots only), plus {slot: why} for the invalid ones. Order: higher seq first, A before B on a
    tie. prev=True (boot-next key "maint-prev") swaps the order."""
    found, bad = [], {}
    for name, (off, size) in slots(part_size).items():
        hd, why = read_slot(read, off, size)
        if hd:
            found.append((name, hd))
        else:
            bad[name] = why
    found.sort(key=lambda x: (-x[1]['seq'], x[0]))
    if prev:
        found.reverse()
    return found, bad


def plan_update(read, part_size, container):
    """Where `sbm update` writes a new container: the slot the loader would NOT start now (invalid, or
    lower seq), restamped with seq = the highest valid seq + 1. Never the slot that boots today, so a
    torn write leaves the old one bootable. Returns (slot, offset, restamped bytes)."""
    found, bad = choose(read, part_size)
    seqs = [hd['seq'] for _, hd in found]
    if found:
        target = 'b' if found[0][0] == 'a' else 'a'
    else:
        target = 'a'
    off, size = slots(part_size)[target]
    new = restamp(container, (max(seqs) if seqs else 0) + 1)
    if len(new) > min(size, STAGE_MAX):
        raise ContainerError(f'container {len(new)} bytes does not fit slot {target} ({size} bytes)')
    return target, off, new


def partition_image(part_size, a=None, b=None):
    """A whole SBMAINT partition image (zeros elsewhere), for rehearsals and tests."""
    img = bytearray(part_size)
    for name, c in (('a', a), ('b', b)):
        if c is None:
            continue
        off, size = slots(part_size)[name]
        if len(c) > size:
            raise ContainerError(f'slot {name}: {len(c)} > {size}')
        img[off:off + len(c)] = c
    return bytes(img)


# ---- ramoops / pstore rings (sbm logs) ----------------------------------------------------------
PRZ_SIG = 0x43474244                       # "DBGC": fs/pstore/ram_core.c persistent_ram_buffer


def ramoops_zones(raw, base=0xFED10000, record=0x4000, console=0x4000):
    """The text of each persistent_ram zone in a raw dump of the ramoops region as the daily DTB lays it
    out (record-size 16 KiB at +0, console-size 16 KiB at +16 KiB; the loader's pmsg ring is the second
    zone, docs/USB-FASTBOOT.md 6.1). Zone header: u32 sig, u32 start (write offset), u32 size (bytes used),
    then the ring. Returns [(name, phys, text or None)]."""
    out = []
    for name, off, ln in (('dmesg', 0, record), ('console-pmsg', record, console)):
        z = raw[off:off + ln]
        if len(z) < 12:
            out.append((name, base + off, None))
            continue
        sig, start, size = struct.unpack_from('<III', z, 0)
        cap = ln - 12
        if sig != PRZ_SIG or start > cap or size > cap:
            out.append((name, base + off, None))
            continue
        data = z[12:12 + cap]
        text = data[start:size] + data[:start] if size == cap else data[:size]
        out.append((name, base + off, text.replace(b'\0', b'').decode('utf-8', 'replace')))
    return out
