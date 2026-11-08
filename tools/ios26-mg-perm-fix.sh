#!/usr/bin/env bash
# iOS 26 wallpaper fix (2026-11-08): PosterBoard (mobile) gets EACCES on the MobileGestalt answer cache because
# Library and Library/Caches of systemgroup.com.apple.mobilegestaltcache are mode 0000, owner -2 (a Mac edit with
# owners off). Queues SBOVL002 operations (kernel 0406+: T4 / v3; older kernels ignore the overlay):
# folders 0755 501:501, the plist 0644 501:501. Phone in TWRP.
#   tools/ios26-mg-perm-fix.sh            queue the fix
#   tools/ios26-mg-perm-fix.sh --undo     queue the values found today (0000 -2:-2 folders, plist 0644 501:-2)
set -euo pipefail
cd "$(dirname "$0")/.."
G=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache
out=build/ios26/mg-perm-fix; mkdir -p "$out"
if [ "${1:-}" = --undo ]; then
    python3 tools/overlay/sboverlay.py "$out/undo.bin" "attr:$G/Library/Caches/com.apple.MobileGestalt.plist:644:501:4294967294" \
        "attr:$G/Library/Caches:0:4294967294:4294967294" "attr:$G/Library:0:4294967294:4294967294"
    tools/overlay/card-overlay.sh "$out/undo.bin"
    exit 0
fi
python3 tools/overlay/sboverlay.py "$out/fix.bin" "attr:$G/Library:755:501:501" "attr:$G/Library/Caches:755:501:501" \
    "attr:$G/Library/Caches/com.apple.MobileGestalt.plist:644:501:501"
tools/overlay/card-overlay.sh "$out/fix.bin"
echo "queued: boot ios26t4 (or v3); check the kernel log for 'STARLTE OVERLAY: setattr ... : 0' x3"
