#!/usr/bin/env bash
# The macOS 26.5 root's Starbite layer, on the mounted image, before the owners step
# (build-macos-root.sh MACOS=26 runs it; standalone: attach the image, run this, detach,
# then hfs-owners.py + fsck_hfs as the builder does). docs/MACOS.md section 12.
#
#   tools/macos/macos26-post.sh MNT OWNERS.json
#
# 1. /usr/bin/true (if the list did not bring it) for the boot-task no-ops
# 2. tools/macos/macos26-boot-tasks.sh: fsck, cc_fips_test, init_data_protection,
#    endpointsecurityd -> no-ops (RO=1 adds the tasks that write; for a root without sdrw=1)
# 3. jobs taken out of /System/Library/xpc/launchd.plist (launchd reads its jobs from there;
#    launchd_unsecure_cache=1 on the command line), tools/macos/macos26-disabled.list
#    (WindowServer for the first boots); their LaunchDaemons plists renamed *.starbite-off
# 4. METAL=1 (default when out/macos-metal exists): tools/macos/metal/build.sh stage, and the
#    Metal test job com.starbite.sbmtltest in /System/Library/LaunchDaemons + launchd.plist,
#    gated on the boot-arg sbmtltest=1 (so mac26-t1 and mac26-m1 boot the same root)
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
mnt=$1 owners=$2
URL=${IPSW_URL:-https://updates.cdn-apple.com/2026SpringFCS/fullrestores/122-58869/DFB1CEEF-5619-4591-9924-E20DB2C8FED0/UniversalMac_26.5_25F71_Restore.ipsw}
OS_DMG=${OS_DMG:-094-56453-087.dmg.aea}
W=${W:-/Volumes/Untitled/starbite-macos26}

# 1
if [ ! -f "$mnt/usr/bin/true" ]; then
    python3 -I -B tools/macos/ipsw-apfs.py "$URL" get "$OS_DMG" "$mnt" /usr/bin/true \
        --keys "$W/keys.txt" --owners "$owners" --thin
fi
# 2
ro=(); [ -z "${RO:-}" ] || ro=(--ro)
tools/macos/macos26-boot-tasks.sh "$mnt" "$owners" ${ro[@]+"${ro[@]}"}
# 3
python3 -I tools/starlte-disable-jobs.py "$mnt" tools/macos/macos26-disabled.list
python3 -I - "$mnt" tools/macos/macos26-disabled.list <<'PY'
import glob, os, plistlib, sys
mnt, lst = sys.argv[1], sys.argv[2]
want = {l.split('#')[0].strip() for l in open(lst)} - {''}
for p in glob.glob(mnt + '/System/Library/LaunchDaemons/*.plist'):
    try:
        label = plistlib.load(open(p, 'rb')).get('Label')
    except Exception:
        continue
    if label in want:
        os.rename(p, p + '.starbite-off')
        print('renamed', p[len(mnt):], '-> .starbite-off')
PY
# 4
if [ "${METAL:-$([ -d out/macos-metal/StarbiteMetal.bundle ] && echo 1)}" = 1 ]; then
    tools/macos/metal/build.sh stage "$mnt"
    rm -f "$mnt/Library/LaunchDaemons/com.starbite.sbmtltest.plist"   # launchd 26.5 bootstraps
    python3 -I - "$mnt" <<'PY'                                          # /System/Library/LaunchDaemons
import plistlib, sys
mnt = sys.argv[1]
key = '/System/Library/LaunchDaemons/com.starbite.sbmtltest.plist'
job = {
    'Label': 'com.starbite.sbmtltest',
    # docs/MACOS-METAL.md section 6 (test M1); runs only with sbmtltest=1 on the command line
    'ProgramArguments': ['/bin/sh', '-c',
        'case " $(/usr/sbin/sysctl -n kern.bootargs) " in *" sbmtltest=1 "*) '
        'exec /usr/local/bin/sbmtltest --seconds 30 ;; *) echo "SBMTLTEST: off (no sbmtltest=1)" ;; esac'],
    'RunAtLoad': True, 'KeepAlive': False, 'LaunchOnlyOnce': True,
    'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
}
with open(mnt + key, 'wb') as f:
    plistlib.dump(job, f)
path = mnt + '/System/Library/xpc/launchd.plist'
d = plistlib.load(open(path, 'rb'))
d['LaunchDaemons'][key] = job
with open(path, 'r+b') as f:
    f.truncate(0)
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
print('sbmtltest job (gated on sbmtltest=1) in', key, 'and launchd.plist')
PY
    chmod 0644 "$mnt/System/Library/LaunchDaemons/com.starbite.sbmtltest.plist"
fi
printf 'Starbite layer: macos26-post.sh %s\n' "$(date -u +%Y-%m-%dT%H:%MZ)" >> "$mnt/.starbite-macos"
