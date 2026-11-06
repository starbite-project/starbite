#!/usr/bin/env python3
"""mk-boot-files.py : the four OS-store files that start One UI 8 (UN1CA 9810) from the Starbite loader's
`android` method (docs/ANDROID-ONEUI.md section 3, docs/STARBITE-HEALTH.md section 6).

  mk-boot-files.py --ramdisk-img RAMDISK.IMG --kernel-zip DS-ACK...-erofs-dtb.zip --out DIR
                   [--dev SYSTEM=NAME ...] [--cmdline TEXT]

  DIR/android.kernel   floyd/G960F-kernel from the DS-ACK package (arm64 Image), as it is
  DIR/android.ramdisk  the cpio from UN1CA's ramdisk.img (an Android boot image without a kernel), with
                       /data moved from USERDATA to A16DATA in every fstab inside it (USERDATA holds the
                       iOS slot and the OS store and must never be mounted or formatted by Android).
                       Edited entry by entry, so every other byte, owner and mode stays as UN1CA ships it.
  DIR/android.dtb      floyd/G960F-dtb (a DTBH) as it is, or with the first-stage fstab's by-name devices
                       renamed in place (--dev SYSTEM=OSYSTM: same length only, so the FDT layout is unchanged)
  DIR/android.cmdline  appended by the loader after S-Boot's command line
  DIR/SHA256SUMS
Refuses when anything is not exactly as expected (no partial output is left behind as final)."""
import argparse
import hashlib
import struct
import sys
import zipfile
from pathlib import Path

CMDLINE = 'androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on'
BYNAME = b'/dev/block/platform/11120000.ufs/by-name/'


def die(msg):
    sys.exit(f'mk-boot-files: {msg}')


def bootimg_ramdisk(b):
    if b[:8] != b'ANDROID!':
        die('ramdisk image: no ANDROID! header')
    ks, _, rs, _, ss, _, _, ps = struct.unpack_from('<8I', b, 8)
    if ks != 0 or ss != 0:
        die(f'ramdisk image: expected no kernel/second stage, got kernel {ks} second {ss}')
    off = ps * (1 + (ks + ps - 1) // ps)
    rd = b[off:off + rs]
    if len(rd) != rs or rd[:6] != b'070701':
        die('ramdisk image: the ramdisk is not an uncompressed newc cpio')
    return rd


def cpio_entries(rd):
    i, out = 0, []
    while True:
        h = rd[i:i + 110]
        if h[:6] != b'070701':
            die(f'cpio: bad header at {i}')
        f = [int(h[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
        nsz, fsz = f[11], f[6]
        name = rd[i + 110:i + 110 + nsz - 1]
        p = (i + 110 + nsz + 3) & ~3
        data = rd[p:p + fsz]
        out.append([h, name, data])
        i = (p + fsz + 3) & ~3
        if name == b'TRAILER!!!':
            return out, rd[i:]


def cpio_pack(entries, tail):
    o = bytearray()
    for h, name, data in entries:
        h = bytearray(h)
        h[6 + 8 * 6:14 + 8 * 6] = b'%08X' % len(data)
        o += h + name + b'\0'
        o += b'\0' * ((-len(o)) % 4)
        o += data
        o += b'\0' * ((-len(o)) % 4)
    return bytes(o) + tail


def patch_ramdisk(rd):
    ents, tail = cpio_entries(rd)
    if cpio_pack(ents, tail) != rd:
        die('cpio: re-packing the untouched archive does not give the same bytes; refusing to edit it')
    changed = []
    for e in ents:
        mode = int(e[0][14:22], 16)
        if (mode & 0o170000) != 0o100000:
            continue
        if BYNAME + b'USERDATA' in e[2]:
            e[2] = e[2].replace(BYNAME + b'USERDATA\t', BYNAME + b'A16DATA\t').replace(
                BYNAME + b'USERDATA ', BYNAME + b'A16DATA ')
            if BYNAME + b'USERDATA' in e[2]:
                die(f'{e[1].decode()}: USERDATA is still named after the edit')
            changed.append(e[1].decode())
    out = cpio_pack(ents, tail)
    for e in cpio_entries(out)[0]:
        if b'by-name/USERDATA' in e[2]:
            die(f'{e[1].decode()} still names USERDATA')
    return out, changed


def dtbh_fdts(b):
    if b[:4] != b'DTBH':
        die('dtb: not a DTBH')
    ver, n = struct.unpack_from('<II', b, 4)
    out = []
    for k in range(n):
        chip, plat, sub, r0, r1, off, size, _ = struct.unpack_from('<8I', b, 12 + 32 * k)
        if b[off:off + 4] != bytes.fromhex('d00dfeed'):
            die(f'dtb: entry {k} has no FDT at {off:#x}')
        out.append((off, size))
    return out


def patch_dtb(b, renames):
    b = bytearray(b)
    for off, size in dtbh_fdts(b):
        fdt = b[off:off + size]
        for old, new in renames.items():
            o, n = BYNAME + old.encode() + b'\0', BYNAME + new.encode() + b'\0'
            if len(o) != len(n):
                die(f'--dev {old}={new}: the new name must have the same length')
            if fdt.count(o) != 1:
                die(f'dtb: {o[:-1].decode()} found {fdt.count(o)} times in the FDT at {off:#x} (expected 1)')
            fdt = fdt.replace(o, n)
        b[off:off + size] = fdt
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ramdisk-img', type=Path, required=True)
    ap.add_argument('--kernel-zip', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--dev', action='append', default=[], metavar='OLD=NEW')
    ap.add_argument('--cmdline', default=CMDLINE)
    a = ap.parse_args()
    renames = dict(x.split('=', 1) for x in a.dev)
    with zipfile.ZipFile(a.kernel_zip) as z:
        kernel = z.read('floyd/G960F-kernel')
        dtb = z.read('floyd/G960F-dtb')
    if kernel[56:60] != b'ARMd':
        die('kernel: not an arm64 Image')
    fdts = dtbh_fdts(dtb)
    for off, size in fdts:
        if b'erofs' not in dtb[off:off + size]:
            die('dtb: the first-stage fstab is not EROFS (wrong DS-ACK package?)')
    rd, changed = patch_ramdisk(bootimg_ramdisk(a.ramdisk_img.read_bytes()))
    if not changed:
        die('ramdisk: no fstab named USERDATA (unexpected for UN1CA 9810: check the ramdisk)')
    dtb2 = patch_dtb(dtb, renames) if renames else dtb
    a.out.mkdir(parents=True, exist_ok=True)
    files = {'android.kernel': kernel, 'android.ramdisk': rd, 'android.dtb': dtb2,
             'android.cmdline': a.cmdline.encode()}
    sums = []
    for n, d in files.items():
        (a.out / n).write_bytes(d)
        sums.append(f'{hashlib.sha256(d).hexdigest()}  {n}')
    (a.out / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
    print(f'ramdisk: /data -> A16DATA in {", ".join(changed)}; dtb: {len(fdts)} FDT(s)'
          + (f', devices renamed {renames}' if renames else ', unchanged'))
    print('\n'.join(sums))


if __name__ == '__main__':
    main()
