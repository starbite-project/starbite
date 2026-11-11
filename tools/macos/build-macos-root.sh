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
#   tools/macos/build-macos-root.sh desktop -> build/macos13/root-desktop.img (SIZE_MB, 15360): nearly the whole
#       OS volume (ventura-desktop.list), files over MAXFILE (16 MiB) left out and listed, + the cache as full's.
#       + the arm64e shared cache (SystemOS cryptex, 3.2 GiB, two streaming passes over
#       the deflated cryptex) translated by tools/a64e/a64e-rewrite.py as the iOS 26 root
#       build does (--skip libcorecrypto), + the files in tools/macos/ventura-full.list.
#
# macOS 26.5 (25F71), the drawing path (docs/MACOS.md section 11): MACOS=26.
#   MACOS=26 tools/macos/build-macos-root.sh t1 -> /Volumes/Untitled/starbite-macos26/root-26t1.img
#       (SIZE_MB, 10240): launchd + dyld, /bin /sbin /usr/{lib,libexec,sbin}, every job's executable
#       (macos26-t1.list + macos26-jobs.list) and the whole arm64e shared cache (12 subcaches,
#       5.8 GB) from the SystemOS cryptex, translated. 26.5's images are AEA-encrypted: ipsw-apfs.py
#       reads them at random through libAppleArchive over ranged GETs (keys in $W/keys.txt, asked of
#       Apple's key service once). Everything large is on the USB drive (W=); the Mac keeps nothing.
#
# macOS 15.6 Sequoia (24G84), MacBookAir10,1 identity, the driver path (docs/APPLE-S9-DRIVERS.md): MACOS=15.
#   MACOS=15 tools/macos/build-macos-root.sh t0      -> /Volumes/SBWORK/starbite-macos15/root-15t0.img (64 MiB)
#       launchd + dyld + launchd.plist + /private (sequoia-t0.list), no cache; launchd and dyld rewritten.
#   MACOS=15 tools/macos/build-macos-root.sh desktop -> /Volumes/SBWORK/starbite-macos15/root-15desktop.img (16 GiB)
#       nearly the whole OS volume (sequoia-desktop.list, --thin, files over MAXFILE left out and listed) + the
#       arm64e cache from the SystemOS cryptex, translated; then EVERY arm64e Mach-O on the root is rewritten
#       and re-signed ad hoc in place (a64e-rewrite.py root; A64E_ALL=0 keeps Apple's bytes outside the cache).
#       AEA keys in $W/keys.txt. RESERVE_MB (5900) stays free on the shared stick.
#
# Env: MACOS (13, 15 or 26), IPSW_URL, OS_DMG, SYS_DMG, W, OUT, SIZE_MB, KEEP_MOUNTED=1,
#      RESUME=1 (keep the image and its OS files, redo from the cache step), CENSUS=0, TRANSLATE_A64E=0,
#      A64E_ALL (MACOS=15: 1), RESERVE_MB.
# Apple software stays local (build/ is gitignored; the USB folder is not a repository).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
TIER=${1:-t0}
CRYPTEX=/System/Volumes/Preboot/Cryptexes/OS   # where dyld looks first for the cache
KEYS=()
if [ "${MACOS:-13}" = 26 ]; then
    URL=${IPSW_URL:-https://updates.cdn-apple.com/2026SpringFCS/fullrestores/122-58869/DFB1CEEF-5619-4591-9924-E20DB2C8FED0/UniversalMac_26.5_25F71_Restore.ipsw}
    OS_DMG=${OS_DMG:-094-56453-087.dmg.aea}    # BuildManifest OS (AEA: a raw APFS container)
    SYS_DMG=${SYS_DMG:-094-56679-089.dmg.aea}  # Cryptex1,SystemOS (AEA: raw APFS)
    W=${W:-/Volumes/Untitled/starbite-macos26}
    KEYS=(--keys "$W/keys.txt")
    CACHE=(dyld_shared_cache_arm64e dyld_shared_cache_arm64e.01 dyld_shared_cache_arm64e.02.dylddata
           dyld_shared_cache_arm64e.03.dyldreadonly dyld_shared_cache_arm64e.04.dyldlinkedit
           dyld_shared_cache_arm64e.05 dyld_shared_cache_arm64e.06.dylddata
           dyld_shared_cache_arm64e.07.dyldreadonly dyld_shared_cache_arm64e.08.dyldlinkedit
           dyld_shared_cache_arm64e.09 dyld_shared_cache_arm64e.10.dylddata dyld_shared_cache_arm64e.11
           dyld_shared_cache_arm64e.12.dyldlinkedit dyld_shared_cache_arm64e.map dyld_shared_cache_arm64e.atlas)
    case $TIER in
        t1) SIZE_MB=${SIZE_MB:-10240}; LISTS=(--list tools/macos/macos26-t1.list --list tools/macos/macos26-jobs.list)
            OUT=${OUT:-$W/root-26t1.img}; CACHE_TIER=1 ;;
        *) echo "usage: MACOS=26 $0 t1"; exit 2 ;;
    esac
    [ -d "$W" ] || { echo "$W missing (the USB drive)"; exit 1; }
elif [ "${MACOS:-13}" = 15 ]; then
    # macOS 15.6 Sequoia (24G84), MacBookAir10,1 (J313AP) identity; docs/APPLE-S9-DRIVERS.md (M9).
    URL=${IPSW_URL:-https://updates.cdn-apple.com/2025SummerFCS/fullrestores/082-08674/51294E4D-A273-44BE-A280-A69FC347FB87/UniversalMac_15.6_24G84_Restore.ipsw}
    OS_DMG=${OS_DMG:-043-18152-096.dmg.aea}    # BuildManifest OS (AEA: raw APFS, "Macintosh HD", 9.26 GB)
    SYS_DMG=${SYS_DMG:-043-18205-097.dmg.aea}  # Cryptex1,SystemOS (AEA: raw APFS, 5.17 GB)
    W=${W:-/Volumes/SBWORK/starbite-macos15}
    KEYS=(--keys "$W/keys.txt")
    # 15.6's arm64e cache is two files (2.71 + 2.20 GB) like 13.5's; the x86_64 and aot_* (Rosetta) caches are not taken
    CACHE=(dyld_shared_cache_arm64e dyld_shared_cache_arm64e.01 dyld_shared_cache_arm64e.map dyld_shared_cache_arm64e.atlas)
    # Every arm64e Mach-O on the root is rewritten too (a64e-rewrite.py root: re-hashed, ad hoc), not only the cache
    A64E_ALL=${A64E_ALL:-1}
    BUILD_DEFAULT=24G84
    PREFILL=${PREFILL-1}             # the stick is exFAT: paced zero-fill first (see the image step)
    RESERVE_MB=${RESERVE_MB:-5900}   # SBWORK is shared: leave 5 GB for the other sessions after the image
    case $TIER in
        t0) SIZE_MB=${SIZE_MB:-64}; LISTS=(--list tools/macos/sequoia-t0.list); OUT=${OUT:-$W/root-15t0.img}; CACHE_TIER= ;;
        # 16 GiB, not Ventura's 15: the list is 10.5 GiB of fat files (418k items) + a 4.6 GiB cache; 16 GiB is
        # the card range apple-m1.txt already names (sdroot=lba:182888448,33554432)
        desktop) SIZE_MB=${SIZE_MB:-16384}; LISTS=(--list tools/macos/sequoia-desktop.list); OUT=${OUT:-$W/root-15desktop.img}
              CACHE_TIER=1; MAXFILE=${MAXFILE:-16777216} ;;
        *) echo "usage: MACOS=15 $0 t0|desktop"; exit 2 ;;
    esac
    [ -d "$W" ] || { echo "$W missing (the USB drive)"; exit 1; }
else
    URL=${IPSW_URL:-https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw}
    OS_DMG=${OS_DMG:-096-63007-081.dmg}       # BuildManifest OS (UDIF, stored in the zip)
    SYS_DMG=${SYS_DMG:-096-62994-081.dmg}     # Cryptex1,SystemOS (raw APFS, deflated in the zip)
    W=${W:-$ROOT/build/macos13}
    CACHE=(dyld_shared_cache_arm64e dyld_shared_cache_arm64e.01 dyld_shared_cache_arm64e.map)
    case $TIER in
        t0)   SIZE_MB=${SIZE_MB:-64};   LISTS=(--list tools/macos/ventura-t0.list);   OUT=${OUT:-$W/root-t0.img}; CACHE_TIER= ;;
        full) SIZE_MB=${SIZE_MB:-3900}; LISTS=(--list tools/macos/ventura-full.list); OUT=${OUT:-$W/root-full.img}; CACHE_TIER=1 ;;
        desktop) SIZE_MB=${SIZE_MB:-15360}; LISTS=(--list tools/macos/ventura-desktop.list); OUT=${OUT:-$W/root-desktop.img}
              CACHE_TIER=1; MAXFILE=${MAXFILE:-16777216} ;;
        *) echo "usage: $0 t0|full|desktop"; exit 2 ;;
    esac
fi
mkdir -p "$W"
RD=(python3 -I -B "$ROOT/tools/macos/ipsw-apfs.py" "$URL")
free_mb=$(df -m "$W" | awk 'NR==2 {print $4}')
[ -n "${RESUME:-}" ] || (( free_mb > SIZE_MB + ${RESERVE_MB:-800} )) || { echo "need $((SIZE_MB + ${RESERVE_MB:-800})) MB free in $W, have $free_mb"; exit 1; }

owners=$W/owners-$TIER.json
attach() { hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$@" 2>/dev/null; }
# Detach only by mount point, only this script's own image, never by /dev/diskN, never -force
# (2026-11-11 house rule: a device-number detach once killed another session's build).
own_detach() {
    python3 -I - "$OUT" "$1" <<'PY' || { echo "not detaching $1: hdiutil info does not show $OUT mounted there"; return 1; }
import os, plistlib, subprocess, sys
# text comparison only: realpath() on another image's path on a hung USB volume blocks for ever (2026-11-11)
img, mnt = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
info = plistlib.loads(subprocess.run(['hdiutil', 'info', '-plist'], capture_output=True, check=True, timeout=60).stdout)
ok = any(i.get('image-path', '') == img and
         any(e.get('mount-point', '/nonexistent') == mnt for e in i.get('system-entities', []))
         for i in info.get('images', []))
sys.exit(0 if ok else 1)
PY
    hdiutil detach "$1" >/dev/null 2>&1
}
if [ -n "${RESUME:-}" ] && [ -f "$OUT" ] && [ -f "$owners" ]; then
    # RESUME=1: the OS files are in place from an earlier run; redo from the cache step.
    mnt=$(attach "$OUT" | awk -F'\t' '/\/Volumes\// {print $NF}')
    [ -d "$mnt" ] || { echo "could not mount $OUT"; exit 1; }
    cleanup() { [ -n "${KEEP_MOUNTED:-}" ] || own_detach "$mnt" || true; }
    trap cleanup EXIT
    echo "resuming $OUT at $mnt"
else
rm -f "$OUT" "$owners"
if [ -n "${PREFILL:-}" ]; then
    # PREFILL=1 (default for MACOS=15): the image file is written with zeros front to back in paced 64 MiB chunks,
    # each synced, before anything else. On exFAT (no sparse files) a plain truncate leaves the zero-fill to the first
    # write past the end (newfs_hfs's alternate volume header): one 16 GiB burst in a single write call, during which
    # the SBWORK stick dropped off the USB bus (2026-11-11). Stops if the volume vanishes.
    python3 -I - "$OUT" "$SIZE_MB" <<'PY' || { echo "prefill of $OUT failed"; exit 1; }
import os, sys, time
out, mb = sys.argv[1], int(sys.argv[2])
chunk = 64 << 20
zero = bytes(chunk)
total = mb << 20
t0 = time.time()
with open(out, 'wb', buffering=0) as f:
    done = 0
    while done < total:
        n = min(chunk, total - done)
        f.write(zero[:n])
        os.fsync(f.fileno())
        done += n
        if done % (1 << 30) == 0 or done == total:
            print(f'prefill {done >> 20} / {mb} MiB, {done / (time.time() - t0) / 1e6:.1f} MB/s', flush=True)
        time.sleep(0.05)
PY
else
    truncate -s "${SIZE_MB}m" "$OUT"          # sparse until written
fi
dev=$(attach -nomount "$OUT" | awk 'NR==1 {print $1}')
[ -n "$dev" ] || { echo "could not attach $OUT"; exit 1; }
/sbin/newfs_hfs -s -b 4096 -v MACROOT "/dev/r${dev#/dev/}"   # case-sensitive, no journal
# mount the new volume on the same attachment (no detach by device), then work by mount point
diskutil mount "$dev" >/dev/null
mnt=$(diskutil info "$dev" | sed -n 's/^ *Mount Point: *//p')
[ -d "$mnt" ] || { echo "could not mount $OUT"; exit 1; }
cleanup() { [ -n "${KEEP_MOUNTED:-}" ] || own_detach "$mnt" || true; }
trap cleanup EXIT
touch "$mnt/.metadata_never_index"               # no Spotlight on the build mount
echo "image $OUT mounted at $mnt"

# 1. the OS volume's files
thin=(); [ -n "$CACHE_TIER" ] && thin=(--thin)      # cache tiers: arm64e slices only
maxf=(); [ -n "${MAXFILE:-}" ] && maxf=(--max-file-size "$MAXFILE" --skipped-list "$W/skipped-$TIER.txt")
"${RD[@]}" get "$OS_DMG" "$mnt" "${LISTS[@]}" ${KEYS[@]+"${KEYS[@]}"} --owners "$owners" ${thin[@]+"${thin[@]}"} ${maxf[@]+"${maxf[@]}"}

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
# dyld-1066 on our kernel looks only in /System/Library/dyld/ (no cryptex mount here: 2026-11-03
# tier 1 log, open("/System/Library/dyld/") = ENOENT), so point that at the cryptex copy.
[ -e "$mnt/System/Library/dyld" ] || ln -s ../Volumes/Preboot/Cryptexes/OS/System/Library/dyld "$mnt/System/Library/dyld"
# dyld-1378 (26.5) also tries /private/preboot/Cryptexes/OS/ (the iOS layout) after the Preboot path.
[ "${MACOS:-13}" = 13 ] || [ -e "$mnt/private/preboot" ] || ln -s ../System/Volumes/Preboot "$mnt/private/preboot"
fi

build=${BUILD_DEFAULT:-22G74}   # PlistBuddy prints "File Doesn't Exist" to stdout with status 0, so test first
[ ! -f "$W/BuildManifest.plist" ] || build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$W/BuildManifest.plist" 2>/dev/null || echo "$build")
# 3. cache tiers: the arm64e shared cache from the SystemOS cryptex, translated in place
if [ -n "$CACHE_TIER" ]; then
    "${RD[@]}" get "$SYS_DMG" "$mnt$CRYPTEX" --index "$W/systemos.idx" ${KEYS[@]+"${KEYS[@]}"} \
        --owners "$owners" --prefix "$CRYPTEX" "${CACHE[@]/#//System/Library/dyld/}"
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
# 3b. MACOS=15 (A64E_ALL=1): every arm64e Mach-O outside the cache, rewritten the same way and re-signed
#     ad hoc in place (code slots re-hashed; entitlements, identifier, flags and size kept). Idempotent,
#     so a RESUME=1 run repeats it harmlessly. Per-file counts: $W/a64e-root-$TIER.txt
if [ "${A64E_ALL:-0}" = 1 ] && [ "${TRANSLATE_A64E:-1}" = 1 ]; then
    [ -x "$ROOT/build/venv/bin/python" ] || { echo "build/venv (numpy) missing: see tools/build-ios-root.sh"; exit 1; }
    "$ROOT/build/venv/bin/python" -I "$ROOT/tools/a64e/a64e-rewrite.py" root "$mnt" \
        --log "$W/a64e-root-$TIER.txt" 2> "$W/a64e-root-$TIER.err" | tee "$W/a64e-root-$TIER.sum"
fi

printf 'Starbite macOS root, tier %s, build %s, made %s by tools/macos/build-macos-root.sh\n' \
    "$TIER" "$build" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$mnt/.starbite-macos"
# 26.5: the Starbite layer (boot-task no-ops, WindowServer job out, Metal staging + the gated
# sbmtltest job): tools/macos/macos26-post.sh, docs/MACOS.md section 12
[ "${MACOS:-13}" != 26 ] || W="$W" tools/macos/macos26-post.sh "$mnt" "$owners"
df -h "$mnt" | tail -1
own_detach "$mnt"
trap - EXIT

# 4. owners, then a read-only fsck of the result
python3 -I "$ROOT/tools/macos/hfs-owners.py" "$OUT" "$owners"
# (read-only mount, fsck of its raw device, detach by mount point)
line=$(attach -readonly "$OUT" | awk -F'\t' '/\/Volumes\// {print}')
dev=$(echo "$line" | awk '{print $1}'); mnt=$(echo "$line" | awk -F'\t' '{print $NF}')
[ -d "$mnt" ] || { echo "could not mount $OUT read-only for fsck"; exit 1; }
/sbin/fsck_hfs -fn "/dev/r${dev#/dev/}" || { own_detach "$mnt"; echo "fsck_hfs failed"; exit 1; }
own_detach "$mnt"
xxd -s 1024 -l 2 -p "$OUT" | grep -qx 4858 || { echo "$OUT: no HX signature at 1024"; exit 1; }
ls -l "$OUT"
shasum -a 256 "$OUT"
