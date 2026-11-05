#!/bin/bash
# Build Mesa 25.0.7 (Panfrost for the Mali-G72, softpipe as the software fallback) for ChromiumOS R158,
# against the image's own sysroot (glibc 2.41, libc++, libdrm 2.4.124), in the Lima VM.
#
#   limactl shell starbite -- bash /Users/dev/project-starbite/tools/chromeos/build-mesa.sh
#
# Inputs: ~/mesa/mesa-25.0.7 (clean release tree), ~/chromeos/sysroot (sysroot_chromeos-base_chromeos-chrome
# from the R158 arm64-generic build, unpacked). Output: ~/chromeos/mesa-root (DESTDIR install, prefix /usr,
# libdir lib64) and its file list. A cross build, so Mesa's build-time tools run against the VM's libraries
# while everything installed links against the image's.
# C++: the sysroot's libc++ 22 headers need clang >= 20 and the VM has clang 18, so Mesa's C++ (63 files) uses
# the VM's GCC 13 libstdc++, linked in statically and hidden (--exclude-libs): nothing new in the image, no
# clash with Chrome's own C++ runtime.
# Replaces the image's Mesa 21.3.9 (swrast/kms_swrast only): libEGL, libGLESv2, libglapi, dri/. No gbm:
# ChromiumOS's libgbm is minigbm and stays. No GLX/X11/Wayland: Chrome uses EGL surfaceless.
set -euo pipefail
SRC=$HOME/mesa/mesa-25.0.7
SR=$HOME/chromeos/sysroot
B=$HOME/mesa/build-cros
OUT=$HOME/chromeos/mesa-root
[ -f "$SRC/VERSION" ] && [ "$(cat "$SRC/VERSION")" = 25.0.7 ] || { echo "no Mesa 25.0.7 tree at $SRC"; exit 1; }
[ -f "$SR/lib64/libc.so.6" ] || { echo "no sysroot at $SR"; exit 1; }
mkdir -p "$B"
CROSS=$B/../cros-aarch64.ini
cat > "$CROSS" <<EOF
[binaries]
c = ['clang', '--target=aarch64-linux-gnu', '--sysroot=$SR']
cpp = ['clang++', '--target=aarch64-linux-gnu', '--sysroot=$SR', '-nostdinc++', '-isystem', '/usr/include/c++/13',
       '-isystem', '/usr/include/aarch64-linux-gnu/c++/13']
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_link_args = ['-fuse-ld=lld', '-L$SR/usr/lib64', '-L$SR/lib64']
cpp_link_args = ['-fuse-ld=lld', '-L$SR/usr/lib64', '-L$SR/lib64', '-L/usr/lib/gcc/aarch64-linux-gnu/13',
                 '-static-libstdc++', '-static-libgcc', '-Wl,--exclude-libs,libstdc++.a:libsupc++.a:libgcc.a:libgcc_eh.a']

[properties]
sys_root = '$SR'
pkg_config_libdir = '$SR/usr/lib64/pkgconfig:$SR/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a75'
endian = 'little'
EOF
OPTS=(--cross-file "$CROSS" --prefix=/usr --libdir=lib64 --buildtype=release -Db_ndebug=true
	-Dgallium-drivers=panfrost,softpipe -Dvulkan-drivers= -Dplatforms= -Degl=enabled -Dgbm=disabled
	-Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dglx=disabled -Dglvnd=disabled -Dllvm=disabled
	-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=enabled -Dexpat=enabled
	-Dbuild-tests=false -Dtools=)
if [ -f "$B/build.ninja" ]; then
	meson configure "$B" "${OPTS[@]:2}" >/dev/null
else
	meson setup "$B" "$SRC" "${OPTS[@]}"
fi
ninja -C "$B"
rm -rf "$OUT"
DESTDIR=$OUT ninja -C "$B" install >/dev/null
(cd "$OUT" && find . \( -type f -o -type l \) | sort) > "$OUT.files"
echo "--- installed ($(wc -l < "$OUT.files") files):"
cat "$OUT.files"
for f in "$OUT"/usr/lib64/libEGL.so.1 "$OUT"/usr/lib64/dri/panfrost_dri.so; do
	echo "--- $f"; readelf -d "$f" | grep -E 'NEEDED|RUNPATH|RPATH' || true
done
echo MESA_CROS_OK
