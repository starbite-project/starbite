#!/usr/bin/env python3
"""Rebuild the S9's UP_PARAM (a tar of the JPEGs S-Boot draws: logo.jpg = the splash, download_L.jpg,
warning_L.jpg, ...) with some members replaced, everything else byte-for-byte as it was: same member
order, owners, modes and times, same tar format, zero-padded to the original image size.

  upparam-pack.py ORIG_UP_PARAM.img OUT.img [NAME=FILE ...]
Without replacements the output must equal the input (checked: a self-test of the writer).
Flash OUT.img to /dev/block/by-name/UP_PARAM from TWRP and read it back; keep ORIG for a restore."""
import io, sys, tarfile


def rebuild(orig, repl):
    raw = open(orig, 'rb').read()
    src = tarfile.open(fileobj=io.BytesIO(raw), mode='r:')
    buf = io.BytesIO()
    dst = tarfile.open(fileobj=buf, mode='w:', format=tarfile.GNU_FORMAT, encoding=src.encoding)  # S-Boot's tar is GNU ('ustar  ')
    done = set()
    for m in src.getmembers():
        data = src.extractfile(m).read() if m.isfile() else None
        if m.name in repl:
            data = open(repl[m.name], 'rb').read()
            m.size = len(data)
            done.add(m.name)
        dst.addfile(m, io.BytesIO(data) if data is not None else None)
    dst.close()
    missing = set(repl) - done
    if missing:
        raise SystemExit(f'not in {orig}: {sorted(missing)}')
    out = buf.getvalue()
    if len(out) > len(raw):
        raise SystemExit(f'new tar {len(out)} > image {len(raw)}')
    return raw, out + b'\0' * (len(raw) - len(out))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    orig, out = sys.argv[1], sys.argv[2]
    repl = dict(a.split('=', 1) for a in sys.argv[3:])
    raw, same = rebuild(orig, {})
    if same != raw:
        diff = next(i for i in range(len(raw)) if raw[i] != same[i])
        raise SystemExit(f'self-test failed: an unchanged rebuild differs at byte {diff}; not writing')
    _, new = rebuild(orig, repl)
    open(out, 'wb').write(new)
    print(f'{out}: {len(new)} bytes, replaced {sorted(repl) or "nothing"}')


if __name__ == '__main__':
    main()
