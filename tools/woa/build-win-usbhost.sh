#!/usr/bin/env bash
# Windows UEFI images with the USB host module (docs/WIN11-USB.md section 6).
#   tools/woa/build-win-usbhost.sh
# The images are today's Windows UEFI (src38) plus one module, nothing else:
#   - SEC/PEI, the bootshim and our PMIC RTC module are src38's own files (build/woa of the main
#     checkout: Star2LtePkg-src38.fd, pmic-rtc.ffs, the first 384 bytes of starbite-uefi-src38.bin).
#     A fresh edk2 build is not byte-reproducible (dates, times and BUILD_EPOCH move strings and
#     constants around), so it is used only for the new module;
#   - first the identity splice (src38's recipe: the release RTC killed, ours added) must give
#     src38.fd byte for byte, which proves the recipe and the tools;
#   - then, for VBUS policy 0 and 1, our edk2 build with patch 0024 enabled
#     (-D STAR2LTE_USB_HOST_DXE -D STARBITE_USBH_VBUS=n; patches 0001,0002,0005,0018-0024,
#     STAR2LTE_RAM_STAGE=3, DEBUG, as src38), the USB module extracted from it and added to the
#     identity recipe; dxe-diff.py checks the result is src38 + that one file.
# Outputs: build/woa/starbite-uefi-usbh-vbus{0,1}.bin (+ .fd, .ffs), build/woa/usbh-SHA256SUMS.
# The edk2 tree in the VM (~/work/edk2) is shared: don't run two UEFI builds at once.
set -euo pipefail
cd "$(dirname "$0")/../.."
MAIN=${STARBITE_MAIN:-/Users/dev/project-starbite}
W=$MAIN/build/woa
rel=$W/release-7582.fd
RTC=b336f62d-4135-4a55-ae4e-4971bbf0885d
USBH=52cc03fc-adc9-4192-9623-0ac86921f9a1
out=build/woa
for f in "$rel" "$W/Star2LtePkg-src38.fd" "$W/pmic-rtc.ffs" "$W/src38.fd" "$W/starbite-uefi-src38.bin"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
[ -d ref/exynos9810-woa ] || { echo "ref/exynos9810-woa missing (in a worktree: ln -s $MAIN/ref ref)" >&2; exit 1; }
if limactl shell starbite -- pgrep -f 'build -a AARCH64' >/dev/null; then
    echo "another edk2 build is running in the VM; try again later" >&2; exit 1
fi
starbite/s9usb/test/run.sh | tail -1
python3 tools/woa/patches/gen-0024.py --check
mkdir -p "$out"
head -c 384 "$W/starbite-uefi-src38.bin" > "$out/src38-bootshim.bin"

echo "== identity: src38's recipe from src38's files"
python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/usbh-identity.fd" \
    --kill "$RTC" --add "$W/pmic-rtc.ffs" >/dev/null
cmp "$out/usbh-identity.fd" "$W/src38.fd"
echo "identity: byte-identical to src38.fd"

export PATCHES='tools/woa/patches/000[125]-*.patch tools/woa/patches/001[89]-*.patch tools/woa/patches/002[0-4]-*.patch'
for v in 0 1; do
    echo "== USB host module, VBUS policy $v"
    tools/woa/build-uefi.sh DEBUG -D STAR2LTE_RAM_STAGE=3 -D STAR2LTE_USB_HOST_DXE=TRUE -D STARBITE_USBH_VBUS=$v | tail -1
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$USBH" "$out/usbh-vbus$v.ffs"
    python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/usbh-vbus$v.fd" \
        --kill "$RTC" --add "$W/pmic-rtc.ffs" --add "$out/usbh-vbus$v.ffs" >/dev/null
    python3 tools/woa/dxe-diff.py "$W/src38.fd" "$out/usbh-vbus$v.fd"
    grep -aq "usbh-policy vbus=$v" "$out/usbh-vbus$v.ffs" ||
        { echo "usbh-vbus$v.ffs lacks its policy marker: the VBUS define did not reach the module" >&2; exit 1; }
    cat "$out/src38-bootshim.bin" "$out/usbh-vbus$v.fd" > "$out/starbite-uefi-usbh-vbus$v.bin"
done
(cd "$out" && shasum -a 256 starbite-uefi-usbh-vbus0.bin starbite-uefi-usbh-vbus1.bin usbh-vbus0.ffs usbh-vbus1.ffs > usbh-SHA256SUMS && cat usbh-SHA256SUMS)
rm -rf tools/woa/__pycache__
