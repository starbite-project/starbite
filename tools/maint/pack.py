#!/usr/bin/env python3
"""Pack, check and take apart Starbite Maintenance containers ("SBMAINT1", tools/maint/sbmaint.py,
docs/MAINTENANCE.md 3). Host only; touches no phone.

  pack.py pack OUT --kernel Image --dtb maint.dtb --initrd maint.initrd [--dtb-rescue maint-rescue.dtb]
               [--cmdline "..."] [--version "..."] [--seq N]
  pack.py verify FILE [--slot-size BYTES]    every check the loader makes (header CRC, bounds, section CRCs)
                                             plus SHA-256, kernel/DTB magics and the DTB facts; exit 1 on failure
  pack.py show FILE                          the header (no body checks)
  pack.py extract FILE DIR                   kernel, dtb, initrd, dtb-rescue, cmdline.txt (for fastboot boot)
  pack.py store-entries FILE DIR             the OS-store files for phones without the partition:
                                             maint.image maint.dtb maint.initrd [maint-rescue.dtb]
  pack.py partition OUT --size BYTES [--a FILE] [--b FILE]   a whole SBMAINT partition image (rehearsals)
  pack.py select IMG [--prev]                which slot the loader starts from a partition image
"""
import argparse
import os
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import sbmaint as sm  # noqa: E402


def human(n):
    return f'{n / sm.MIB:.2f} MiB' if n >= sm.MIB else f'{n} B'


def describe(hd, out=sys.stdout):
    print(f'SBMAINT1 seq {hd["seq"]}, {human(hd["total"])}, built '
          f'{time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime(hd["build_time"])) if hd["build_time"] else "?"}', file=out)
    print(f'  version  {hd["version"] or "-"}', file=out)
    print(f'  cmdline  {hd["cmdline"] or "-"}', file=out)
    for k, (o, n, c) in hd['sections'].items():
        if n:
            print(f'  {k:<10} @{o:>9}  {n:>9} B  crc {c:08x}  sha256 {hd["sha256"][k][:16]}', file=out)
        else:
            print(f'  {k:<10} (none)', file=out)
    for k, f in hd.get('dtb', {}).items():
        print(f'  {k}: dr_mode {f["dr_mode"]}{" + role switch" if f["role_switch"] else ""}; ramoops '
              f'{"bound (Linux reuses the log rings)" if f["ramoops_bound"] else "kept (previous boot logs survive)"}; '
              f'bootargs "{f["bootargs"]}"', file=out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack')
    p.add_argument('out')
    p.add_argument('--kernel', required=True)
    p.add_argument('--dtb', required=True)
    p.add_argument('--initrd', required=True)
    p.add_argument('--dtb-rescue')
    p.add_argument('--cmdline', default='')
    p.add_argument('--version', default='')
    p.add_argument('--seq', type=int, default=1)
    p = sub.add_parser('verify')
    p.add_argument('file')
    p.add_argument('--slot-size', type=int, default=sm.STAGE_MAX)
    p = sub.add_parser('show')
    p.add_argument('file')
    p = sub.add_parser('extract')
    p.add_argument('file')
    p.add_argument('dir')
    p = sub.add_parser('store-entries')
    p.add_argument('file')
    p.add_argument('dir')
    p = sub.add_parser('partition')
    p.add_argument('out')
    p.add_argument('--size', type=int, required=True)
    p.add_argument('--a')
    p.add_argument('--b')
    p = sub.add_parser('select')
    p.add_argument('img')
    p.add_argument('--prev', action='store_true')
    a = ap.parse_args(argv)

    def rd(path):
        with open(path, 'rb') as f:
            return f.read()

    try:
        if a.cmd == 'pack':
            c = sm.pack(rd(a.kernel), rd(a.dtb), rd(a.initrd), rd(a.dtb_rescue) if a.dtb_rescue else b'',
                        cmdline=a.cmdline, version=a.version, seq=a.seq, build_time=int(time.time()))
            hd = sm.verify(c)
            with open(a.out + '.part', 'wb') as f:
                f.write(c)
            os.replace(a.out + '.part', a.out)
            describe(hd)
            print(f'{a.out}: {human(len(c))} of the loader\'s {human(sm.STAGE_MAX)}')
        elif a.cmd == 'verify':
            hd = sm.verify(rd(a.file), a.slot_size)
            describe(hd)
            print(f'{a.file}: OK (every CRC-32 and SHA-256 matches)')
        elif a.cmd == 'show':
            describe(sm.parse_header(rd(a.file)[:sm.HDR]))
        elif a.cmd in ('extract', 'store-entries'):
            hd = sm.verify(rd(a.file))
            os.makedirs(a.dir, exist_ok=True)
            names = ({'kernel': 'kernel', 'dtb': 'dtb', 'initrd': 'initrd', 'dtb-rescue': 'dtb-rescue'}
                     if a.cmd == 'extract' else
                     {'kernel': 'maint.image', 'dtb': 'maint.dtb', 'initrd': 'maint.initrd',
                      'dtb-rescue': 'maint-rescue.dtb'})
            for k, b in hd['blobs'].items():
                if b:
                    with open(os.path.join(a.dir, names[k]), 'wb') as f:
                        f.write(b)
                    print(f'{os.path.join(a.dir, names[k])}: {human(len(b))}')
            if a.cmd == 'extract':
                with open(os.path.join(a.dir, 'cmdline.txt'), 'w') as f:
                    f.write(hd['cmdline'] + '\n')
            else:
                print(f'catalog cmdline for the hidden "maint" entry: sb.maint=1 sb.maint.src=store {hd["cmdline"]}')
        elif a.cmd == 'partition':
            img = sm.partition_image(a.size, rd(a.a) if a.a else None, rd(a.b) if a.b else None)
            with open(a.out, 'wb') as f:
                f.write(img)
            print(f'{a.out}: {human(len(img))}, slot B at {sm.slot_b_offset(a.size)}')
        elif a.cmd == 'select':
            size = os.path.getsize(a.img)
            with open(a.img, 'rb') as f:
                def read(off, n):
                    f.seek(off)
                    return f.read(n)
                found, bad = sm.choose(read, size, prev=a.prev)
            for name, why in bad.items():
                print(f'slot {name}: invalid ({why})')
            for name, hd in found:
                print(f'slot {name}: seq {hd["seq"]} {hd["version"]}')
            if not found:
                print('no valid slot: the loader goes on to the OS store (maint.*)')
                return 1
            print(f'the loader starts slot {found[0][0]}' + (' (maint-prev)' if a.prev else ''))
    except (sm.ContainerError, OSError) as e:
        print(f'pack.py {a.cmd}: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
