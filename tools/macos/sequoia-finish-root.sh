#!/usr/bin/env bash
# Finish a built Sequoia desktop root image for the phone (host only): the configuration layer, the Mali Metal
# userland for Metal's own discovery, Apple's owners, a read-only fsck and the image's sha256.
#   tools/macos/sequoia-finish-root.sh IMAGE [PASSWORD_FILE]
# Every attach is of IMAGE itself at a private mount point; every detach is by that mount point, after hdiutil info
# shows it belongs to IMAGE (never by /dev/diskN, never -force).
set -euo pipefail
cd "$(dirname "$0")/../.."
IMG=${1:?IMAGE}; PW=${2:-}
name=$(basename "$IMG" .img); dir=$(dirname "$IMG")
log=build/macos15/finish-$name.log
mkdir -p build/macos15
exec > >(tee -a "$log") 2>&1
echo "finish $IMG $(date '+%F %T')"
# 1. configuration layer (user, auto-login, setup markers, job disables; no binary patches)
tools/macos/sequoia-post-image.sh "$IMG" ${PW:+"$PW"}
owners="$dir/$name.owners-sequoia.json"
[ -f "$owners" ] || { echo "no $owners from the post layer"; exit 1; }
# 2. Metal userland (StarbiteMetal.bundle, mtl2-clients, m2v-cache) on our own mount point
mnt=$PWD/build/macos15/finish-mnt
mkdir -p "$mnt"
hdiutil attach -nobrowse -owners off -mountpoint "$mnt" "$IMG" >/dev/null
own_detach() {
    python3 -I - "$IMG" "$mnt" <<'PY' || { echo "not detaching $mnt: not $IMG's mount point"; return 1; }
import os, plistlib, subprocess, sys
img, mnt = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
info = plistlib.loads(subprocess.run(['hdiutil', 'info', '-plist'], capture_output=True, check=True, timeout=60).stdout)
sys.exit(0 if any(i.get('image-path', '') == img and any(e.get('mount-point') == mnt for e in i.get('system-entities', []))
                  for i in info.get('images', [])) else 1)
PY
    hdiutil detach "$mnt" >/dev/null
}
trap 'own_detach || true' EXIT
sh tools/macos/sequoia-metal-stage.sh "$mnt" "$owners"
own_detach; trap - EXIT
# 3. owners (Apple's + every staged path) and a check
python3 -I tools/macos/hfs-owners.py "$IMG" "$owners"
# 4. read-only fsck on our own mount point
hdiutil attach -nobrowse -readonly -owners on -mountpoint "$mnt" "$IMG" >/dev/null
dev=$(mount | awk -v m="$mnt" '$3 == m {print $1}')
trap 'own_detach || true' EXIT
[ -n "$dev" ] && fsck_hfs -fn "${dev/disk/rdisk}" || echo "fsck: no device for $mnt"
ls -ln "$mnt/System/Library/Extensions/StarbiteMetal.bundle/Contents/MacOS" "$mnt/Users" || true
own_detach; trap - EXIT
# 5. hash for the flash
shasum -a 256 "$IMG" | tee "$dir/$name.sha256"
echo "finish done $(date '+%F %T')"
