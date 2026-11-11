#!/usr/bin/env python3
"""The card-only BOOT (docs/STORE-ON-CARD.md section 6): the BOOT image now on the phone with only two changes,
the loader Image replaced and " sbstore=card" appended to the SWEETXNU payload's command line (the loader's own
command line). Ramdisk (menu assets), DTBH, header fields, the payload's other bytes and the bundles after it
stay as they are, and are checked byte for byte afterwards.

  boot-card.py CURRENT_BOOT.img LOADER.Image OUT.img [--ramdisk NEW.sbm]

--ramdisk: the menu assets replaced too (tools/brand/menu7-assets.py's build/brand/menu-v7.sbm); checked to be
exactly that file, everything else as above.

CURRENT_BOOT.img: BOOT as pulled from the phone just before the flash (it is the rollback image too). Host only.
"""
import hashlib
import importlib.util
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACK = HERE.parent / 'starlte-pack-boot.py'
spec = importlib.util.spec_from_file_location('packboot', PACK)
pb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pb)

TOKEN = b'sbstore=card'
CMD_OFF, CMD_LEN = 0x48, 608          # struct payload: cmdline[BOOT_LINE_LENGTH] (607 + NUL)


def find_payload(kernel):
    """Offset of the SWEETXNU payload appended to the loader (4 KiB aligned, as pack places it)."""
    off = 0
    while True:
        off = kernel.find(b'SWEETXNU', off)
        if off < 0:
            sys.exit('no SWEETXNU payload in the BOOT kernel slot')
        if off % 4096 == 0 and off > 0:
            return off
        off += 1


def main():
    args = sys.argv[1:]
    new_rd = None
    if '--ramdisk' in args:
        i = args.index('--ramdisk')
        new_rd = Path(args[i + 1]).read_bytes()
        del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    cur_path, loader_path, out_path = map(Path, args)
    cur = cur_path.read_bytes()
    h = pb.parse(cur)
    kernel = h['kernel']
    off = find_payload(kernel)
    tail = bytearray(kernel[off:])
    cmd = bytes(tail[CMD_OFF:CMD_OFF + CMD_LEN]).split(b'\0')[0]
    print(f'current BOOT: loader {off} bytes, payload + bundles {len(tail)} bytes, ramdisk {len(h["ramdisk"])}')
    print(f'payload command line ({len(cmd)} bytes) ends: ...{cmd[-60:].decode(errors="replace")}')
    if b'starmenu7=' not in cmd:
        sys.exit('the payload command line has no starmenu7=: not the loader\'s BOOT? refusing')
    if TOKEN in cmd.split():
        print('sbstore=card is already there')
        new = cmd
    else:
        new = cmd + b' ' + TOKEN
    if len(new) >= CMD_LEN:
        sys.exit(f'command line would be {len(new)} bytes (max {CMD_LEN - 1})')
    tail[CMD_OFF:CMD_OFF + CMD_LEN] = new.ljust(CMD_LEN, b'\0')
    loader = Path(loader_path).read_bytes()
    if b'sbstore=card' not in loader:
        sys.exit(f'{loader_path}: this loader does not know sbstore=card (built before the card store?)')
    with tempfile.TemporaryDirectory() as t:
        (Path(t) / 'tail').write_bytes(tail)
        (Path(t) / 'rd').write_bytes(h['ramdisk'] if new_rd is None else new_rd)
        subprocess.run([sys.executable, str(PACK), 'pack', str(cur_path), str(loader_path), str(out_path),
                        '--append', str(Path(t) / 'tail'), '--ramdisk', str(Path(t) / 'rd')], check=True,
                       stdout=subprocess.DEVNULL)
    # check: everything but the loader and the one command line is the phone's
    o = pb.parse(Path(out_path).read_bytes())
    ok = True
    def chk(c, what):
        nonlocal ok
        print(('  ok   ' if c else '  FAIL ') + what)
        ok &= bool(c)
    if new_rd is None:
        chk(o['ramdisk'] == h['ramdisk'], 'ramdisk (menu assets) unchanged')
    else:
        chk(o['ramdisk'] == new_rd and new_rd.find(b'SB7ASSET') > 0, 'ramdisk = the new menu assets (SB7ASSET inside)')
    chk(o['dt'] == h['dt'], 'DTBH unchanged')
    chk((o['kernel_addr'], o['ramdisk_addr'], o['tags_addr'], o['os_version'], o['name'], o['cmdline'], o['extra_cmdline']) ==
        (h['kernel_addr'], h['ramdisk_addr'], h['tags_addr'], h['os_version'], h['name'], h['cmdline'], h['extra_cmdline']),
        'BOOT header fields unchanged')
    ok2 = o['kernel'][32:len(loader)] == loader[32:]
    chk(ok2, 'loader = the given Image (Image header fields 8..31 from the template, as always)')
    noff = find_payload(o['kernel'])
    ntail = o['kernel'][noff:]
    chk(noff % 4096 == 0 and noff >= len(loader), f'payload at {noff} (4 KiB aligned, after the loader)')
    diff = [i for i in range(len(tail)) if i >= len(ntail) or ntail[i] != kernel[off + i]]
    allowed = set(range(CMD_OFF, CMD_OFF + CMD_LEN))
    chk(len(ntail) == len(tail) and all(i in allowed for i in diff),
        f'payload + bundles unchanged except the command line ({len(diff)} bytes differ, all inside it)')
    ncmd = bytes(ntail[CMD_OFF:CMD_OFF + CMD_LEN]).split(b'\0')[0]
    chk(ncmd.split()[-1] == TOKEN and ncmd.startswith(cmd), 'command line = the old one + " sbstore=card"')
    img_size = struct.unpack_from('<Q', o['kernel'], 16)[0]
    chk(img_size >= noff + len(ntail), f'Image header image_size {img_size:#x} covers the payload')
    if not ok:
        Path(out_path).unlink()
        sys.exit('checks failed: no image written')
    sha = hashlib.sha256(Path(out_path).read_bytes()).hexdigest()
    Path(str(out_path) + '.sha256').write_text(f'{sha}  {Path(out_path).name}\n')
    print(f'{out_path}: sha256 {sha}')


if __name__ == '__main__':
    main()
