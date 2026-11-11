#!/bin/sh
# The Mali's Metal driver userland on a macOS 15 Sequoia root, for Metal's own discovery (the driver path:
# kernel 0361 publishes an IOAccelerator with sbmtl=2, the Mac ADT has SIP off). No sbmac, no injection.
#
#   tools/macos/sequoia-metal-stage.sh MNT [OWNERS.json]
#
# MNT is the root image mounted read-write (by its mount point; this script never attaches or detaches).
# Puts there:
#   /System/Library/Extensions/StarbiteMetal.bundle  from out/macos-metal (tools/macos/metal/build.sh, ARM64E=1):
#       Contents/Info.plist, Contents/MacOS/StarbiteMetal (frontend + metal2vulkan bridge),
#       Contents/Frameworks/libvulkan_panfrost.dylib (PanVK + starbite-drm -> StarbiteMali), _CodeSignature.
#       Apple's modes (as AGXMetal*.bundle): directories 0755, files 0644, the two Mach-O images 0755.
#   /private/var/local/mtl2-clients  0644: the processes that get the Mali device (WindowServer sbmtltest)
#   /private/var/local/m2v-cache     0777: metal2vulkan's shader cache (stays empty on a read-only root)
# Owners: files made on the mounted image belong to the Mac user. root:wheel comes from tools/macos/hfs-owners.py,
# run on the detached image afterwards: it gives every record OWNERS.json does not list 0:0 and keeps its mode.
# With OWNERS.json (the build's $W/owners-desktop.json), the staged paths are also written into it as [0, 0, mode],
# so the owners step states them explicitly. Then, as build-macos-root.sh does:
#   python3 -I tools/macos/hfs-owners.py IMAGE OWNERS.json && (read-only attach, fsck_hfs -fn, detach by mount point)
#
# Checks before copying: both images are arm64e (dyld refuses arm64 in WindowServer), ad hoc signatures valid,
# hashes as in out/macos-metal/SHA256SUMS. Refuses a root without /System/Library or with sbmac staged.
set -eu
cd "$(dirname "$0")/../.."
ROOT=$PWD
mnt=${1:?usage: $0 MNT [OWNERS.json]}
owners=${2:-}
SRC=${SRC:-$ROOT/out/macos-metal}
B=$SRC/StarbiteMetal.bundle
[ -d "$mnt/System/Library" ] || { echo "$mnt: not a macOS root (no /System/Library)"; exit 1; }
[ -d "$B/Contents/MacOS" ] || { echo "$B missing: tools/macos/metal/build.sh"; exit 1; }
[ ! -e "$mnt/usr/lib/sbmac.dylib" ] || { echo "$mnt/usr/lib/sbmac.dylib exists: the driver path has no injection"; exit 1; }
[ -z "$owners" ] || [ -f "$owners" ] || { echo "$owners: no such owners file"; exit 1; }

# 1. what is staged: arm64e images, valid ad hoc signatures, the hashes build.sh dest listed
python3 -I - "$B" <<'PY'
import struct, sys
for rel in ('Contents/MacOS/StarbiteMetal', 'Contents/Frameworks/libvulkan_panfrost.dylib'):
    magic, ct, st = struct.unpack('<III', open(f'{sys.argv[1]}/{rel}', 'rb').read(12))
    if magic != 0xFEEDFACF or ct != 0x0100000C or st != 0x80000002:
        sys.exit(f'{rel}: cputype {ct:#x} subtype {st:#x}: not arm64e (build.sh with ARM64E=1)')
    print(f'{rel}: arm64e (0x80000002)')
PY
codesign -v "$B/Contents/Frameworks/libvulkan_panfrost.dylib"
codesign -v "$B"
if [ -f "$SRC/SHA256SUMS" ]; then
    (cd "$SRC" && grep ' ./StarbiteMetal.bundle/' SHA256SUMS | shasum -a 256 -c --quiet -) ||
        { echo "$SRC: files differ from SHA256SUMS (rerun build.sh dest)"; exit 1; }
fi

# 2. copy, with Apple's modes
X=$mnt/System/Library/Extensions
mkdir -p "$X"
chmod 0755 "$X"
rm -rf "$X/StarbiteMetal.bundle"
cp -R "$B" "$X/"
find "$X/StarbiteMetal.bundle" -name '._*' -delete
find "$X/StarbiteMetal.bundle" -type d -exec chmod 0755 {} +
find "$X/StarbiteMetal.bundle" -type f -exec chmod 0644 {} +
chmod 0755 "$X/StarbiteMetal.bundle/Contents/MacOS/StarbiteMetal" \
    "$X/StarbiteMetal.bundle/Contents/Frameworks/libvulkan_panfrost.dylib"
codesign -v "$X/StarbiteMetal.bundle"
L=$mnt/private/var/local
mkdir -p "$L/m2v-cache"
chmod 0755 "$L"
chmod 0777 "$L/m2v-cache"
cp "$ROOT/tools/macos/metal/mtl2-clients" "$L/mtl2-clients"
chmod 0644 "$L/mtl2-clients"

# 3. the owners list (optional)
if [ -n "$owners" ]; then
    python3 -I - "$mnt" "$owners" <<'PY'
import json, os, stat, sys
mnt, path = sys.argv[1], sys.argv[2]
own = json.load(open(path))
n = 0
tops = ['/System/Library/Extensions/StarbiteMetal.bundle', '/private/var/local']
for top in tops:
    for d, dirs, files in os.walk(mnt + top):
        for p in [d] + [os.path.join(d, f) for f in files]:
            own[p[len(mnt):]] = [0, 0, stat.S_IMODE(os.lstat(p).st_mode)]
            n += 1
own.setdefault('/System/Library/Extensions', [0, 0, 0o755])
tmp = path + '.tmp'
with open(tmp, 'w') as f:
    json.dump(own, f)
os.replace(tmp, path)
print(f'{path}: {n} staged paths as root:wheel')
PY
fi

echo "staged into $mnt:"
(cd "$mnt" && find System/Library/Extensions/StarbiteMetal.bundle private/var/local -exec ls -ld {} + | awk '{print $1, $5, $NF}')
(cd "$mnt" && shasum -a 256 System/Library/Extensions/StarbiteMetal.bundle/Contents/MacOS/StarbiteMetal \
    System/Library/Extensions/StarbiteMetal.bundle/Contents/Frameworks/libvulkan_panfrost.dylib private/var/local/mtl2-clients)
echo "next: detach $mnt by its mount point, then tools/macos/hfs-owners.py IMAGE OWNERS.json and a read-only fsck_hfs -fn"
