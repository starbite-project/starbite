#!/usr/bin/env bash
# Windows UEFI images with USB (Star2LteUsbDxe) and the shared SSDT installer (Star2LteSsdtDxe);
# docs/WIN11-USB.md sections 4.1 and 6.
#   tools/woa/build-win-usbhost.sh                    the three USB images below
#   EXTRA_AML="a.aml b.aml" SUFFIX=-gpu tools/woa/build-win-usbhost.sh
#                                                     + other tracks' blobs in every image, names
#                                                       starbite-uefi-<name><SUFFIX>.bin
# Every image is today's Windows UEFI (src38) plus added files only:
#   - SEC/PEI, the bootshim and our PMIC RTC module are src38's own files (build/woa of the main
#     checkout: Star2LtePkg-src38.fd, pmic-rtc.ffs, the first 384 bytes of starbite-uefi-src38.bin).
#     A fresh edk2 build is not byte-reproducible (dates, times and BUILD_EPOCH move strings and
#     constants around), so it is used only for the new modules;
#   - first the identity splice (src38's recipe: the release RTC killed, ours added) must give
#     src38.fd byte for byte, which proves the recipe and the tools;
#   - blobs: tools/woa/acpi/*.asl -> iasl (VM) -> tools/woa/ssdt-ffs.py, plus EXTRA_AML;
#   - per image, our edk2 build with patch 0024 enabled (patches 0001,0002,0005,0018-0024,
#     STAR2LTE_RAM_STAGE=3, DEBUG, as src38) and that image's -D policy; both modules extracted
#     and added with the blobs; dxe-diff.py checks the result is src38 + exactly those files, and
#     a per-image marker string proves the policy reached the module.
# Images (build/woa/starbite-uefi-<name>.bin, sha256s in build/woa/usbh-SHA256SUMS):
#   usbh-vbus0   role host, layout A (SBUSBXHC), VBUS off     -> test T1
#   usbh-vbus1   role host, layout A (SBUSBXHC), MCU VBUS on  -> tests T2-T5
#   usb-urs-auto role auto, layout B (SBUSBURS), VBUS off     -> URS experiments (section 5)
#   usb-ufn-device role device, layout C (SBUSBUFN), VBUS off -> L1 without URS (section 5)
# The edk2 tree in the VM (~/work/edk2) is shared: don't run two UEFI builds at once.
set -euo pipefail
cd "$(dirname "$0")/../.."
MAIN=${STARBITE_MAIN:-/Users/dev/project-starbite}
W=$MAIN/build/woa
rel=$W/release-7582.fd
RTC=b336f62d-4135-4a55-ae4e-4971bbf0885d
USBDXE=93eb628b-5eff-4ae2-834d-cf7aaf7bdc7c
SSDTDXE=5dd14183-80d0-4d05-938e-f74bc42e4e6a
PPTT=${PPTT:-0}                                   # 1: Star2LteSsdtDxe also fixes the PPTT (section 10)
# Our modules' edk2 target. RELEASE drops their DEBUG (serial-only) output; their log goes to the
# RAM rings either way (StarbiteLog.h). Needed when the volume is full: the release's outer FV has
# a fixed slot for the compressed DXE volume.
UEFI_TARGET=${UEFI_TARGET:-DEBUG}
out=build/woa
SUFFIX=${SUFFIX:-}
[ -z "${EXTRA_AML:-}" ] || [ -n "$SUFFIX" ] || { echo "EXTRA_AML needs a SUFFIX so the plain images are kept" >&2; exit 1; }
[ "$PPTT" = 0 ] || [ -n "$SUFFIX" ] || { echo "PPTT=1 needs a SUFFIX so the plain images are kept" >&2; exit 1; }
for f in "$rel" "$W/Star2LtePkg-src38.fd" "$W/pmic-rtc.ffs" "$W/src38.fd" "$W/starbite-uefi-src38.bin"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
[ -d ref/exynos9810-woa ] || { echo "ref/exynos9810-woa missing (in a worktree: ln -s $MAIN/ref ref)" >&2; exit 1; }
if limactl shell starbite -- pgrep -f 'build -a AARCH64' >/dev/null; then
    echo "another edk2 build is running in the VM; try again later" >&2; exit 1
fi
starbite/s9usb/test/run.sh | tail -1
tools/woa/modules/Star2LteSsdtDxe/test/run.sh | head -1
python3 tools/woa/patches/gen-0024.py --check
mkdir -p "$out/acpi"
head -c 384 "$W/starbite-uefi-src38.bin" > "$out/src38-bootshim.bin"

echo "== blobs"
for asl in tools/woa/acpi/*.asl; do
    n=$(basename "$asl" .asl)
    limactl shell starbite -- bash -c 'set -e; d=$(mktemp -d); cd "$d"; cat > t.asl; iasl -p t t.asl >log 2>&1 || { cat log >&2; exit 1; }; cat t.aml; rm -rf "$d"' \
        < "$asl" > "$out/acpi/$n.aml"
    python3 -I tools/woa/ssdt-ffs.py --gated "$out/acpi/$n.aml" "$out/acpi/$n.ffs"   # USB blobs fail closed
done
extra=()
for aml in ${EXTRA_AML:-}; do
    n=$(basename "$aml" .aml)
    python3 -I tools/woa/ssdt-ffs.py "$aml" "$out/acpi/extra-$n.ffs"
    extra+=(--add "$out/acpi/extra-$n.ffs")
done
# the enable GUIDs compiled into UsbDxe.c must be the gated blobs' GUIDs
[ "$(python3 -I tools/woa/ssdt-ffs.py --gated --guid "$out/acpi/UsbXhc.aml")" = 53534454-5342-4754-5342-555342584843 ] &&
[ "$(python3 -I tools/woa/ssdt-ffs.py --gated --guid "$out/acpi/UsbUrs.aml")" = 53534454-5342-4754-5342-555342555253 ] &&
[ "$(python3 -I tools/woa/ssdt-ffs.py --gated --guid "$out/acpi/UsbUfn.aml")" = 53534454-5342-4754-5342-55534255464e ] ||
    { echo "a USB blob's GUID differs from the enable GUIDs in UsbDxe.c" >&2; exit 1; }

echo "== identity: src38's recipe from src38's files"
python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/usbh-identity.fd" \
    --kill "$RTC" --add "$W/pmic-rtc.ffs" >/dev/null
cmp "$out/usbh-identity.fd" "$W/src38.fd"
echo "identity: byte-identical to src38.fd"

export PATCHES='tools/woa/patches/000[125]-*.patch tools/woa/patches/001[89]-*.patch tools/woa/patches/002[0-4]-*.patch'
image() {   # image NAME ROLE ACPI VBUS BLOB
    local name=$1 role=$2 acpi=$3 vbus=$4 blob=$5
    echo "== $name: role $role, ACPI layout $acpi, VBUS $vbus, blob $blob"
    tools/woa/build-uefi.sh "$UEFI_TARGET" -D STAR2LTE_RAM_STAGE=3 -D STAR2LTE_USB_DXE=TRUE -D STAR2LTE_SSDT_DXE=TRUE \
        -D STARBITE_USB_ROLE=$role -D STARBITE_USB_ACPI=$acpi -D STARBITE_USB_VBUS=$vbus -D STARBITE_PPTT_FIX=$PPTT | tail -1
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$USBDXE" "$out/$name-usb.ffs"
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$SSDTDXE" "$out/$name-ssdt.ffs"
    grep -aq "usb-policy role=$role acpi=$acpi vbus=$vbus" "$out/$name-usb.ffs" ||
        { echo "$name-usb.ffs lacks its policy marker: the -D values did not reach the module" >&2; exit 1; }
    # PpttFix is linked in only with STARBITE_PPTT_FIX=1 (LTO drops it otherwise)
    if grep -aq "=PPTT v%d" "$out/$name-ssdt.ffs"; then has=1; else has=0; fi
    [ "$has" = "$PPTT" ] || { echo "$name-ssdt.ffs: PPTT fix present=$has, wanted $PPTT" >&2; exit 1; }
    python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/$name.fd" \
        --kill "$RTC" --add "$W/pmic-rtc.ffs" --add "$out/$name-usb.ffs" --add "$out/$name-ssdt.ffs" \
        --add "$out/acpi/$blob.ffs" ${extra[@]+"${extra[@]}"} >/dev/null
    python3 tools/woa/dxe-diff.py "$W/src38.fd" "$out/$name.fd"
    cat "$out/src38-bootshim.bin" "$out/$name.fd" > "$out/starbite-uefi-$name$SUFFIX.bin"
}
if [ "$PPTT" = 1 ]; then
    # The PPTT experiment alone: src38 + Star2LteSsdtDxe with the PPTT fix, no USB module and no
    # blobs (the 0xC0000094 crashes happen on plain src38). The USB images with the fix don't fit
    # the release's DXE slot yet (section 10).
    echo "== pptt-only: Star2LteSsdtDxe with the PPTT fix, nothing else"
    tools/woa/build-uefi.sh "$UEFI_TARGET" -D STAR2LTE_RAM_STAGE=3 -D STAR2LTE_SSDT_DXE=TRUE -D STARBITE_PPTT_FIX=1 | tail -1
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$SSDTDXE" "$out/pptt-only-ssdt.ffs"
    grep -aq "=PPTT v%d" "$out/pptt-only-ssdt.ffs" || { echo "pptt-only-ssdt.ffs lacks the PPTT fix" >&2; exit 1; }
    python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/pptt-only.fd" \
        --kill "$RTC" --add "$W/pmic-rtc.ffs" --add "$out/pptt-only-ssdt.ffs" >/dev/null
    python3 tools/woa/dxe-diff.py "$W/src38.fd" "$out/pptt-only.fd"
    cat "$out/src38-bootshim.bin" "$out/pptt-only.fd" > "$out/starbite-uefi-pptt-only$SUFFIX.bin"
    (cd "$out" && shasum -a 256 "starbite-uefi-pptt-only$SUFFIX.bin" | tee "usbh-SHA256SUMS$SUFFIX")
    rm -rf tools/woa/__pycache__
    exit 0
fi
image usbh-vbus0 0 0 0 UsbXhc
image usbh-vbus1 0 0 1 UsbXhc
image usb-urs-auto 2 1 0 UsbUrs
image usb-ufn-device 1 2 0 UsbUfn
(cd "$out" && shasum -a 256 starbite-uefi-usbh-vbus0$SUFFIX.bin starbite-uefi-usbh-vbus1$SUFFIX.bin \
    starbite-uefi-usb-urs-auto$SUFFIX.bin starbite-uefi-usb-ufn-device$SUFFIX.bin \
    acpi/*.aml ${EXTRA_AML:-} > usbh-SHA256SUMS$SUFFIX && cat usbh-SHA256SUMS$SUFFIX)
rm -rf tools/woa/__pycache__
