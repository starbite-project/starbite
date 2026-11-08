#!/usr/bin/env python3
"""Turn tools/ios26-perm-scan.py output into SBOVL002 owner/mode operations (kernel 0406+) and their undo.
    ios26-perm-fix.py SCAN.txt OUTDIR      -> OUTDIR/fix.args, OUTDIR/undo.args (one sboverlay argument per line)
Owner: root (uid 0) ignores mode bits, so only non-root users were locked out. Each damaged entry gets the
uid of a non-root, non-(-2) entry in the same container if the scan saw one (e.g. Library/local.sqlite 501),
else 501 (mobile); folders 0777 so every service user can use them, files 0644. The undo restores exactly
the mode / uid / gid the scan read."""
import re
import sys
from pathlib import Path

BAD = 4294967294


def container(path):
    m = re.match(r'(/private/var/containers/Shared/SystemGroup/[^/]+|/private/var/mobile/Containers/[^/]+/[^/]+/[^/]+)', path)
    return m.group(1) if m else path


def main():
    scan, out = Path(sys.argv[1]), Path(sys.argv[2])
    rows = []
    for line in scan.read_text().splitlines():
        m = re.match(r'(\d{4}) (\d+):(\d+) (dir|file) (/.*)$', line.strip())
        if m:
            rows.append((int(m.group(1), 8), int(m.group(2)), int(m.group(3)), m.group(4), m.group(5)))
    hint = {}
    for mode, uid, gid, kind, path in rows:
        if uid not in (0, BAD):
            hint.setdefault(container(path), uid)
    fix, undo = [], []
    # parents before children (fix), children before parents (undo)
    for mode, uid, gid, kind, path in sorted(rows, key=lambda r: r[4].count('/')):
        owner = hint.get(container(path), 501)
        fix.append(f'attr:{path}:{"777" if kind == "dir" else "644"}:{owner}:{owner}')
    for mode, uid, gid, kind, path in sorted(rows, key=lambda r: -r[4].count('/')):
        undo.append(f'attr:{path}:{mode:o}:{uid}:{gid}')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'fix.args').write_text('\n'.join(fix) + '\n')
    (out / 'undo.args').write_text('\n'.join(undo) + '\n')
    owners = {}
    for a in fix:
        owners[a.rsplit(':', 2)[1]] = owners.get(a.rsplit(':', 2)[1], 0) + 1
    print(f'{len(fix)} operations, owners {owners} -> {out}/fix.args, undo {out}/undo.args')


if __name__ == '__main__':
    main()
