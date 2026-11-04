#!/usr/bin/env bash
# The macOS 13.5 (Ventura) root's Starbite layer, on the mounted image, before the owners step: what the macOS 26.5
# root needed over a day of boots on 2026-11-04 (docs/MACOS.md), applied up front.
#
#   tools/macos/ventura-post.sh MNT OWNERS.json [PASSWORD_FILE]
#
# 1. tools/macos/macos26-post.sh with Ventura's IPSW: boot-task no-ops, the noisy jobs out
#    (tools/macos/ventura-disabled.list: WindowServer stays IN), the Mali Metal bundle + sbmac staged
# 2. Setup Assistant marked done; loginwindow: name + password fields (no user pictures: AvatarKit had no Memoji
#    assets on 26.5), auto-login of the test user
# 3. the test user "starbite" (uid 501, admin; tools/macos/mkuser-dslocal.py, with the attributes a real account has)
#    and /etc/kcpassword for the auto-login
# 4. Reduce Transparency + Reduce Motion (system, root, the user): no blur materials on the Mali
set -euo pipefail
cd "$(dirname "$0")/../.."
mnt=$1 owners=$2 pwfile=${3:-}
export IPSW_URL=https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw
export OS_DMG=096-63007-081.dmg W=${W:-$PWD/build/macos13}

# 1 (macos26-post.sh reads its disabled list by name: point it at Ventura's)
sed 's|tools/macos/macos26-disabled.list|tools/macos/ventura-disabled.list|g' tools/macos/macos26-post.sh > "$W/.post.sh"
bash "$W/.post.sh" "$mnt" "$owners"
rm -f "$W/.post.sh"

# 2-4
if [ -z "$pwfile" ]; then pwfile=$(mktemp); printf 'starbite\n' > "$pwfile"; fi
mkdir -p "$mnt/private/var/db" "$mnt/Library/Preferences" "$mnt/Users"
touch "$mnt/private/var/db/.AppleSetupDone"
for d in "$mnt/private/var/db/dslocal/nodes/Default" "$mnt/private/var/db/dslocal/nodes/Default/users" \
         "$mnt/private/var/db/dslocal/nodes/Default/groups"; do chmod u+rwx "$d" 2>/dev/null || true; done
python3 -I tools/macos/mkuser-dslocal.py "$mnt" starbite "Starbite" --password-file "$pwfile" > "$W/mkuser.out"
python3 -I - "$mnt" "$owners" "$pwfile" <<'PY'
import json, os, plistlib, sys
mnt, owners, pwfile = sys.argv[1:4]
own = json.load(open(owners))
def put(path, data, o):
    os.makedirs(os.path.dirname(mnt + path), exist_ok=True)
    with open(mnt + path, 'wb') as f:
        f.write(data)
    own[path] = o
# the user record: the attributes a real account carries (26.5's opendirectoryd wanted them)
up = '/private/var/db/dslocal/nodes/Default/users/starbite.plist'
u = plistlib.load(open(mnt + up, 'rb'))
t = 1793000000.0
u.update({'record_daemon_version': ['4850000'],
          'accountPolicyData': [plistlib.dumps({'creationTime': t, 'failedLoginCount': 0,
                                                'failedLoginTimestamp': 0, 'passwordLastSetTime': t})],
          '_writers_passwd': ['starbite'], '_writers_hint': ['starbite'], '_writers_picture': ['starbite'],
          '_writers_jpegphoto': ['starbite'], '_writers_UserCertificate': ['starbite']})
put(up, plistlib.dumps(u, fmt=plistlib.FMT_BINARY), [0, 0, 0o600])
own['/Users'] = [0, 80, 0o755]
own['/Users/starbite'] = [501, 20, 0o755]
# loginwindow: fields, no pictures; auto-login
put('/Library/Preferences/com.apple.loginwindow.plist', plistlib.dumps(
    {'SHOWFULLNAME': True, 'GuestEnabled': False, 'autoLoginUser': 'starbite', 'autoLoginUserUID': 501},
    fmt=plistlib.FMT_BINARY), [0, 0, 0o644])
key = [125, 137, 82, 35, 210, 188, 221, 234, 163, 185, 31]
pw = list(open(pwfile, 'rb').readline().rstrip(b'\n'))
pw += [0] * (12 - len(pw) % 12) if len(pw) % 12 else []     # zero-padded to whole 12-byte rounds
put('/private/etc/kcpassword', bytes(b ^ key[i % len(key)] for i, b in enumerate(pw)), [0, 0, 0o600])
# no blur, no motion
ua = plistlib.dumps({'reduceTransparency': True, 'reduceMotion': True}, fmt=plistlib.FMT_BINARY)
put('/Library/Preferences/com.apple.universalaccess.plist', ua, [0, 0, 0o644])
put('/private/var/root/Library/Preferences/com.apple.universalaccess.plist', ua, [0, 0, 0o600])
put('/Users/starbite/Library/Preferences/com.apple.universalaccess.plist', ua, [501, 20, 0o600])
own['/Users/starbite/Library'] = [501, 20, 0o700]
own['/Users/starbite/Library/Preferences'] = [501, 20, 0o700]
own['/private/var/db/.AppleSetupDone'] = [0, 0, 0o644]
json.dump(own, open(owners, 'w'))
print('Ventura Starbite layer: setup done, user starbite (auto-login), reduce transparency/motion')
PY
printf 'Starbite layer: ventura-post.sh %s\n' "$(date -u +%Y-%m-%dT%H:%MZ)" >> "$mnt/.starbite-macos"
