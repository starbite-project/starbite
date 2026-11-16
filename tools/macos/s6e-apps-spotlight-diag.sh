#!/bin/bash
# S6e (2026-11-16): three usability fixes in one card write.   Phone in TWRP.   tools/macos/s6e-apps-spotlight-diag.sh
#
# 1. Apps that would not open: Books, Freeform, Maps, Music, Photos, TV and Weather had NO executable on the card
#    root (the Sequoia root build leaves out files over 16 MiB: build/macos15/skipped-desktop.txt). Their arm64e
#    slices, fetched from the 15.6 IPSW (ipsw-apfs.py --thin), rewritten for the S9's cores and ad hoc signed
#    (a64e-rewrite.py root), are in build/macos15/s6e/src, with Maps' Assets.car (168 MB in all).
# 2. Finder search: the root disables Spotlight (tools/macos/sequoia-disabled.list). com.apple.metadata.mds and
#    com.apple.metadata.mds.index go back into /System/Library/xpc/launchd.plist (launchd-enable-jobs.py on the
#    CARD's copies of the two launchd files) and their job plists get their names back. /.metadata_never_index stays:
#    this step shows whether mds runs here at all (it crash-looped on Ventura) before any index is built.
# 3. Diagnostics: S3zy's leftover agent (org.starbite.dispdiag: it opens System Settings by itself and runs `log`)
#    goes; /Library/LaunchAgents/org.starbite.diag.plist + /Library/Starbite/sbdiag.sh come. They do nothing unless
#    the kernel command line has sbdiag=1 (then: do the seven apps start, mds state, and the error lines of
#    Notification Center, the wallpaper programs, the icon services, mds and the apps, as "SBDIAG"/"SBLOG" lines).
#
# v2: cycle 1 also frees room (tools/macos/s6e-free-space.txt); the card root had 304 MiB free of 16 GiB.
# Two cycles, never one (s3zg): blocks freed by remove and taken again by add in one image would not be pushed whole.
# BETWEEN THE CYCLES THE CARD HAS NO launchd.plist: macOS must not be booted until "S6E OK". If the script stops
# after "cycle 1 pushed", run it again with RESUME=2 (the card's originals are kept in build/macos15/s6e/card).
set -euo pipefail
cd "$(dirname "$0")/../.."
T=tools/macos/card-root-meta-sync.py
G=tools/macos/card-file-get.py
D=build/macos15/s6e
X=/System/Library/xpc
LD=/System/Library/LaunchDaemons
[ -d "$D/src/System/Applications/Books.app" ] || { echo "no $D/src (the fetched app executables)"; exit 1; }

if [ "${RESUME:-1}" = 1 ]; then
	# the card's own copies (read only): other steps edited launchd.plist on the card, no image on the Mac has it
	rm -rf "$D/card" "$D/new" "$D/add"
	mkdir -p "$D/card"
	python3 $G $X/launchd.plist "$D/card/launchd.plist"
	python3 $G $X/launchd-disabled.plist "$D/card/launchd-disabled.plist"
	python3 $G $LD/com.apple.metadata.mds.plist.starbite-off "$D/card/com.apple.metadata.mds.plist"
	python3 $G $LD/com.apple.metadata.mds.index.plist.starbite-off "$D/card/com.apple.metadata.mds.index.plist"
fi
[ -s "$D/card/launchd.plist" ] && [ -s "$D/card/launchd-disabled.plist" ] || { echo "no card copies in $D/card"; exit 1; }
rm -rf "$D/new" "$D/add"
python3 -I tools/macos/launchd-enable-jobs.py "$D/card/launchd.plist" "$D/card/launchd-disabled.plist" "$D/new" \
	com.apple.metadata.mds com.apple.metadata.mds.index
plutil -lint "$D/new/launchd.plist" "$D/new/launchd-disabled.plist"

# what cycle 2 adds, laid out like the volume root
mkdir -p "$D/add$X" "$D/add$LD" "$D/add/Library/LaunchAgents" "$D/add/Library/Starbite"
cp -R "$D/src/System/Applications" "$D/add/System/"
cp "$D/new/launchd.plist" "$D/new/launchd-disabled.plist" "$D/add$X/"
cp "$D/card/com.apple.metadata.mds.plist" "$D/card/com.apple.metadata.mds.index.plist" "$D/add$LD/"
cp tools/macos/diag/org.starbite.diag.plist "$D/add/Library/LaunchAgents/"
cp tools/macos/diag/sbdiag.sh "$D/add/Library/Starbite/"
chmod 755 "$D/add/Library/Starbite/sbdiag.sh"
find "$D/add" -name .DS_Store -delete
need_mb=$(( $(du -sm "$D/add" | cut -f1) + 150 ))

cat > "$D/remove.txt" <<EOF
# S6e cycle 1: the files cycle 2 puts back changed, and S3zy's diagnostic agent
$X/launchd.plist
$X/launchd-disabled.plist
$LD/com.apple.metadata.mds.plist.starbite-off
$LD/com.apple.metadata.mds.index.plist.starbite-off
/Library/LaunchAgents/org.starbite.dispdiag.plist
EOF

# free blocks of an HFS+ volume image, in MiB (volume header at 1024: blockSize +40, freeBlocks +48, big-endian)
free_mb() {
	python3 -I - "$1" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb'); f.seek(1024); h = f.read(512)
assert h[:2] in (b'HX', b'H+'), h[:2]
bs, total, free = struct.unpack('>III', h[40:52])
print(free * bs >> 20)
PY
}

if [ "${RESUME:-1}" = 1 ]; then
	I=$D/root-card-rm.img
	python3 $T pull "$I"
	have=$(free_mb "$I")
	# S6e v2 (the first run stopped here: 304 MiB free, 320 needed): cycle 1 also removes things macOS on the phone has
	# no use for (tools/macos/s6e-free-space.txt), and the room is checked on the image AFTER the removals, still
	# before anything is written to the card.
	cat "$D/remove.txt" tools/macos/s6e-free-space.txt > "$D/remove-all.txt"
	python3 $T remove "$I" "$D/remove-all.txt"
	after=$(free_mb "$I")
	echo "card root: $have MiB free, $after MiB after the removals; cycle 2 needs $need_mb MiB (the files + 150 MiB kept free)"
	[ "$after" -ge "$need_mb" ] || { echo "NOT ENOUGH ROOM on the card root: nothing was changed"; rm -f "$I" "$I".*; exit 3; }
	python3 $T plan "$I"
	python3 $T push "$I" --dry-run
	python3 $T push "$I"
	rm -f "$I" "$I".*
	echo "S6E cycle 1 pushed (the card has no launchd.plist now: do not boot macOS; RESUME=2 runs cycle 2 again)"
fi
I=$D/root-card-add.img
python3 $T pull "$I"
python3 $T add "$I" "$D/add"
python3 $T plan "$I"
python3 $T push "$I" --dry-run
python3 $T push "$I"
rm -f "$I" "$I".*
echo "S6E OK: 7 app executables added, mds + mds.index enabled, diagnostic agent replaced"
