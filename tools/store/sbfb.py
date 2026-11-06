#!/usr/bin/env python3
"""Drive the phone's Starbite loader over USB fastboot (docs/USB-FASTBOOT.md section 11).

The loader serves fastboot not only in its fastboot mode but also in the background while the boot
menu (v7), Starbite Recovery's menu or the crash report waits for keys. This wraps the `fastboot`
tool (Android platform-tools) for that:

  sbfb.py status                        mode (menu, recovery, crash, fastboot), battery, catalog
  sbfb.py wait [MODE] [--timeout S]     until the phone answers (in MODE, if given)
  sbfb.py key down down power           presses for the menu on screen (one after the other)
  sbfb.py key bixby:900                 a hold (900 ms)
  sbfb.py screen out.png [--half]       the panel as PNG (1440 x 2960; --half: 720 x 1480)
  sbfb.py logs [pmsg|dmesg|loader|usb|crash] [-o FILE]
  sbfb.py crash [--level N]             the crash log (sbstore2.py crashlog of `oem log crashraw`)
  sbfb.py boot KEY                      reboot and start catalog entry KEY (oem reboot-to)
  sbfb.py next KEY                      boot-next only, no reboot (oem boot-next)
  sbfb.py menu                          reboot to the boot menu (no boot-next)
  sbfb.py maint [prev|rescue]           start Starbite Maintenance from UFS now (oem maint)
  sbfb.py reboot [recovery|download|bootloader]
  sbfb.py run IMAGE [fastboot boot args...]   fastboot boot (also from the menu)

-s SERIAL picks the phone when several are attached. Nothing here writes the phone's storage.
"""
import argparse
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

W, H = 1440, 2960


def fb(args, serial=None, timeout=60, check=True):
    cmd = ['fastboot'] + (['-s', serial] if serial else []) + args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        sys.exit('fastboot not found (Android platform-tools: brew install --cask android-platform-tools)')
    except subprocess.TimeoutExpired:
        sys.exit(f"{' '.join(cmd)}: no answer in {timeout} s (is the phone at a loader menu or in its fastboot?)")
    out = r.stdout + r.stderr
    if check and r.returncode:
        sys.exit(f"{' '.join(cmd)}: {out.strip()[-400:]}")
    return out, r.returncode


def lines(out):
    """(bootloader) / INFO lines without the prefix, and 'name: value' answers."""
    res = []
    for ln in out.splitlines():
        ln = ln.rstrip()
        if ln.startswith('(bootloader) '):
            ln = ln[13:]
        if not ln or ln.startswith('Finished.') or ln.startswith('OKAY') or ln.startswith('Sending'):
            continue
        res.append(ln)
    return res


def getvar(name, serial=None):
    out, rc = fb(['getvar', name], serial, timeout=10, check=False)
    for ln in lines(out):
        if ln.startswith(name + ':'):
            return ln.split(':', 1)[1].strip()
    return None


def staged(args, serial, path, timeout=120):
    fb(['oem'] + args, serial)
    fb(['get_staged', str(path)], serial, timeout=timeout)
    return Path(path).read_bytes()


def png(raw, out, half=False):
    if len(raw) != W * H * 4:
        sys.exit(f'screen: {len(raw)} bytes, want {W * H * 4} (1440 x 2960 x 4)')
    w, h = W, H
    if half:
        px = memoryview(raw).cast('I')
        rows = [px[y * W:(y + 1) * W:2].tobytes() for y in range(0, H, 2)]
        raw = b''.join(rows)
        w, h = W // 2, H // 2
    rgb = bytearray(w * h * 3)
    rgb[0::3] = raw[2::4]                     # the panel's pixels are B G R A in memory
    rgb[1::3] = raw[1::4]
    rgb[2::3] = raw[0::4]
    stride = w * 3
    data = b''.join(b'\0' + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(h))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    Path(out).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(data, 6)) + chunk(b'IEND', b''))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-s', dest='serial')
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status')
    w = sub.add_parser('wait')
    w.add_argument('mode', nargs='?')
    w.add_argument('--timeout', type=float, default=60)
    k = sub.add_parser('key')
    k.add_argument('keys', nargs='+', help='up down bixby power, or name:ms for a hold')
    s = sub.add_parser('screen')
    s.add_argument('out')
    s.add_argument('--half', action='store_true')
    s.add_argument('--raw', action='store_true', help='keep the raw B G R A file too')
    lg = sub.add_parser('logs')
    lg.add_argument('which', nargs='?', default='pmsg', choices=['pmsg', 'dmesg', 'loader', 'usb', 'crash'])
    lg.add_argument('-o', dest='out')
    c = sub.add_parser('crash')
    c.add_argument('--level', type=int, default=3)
    b = sub.add_parser('boot')
    b.add_argument('key')
    n = sub.add_parser('next')
    n.add_argument('key')
    sub.add_parser('menu')
    m = sub.add_parser('maint')
    m.add_argument('variant', nargs='?', choices=['prev', 'rescue'])
    r = sub.add_parser('reboot')
    r.add_argument('where', nargs='?', choices=['recovery', 'download', 'bootloader'])
    rn = sub.add_parser('run')
    rn.add_argument('image')
    rn.add_argument('rest', nargs=argparse.REMAINDER)
    a = p.parse_args()
    sv = a.serial

    if a.cmd == 'status':
        out, _ = fb(['getvar', 'all'], sv, timeout=20)
        v = {}
        for ln in lines(out):
            if ': ' in ln:
                key, val = ln.split(': ', 1)
                v[key] = val
        print(f"mode {v.get('mode', '?')}  serial {v.get('serialno', '?')}  battery {v.get('battery-level', '?')} % "
              f"{v.get('battery-voltage', '?')} mV  usb {v.get('usb-speed', '?')}  catalog {v.get('catalog-count', '?')}")
        for i in range(int(v.get('catalog-count', '0') or 0)):
            if f'catalog:{i}' in v:
                print(f"  {v[f'catalog:{i}']}")
    elif a.cmd == 'wait':
        t0 = time.time()
        while time.time() - t0 < a.timeout:
            mode = getvar('mode', sv)
            if mode and (not a.mode or mode == a.mode):
                print(mode)
                return
            time.sleep(1)
        sys.exit(f"no phone{' in ' + a.mode if a.mode else ''} after {a.timeout:.0f} s")
    elif a.cmd == 'key':
        for kk in a.keys:
            name, _, ms = kk.partition(':')
            out, _ = fb(['oem', 'key', name] + ([ms] if ms else []), sv, timeout=10)
            time.sleep(((int(ms) if ms else 150) + 150) / 1000)   # the press, then the gap
        print(f"{len(a.keys)} key(s) sent")
    elif a.cmd == 'screen':
        with tempfile.TemporaryDirectory() as t:
            raw = staged(['screen'], sv, Path(t, 'screen.raw'))
            if a.raw:
                Path(a.out).with_suffix('.raw').write_bytes(raw)
        png(raw, a.out, a.half)
        print(f"{a.out}: {'720 x 1480' if a.half else '1440 x 2960'}")
    elif a.cmd == 'logs':
        with tempfile.TemporaryDirectory() as t:
            data = staged(['log', a.which], sv, Path(t, 'log.txt'))
        if a.out:
            Path(a.out).write_bytes(data)
            print(f"{a.out}: {len(data)} bytes")
        else:
            sys.stdout.write(data.decode(errors='replace'))
    elif a.cmd == 'crash':
        here = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as t:
            staged(['log', 'crashraw'], sv, Path(t, 'crashlog.bin'))
            r = subprocess.run([sys.executable, '-I', str(here / 'sbstore2.py'), 'crashlog', str(Path(t, 'crashlog.bin')),
                                '--level', str(a.level)])
            sys.exit(r.returncode)
    elif a.cmd == 'boot':
        fb(['oem', 'reboot-to', a.key], sv)
        print(f'rebooting into {a.key}')
    elif a.cmd == 'next':
        fb(['oem', 'boot-next', a.key], sv)
        print(f'boot-next {a.key}')
    elif a.cmd == 'menu':
        fb(['oem', 'reboot-to', 'menu'], sv)
        print('rebooting to the menu')
    elif a.cmd == 'maint':
        fb(['oem', 'maint'] + ([a.variant] if a.variant else []), sv)
        print('starting Starbite Maintenance')
    elif a.cmd == 'reboot':
        fb(['reboot'] + ([a.where] if a.where else []), sv)
    elif a.cmd == 'run':
        out, _ = fb(['boot', a.image] + a.rest, sv, timeout=300)
        sys.stdout.write(out)


if __name__ == '__main__':
    main()
