#!/usr/bin/env python3
"""launchd-enable-jobs.py LAUNCHD.plist DISABLED.plist OUTDIR LABEL...

Put jobs that tools/starlte-disable-jobs.py moved out of the root's /System/Library/xpc/launchd.plist (into
launchd-disabled.plist beside it) back in: the inverse, on copies of the two files as they are on the card
(card-file-get.py). Writes OUTDIR/launchd.plist and OUTDIR/launchd-disabled.plist (binary plists) and prints, one
per line, the job plist paths whose "<path>.starbite-off" file must be renamed back to "<path>"."""
import os, plistlib, sys

lp, kp, out, labels = sys.argv[1], sys.argv[2], sys.argv[3], set(sys.argv[4:])
d, kept = plistlib.load(open(lp, 'rb')), plistlib.load(open(kp, 'rb'))
done = []
for key, job in list(kept.items()):
    if job.get('Label') in labels:
        if key in d['LaunchDaemons']:
            sys.exit(f'{key} is already in launchd.plist')
        d['LaunchDaemons'][key] = kept.pop(key)
        done.append((job['Label'], key))
missing = labels - {l for l, _ in done}
if missing:
    sys.exit('not in launchd-disabled.plist: ' + ', '.join(sorted(missing)))
os.makedirs(out, exist_ok=True)
plistlib.dump(d, open(os.path.join(out, 'launchd.plist'), 'wb'), fmt=plistlib.FMT_BINARY)
plistlib.dump(kept, open(os.path.join(out, 'launchd-disabled.plist'), 'wb'), fmt=plistlib.FMT_BINARY)
for _, key in sorted(done):
    print(key)
