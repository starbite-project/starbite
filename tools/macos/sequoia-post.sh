#!/usr/bin/env bash
# The macOS 15.6 (Sequoia, 24G84) root's Starbite layer: CONFIGURATION ONLY (the user's rule, 2026-11-11; plan
# docs/APPLE-S9-DRIVERS.md). Runs on the mounted root (MacBookAir10,1 identity, our xnu-12377 macOS-target kernel)
# before the owners step; host only, it never touches the phone. tools/macos/sequoia-post-image.sh does attach,
# this, owners and detach for an image file.
#
#   tools/macos/sequoia-post.sh MNT OWNERS.json [PASSWORD_FILE]
#       MNT        the mounted HFSX root, writable by this user (a non-root attach ignores ownership; the owners
#                  are written afterwards by tools/macos/hfs-owners.py from OWNERS.json)
#       OWNERS     the owners JSON ({"/path": [uid, gid, mode]}); every path this script adds or renames is put
#                  in it. Run hfs-owners.py IMAGE OWNERS.json after the detach.
#       PASSWORD   first line = the test user's password (default "starbite", as Ventura's)
#   Env: SB_USER (starbite), SB_REALNAME (Starbite), SB_UID (501), JOBS=0 (leave launchd.plist alone),
#        DISABLE_LIST (tools/macos/sequoia-disabled.list)
#
# What it writes (all plists/preferences/markers/metadata; idempotent, a second run changes nothing it already did):
#  1. launchd jobs out, by label (DISABLE_LIST; evidence per label in that file): the entries move from
#     /System/Library/xpc/launchd.plist to launchd-disabled.plist (tools/starlte-disable-jobs.py, undo with
#     root26-cycle.py --enable-job), their LaunchDaemons/LaunchAgents plists renamed *.starbite-off.
#     WindowServer stays in.
#  2. Setup Assistant done: /private/var/db/.AppleSetupDone; the user's com.apple.SetupAssistant DidSee* and
#     LastSeen*/LastPreLoginTasksPerformed*/PreviousBuildVersion = this root's version and build, so neither
#     MiniBuddy nor loginwindow's "different build" first-login work starts (the key set is the one a real Sequoia+
#     account carries; checked on this Mac's own domain).
#  3. The test user (uid 501, admin; tools/macos/mkuser-dslocal.py, plus the attributes a real account carries,
#     as on Ventura) and its home; skipped if the record exists (re-run keeps the same GeneratedUID).
#  4. loginwindow: name + password fields, no user pictures, guest off, auto-login of the test user;
#     /etc/kcpassword (the password XORed with the auto-login key, zero-padded to 12-byte rounds).
#  5. Accessibility: Reduce Transparency + Reduce Motion (+ the Accessibility Keyboard switch) for the system,
#     root and the user: no blur materials on the Mali.
#  6. /.starbite-macos gets a line; /.starbite-sequoia-post lists what this run did.
#
# Dropped from Ventura (tools/macos/ventura-post.sh -> macos26-post.sh), and what replaces each in the driver plan:
#  - /usr/bin/true in place of the launchd Boot tasks (macos26-boot-tasks.sh). These tasks live in the Boot
#    dictionary inside /sbin/launchd's __TEXT (15.6: checked, 46 tasks), so no configuration can switch them off;
#    they now run Apple's real programs:
#      fsck / fsck-safe (/sbin/fsck -qL -R 1, RequireSuccess+RequireRun) -> the read-only first boots keep the
#        volume clean (an unjournaled HFSX root that is never mounted rw stays clean, so fsck -q passes), then
#        StarbiteUFS/StarbiteSD publishing real IOMedia + /dev nodes for fsck_hfs (Part 3 storage).
#      fips (/usr/libexec/cc_fips_test, RequireSuccess+RequireRun) -> StarbiteCPUFeatures emulating PAC/DIT/SHA3/
#        SHA512 exactly (libcorecrypto is left un-rewritten for its self-MAC), "corecrypto handling" in Part 4.
#      data-protection (/usr/libexec/init_data_protection, RequireSuccess) -> StarbiteKeyStore keybag ops.
#      EndpointSecurity (/usr/libexec/endpointsecurityd --init, RequireSuccess) -> the ES policy answer in
#        StarbitePolicy.
#    RISK, stated plainly: each of these is a RequireSuccess task, so if one fails the boot ends there. That is
#    the driver plan's test for each driver, not something a plist can hide. Read the launchd trace / pmsg.
#  - the dirs_cleaner wrapper (ventura-dirs-cleaner.sh, a script in place of /usr/libexec/dirs_cleaner) ->
#    /chosen/boot-uuid from the board ADT (IOGetBootUUID, Part 2.7). On a read-only root nothing persists in
#    /var/run between boots, so the stale com.apple.loginwindow.didRunThisBoot that needed the wrapper cannot
#    happen; once a line adds sdrw=1, boot-uuid must be in the ADT first.
#  - the securityd NOPs (ventura-securityd-patch.py) -> StarbitePolicy answering the sandbox sentinel and
#    allowing the MDS DB creation. securityd cannot be switched off by configuration (loginwindow needs it).
#  - sbmac.dylib injection (sweetinject=, the QuartzCore/Metal interposes) and the Metal staging that installs it
#    (macos26-post.sh step 4) -> StarbiteDisplay colour caps + StarbiteMali discovery. The StarbiteMetal bundle is
#    a driver, staged by the driver work, not here.
#  - UserAccountUpdater -> /usr/bin/true (loginwindow waited on it on Ventura). It is not a launchd job, so no
#    disable is possible; step 2's LastPreLoginTasksPerformed*/PreviousBuildVersion = this build is the
#    configuration that tells loginwindow the pre-login work for this build is done [I: untested on the phone].
#  - kernelmanagerd's _PanicOnCrash edit -> the job is out instead (list A); kext-management answers later.
#  - /usr/bin/true fetched from the IPSW (macos26-post.sh step 1): nothing needs it any more.
# Jobs taken out because they are likely to block or panic the boot: list A in DISABLE_LIST, with evidence.
set -euo pipefail
cd "$(dirname "$0")/../.."
[ $# -ge 2 ] || { sed -n '6,13p' "$0"; exit 2; }
mnt=${1%/} owners=$2 pwfile=${3:-}
user=${SB_USER:-starbite} real=${SB_REALNAME:-Starbite} uid=${SB_UID:-501}
list=${DISABLE_LIST:-tools/macos/sequoia-disabled.list}
[ -d "$mnt/private/var/db/dslocal/nodes/Default" ] || { echo "$mnt: no dslocal node: not a macOS root"; exit 1; }
[ -f "$owners" ] || { echo "$owners: no owners JSON"; exit 1; }
[ -w "$mnt/private/var/db" ] || { echo "$mnt is not writable by $(id -un) (attach with -owners off as a user, or run as root)"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
log=$tmp/done.txt; : > "$log"
if [ -z "$pwfile" ]; then pwfile=$tmp/pw; printf 'starbite\n' > "$pwfile"; fi

# 1. launchd jobs out (configuration: launchd.plist entries + plist names)
if [ "${JOBS:-1}" = 1 ] && [ -f "$mnt/System/Library/xpc/launchd.plist" ]; then
    python3 -I tools/starlte-disable-jobs.py "$mnt" "$list" | tee -a "$log"
    python3 -I - "$mnt" "$owners" "$list" <<'PY' | tee -a "$log"
import glob, json, os, plistlib, sys
mnt, owners, lst = sys.argv[1:4]
want = {l.split('#')[0].strip() for l in open(lst)} - {''}
own = json.load(open(owners))
n = 0
for p in sorted(glob.glob(mnt + '/System/Library/LaunchDaemons/*.plist') + glob.glob(mnt + '/System/Library/LaunchAgents/*.plist')):
    try:
        label = plistlib.load(open(p, 'rb')).get('Label')
    except Exception:
        continue
    if label in want:
        os.rename(p, p + '.starbite-off')
        k = p[len(mnt):]
        own[k + '.starbite-off'] = own.pop(k, [0, 0, 0o644])
        n += 1
own['/System/Library/xpc/launchd-disabled.plist'] = [0, 0, 0o644]
own.setdefault('/System/Library/xpc/launchd.plist', [0, 0, 0o644])
json.dump(own, open(owners, 'w'), indent=0, sort_keys=True)
print(f'{n} job plists renamed *.starbite-off')
PY
else
    echo "jobs: left alone (JOBS=0 or no launchd.plist)" | tee -a "$log"
fi

# 3a. the user record (mkuser-dslocal.py writes it; skipped when it exists, so the GeneratedUID stays)
node=$mnt/private/var/db/dslocal/nodes/Default
for d in "$node" "$node/users" "$node/groups"; do chmod u+rwx "$d" 2>/dev/null || true; done
if [ -f "$node/users/$user.plist" ]; then
    echo "user $user: record exists, kept" | tee -a "$log"
else
    python3 -I tools/macos/mkuser-dslocal.py "$mnt" "$user" "$real" --uid "$uid" --password-file "$pwfile" | tee -a "$log"
fi

# 2-5 (plists, markers, owners)
ver=15.6 build=24G84
sv=$mnt/System/Library/CoreServices/SystemVersion.plist
if [ -f "$sv" ]; then
    ver=$(/usr/libexec/PlistBuddy -c 'Print :ProductVersion' "$sv" 2>/dev/null || echo "$ver")
    build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$sv" 2>/dev/null || echo "$build")
fi
python3 -I - "$mnt" "$owners" "$pwfile" "$user" "$uid" "$ver" "$build" <<'PY' | tee -a "$log"
import json, os, plistlib, sys
mnt, owners, pwfile, user, uid, ver, build = sys.argv[1:8]
uid = int(uid)
own = json.load(open(owners))
def mkdir(path, o):
    os.makedirs(mnt + path, exist_ok=True)
    own[path] = o
def put(path, data, o):
    os.makedirs(os.path.dirname(mnt + path), exist_ok=True)
    with open(mnt + path, 'wb') as f:
        f.write(data)
    os.chmod(mnt + path, o[2])
    own[path] = o
    print('wrote', path, '%d:%d %o' % tuple(o))
bp = lambda d: plistlib.dumps(d, fmt=plistlib.FMT_BINARY)

# folders a Data volume has (t0 roots have no /Library or /Users); Apple's owners
mkdir('/Library', own.get('/Library', [0, 0, 0o755]))
mkdir('/Library/Preferences', own.get('/Library/Preferences', [0, 80, 0o755]))
mkdir('/Users', own.get('/Users', [0, 80, 0o755]))
mkdir('/private/var/root/Library', own.get('/private/var/root/Library', [0, 0, 0o700]))
mkdir('/private/var/root/Library/Preferences', own.get('/private/var/root/Library/Preferences', [0, 0, 0o700]))
home = f'/Users/{user}'
for p, o in ((home, [uid, 20, 0o755]), (home + '/Library', [uid, 20, 0o700]),
             (home + '/Library/Preferences', [uid, 20, 0o700])):
    mkdir(p, o)

# 3b. the record: the attributes a real account carries (26.5's opendirectoryd wanted them; Ventura kept them)
up = f'/private/var/db/dslocal/nodes/Default/users/{user}.plist'
u = plistlib.load(open(mnt + up, 'rb'))
t = 1793000000.0                    # fixed (as Ventura's), so a re-run writes the same record
u.update({'record_daemon_version': ['4850000'],
          'accountPolicyData': [plistlib.dumps({'creationTime': t, 'failedLoginCount': 0,
                                                'failedLoginTimestamp': 0, 'passwordLastSetTime': t})],
          '_writers_passwd': [user], '_writers_hint': [user], '_writers_picture': [user],
          '_writers_jpegphoto': [user], '_writers_UserCertificate': [user]})
put(up, bp(u), [0, 0, 0o600])
gp = '/private/var/db/dslocal/nodes/Default/groups/admin.plist'
if os.path.exists(mnt + gp):
    own[gp] = own.get(gp, [0, 0, 0o600])
    g = plistlib.load(open(mnt + gp, 'rb'))
    print('admin group:', ' '.join(g.get('users', [])))

# 2. Setup Assistant done (system marker + the per-user keys a finished Sequoia setup leaves)
put('/private/var/db/.AppleSetupDone', b'', [0, 0, 0o644])
sa = {k: True for k in ('DidSeeAccessibility', 'DidSeeActivationLock', 'DidSeeAppStore', 'DidSeeAppearanceSetup',
                        'DidSeeApplePaySetup', 'DidSeeCloudSetup', 'DidSeeLockdownMode', 'DidSeePrivacy',
                        'DidSeeScreenTime', 'DidSeeSiriSetup', 'DidSeeSyncSetup', 'DidSeeSyncSetup2',
                        'DidSeeTermsOfAddress', 'DidSeeTouchIDSetup', 'DidSeeiCloudLoginForStorageServices',
                        'DidSeeIntelligence', 'SkipFirstLoginOptimization')}
sa.update({k: ver for k in ('LastSeenCloudProductVersion', 'LastSeenSiriProductVersion',
                            'LastSeenDiagnosticsProductVersion', 'LastSeenIntelligenceProductVersion',
                            'LastSeenSyncProductVersion', 'LastSeeniCloudStorageServicesProductVersion',
                            'LastPreLoginTasksPerformedVersion', 'PreviousSystemVersion', 'InitialSetupProductVersion')})
sa.update({k: build for k in ('LastSeenBuddyBuildVersion', 'LastPreLoginTasksPerformedBuild',
                              'PreviousBuildVersion', 'InitialSetupBuildVersion')})
sa.update({'MiniBuddyLaunchReason': 0, 'LastPrivacyBundleVersion': '2', 'InitialAccountOnMac': True})
put(home + '/Library/Preferences/com.apple.SetupAssistant.plist', bp(sa), [uid, 20, 0o600])

# 4. loginwindow: fields, no pictures, guest off, auto-login; kcpassword
put('/Library/Preferences/com.apple.loginwindow.plist', bp(
    {'SHOWFULLNAME': True, 'GuestEnabled': False, 'autoLoginUser': user, 'autoLoginUserUID': uid}), [0, 0, 0o644])
key = [125, 137, 82, 35, 210, 188, 221, 234, 163, 185, 31]
pw = list(open(pwfile, 'rb').readline().rstrip(b'\n'))
pw += [0] * (12 - len(pw) % 12) if len(pw) % 12 else []
put('/private/etc/kcpassword', bytes(b ^ key[i % len(key)] for i, b in enumerate(pw)), [0, 0, 0o600])

# 5. no blur, no motion (+ the Accessibility Keyboard switch)
ua = bp({'reduceTransparency': True, 'reduceMotion': True, 'virtualKeyboardOnOff': True})
put('/Library/Preferences/com.apple.universalaccess.plist', ua, [0, 0, 0o644])
put('/private/var/root/Library/Preferences/com.apple.universalaccess.plist', ua, [0, 0, 0o600])
put(home + '/Library/Preferences/com.apple.universalaccess.plist', ua, [uid, 20, 0o600])

own['/.starbite-sequoia-post'] = [0, 0, 0o644]
own.setdefault('/.starbite-macos', [0, 0, 0o644])
json.dump(own, open(owners, 'w'), indent=0, sort_keys=True)
print(f'Sequoia layer ({ver} {build}): setup done, user {user} uid {uid} (auto-login), reduce transparency/motion')
PY
printf 'Starbite layer: sequoia-post.sh %s (configuration only)\n' "$(date -u +%Y-%m-%dT%H:%MZ)" >> "$mnt/.starbite-macos"
{ printf '# tools/macos/sequoia-post.sh %s\n' "$(date -u +%Y-%m-%dT%H:%MZ)"; cat "$log"; } > "$mnt/.starbite-sequoia-post"
chmod 0644 "$mnt/.starbite-sequoia-post"
echo "done: $mnt (now: hfs-owners.py IMAGE $owners after the detach)"
