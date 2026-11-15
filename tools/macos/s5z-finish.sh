#!/bin/bash
# S5z: link kernel seq2f (= seq2d + 0459 really linked: S5y's seq2e was linked from the old StarbiteMali object)
# and pack its payloads. Needs the USB stick (the kernel build tree is the image
# /Volumes/SBWORK/claude-scratch-apple/dxb.sparseimage) and the Apple side session's scratch build directory.
#   tools/macos/s5z-finish.sh SCRATCH      (SCRATCH = that session's scratchpad: kbuild/, dxbmnt/)
set -euo pipefail
cd "$(dirname "$0")/../.."
S=$1
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
[ -d "$S/dxbmnt/dxb-12377" ] || hdiutil attach -nobrowse -mountpoint "$S/dxbmnt" /Volumes/SBWORK/claude-scratch-apple/dxb.sparseimage >/dev/null
K=$S/kbuild
# every object compiled in the scratch directory must be the one that is linked
for f in "$K"/obj/*.o "$K"/obj/*.cpo; do
	n=$(grep -c "/kbuild/obj/$(basename "$f")\$" "$K/seq2f.filelist" || true)
	[ "$n" = 1 ] || { echo "not linked from obj: $(basename "$f")"; exit 1; }
done
(cd "$K" && ./link3.sh "$K/seq2f.filelist" "$K/seq2f.noctf" 2>&1 | grep -v libTightbeam || true)
for s in 'STARLTE GPU: buffer cache' 'STARLTE GPU CACHE' 'SWEET PROF: sampling' 'SWEET KBD: not the swipe'; do
	strings -a "$K/seq2f.noctf" | grep -q "$s" || { echo "seq2f lacks \"$s\""; exit 1; }
done
cp "$K/seq2f.noctf" out/kernel.development.starlte-12377-seq2f
export KERNEL=out/kernel.development.starlte-12377-seq2f ADT=tools/macos/adt/starlte-mac-j313-sip-native.adt
for n in s5z s5zauto s5zday; do
	bash tools/mkpayload-ios26.sh macos15-$n tools/cmdline/macos15-$n.txt | tail -1
	strings -a out/xnu-macos15-$n.xnu | grep -q 'STARLTE GPU CACHE' || { echo "payload $n lacks the cache"; exit 1; }
done
shasum -a 256 out/kernel.development.starlte-12377-seq2f out/xnu-macos15-s5z.xnu out/xnu-macos15-s5zauto.xnu out/xnu-macos15-s5zday.xnu
ls -l out/xnu-macos15-s5z*.xnu | awk '{print $5, $NF}'
echo "S5Z BUILT"
