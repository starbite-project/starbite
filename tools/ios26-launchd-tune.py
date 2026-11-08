#!/usr/bin/env python3
"""Edit a copy of iOS 26's /System/Library/xpc/launchd.plist (launchd on this root reads its jobs from it)
for the card overlay: take jobs out by program name, and give chosen jobs an os_log mirror on stderr.
    ios26-launchd-tune.py IN_LAUNCHD IN_DISABLED|- OUTDIR [--off-list FILE] [--log NAME]... [--dry]
IN_DISABLED is the root's launchd-disabled.plist ("-" when it has none); removed jobs are kept there, as
tools/starlte-disable-jobs.py does, so they can be put back. A job matches --off-list / --log NAME when the
basename of its Program / ProgramArguments[0] starts with NAME (STARLTE TOP's process names), or its Label
equals NAME. --log adds OS_ACTIVITY_DT_MODE=YES and StandardErrorPath /private/var/tmp/NAME.log
(LOGDEST=console: /dev/console, which kernel 0371 copies into the kernel log with sweetcontee=1).
Writes OUTDIR/launchd.plist, OUTDIR/launchd-disabled.plist, OUTDIR/report.txt. Then:
    python3 tools/overlay/sboverlay.py OUT.bin OUTDIR/launchd.plist=/System/Library/xpc/launchd.plist:644 \\
        OUTDIR/launchd-disabled.plist=/System/Library/xpc/launchd-disabled.plist:644
    tools/overlay/card-overlay.sh OUT.bin"""
import argparse
import os
import plistlib
from pathlib import Path


def prog(job):
    p = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
    return os.path.basename(p)


def matches(job, names):
    b = prog(job)
    return next((n for n in names if job.get('Label') == n or (b and b.startswith(n))), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('launchd')
    ap.add_argument('disabled')
    ap.add_argument('outdir')
    ap.add_argument('--off-list')
    ap.add_argument('--log', action='append', default=[])
    ap.add_argument('--dry', action='store_true')
    a = ap.parse_args()
    d = plistlib.load(open(a.launchd, 'rb'))
    kept = plistlib.load(open(a.disabled, 'rb')) if a.disabled != '-' else {}
    off = []
    if a.off_list:
        off = [l.split('#')[0].strip() for l in open(a.off_list)]
        off = [n for n in off if n]
    report = []
    jobs = d['LaunchDaemons']
    hit = set()
    for key, job in list(jobs.items()):
        n = matches(job, off)
        if n:
            kept[key] = jobs.pop(key)
            hit.add(n)
            report.append(f'off  {job.get("Label")} ({prog(job)})')
    for key, job in jobs.items():
        n = matches(job, a.log)
        if n:
            env = job.setdefault('EnvironmentVariables', {})
            env['OS_ACTIVITY_DT_MODE'] = 'YES'
            job['StandardErrorPath'] = '/dev/console' if os.environ.get('LOGDEST') == 'console' else f'/private/var/tmp/{n}.log'
            hit.add(n)
            report.append(f'log  {job.get("Label")} ({prog(job)}) -> {job["StandardErrorPath"]}')
    for n in off + a.log:
        if n not in hit:
            report.append(f'none {n}: no job with that program or label (already off, or not a launchd job)')
    report.append(f'{len(jobs)} jobs left, {len(kept)} in launchd-disabled.plist')
    print('\n'.join(report))
    if a.dry:
        return
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    with open(out / 'launchd.plist', 'wb') as f:
        plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
    with open(out / 'launchd-disabled.plist', 'wb') as f:
        plistlib.dump(kept, f, fmt=plistlib.FMT_BINARY)
    (out / 'report.txt').write_text('\n'.join(report) + '\n')


if __name__ == '__main__':
    main()
