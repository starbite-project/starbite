#!/usr/bin/env python3
"""A local macOS user as opendirectoryd keeps it, written offline into a root tree:
<ROOT>/private/var/db/dslocal/nodes/Default/users/<name>.plist (binary plist, mode 0600 root:wheel),
the home <ROOT>/Users/<name> (uid:20, 0755), and <name> added to the admin group
(<ROOT>/private/var/db/dslocal/nodes/Default/groups/admin.plist, users + groupmembership).

  mkuser-dslocal.py ROOT NAME "Real Name" [--uid 501] [--password-file FILE]

The password comes from FILE (first line) or is asked for on the terminal; it is stored only as
SALTED-SHA512-PBKDF2 ShadowHashData (128-byte key, 32-byte salt), as macOS does. Owners in the HFSX
image: set them with hfs-owners.py (the script prints the owners JSON entries to add).
"""
import getpass
import hashlib
import json
import os
import plistlib
import sys
import uuid


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    root, name, real = sys.argv[1], sys.argv[2], sys.argv[3]
    uid = int(sys.argv[sys.argv.index('--uid') + 1]) if '--uid' in sys.argv else 501
    if '--password-file' in sys.argv:
        password = open(sys.argv[sys.argv.index('--password-file') + 1]).readline().rstrip('\n')
    else:
        password = getpass.getpass(f'password for {name}: ')
    salt = os.urandom(32)
    iterations = 45000
    entropy = hashlib.pbkdf2_hmac('sha512', password.encode(), salt, iterations, 128)
    shadow = plistlib.dumps({'SALTED-SHA512-PBKDF2': {'entropy': entropy, 'salt': salt, 'iterations': iterations}},
                            fmt=plistlib.FMT_BINARY)
    guid = str(uuid.uuid4()).upper()
    user = {
        'name': [name],
        'realname': [real],
        'uid': [str(uid)],
        'gid': ['20'],
        'home': [f'/Users/{name}'],
        'shell': ['/bin/zsh'],
        'generateduid': [guid],
        'passwd': ['********'],
        'authentication_authority': [';ShadowHash;HASHLIST:<SALTED-SHA512-PBKDF2>'],
        'ShadowHashData': [shadow],
        'hint': [''],
    }
    node = os.path.join(root, 'private/var/db/dslocal/nodes/Default')
    upath = os.path.join(node, 'users', f'{name}.plist')
    os.makedirs(os.path.dirname(upath), exist_ok=True)
    with open(upath, 'wb') as f:
        plistlib.dump(user, f, fmt=plistlib.FMT_BINARY)
    os.chmod(upath, 0o600)
    owners = {f'/private/var/db/dslocal/nodes/Default/users/{name}.plist': [0, 0, 0o600]}
    gpath = os.path.join(node, 'groups', 'admin.plist')
    if os.path.exists(gpath):
        g = plistlib.load(open(gpath, 'rb'))
        if name not in g.get('users', []):
            g['users'] = g.get('users', []) + [name]
        if guid not in g.get('groupmembership', []):
            g['groupmembership'] = g.get('groupmembership', []) + [guid]
        with open(gpath, 'wb') as f:
            plistlib.dump(g, f, fmt=plistlib.FMT_BINARY)
        print(f'added {name} to admin ({gpath})')
    else:
        print(f'no {gpath}: {name} is a standard user', file=sys.stderr)
    home = os.path.join(root, 'Users', name)
    os.makedirs(home, exist_ok=True)
    os.chmod(home, 0o755)
    owners[f'/Users/{name}'] = [uid, 20, 0o755]
    print(f'wrote {upath} (uid {uid}, generateduid {guid}) and {home}')
    print('owners for hfs-owners.py:', json.dumps(owners))


if __name__ == '__main__':
    main()
