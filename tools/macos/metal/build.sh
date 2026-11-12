#!/bin/sh
# The Metal layer for macOS 26.5 on the Galaxy S9 (docs/MACOS-METAL.md), built on the Mac:
#   PanVK (Mesa 26.2.6's Vulkan driver for the Mali-G72, through starbite-drm -> StarbiteMali)
#   + the metal2vulkan bridge (Rust) + the Metal frontend (tools/metal2, IOS-26-Hot/UniXNU, MIT)
#   = StarbiteMetal.bundle for macOS (plain arm64 code, images marked arm64e: arm64e-tag.py), plus
#   sbmac.dylib  the injection (sweetinject=/usr/lib/sbmac.dylib,WindowServer,...): it publishes the
#                bundle's device with MTLAddDevice before WindowServer's 30 s Metal device check
#   sbmtltest    a command-line test: device, IOSurface, a Metal clear + triangle, readback, and the
#                frame swapped onto the panel through IOMobileFramebuffer
#
#   tools/macos/metal/build.sh [mount|bridge|panvk|bundle|shim|test|stage DIR|all]   (default: all)
#
# Big intermediates (Rust target ~1.5 GB, Mesa build ~200 MB) live in an APFS sparse image on the
# external drive, mounted at $WORK (an exFAT drive puts "._" files beside every file, which Cargo
# build scripts that hash src/ would pick up); WORK_DIR=/some/apfs/dir uses a plain directory instead.
# Products: $WORK/out, copied to out/macos-metal/ (git-ignored; a few MB). Nothing here writes outside
# the repo, the image or the scratch dirs.
# Env: MINOS (default 15.0, macOS Sequoia), ARM64E (default 1: the bundle's images are marked arm64e, so
# Metal's own discovery loads them into WindowServer, an arm64e process; ARM64E=0 keeps plain arm64 for
# the sbmac/sweetinject path, whose processes run with ptrauth_disabled=1).
set -eu
cd "$(dirname "$0")/../../.."
ROOT=$PWD
HERE=$ROOT/tools/macos/metal
IMAGE=${IMAGE:-/Volumes/Untitled/starbite-metal/sbmtl-build.sparseimage}
WORK=${WORK_DIR:-${WORK:-/Volumes/sbmtl-build}}
OUT=$WORK/out
DEST=$ROOT/out/macos-metal
JOBS=${JOBS:-4}
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
SDK=$(xcrun --sdk macosx --show-sdk-path)
MINOS=${MINOS:-15.0}
CC="xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=$MINOS"
export PATH=$HOME/.cargo/bin:/opt/homebrew/opt/rustup/bin:$PATH

log() { printf '%s\n' "build.sh: $*"; }

do_mount() {
	if [ -n "${WORK_DIR:-}" ]; then
		mkdir -p "$WORK"
	elif [ ! -d "$WORK" ] || ! mount | grep -q " on $WORK "; then
		[ -f "$IMAGE" ] || hdiutil create -size 24g -type SPARSE -fs APFS -volname sbmtl-build "$IMAGE"
		hdiutil attach -nobrowse -mountpoint "$WORK" "$IMAGE" >/dev/null
	fi
	mkdir -p "$OUT" "$DEST" "$WORK/tmp"
	export TMPDIR=$WORK/tmp	# rustc, clang and meson temp files: not on the Mac's disk
}

# --- metal2vulkan bridge for aarch64-apple-darwin -------------------------------------------------
# Sources: build/m2v/metal2vulkan (720ef1ce + tools/metal2/bridge/patches, as the iOS build) copied to
# $WORK/m2v-src without the 1.5 GB validation corpus (or, with no build/m2v, a sparse clone of
# github.com/steelbrain/metal2vulkan at 720ef1ce without the corpus, patched the same way), plus
# tools/macos/metal/patches/m2v-*.patch (a cache directory that cannot be written is no error with
# METAL2VULKAN_CACHE_OPTIONAL=1: the macOS root is read-only).
do_bridge() {
	do_mount
	SRC=$WORK/m2v-src
	if [ ! -f "$SRC/.starbite-macos" ]; then
		if [ -d "$ROOT/build/m2v/metal2vulkan" ]; then
			rsync -a --exclude .git --exclude target --exclude 'crates/metal2vulkan-validation/corpus' \
				"$ROOT/build/m2v/metal2vulkan/" "$SRC/"
		else
			rm -rf "$SRC"
			git clone -q --filter=blob:none --no-checkout https://github.com/steelbrain/metal2vulkan "$SRC"
			git -C "$SRC" sparse-checkout set --no-cone '/*' '!/crates/metal2vulkan-validation/corpus/'
			git -C "$SRC" checkout -q 720ef1ce
			for p in "$ROOT"/tools/metal2/bridge/patches/*.patch; do git -C "$SRC" apply "$p"; done
		fi
		for p in "$HERE"/patches/m2v-*.patch; do patch -s -p1 -d "$SRC" < "$p"; done
		date > "$SRC/.starbite-macos"
	fi
	B=$WORK/m2v-bridge
	mkdir -p "$B"
	/opt/homebrew/opt/llvm/bin/llvm-as -o "$B/runtime_examples.air" \
		"$SRC/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
	/opt/homebrew/opt/llvm/bin/llvm-as -o "$B/function_constant.air" \
		"$SRC/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
	cat > "$B/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$ROOT/tools/metal2/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$SRC/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$SRC/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOF
	[ -f "$B/Cargo.lock" ] || [ ! -f "$ROOT/build/m2v-bridge/Cargo.lock" ] || cp "$ROOT/build/m2v-bridge/Cargo.lock" "$B/Cargo.lock"
	log "bridge: cargo build (aarch64-apple-darwin, $JOBS jobs; log $B/build-macos.log)"
	NMB_FIXTURE_AIR=$B/runtime_examples.air NMB_FC_AIR=$B/function_constant.air MACOSX_DEPLOYMENT_TARGET=$MINOS \
		cargo +1.98.0 build --release -j "$JOBS" --manifest-path "$B/Cargo.toml" --target aarch64-apple-darwin \
		--target-dir "$WORK/m2v-target" > "$B/build-macos.log" 2>&1 ||
		{ grep -E "^error" -A8 "$B/build-macos.log" | head -60; exit 1; }
	cp "$WORK/m2v-target/aarch64-apple-darwin/release/libnative_metal_bridge.a" "$OUT/"
	ls -la "$OUT/libnative_metal_bridge.a"
}

# --- PanVK (Mesa 26.2.6) for macOS -----------------------------------------------------------------
# The source tree is build/mesa-ios/mesa-26.2.6, or $MESA_DIR/mesa-26.2.6 (already carries tools/mesa-ios/mesa26-panvk-ios.patch,
# whose changes are all #ifdef __APPLE__, and src/starbite-drm); the host tools (mesa_clc,
# vtn_bindgen2, panfrost_compile) are build/mesa-ios/build-clc-host's (tools/mesa-ios/mk-panvk26-ios.sh).
do_panvk() {
	do_mount
	M=${MESA_DIR:-$ROOT/build/mesa-ios}	# MESA_DIR: a copy elsewhere (build/mesa-ios is parked off the Mac)
	[ -d "$M/mesa-26.2.6/src/starbite-drm" ] || { log "run tools/mesa-ios/mk-panvk26-ios.sh once first"; exit 1; }
	# starbite-drm may have changed since the iOS build copied it in
	cmp -s "$ROOT/tools/mesa-ios/starbite-drm/starbite_drm.c" "$M/mesa-26.2.6/src/starbite-drm/starbite_drm.c" ||
		{ log "src/starbite-drm differs from tools/mesa-ios/starbite-drm: rerun tools/mesa-ios/mk-panvk26-ios.sh"; exit 1; }
	TC=$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin
	cat > "$WORK/cross-macos.ini" <<EOF
[binaries]
c = ['$TC/clang', '-arch', 'arm64', '-isysroot', '$SDK', '-mmacosx-version-min=$MINOS']
cpp = ['$TC/clang++', '-arch', 'arm64', '-isysroot', '$SDK', '-mmacosx-version-min=$MINOS']
objc = ['$TC/clang', '-arch', 'arm64', '-isysroot', '$SDK', '-mmacosx-version-min=$MINOS']
ar = '$TC/ar'
ranlib = '$TC/ranlib'
strip = '$TC/strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'macos'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
# Chained fixups: Mesa links with -undefined dynamic_lookup (its weak panvk_* entry points), and then ld writes
# old-style bind opcodes, which dyld refuses in an arm64e image ("old arm64e bind opcodes not supported"; S2g
# 2026-11-12: WindowServer's dlopen of this dylib failed, LoaderNotFound). -fixup_chains keeps them as chained
# flat-namespace weak imports.
c_link_args = ['-Wl,-fixup_chains']
cpp_link_args = ['-Wl,-fixup_chains']
EOF
	COMMON="-Dplatforms= -Dgallium-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled
		-Dgles2=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled
		-Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dbuild-tests=false"
	(
		export PATH=$M/build-clc-host/src/compiler/clc:$M/build-clc-host/src/compiler/spirv:$M/build-clc-host/src/panfrost/clc:/opt/homebrew/opt/bison/bin:$M/venv/bin:$PATH
		BD=$WORK/build-vk26-macos
		# shellcheck disable=SC2086
		[ -f "$BD/build.ninja" ] || meson setup "$BD" "$M/mesa-26.2.6" --cross-file "$WORK/cross-macos.ini" \
			--wrap-mode=nofallback --prefix=/usr/local/starbite --libdir=lib --buildtype=release -Db_ndebug=true \
			$COMMON -Dvulkan-drivers=panfrost -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
			-Dshader-cache=disabled -Dlmsensors=disabled > "$WORK/meson-vk26-macos.log" 2>&1 ||
			{ tail -30 "$WORK/meson-vk26-macos.log"; exit 1; }
		log "panvk: ninja (-j $JOBS; log $WORK/ninja-vk26-macos.log)"
		ninja -C "$BD" -j "$JOBS" > "$WORK/ninja-vk26-macos.log" 2>&1 || { grep -E "error|FAILED" "$WORK/ninja-vk26-macos.log" | head -40; exit 1; }
		cp "$BD/src/panfrost/vulkan/libvulkan_panfrost.dylib" "$OUT/libvulkan_panfrost.dylib"
	)
	ls -la "$OUT/libvulkan_panfrost.dylib"
}

# --- StarbiteMetal.bundle (macOS layout: Contents/MacOS, Contents/Info.plist) ----------------------
do_bundle() {
	do_mount
	BR=$OUT/libnative_metal_bridge.a
	[ -f "$BR" ] && [ -f "$OUT/libvulkan_panfrost.dylib" ] || { log "bundle: run the bridge and panvk stages first"; exit 1; }
	B=$OUT/StarbiteMetal.bundle
	rm -rf "$B" && mkdir -p "$B/Contents/MacOS" "$B/Contents/Frameworks"
	# -no_function_starts (2026-11-12): fixes go onto the card in place (card-file-replace.py), which needs the file at
	# exactly the card copy's size; without this ~38 KB table (not used at run time) a rebuilt binary is smaller and is
	# padded with zeros after its signature, as the PanVK dylib is.
	# shellcheck disable=SC2086
	$CC -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall -Wno-deprecated-declarations -Wno-unused-function \
		-I"$ROOT/tools/metal2" -bundle -o "$B/Contents/MacOS/StarbiteMetal" \
		"$HERE/StarbiteMetalMac.mm" "$ROOT/tools/metal2/NativeBridgeMetalFrontend.mm" -x none "$BR" \
		-Wl,-dead_strip -Wl,-no_function_starts -Wl,-exported_symbol,_native_bridge_metal_open -Wl,-exported_symbol,_sbmtl_device \
		-Wl,-exported_symbol,_sbmtl_status -Wl,-exported_symbol,'_OBJC_CLASS_$_StarbiteMTLDevice' \
		-Wl,-exported_symbol,'_OBJC_METACLASS_$_StarbiteMTLDevice' \
		-lc++ -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation
	mkdir -p "$OUT/sym" && cp "$B/Contents/MacOS/StarbiteMetal" "$OUT/sym/StarbiteMetal"
	rm -rf "$OUT/sym/StarbiteMetal.dSYM" && mv "$B/Contents/MacOS/StarbiteMetal.dSYM" "$OUT/sym/" 2>/dev/null || true
	cp "$OUT/libvulkan_panfrost.dylib" "$OUT/sym/libvulkan_panfrost.dylib"
	xcrun strip -x "$B/Contents/MacOS/StarbiteMetal"
	cp "$HERE/Info.plist" "$B/Contents/Info.plist"
	cp "$OUT/libvulkan_panfrost.dylib" "$B/Contents/Frameworks/libvulkan_panfrost.dylib"
	install_name_tool -id @loader_path/libvulkan_panfrost.dylib "$B/Contents/Frameworks/libvulkan_panfrost.dylib" 2>/dev/null
	xcrun strip -x "$B/Contents/Frameworks/libvulkan_panfrost.dylib"
	[ "${ARM64E:-1}" = 0 ] || python3 -I "$HERE/arm64e-tag.py" "$B/Contents/MacOS/StarbiteMetal" \
		"$B/Contents/Frameworks/libvulkan_panfrost.dylib"
	codesign -f -s - "$B/Contents/Frameworks/libvulkan_panfrost.dylib" 2>/dev/null
	codesign -f -s - "$B" 2>/dev/null
	log "bundle: $B"
}

# --- sbmac.dylib: the WindowServer (and other clients') injection ----------------------------------
do_shim() {
	do_mount
	# shellcheck disable=SC2086
	$CC -x objective-c -fobjc-arc -O2 -g -Wall -dynamiclib -install_name /usr/lib/sbmac.dylib -Wl,-not_for_dyld_shared_cache -Wl,-no_warn_inits \
		-o "$OUT/sbmac.dylib" "$HERE/sbmac.m" -framework Foundation -framework Metal -framework IOKit -framework ColorSync \
		-framework CoreFoundation
	mkdir -p "$OUT/sym" && cp "$OUT/sbmac.dylib" "$OUT/sym/sbmac.dylib"
	rm -rf "$OUT/sym/sbmac.dylib.dSYM" && mv "$OUT/sbmac.dylib.dSYM" "$OUT/sym/" 2>/dev/null || true
	xcrun strip -x "$OUT/sbmac.dylib"
	codesign -f -s - "$OUT/sbmac.dylib"
	log "shim: $OUT/sbmac.dylib"
}

# --- sbmtltest ---------------------------------------------------------------------------------------
do_test() {
	do_mount
	# shellcheck disable=SC2086
	$CC -x objective-c -fobjc-arc -O2 -g -Wall -o "$OUT/sbmtltest" "$HERE/sbmtltest.m" \
		-Wl,-sectcreate,__TEXT,__info_plist,"$HERE/sbmtltest-Info.plist" \
		-framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation
	mkdir -p "$OUT/sym" && cp "$OUT/sbmtltest" "$OUT/sym/sbmtltest"
	rm -rf "$OUT/sym/sbmtltest.dSYM" && mv "$OUT/sbmtltest.dSYM" "$OUT/sym/" 2>/dev/null || true
	xcrun strip -x "$OUT/sbmtltest"
	codesign -f -s - "$OUT/sbmtltest"
	log "test: $OUT/sbmtltest"
}

# --- copy the products to out/macos-metal and list their hashes --------------------------------------
do_dest() {
	mkdir -p "$DEST"
	rm -rf "$DEST/StarbiteMetal.bundle"
	for f in StarbiteMetal.bundle sbmac.dylib sbmtltest; do
		[ -e "$OUT/$f" ] && cp -R "$OUT/$f" "$DEST/"
	done
	cp "$HERE/com.starbite.sbmtltest.plist" "$HERE/mtl2-clients" "$DEST/" 2>/dev/null || true
	(cd "$DEST" && find . -type f ! -name SHA256SUMS ! -name '._*' | sort | xargs shasum -a 256 > SHA256SUMS && cat SHA256SUMS)
}

# --- stage the products into a macOS root directory (the root builder's tree, never this Mac) ---------
# tools/macos/metal/build.sh stage /path/to/root
do_stage() {
	R=$1
	[ -d "$R/System/Library" ] || { log "stage: $R is not a macOS root"; exit 1; }
	mkdir -p "$R/System/Library/Extensions" "$R/usr/lib" "$R/usr/local/bin" "$R/private/var/local/m2v-cache" \
		"$R/Library/LaunchDaemons"
	rm -rf "$R/System/Library/Extensions/StarbiteMetal.bundle"
	cp -R "$DEST/StarbiteMetal.bundle" "$R/System/Library/Extensions/"
	cp "$DEST/sbmac.dylib" "$R/usr/lib/sbmac.dylib"
	cp "$DEST/sbmtltest" "$R/usr/local/bin/sbmtltest"
	cp "$DEST/mtl2-clients" "$R/private/var/local/mtl2-clients"
	cp "$DEST/com.starbite.sbmtltest.plist" "$R/Library/LaunchDaemons/"
	chmod 0777 "$R/private/var/local" "$R/private/var/local/m2v-cache"
	log "staged into $R (owners: root:wheel for the bundle, the dylib, the plist; hfs-owners.py applies them)"
}

stage=${1:-all}
case $stage in
mount) do_mount ;;
bridge) do_bridge ;;
panvk) do_panvk ;;
bundle) do_bundle ;;
shim) do_shim ;;
test) do_test ;;
dest) do_dest ;;
stage) do_stage "${2:?root directory}" ;;
all) do_bridge && do_panvk && do_bundle && do_shim && do_test && do_dest ;;
*) echo "usage: $0 [mount|bridge|panvk|bundle|shim|test|dest|stage DIR|all]" >&2; exit 2 ;;
esac
