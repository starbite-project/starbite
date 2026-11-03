#!/usr/bin/env python3
"""Give a known-good S9 ADT a Mac identity for a macOS userland (docs/MACOS.md, "Device identity").

  mkadt-mac.py BASE.adt MAC.adt OUT.adt [--serial S] [--keep-seed]
      BASE.adt  the S9 tree the kernel already boots with (out/starlte-iphone11-1792.adt:
                CPUs, 1792 MiB in bank 2, GIC, UFS, /pram, fbcon; 256-byte seed for 12377)
      MAC.adt   the real Mac device tree from the IPSW
                (Firmware/all_flash/DeviceTree.j313ap.im4p, unwrapped with pyimg4)

Only identity changes; every S9 hardware node stays as it is:
  /           model, target-type, target-sub-type, compatible = <Mac> + the S9 names
              (SweetPlatformExpert matches 'samsung,a5xelte'), manufacturer, and a
              synthetic serial-number (IOPlatformSerialNumber)
  /product    replaced by the Mac's (product-name, artwork-device-idiom "mac",
              graphics-featureset-class, ...), iBoot's syscfg placeholders dropped
  /chosen     chip-id, board-id of the Mac; display-scale 2; a fresh 256-byte seed
  /defaults   sleep-disabled = 1 (IOPMrootDomain never allows system sleep, as in Apple's
              VMs: macOS's powerd idle-sleeps a laptop and the S9 port has no sleep path;
              --allow-sleep leaves it out)
  /efi/platform  system-id (16 bytes, from the serial). On arm64 IOPlatformUUID does NOT come
              from here: generatePlatformUUID() hashes /chosen unique-chip-id + chip-id
              (the base ADT has both, so the UUID exists); system-id and --nvram's
              /options platform-uuid are the x86_64 sources. --nvram still brings up an
              (in-memory, controller-less) IODTNVRAM on /options
"""
import argparse, hashlib, os, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'ref/ios-herolte/tools'))
import adt     # noqa: E402
import mkadt   # noqa: E402

MACS = {  # target-sub-type -> (model, target-type, chip-id, board-id): BuildManifest ApChipID/ApBoardID
    'J313AP': ('MacBookAir10,1', 'J313', 0x8103, 0x26),
    'J274AP': ('Macmini9,1', 'J274', 0x8103, 0x22),
    'J293AP': ('MacBookPro17,1', 'J293', 0x8103, 0x24),
}


def props(node):
    return {k: v for k, f, v in node.props}


def name_of(node):
    return props(node).get('name', b'').rstrip(b'\0').decode()


def setp(node, key, val):
    for i, (k, f, _) in enumerate(node.props):
        if k == key:
            node.props[i] = (k, 0, val)
            return
    node.props.append((key, 0, val))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', type=Path)
    ap.add_argument('mac', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--serial', default='STARBITES9M1')
    ap.add_argument('--keep-seed', action='store_true')
    ap.add_argument('--nvram', action='store_true', help='UUID in a new /options (IODTNVRAM) instead of /efi/platform')
    ap.add_argument('--allow-sleep', action='store_true', help='no /defaults sleep-disabled')
    a = ap.parse_args()
    root, _ = adt.parse(a.base.read_bytes())
    mac, _ = adt.parse(a.mac.read_bytes())
    sub = props(mac)['target-sub-type'].rstrip(b'\0').decode()
    if sub not in MACS:
        sys.exit(f'{a.mac}: {sub} not in {sorted(MACS)}')
    model, ttype, chip, board = MACS[sub]
    assert props(mac)['model'].rstrip(b'\0').decode() == model, 'model differs from the table'

    setp(root, 'model', mkadt.cstr(model))
    setp(root, 'target-type', mkadt.cstr(ttype))
    setp(root, 'target-sub-type', mkadt.cstr(sub))
    setp(root, 'manufacturer', mkadt.cstr('Apple Inc.'))
    setp(root, 'serial-number', mkadt.cstr(a.serial))
    old = [n for n in props(root).get('compatible', b'').split(b'\0') if n.startswith(b'samsung,')]
    setp(root, 'compatible', b''.join(mkadt.cstr(s) for s in (sub, model)) +
         b''.join(n + b'\0' for n in old) + mkadt.cstr('AppleARM'))

    prod = next(c for c in root.children if name_of(c) == 'product')
    phandle = props(prod).get('AAPL,phandle')
    macprod = next(c for c in mac.children if name_of(c) == 'product')
    prod.props = [('name', 0, mkadt.cstr('product'))] + ([('AAPL,phandle', 0, phandle)] if phandle else []) + \
        [(k, 0, v) for k, f, v in macprod.props if not f and k not in ('name', 'AAPL,phandle')]
    setp(prod, 'artwork-scale-factor', mkadt.u32(2))

    chosen = next(c for c in root.children if name_of(c) == 'chosen')
    setp(chosen, 'chip-id', mkadt.u32(chip))
    setp(chosen, 'board-id', mkadt.u32(board))
    setp(chosen, 'display-scale', mkadt.u32(2))
    if not a.keep_seed:
        setp(chosen, 'random-seed', os.urandom(256))

    # IOPlatformExpertDevice::generatePlatformUUID() (IOPlatformExpert.cpp) on arm64 hashes
    # /chosen unique-chip-id (8 bytes) + chip-id (4 bytes); /efi/platform system-id and
    # /options platform-uuid are its x86_64 inputs. Kept for the x86 path; an /options node
    # also creates an IODTNVRAM (createNVRAM).
    assert len(props(chosen).get('unique-chip-id', b'')) == 8, 'base ADT has no unique-chip-id: no IOPlatformUUID'
    if not a.allow_sleep:
        # IOPMrootDomain reads PE_get_default("sleep-disabled") when powerd reports boot complete
        defaults = next((c for c in root.children if name_of(c) == 'defaults'), None)
        if defaults is None:
            defaults = mkadt.node('defaults', [])
            root.children.append(defaults)
        setp(defaults, 'sleep-disabled', mkadt.u32(1))
    sysid = hashlib.sha256(b'starbite-s9:' + a.serial.encode()).digest()[:16]
    if a.nvram:
        opts = next((c for c in root.children if name_of(c) == 'options'), None)
        if opts is None:
            opts = mkadt.node('options', [])
            root.children.append(opts)
        uuid = bytearray(sysid)
        uuid[6] = (uuid[6] & 0x0f) | 0x50
        uuid[8] = (uuid[8] & 0x3f) | 0x80
        setp(opts, 'platform-uuid', bytes(uuid))
    else:
        root.children = [c for c in root.children if name_of(c) != 'efi']
        root.children.append(mkadt.node('efi', [], [mkadt.node('platform', [('system-id', sysid)])]))

    blob = adt.serialize(root)
    a.out.write_bytes(blob)
    print(f'{a.out}: {len(blob)} bytes, {sub} / {model}, chip-id 0x{chip:x} board-id 0x{board:x}, '
          f'serial {a.serial}, system-id {sysid.hex()}' + (' in /options' if a.nvram else ' in /efi/platform') +
          ('' if a.allow_sleep else ', /defaults sleep-disabled'))


if __name__ == '__main__':
    main()
