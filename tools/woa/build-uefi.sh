#!/usr/bin/env bash
# Build the exynos9810-woa EDK2 UEFI (Star2LtePkg, runs unchanged on starlte:
# NOTES.md W2) in the lima VM and pack it with the woa bootshim.
#   tools/woa/build-uefi.sh [DEBUG|RELEASE] [extra build.py args...]
# Source: ref/exynos9810-woa/firmware + tools/woa/patches/*.patch (copied to
# build/woa/fw; edk2 gets tools/woa/patches-edk2/*.patch on a clean checkout).
# edk2: ~/work/edk2 in lima at master 2938b830 (2026-06-19:
# after DefaultExceptionHandlerLib was merged, before GptLib), clang/lld
# CLANGPDB like the v0.1.0 release (DEBUG). CLANGDWARF misses the DSC's
# /ALIGN:0x1000 and hangs on the phone before drawing anything.
# Output: build/woa/Star2LtePkg.fd and build/woa/starbite-uefi.bin (bootshim +
# FD, for tools/make-xnu-boot.sh UEFI=).
set -euo pipefail
cd "$(dirname "$0")/../.."
target=${1:-DEBUG}
shift $(( $# > 0 ? 1 : 0 ))
src=build/woa/fw
rm -rf "$src"
mkdir -p build/woa
cp -R ref/exynos9810-woa/firmware "$src"
# PATCHES: glob of patches to apply (default all), e.g. PATCHES='tools/woa/patches/000[1-6]-*.patch'
for p in ${PATCHES:-tools/woa/patches/*.patch}; do
    patch -s -d "$src" -p1 < "$p"
done
limactl shell starbite -- bash -c "
    set -e
    cd ~/work/edk2
    [ \"\$(git rev-parse --short=8 HEAD)\" = 2938b830 ] || { echo 'edk2 is not at 2938b830'; exit 1; }
    git checkout -q -- .
    for p in '$PWD'/tools/woa/patches-edk2/*.patch; do [ -f \"\$p\" ] && git apply \"\$p\"; done
    export WORKSPACE=\$PWD PACKAGES_PATH=\$PWD:'$PWD/$src' CLANG_BIN=/usr/bin/
    source edksetup.sh >/dev/null
    rm -rf Build/Star2LtePkg
    build -a AARCH64 -t CLANGPDB -b $target -p Platform/Star2LtePkg/Star2LtePkg.dsc $* \
        -n \$(nproc) > /tmp/uefi-build.log 2>&1 ||
        { grep -n -E 'error|Error' /tmp/uefi-build.log | head -20; tail -15 /tmp/uefi-build.log; exit 1; }
    clang -target aarch64-none-elf -c '$PWD/$src/tools/bootshim/bootshim.S' -o /tmp/bootshim.o
    llvm-objcopy -O binary /tmp/bootshim.o /tmp/bootshim.bin
"
# The virtiofs mount is read-only from the guest (lima 2.x/vz), so copy the
# outputs out through the shell instead of writing to the mount.
rm -f build/woa/Star2LtePkg.fd build/woa/bootshim.bin build/woa/starbite-uefi.bin
limactl shell starbite -- bash -lc "cat ~/work/edk2/Build/Star2LtePkg/${target}_CLANGPDB/FV/STAR2LTE_UEFI.fd" </dev/null > build/woa/Star2LtePkg.fd
limactl shell starbite -- cat /tmp/bootshim.bin </dev/null > build/woa/bootshim.bin
[ "$(stat -f %z build/woa/Star2LtePkg.fd)" -gt 1000000 ] || { echo "FD copy failed" >&2; exit 1; }
[ "$(stat -f %z build/woa/bootshim.bin)" -gt 100 ] || { echo "bootshim copy failed" >&2; exit 1; }
cat build/woa/bootshim.bin build/woa/Star2LtePkg.fd > build/woa/starbite-uefi.bin
echo "starbite-uefi.bin: $(stat -f %z build/woa/starbite-uefi.bin) bytes, sha256 $(shasum -a 256 build/woa/starbite-uefi.bin | cut -c1-16)"
ls -l build/woa/Star2LtePkg.fd build/woa/starbite-uefi.bin
