"""No user password or passcode on any OS (docs/STARBITE-2-PLAN.md "Current order" 7; the user:
"make sure in the installer there isn't a user password/passcode set ... cause that'll cause
confusion"). One check per OS. Each says how the guarantee is made, and checks what can be checked
from the host (the bundle's images, the repo's build configs) and from the install journal.
Status words: ok (checked), by-design (true by construction, not separately checkable here),
OPEN (a password exists or cannot be ruled out: written on the final screen), n/a."""
import re
from dataclasses import dataclass

from .common import REPO
from . import ext4


@dataclass
class Check:
    os: str
    how: str
    status: str
    evidence: str


def _shadow_user(shadow, user='user'):
    for line in shadow.decode(errors='replace').splitlines():
        f = line.split(':')
        if f[0] == user and len(f) > 1:
            return f[1]
    return None


def linux_image(path):
    """(status, evidence) for a Linux root image built by tools/linux/distros/ (user "user")."""
    try:
        fs = ext4.Ext4(path)
    except (OSError, ext4.Ext4Error) as e:
        return 'OPEN', f'cannot read the image as ext4 ({e})'
    try:
        shadow = fs.read_file('/etc/shadow')
        login = fs.read_file('/etc/starbite/login')
    except ext4.Ext4Error as e:
        return 'OPEN', f'cannot read /etc/shadow ({e})'
    finally:
        fs.close()
    if shadow is None:
        return 'OPEN', 'no /etc/shadow in the image'
    pw = _shadow_user(shadow)
    lock = (login or b'').decode(errors='replace').strip()
    if pw is None:
        return 'OPEN', 'no user "user" in /etc/shadow'
    if pw == '':
        if lock == 'none':
            return 'ok', 'user "user": empty password; /etc/starbite/login = none (lock screen off)'
        return 'OPEN', 'empty password, but no /etc/starbite/login = none: the lock screen may still show'
    if pw.startswith('$'):
        return 'OPEN', 'user "user" has a PIN: rebuild the image with SB_NO_PIN=1'
    return 'OPEN', f'user "user" password field is "{pw[:1]}..." (locked): the lock screen could never be unlocked'


def _grep(path, pattern):
    try:
        return re.search(pattern, (REPO / path).read_text(errors='replace'), re.S) is not None
    except OSError:
        return False


def report(manifest, journal=None, plan=None):
    """The per-OS list for the plan and final screens."""
    items = {i.dest.kind + ':' + (i.dest.name or str(i.dest.offset)): i for i in (manifest.items if manifest else [])}
    out = []

    # Android: LineageOS (an A16DATA image + a fresh /metadata on ODM) or One UI 8 (A16DATA made fresh on the
    # phone, mkfs-oneui-data; no /metadata)
    a16 = items.get('part:A16DATA')
    if a16 is None:
        out.append(Check('Android 17', 'fresh /data (A16DATA) and /metadata (ODM)', 'n/a', 'not in this bundle'))
    else:
        oneui = a16.action == 'mkfs-oneui-data'
        name = 'One UI 8' if oneui else 'Android 17'
        rec = journal.item_rec(a16) if journal else None
        planned = (plan or {}).get(a16.key)
        fresh = bool(rec and rec.get('fresh'))
        if (planned == 'keep' and not fresh) or (rec and rec.get('status') == 'kept'):
            st, ev = 'OPEN', 'existing Android data kept: a screen lock set earlier stays (Settings > Security > Screen lock > None)'
        elif fresh:
            st, ev = 'ok', ('A16DATA made fresh (mke2fs) by this install: no lock credential exists' if oneui else
                            'A16DATA and ODM written fresh by this install: no lock credential exists')
        else:
            st, ev = 'by-design', ('a fresh ext4 made on the phone: no lock credential exists' if oneui else
                                   'empty f2fs from make-a16data.sh + fresh ODM: no lock credential exists')
        how = ('fresh /data (no /metadata); setup finished by first-boot.sh (docs/ANDROID-ONEUI.md 2)' if oneui else
               'fresh /data + /metadata; the setup wizard\'s "Screen lock" page: Skip')
        out.append(Check(name, how, st, ev))

    # Linux images on the card
    for i in (manifest.items if manifest else []):
        if i.dest.kind == 'card-file' and i.os == 'linux':
            if i.path is None:
                out.append(Check(f'Linux {i.dest.name}', 'empty password + lock screen off (SB_NO_PIN=1 build)', 'n/a', 'not in this bundle'))
                continue
            st, ev = linux_image(i.path)
            out.append(Check(f'Linux {i.dest.name}', 'empty password + lock screen off (SB_NO_PIN=1 build); greetd/tinydm autologin', st, ev))

    # ChromiumOS
    if any(i.os == 'chromeos' for i in (manifest.items if manifest else [])):
        out.append(Check('ChromiumOS', 'test image (cros_debug); sign in as Guest', 'by-design',
                         'no account is created; Guest needs no password'))

    # Windows
    if any(i.dest.name in ('WINDOWS', 'WINESP') for i in (manifest.items if manifest else []) if i.present):
        ok = _grep('tools/woa/vm/build-image.ps1', r'<Password><Value></Value>.*<AutoLogon><Enabled>true')
        out.append(Check('Windows 11', 'unattend: local account "starbite" with an empty password + AutoLogon',
                         'ok' if ok else 'OPEN',
                         'build-image.ps1 unattend: empty password + AutoLogon' if ok
                         else 'unattend not found or has a password'))
    elif manifest:
        out.append(Check('Windows 11', 'unattend: empty password + AutoLogon', 'n/a',
                         'no Windows image in this bundle (a clean image is not built yet; licence per phone)'))

    # macOS Ventura
    if any(i.os == 'macos' for i in (manifest.items if manifest else []) if i.present):
        auto = _grep('tools/macos/ventura-post.sh', r'autoLoginUser') and _grep('tools/macos/ventura-post.sh', r'kcpassword')
        out.append(Check('macOS Ventura', 'auto-login as "starbite" (autoLoginUser + kcpassword)', 'OPEN',
                         ('auto-login, but the account HAS a password ("starbite"): asked only if WindowServer '
                          'restarts; an empty password is untested on the phone')
                         if auto else 'auto-login not found in tools/macos/ventura-post.sh'))

    # iOS 16.5 (an earlier port)
    if any(i.dest.kind == 'slot' for i in (manifest.items if manifest else []) if i.present):
        out.append(Check('iOS 16.5', 'no passcode in the root build', 'by-design',
                         'no passcode until one is set in Settings'))

    out.append(Check('Engineer OS / Starbite', 'no accounts', 'ok',
                     'no login; armed mode is hold-and-slide, not a PIN'))
    return out
