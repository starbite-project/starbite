#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# Stage Apple's open-source IOKit families for building INTO our xnu-12377 STARLTE kernel.
#   sh starbite/apple-families/stage.sh [IOStorageFamily|IOHIDFamily|IONetworkingFamily ...]   (default: all)
# For each family:
#   1. clones github.com/apple-oss-distributions/<Family> at the macOS 26.5 tag (distribution-macOS macos-265,
#      the release that ships xnu-12377.121.6) into build/families/<Family> (gitignored), if not there;
#   2. applies starbite/apple-families/<Family>/patches/*.diff to the clone (idempotent: skipped if already applied);
#   3. builds build/families/_inc/<Family>/: the header layout the sources include (<IOKit/storage/...> etc.) as
#      symlinks into the clone, plus iig-generated headers/impls under build/families/_gen/<Family>/.
# Nothing is written under build/xnu-12377 or build/dxb-12377. Compile-check afterwards with
#   sh starbite/apple-families/check.sh <Family>
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FAM=$ROOT/build/families
HERE=$ROOT/starbite/apple-families
XNU=$ROOT/build/dxb-12377/xnu
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
IIG=$(xcrun --find iig)
SDK=$(xcrun --sdk macosx --show-sdk-path)
DKSDK=$(xcrun --sdk driverkit --show-sdk-path)
SDKK=$SDK/System/Library/Frameworks/Kernel.framework/Versions/A/Headers

tag_of() {
	case $1 in
	IOStorageFamily) echo IOStorageFamily-337.100.1 ;;
	IOHIDFamily) echo IOHIDFamily-2238.120.5 ;;
	IONetworkingFamily) echo IONetworkingFamily-188 ;;
	*) echo "unknown family $1" >&2; exit 2 ;;
	esac
}

clone() { # family
	[ -d "$FAM/$1/.git" ] && return 0
	mkdir -p "$FAM"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "$(tag_of "$1")" \
		"https://github.com/apple-oss-distributions/$1.git" "$FAM/$1"
}

patch_family() { # family
	for d in "$HERE/$1"/patches/*.diff; do
		[ -f "$d" ] || continue
		if git -C "$FAM/$1" apply --reverse --check "$d" 2>/dev/null; then
			continue # already applied
		fi
		git -C "$FAM/$1" apply --whitespace=nowarn "$d"
		echo "  applied $(basename "$d")"
	done
}

common() { # incdir: what a kext gets from Kernel.framework but XNU's kernel include path lacks
	# TargetConditionals.h: our tree's copy (bsd/sweet_hfs, identical to the SDK's Kernel.framework one); it keys
	# TARGET_OS_OSX / TARGET_OS_IPHONE off the compiler target, so it follows the kernel target (macOS or iOS).
	link_all "$1" "$XNU/bsd/sweet_hfs/TargetConditionals.h"
}

link_all() { # destdir file...
	dst=$1; shift
	mkdir -p "$dst"
	for lf in "$@"; do ln -sf "$lf" "$dst/$(basename "$lf")"; done
}

iig_gen() { # family .iig-file outdir framework-name sdk(xnu|driverkit)
	mkdir -p "$3"
	b=$(basename "$2" .iig)
	if [ "$5" = xnu ]; then
		# as XNU's own iokit/DriverKit Makefile: <DriverKit/*.iig> from our kernel tree
		set -- "$@" -I"$FAM/_inc/$1" -I"$XNU/iokit" -I"$XNU/osfmk" -I"$XNU/bsd" -isysroot "$SDK" -DDRIVERKIT_PRIVATE=1
	else
		# as Apple's iigFramework.sh: DriverKit SDK (TARGET_OS_DRIVERKIT=1), the family's headers first. Needed when
		# the .iig pulls family headers that pick <IOKit/...> vs <DriverKit/...> on TARGET_OS_DRIVERKIT (iig defines
		# KERNEL as an annotation, so XNU's "#if KERNEL" headers cannot be parsed by it).
		set -- "$@" -I"$FAM/_inc/$1" -isysroot "$DKSDK" -iframework "$DKSDK/System/DriverKit/System/Library/Frameworks" \
			-isystem "$DKSDK/System/DriverKit/usr/include" -DPLATFORM_DriverKit=1
	fi
	fam_=$1 def_=$2 out_=$3 fw_=$4
	shift 5
	"$IIG" --def "$def_" --header "$out_/$b.h" --impl "$out_/$b.iig.cpp" --framework-name "$fw_" -- \
		"$@" -x c++ -std=gnu++2b -D__IIG=1
	: "$fam_"
}

stage_IOStorageFamily() {
	S=$FAM/IOStorageFamily I=$FAM/_inc/IOStorageFamily G=$FAM/_gen/IOStorageFamily
	rm -rf "$I" "$G"
	common "$I"
	link_all "$I/IOKit/storage" "$S"/*.h
	# kernel-side IOUserBlockStorageDevice (the dext bridge IOBlockStorageDriver casts to): header + dispatch impl
	iig_gen IOStorageFamily "$S/BlockStorageDeviceDriverKit/IOUserBlockStorageDevice.iig" "$G" BlockStorageDeviceDriverKit xnu
	link_all "$I" "$G/IOUserBlockStorageDevice.h"                        # "IOUserBlockStorageDevice.h" (kext .h)
	link_all "$I/BlockStorageDeviceDriverKit" "$G/IOUserBlockStorageDevice.h" # <BlockStorageDeviceDriverKit/...> (.iig.cpp)
}

stage_IOHIDFamily() {
	S=$FAM/IOHIDFamily I=$FAM/_inc/IOHIDFamily G=$FAM/_gen/IOHIDFamily
	rm -rf "$I" "$G"
	common "$I"
	# <IOKit/hid/...>: the family's own headers (IOHIDFamily/) and the four event headers the published tree lacks,
	# from HERE/IOHIDFamily/missing-headers (IOHIDFamily-1915.140.3's, APSL, adapted to 2238; see NOTES.md)
	link_all "$I/IOKit/hid" "$S"/IOHIDFamily/*.h "$HERE"/IOHIDFamily/missing-headers/*.h
	link_all "$I/IOKit/hidevent" "$S"/IOHIDFamily/IOHIDEventService.h "$S"/IOHIDFamily/IOHIDEventDriver.h "$S"/IOHIDFamily/IOHIDUtility.h
	link_all "$I/IOKit/hidsystem" "$S"/IOHIDFamily/IOHIDUsageTables.h
	# other families' public headers the HID sources include for constants and types: the SDK's Kernel.framework
	# (macOS 15.5 SDK; IOGraphics and IOUSBHostFamily are not built in, only their headers are read)
	link_all "$I/IOKit/graphics" "$SDKK"/IOKit/graphics/IOGraphicsTypes.h "$SDKK"/IOKit/graphics/IOGraphicsDevice.h
	link_all "$I/IOKit/usb" "$SDKK"/IOKit/usb/*.h
	# small compat headers for internal-SDK headers the sources include (see each file)
	link_all "$I" "$HERE"/IOHIDFamily/compat/math.h
	link_all "$I/IOKit" "$HERE"/IOHIDFamily/compat/IOKit/*.h
	link_all "$I/ipc" "$HERE"/IOHIDFamily/compat/ipc/*.h
	# <IOKit/hidsystem/...> from IOHIDSystem/ and IOHIDSystem/IOKit/hidsystem
	link_all "$I/IOKit/hidsystem" "$S"/IOHIDSystem/*.h "$S"/IOHIDSystem/IOKit/hidsystem/*.h
	[ -d "$S/IOHIDSystem/IOKit/hid" ] && link_all "$I/IOKit/hid" "$S"/IOHIDSystem/IOKit/hid/*.h
	# HIDDriverKit: the framework's plain headers are IOHIDFamily's own (<HIDDriverKit/IOHIDDeviceTypes.h> ...);
	# the class headers + dispatch impls are generated from HIDDriverKit/*.iig (iig, Xcode 16.4), kernel side
	link_all "$I/HIDDriverKit" "$S"/IOHIDFamily/*.h "$HERE"/IOHIDFamily/missing-headers/*.h
	for iigf in "$S"/HIDDriverKit/*.iig; do
		iig_gen IOHIDFamily "$iigf" "$G/HIDDriverKit" HIDDriverKit driverkit
	done
	link_all "$I/HIDDriverKit" "$G"/HIDDriverKit/*.h
}

stage_IONetworkingFamily() {
	S=$FAM/IONetworkingFamily I=$FAM/_inc/IONetworkingFamily
	rm -rf "$I"
	common "$I"
	link_all "$I/IOKit/network" "$S"/*.h
}

[ $# -gt 0 ] || set -- IOStorageFamily IOHIDFamily IONetworkingFamily
for fam in "$@"; do
	echo "== $fam ($(tag_of "$fam"))"
	clone "$fam"
	patch_family "$fam"
	"stage_$fam"
	echo "  $(git -C "$FAM/$fam" rev-parse HEAD) staged into build/families/_inc/$fam"
done
