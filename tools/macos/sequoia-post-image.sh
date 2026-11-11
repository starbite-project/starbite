#!/usr/bin/env bash
# Apply the Sequoia configuration layer (tools/macos/sequoia-post.sh) to a macOS 15.6 root IMAGE file, host only.
#
#   tools/macos/sequoia-post-image.sh IMAGE [PASSWORD_FILE]
#
# 1. refuses an image that is already attached (a build may still be writing it) and other sessions' areas
#    (build/refport27, /Volumes/IOS27WORK); needs > 5 GB free on the Mac
# 2. hfs-owners.py --dump: the image's current owners, read offline from its catalog, as the starting OWNERS
#    (complete, unlike a walk of a mount, which cannot enter root-only folders as a user)
# 3. attaches IMAGE read-write at a private mount point (-nobrowse; -owners on when run as root, as asked; as a
#    user -owners on makes every root-owned folder read-only (checked 2026-11-11: "Permission denied" in
#    /private/var/db), so the write pass ignores ownership and step 4 writes the owners), runs sequoia-post.sh,
#    detaches by that mount point
# 4. hfs-owners.py IMAGE OWNERS (re-applies Apple's owners + the new entries), then --check (0 must differ)
# 5. evidence: a second --dump, diffed against the first (every added or changed path with uid:gid mode), then a
#    read-only -owners on mount for fsck_hfs -fn and ls -ln, detached by its mount point. VERIFY=0 skips the mount.
# Output beside IMAGE (WORK= to put it elsewhere): <name>.owners-before.json, <name>.owners-sequoia.json,
# <name>.sequoia-post.diff.
# Detach rule (2026-11-11): only by mount point, only after hdiutil info shows that mount point belongs to
# IMAGE; never by /dev/diskN, never -force. A busy detach is retried, then left attached with a message.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
[ $# -ge 1 ] || { sed -n '4p' "$0"; exit 2; }
IMG=$(python3 -I -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1")
pwfile=${2:-}
[ -f "$IMG" ] || { echo "$IMG: no such image file"; exit 1; }
case $IMG in
    */build/refport27/*|/Volumes/IOS27WORK/*) echo "$IMG: another session's area, not touched"; exit 1 ;;
esac
xxd -s 1024 -l 2 -p "$IMG" | grep -qx 4858 || { echo "$IMG: no HFSX (HX) signature at 1024"; exit 1; }
free_mb=$(df -m "$HOME" | awk 'NR==2 {print $4}')
(( free_mb > 5120 )) || { echo "Mac free disk is $free_mb MB: needs > 5 GB"; exit 1; }
name=$(basename "$IMG" .img)
WORK=${WORK:-$(dirname "$IMG")}
before=$WORK/$name.owners-before.json owners=$WORK/$name.owners-sequoia.json after=$WORK/$name.owners-after.json
diffout=$WORK/$name.sequoia-post.diff

# image-path -> mount points, from hdiutil info (a timeout, so a busy diskimages-helper cannot hang us forever).
# Paths are compared as text, never stat'ed: other images may sit on a hung volume (2026-11-11: a realpath() on
# an image path on the failing SBWORK stick hung in uninterruptible I/O). IMG is already a realpath.
mounted_at() {
    timeout 300 hdiutil info -plist | python3 -I -c '
import plistlib, sys
norm = lambda p: p[len("/private"):] if p.startswith("/private/") else p
img = norm(sys.argv[1])
info = plistlib.loads(sys.stdin.buffer.read())
for i in info.get("images", []):
    if norm(i.get("image-path", "")) == img:
        ents = [e["mount-point"] for e in i.get("system-entities", []) if e.get("mount-point")]
        print("\n".join(ents) if ents else "(attached, not mounted)")' "$IMG"
}
own_detach() {     # $1 = the mount point we attached at
    local mp=$1 n
    for n in 1 2 3 4 5; do
        if ! mounted_at | grep -qxF "$mp"; then
            echo "not detaching $mp: hdiutil info does not show $IMG mounted there"; return 1
        fi
        hdiutil detach "$mp" >/dev/null 2>&1 && { echo "detached $mp"; return 0; }
        sleep 3
    done
    echo "could not detach $mp (busy): left attached; detach it by that mount point later"; return 1
}
attach_at() {      # $1 = mount point, $2 = on|off, rest = extra hdiutil options; prints the device line
    hdiutil attach -nobrowse -owners "$2" -imagekey diskimage-class=CRawDiskImage -mountpoint "$1" "${@:3}" "$IMG" 2>/dev/null
}

[ -z "$(mounted_at)" ] || { echo "$IMG is already attached (a build may be using it):"; mounted_at; exit 1; }
for f in "$before" "$owners" "$after"; do [ ! -e "$f" ] || mv "$f" "$f.prev"; done
tmp=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/sbseqpost.XXXXXX")" && pwd -P)    # real path: hdiutil reports that
mp=
cleanup() { [ -z "$mp" ] || own_detach "$mp" || true; rm -rf "$tmp" 2>/dev/null || true; }
trap cleanup EXIT

# 2
python3 -I "$ROOT/tools/macos/hfs-owners.py" "$IMG" "$before" --dump
cp "$before" "$owners"

# 3
ownmode=off; [ "$(id -u)" = 0 ] && ownmode=on
mkdir "$tmp/rw"
attach_at "$tmp/rw" "$ownmode" >/dev/null || { echo "could not attach $IMG"; exit 1; }
mp=$tmp/rw
mounted_at | grep -qxF "$mp" || { echo "$IMG did not mount at $mp"; exit 1; }
echo "$IMG attached read-write at $mp (-owners $ownmode)"
"$ROOT/tools/macos/sequoia-post.sh" "$mp" "$owners" ${pwfile:+"$pwfile"}
sync
own_detach "$mp"; mp=

# 4
python3 -I "$ROOT/tools/macos/hfs-owners.py" "$IMG" "$owners"
python3 -I "$ROOT/tools/macos/hfs-owners.py" "$IMG" "$owners" --check

# 5
python3 -I "$ROOT/tools/macos/hfs-owners.py" "$IMG" "$after" --dump
python3 -I - "$before" "$after" > "$diffout" <<'PY'
import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
fmt = lambda o: '%d:%d %04o' % tuple(o)
for p in sorted(set(a) | set(b)):
    if p not in b:
        print(f'- {p}  {fmt(a[p])}')
    elif p not in a:
        print(f'+ {p}  {fmt(b[p])}')
    elif a[p] != b[p]:
        print(f'~ {p}  {fmt(a[p])} -> {fmt(b[p])}')
PY
echo "changes (catalog, offline): $diffout"
cat "$diffout"
if [ "${VERIFY:-1}" = 1 ]; then
    mkdir "$tmp/ro"
    dev=$(attach_at "$tmp/ro" on -readonly | awk 'NR==1 {print $1}')
    mp=$tmp/ro
    mounted_at | grep -qxF "$mp" || { echo "read-only mount of $IMG failed"; exit 1; }
    echo "read-only check mount (-owners on) at $mp ($dev)"
    ls -lnd "$mp/Users/"* "$mp/Users/"*/Library/Preferences/* "$mp/private/var/db/.AppleSetupDone" \
        "$mp/Library/Preferences/com.apple.loginwindow.plist" "$mp/Library/Preferences/com.apple.universalaccess.plist" \
        "$mp/private/etc/kcpassword" "$mp/System/Library/xpc/"launchd*.plist 2>&1 | sed "s|$mp||"
    /sbin/fsck_hfs -fn "/dev/r${dev#/dev/}" || { echo "fsck_hfs reported problems"; exit 1; }
    own_detach "$mp"; mp=
fi
echo "done: $IMG (owners: $owners)"
