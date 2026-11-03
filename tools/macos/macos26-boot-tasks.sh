#!/usr/bin/env bash
# launchd 26.5's own boot tasks (the "Boot" dictionary embedded in /sbin/launchd's __TEXT; every
# RequireSuccess failure ends the boot) that cannot succeed on our kernel and card root yet get
# Apple's /usr/bin/true in place of their program. The original stays beside it as
# <name>.starbite-orig; --undo puts everything back.
#
#   tools/macos/macos26-boot-tasks.sh MNT OWNERS.json [--ro] [--undo]
#       MNT      the mounted HFSX root (build-macos-root.sh MACOS=26 runs it before the owners step)
#       OWNERS   the build's owners JSON: wrapped paths get 0:0 0755, the originals keep theirs
#
# Always wrapped (docs/MACOS.md section 12.4):
#   /sbin/fsck                      fsck -qL -R 1 (RequireRun), fsck -fy: the kernel's fsck_hfs path is untested
#   /usr/libexec/cc_fips_test       corecrypto's FIPS self-test (RequireRun); iOS 26 needed fips-ok too
#   /usr/libexec/init_data_protection   the Data volume keybag: no AppleKeyStore in our kernel
#   /usr/libexec/endpointsecurityd  endpointsecurityd --init: no EndpointSecurity kext
# --ro (a root without sdrw=1) also wraps the tasks that write to the root:
#   /usr/libexec/dirs_cleaner (/tmp /var/run /var/boot), /sbin/mount (mount -P 1, -P 2),
#   /usr/libexec/tzinit (/var/db/timezone), /usr/libexec/dirhelper (-machineBoot: /var/folders)
# Not wrapped: launchd's internal Block tasks, rc.netboot (exits 0 when not netbooted), and the
# installer/demo/obliteration tasks (their programs are not on the root: launchd skips them).
set -euo pipefail
mnt=$1 owners=$2; shift 2
TASKS=(/sbin/fsck /usr/libexec/cc_fips_test /usr/libexec/init_data_protection /usr/libexec/endpointsecurityd)
mode=
for a in "$@"; do
    case $a in
        --ro) TASKS+=(/usr/libexec/dirs_cleaner /sbin/mount /usr/libexec/tzinit /usr/libexec/dirhelper) ;;
        --undo) mode=--undo
                TASKS+=(/usr/libexec/dirs_cleaner /sbin/mount /usr/libexec/tzinit /usr/libexec/dirhelper) ;;
        *) echo "unknown option $a"; exit 2 ;;
    esac
done
[ "$mode" = --undo ] || [ -f "$mnt/usr/bin/true" ] || { echo "$mnt/usr/bin/true missing (macos26-t1.list has it)"; exit 1; }
python3 -I - "$mnt" "$owners" "$mode" "${TASKS[@]}" <<'PY'
import json, os, shutil, sys
mnt, owners, mode, tasks = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
own = json.load(open(owners))
done = []
for t in tasks:
    p, orig = mnt + t, mnt + t + '.starbite-orig'
    if mode == '--undo':
        if os.path.exists(orig):
            os.replace(orig, p)
            own[t] = own.pop(t + '.starbite-orig', own.get(t, [0, 0, 0o755]))
            done.append(t)
        continue
    if os.path.exists(orig) or not os.path.exists(p):
        continue                     # already wrapped, or not in this root
    os.rename(p, orig)
    own[t + '.starbite-orig'] = own.get(t, [0, 0, 0o755])
    shutil.copy2(mnt + '/usr/bin/true', p)
    own[t] = [0, 0, 0o755]
    done.append(t)
json.dump(own, open(owners, 'w'), indent=0, sort_keys=True)
tag = os.path.join(mnt, '.starbite-boot-tasks')
if mode == '--undo':
    if os.path.exists(tag):
        os.unlink(tag)
else:
    with open(tag, 'a') as f:
        for t in done:
            f.write(f'{t} -> /usr/bin/true (original: {t}.starbite-orig)\n')
print(('restored: ' if mode == '--undo' else 'boot tasks as no-ops: ') + (' '.join(done) or 'nothing'))
PY
