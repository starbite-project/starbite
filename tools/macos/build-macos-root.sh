#!/usr/bin/env bash
# The macOS root for the S9's microSD card: a raw, read-only, NOT journaled HFSX volume
# (no partition map, the volume starts at sector 0 of the sdroot= range), filled straight
# from Apple's CDN with tools/macos/ipsw-apfs.py (ranged reads, nothing else downloaded).
# No sudo: the image is made and filled as the user, then tools/macos/hfs-owners.py writes
# Apple's owners/groups/modes into the catalog offline. Docs: docs/MACOS.md.
#
#   tools/macos/build-macos-root.sh t0      -> build/macos13/root-t0.img   (64 MiB)
#       launchd + dyld + launchd.plist + /private from the Data template, NO shared cache.
#       Fits the card's 64 MiB test range (sdroot=lba:249868288,131072). Expected boot:
#       the kernel mounts it, execs /sbin/launchd, dyld runs and stops at the missing
#       cache; launchd (pid 1) dies -> "initproc exited" panic -> sweetreboot -> TWRP.
#   tools/macos/build-macos-root.sh full    -> build/macos13/root-full.img (SIZE_MB, 3900)
#       + the arm64e shared cache (SystemOS cryptex, 3.2 GiB, two streaming passes over
#       the deflated cryptex) translated by tools/a64e/a64e-rewrite.py as the iOS 26 root
#       build does (--skip libcorecrypto), + the files in tools/macos/ventura-full.list.
#
# Env: IPSW_URL, OS_DMG, SYS_DMG (default macOS 13.5 22G74), OUT, SIZE_MB, KEEP_MOUNTED=1,
#      RESUME=1 (keep the image and its OS files, redo from the cache step), CENSUS=0, TRANSLATE_A64E=0.
# Apple software stays local (build/ is gitignored).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
TIER=${1:-t0}
URL=${IPSW_URL:-https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw}
OS_DMG=${OS_DMG:-096-63007-081.dmg}       # BuildManifest OS (UDIF, stored in the zip)
SYS_DMG=${SYS_DMG:-096-62994-081.dmg}     # Cryptex1,SystemOS (raw APFS, deflated in the zip)
W=$ROOT/build/macos13
CRYPTEX=/System/Volumes/Preboot/Cryptexes/OS   # where dyld-1066 looks first for the cache
mkdir -p "$W"
RD=(python3 -I "$ROOT/tools/macos/ipsw-apfs.py" "$URL")
case $TIER in
    t0)   SIZE_MB=${SIZE_MB:-64};   LIST=tools/macos/ventura-t0.list;   OUT=${OUT:-$W/root-t0.img} ;;
    full) SIZE_MB=${SIZE_MB:-3900}; LIST=tools/macos/ventura-full.list; OUT=${OUT:-$W/root-full.img} ;;
    *) echo "usage: $0 t0|full"; exit 2 ;;
esac
free_mb=$(df -m "$W" | awk 'NR==2 {print $4}')
[ -n "${RESUME:-}" ] || (( free_mb > SIZE_MB + 800 )) || { echo "need $((SIZE_MB + 800)) MB free in $W, have $free_mb"; exit 1; }

owners=$W/owners-$TIER.json
attach() { hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$@" 2>/dev/null; }
if [ -n "${RESUME:-}" ] && [ -f "$OUT" ] && [ -f "$owners" ]; then
    # RESUME=1: the OS files are in place from an earlier run; redo from the cache step.
    mnt=$(attach "$OUT" | awk -F'\t' '/\/Volumes\// {print $NF}')
    [ -d "$mnt" ] || { echo "could not mount $OUT"; exit 1; }
    cleanup() { [ -n "${KEEP_MOUNTED:-}" ] || hdiutil detach "$mnt" >/dev/null 2>&1 || true; }
    trap cleanup EXIT
    echo "resuming $OUT at $mnt"
else
rm -f "$OUT" "$owners"
truncate -s "${SIZE_MB}m" "$OUT"          # sparse until written
dev=$(attach -nomount "$OUT" | awk 'NR==1 {print $1}')
/sbin/newfs_hfs -s -b 4096 -v MACROOT "/dev/r${dev#/dev/}"   # case-sensitive, no journal
hdiutil detach "$dev" >/dev/null 2>&1
mnt=$(attach "$OUT" | awk -F'\t' '/\/Volumes\// {print $NF}')
[ -d "$mnt" ] || { echo "could not mount $OUT"; exit 1; }
cleanup() { [ -n "${KEEP_MOUNTED:-}" ] || hdiutil detach "$mnt" >/dev/null 2>&1 || true; }
trap cleanup EXIT
echo "image $OUT mounted at $mnt"

# 1. the OS volume's files
thin=(); [ "$TIER" = full ] && thin=(--thin)       # full: arm64e slices only
"${RD[@]}" get "$OS_DMG" "$mnt" --list "$LIST" --owners "$owners" "${thin[@]}"

# 2. System and Data are one volume here: the Data template (what the installer copies to
#    a new Data volume) is moved into place, its owners re-keyed.
python3 -I - "$mnt" "$owners" <<'PY'
import json, os, sys
mnt, path = sys.argv[1], sys.argv[2]
tpl = os.path.join(mnt, 'System/Library/Templates/Data')
own = json.load(open(path))
def merge(src, dst):
    for name in os.listdir(src):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if os.path.isdir(d) and not os.path.islink(d) and os.path.isdir(s) and not os.path.islink(s):
            merge(s, d)
        elif not os.path.lexists(d):
            os.rename(s, d)
moved = 0
if os.path.isdir(tpl):
    merge(tpl, mnt)
    pre = '/System/Library/Templates/Data'
    for k in [k for k in own if k.startswith(pre + '/')]:
        own.setdefault(k[len(pre):], own[k]); moved += 1
json.dump(own, open(path, 'w'), indent=0, sort_keys=True)
print(f'Data template merged ({moved} owner entries re-keyed)')
PY
mkdir -p "$mnt$CRYPTEX/System/Library/dyld" "$mnt/dev" "$mnt/private/var/tmp" "$mnt/private/var/db"
fi

build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$W/BuildManifest.plist" 2>/dev/null || echo 22G74)
# 3. full: the arm64e shared cache from the SystemOS cryptex, translated in place
if [ "$TIER" = full ]; then
    "${RD[@]}" get "$SYS_DMG" "$mnt$CRYPTEX" --index "$W/systemos.idx" \
        --owners "$owners" --prefix "$CRYPTEX" \
        /System/Library/dyld/dyld_shared_cache_arm64e \
        /System/Library/dyld/dyld_shared_cache_arm64e.01 \
        /System/Library/dyld/dyld_shared_cache_arm64e.map
    [ -x "$ROOT/build/venv/bin/python" ] || { echo "build/venv (numpy) missing: see tools/build-ios-root.sh"; exit 1; }
    # The census runs on Apple's bytes, before the rewrite (CENSUS=0 skips it).
    if [ "${CENSUS:-1}" = 1 ]; then
        "$ROOT/build/venv/bin/python" -I "$ROOT/tools/dsc-isa-census.py" \
            "$mnt$CRYPTEX/System/Library/dyld/dyld_shared_cache_arm64e" | tee "$W/census-$build.txt"
    fi
    if [ "${TRANSLATE_A64E:-1}" = 1 ]; then
        "$ROOT/build/venv/bin/python" -I "$ROOT/tools/a64e/a64e-rewrite.py" cache \
            "$mnt$CRYPTEX/System/Library/dyld/dyld_shared_cache_arm64e" --skip libcorecrypto
    fi
fi

printf 'Starbite macOS root, tier %s, build %s, made %s by tools/macos/build-macos-root.sh\n' \
    "$TIER" "$build" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$mnt/.starbite-macos"
df -h "$mnt" | tail -1
hdiutil detach "$mnt" >/dev/null
trap - EXIT

# 4. owners, then a read-only fsck of the result
python3 -I "$ROOT/tools/macos/hfs-owners.py" "$OUT" "$owners"
dev=$(attach -nomount "$OUT" | awk 'NR==1 {print $1}')
/sbin/fsck_hfs -fn "/dev/r${dev#/dev/}" || { hdiutil detach "$dev" >/dev/null 2>&1; echo "fsck_hfs failed"; exit 1; }
hdiutil detach "$dev" >/dev/null 2>&1
xxd -s 1024 -l 2 -p "$OUT" | grep -qx 4858 || { echo "$OUT: no HX signature at 1024"; exit 1; }
ls -l "$OUT"
shasum -a 256 "$OUT"
