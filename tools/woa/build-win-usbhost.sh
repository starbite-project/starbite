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
PPTT=${PPTT:-0}                                   # 1: Star2LteSsdtDxe also fixes the PPTT at run time (section 10;
                                                  #    the release has no SDT protocol, so on the phone it only logs the caches)
PPTT_STATIC=${PPTT_STATIC:-0}                     # 1: the release's platform ACPI file with the cache-augmented PPTT
                                                  #    (tools/woa/pptt-patch.py, build time; section 10)
ACPIFILE=11111111-2222-3333-4444-555555555501
M3=${M3:-0}                                       # 1: Star2LteSsdtDxe also runs the M3 probe (WIN11-BIGCORES B1)
M3PARK=${M3PARK:-0}                               # 1: Star2LteSsdtDxe parks M3 cores 1..3 for the OS (B2)
M3RUN=${M3RUN:-0}                                 # 1: each M3 runs a test in the UEFI context (B3a)
M3BOOT=${M3BOOT:-0}                               # 1: M3 core 1 becomes the boot CPU at ExitBootServices (B3,
                                                  #    with A55 1..3); 2: the same, M3 1 alone ("solo");
                                                  #    3: "big", with CORES8: M3 1 boots, the parked M3s are its
                                                  #    secondaries, no A55 (CORES8=1: 3 M3s, =2: all 4)
CORES8=${CORES8:-0}                               # 1: M3 cores 1..3 parked for Windows + MADT/_MAT at EBS (7 CPUs);
                                                  #    2: also M3 core 0 (8 CPUs); docs/WIN11-24H2-A55.md
PPTT_OEM=${PPTT_OEM:-0}                           # 1: the platform ACPI file with the OEM-shaped PPTT (tools/woa/pptt-gen.py)
SOLO=${SOLO:-}                                    # name: only the isolated image src38 + Star2LteSsdtDxe
                                                  # (with the PPTT/M3 flags above), no USB, no blobs
KILL_SERIAL=${KILL_SERIAL:-0}                     # 1: drop the release's SerialDxe (room in the DXE slot)
SERIAL=9a5163e7-5c29-453f-825c-837a46a81e15
ONLY=${ONLY:-}                                    # build only this image name (e.g. usb-ufn-device)
# Our modules' edk2 target. RELEASE drops their DEBUG (serial-only) output; their log goes to the
# RAM rings either way (StarbiteLog.h). Needed when the volume is full: the release's outer FV has
# a fixed slot for the compressed DXE volume.
UEFI_TARGET=${UEFI_TARGET:-DEBUG}
out=build/woa
SUFFIX=${SUFFIX:-}
[ -z "${EXTRA_AML:-}" ] || [ -n "$SUFFIX" ] || { echo "EXTRA_AML needs a SUFFIX so the plain images are kept" >&2; exit 1; }
[ "$PPTT" = 0 ] || [ -n "$SUFFIX" ] || { echo "PPTT=1 needs a SUFFIX so the plain images are kept" >&2; exit 1; }
if [ "$M3BOOT" = 3 ]; then
    # big cores (docs/WIN11-24H2-A55.md): the B3 hand-off to M3 1 plus Cores8's parked M3s, nothing else
    [ "$CORES8" -ge 1 ] && [ $((M3 + M3PARK + M3RUN)) = 0 ] || { echo "M3BOOT=3 needs CORES8=1|2 and no other M3 experiment" >&2; exit 1; }
else
    [ $((M3 + M3PARK + M3RUN + (M3BOOT > 0) + (CORES8 > 0))) -le 1 ] || { echo "one M3 experiment per image (M3, M3PARK, M3RUN, M3BOOT, CORES8)" >&2; exit 1; }
fi
[ $((PPTT_STATIC + PPTT_OEM)) -le 1 ] || { echo "PPTT_STATIC and PPTT_OEM exclude each other" >&2; exit 1; }
[ "$PPTT_STATIC" = 0 ] || [ -n "$SUFFIX" ] || { echo "PPTT_STATIC=1 needs a SUFFIX so the plain images are kept" >&2; exit 1; }
for f in "$rel" "$W/Star2LtePkg-src38.fd" "$W/pmic-rtc.ffs" "$W/src38.fd" "$W/starbite-uefi-src38.bin"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
[ -d ref/exynos9810-woa ] || { echo "ref/exynos9810-woa missing (in a worktree: ln -s $MAIN/ref ref)" >&2; exit 1; }
if limactl shell starbite -- pgrep -f 'build -a AARCH64' >/dev/null; then
    echo "another edk2 build is running in the VM; try again later" >&2; exit 1
fi
starbite/s9usb/test/run.sh | tail -1
tools/woa/modules/Star2LteSsdtDxe/test/run.sh | grep -E "checks|FAIL"
python3 tools/woa/patches/gen-0024.py --check
python3 tools/woa/modules/Star2LteSsdtDxe/gen-stubs.py --check
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
    [ -z "$ONLY" ] || [ "$ONLY" = "$name" ] || return 0
    echo "== $name: role $role, ACPI layout $acpi, VBUS $vbus, blob $blob"
    tools/woa/build-uefi.sh "$UEFI_TARGET" -D STAR2LTE_RAM_STAGE=3 -D STAR2LTE_USB_DXE=TRUE -D STAR2LTE_SSDT_DXE=TRUE \
        -D STARBITE_USB_ROLE=$role -D STARBITE_USB_ACPI=$acpi -D STARBITE_USB_VBUS=$vbus -D STARBITE_PPTT_FIX=$PPTT \
        -D STARBITE_M3_PROBE=$M3 -D STARBITE_M3_PARK=$M3PARK -D STARBITE_M3_RUN=$M3RUN -D STARBITE_M3_BOOT=$M3BOOT \
        -D STARBITE_CORES8=$CORES8 | tail -1
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$USBDXE" "$out/$name-usb.ffs"
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$SSDTDXE" "$out/$name-ssdt.ffs"
    grep -aq "usb-policy role=$role acpi=$acpi vbus=$vbus" "$out/$name-usb.ffs" ||
        { echo "$name-usb.ffs lacks its policy marker: the -D values did not reach the module" >&2; exit 1; }
    flags_ok "$out/$name-ssdt.ffs"
    python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/$name.fd" \
        --kill "$RTC" ${serial[@]+"${serial[@]}"} --add "$W/pmic-rtc.ffs" --add "$out/$name-usb.ffs" --add "$out/$name-ssdt.ffs" \
        --add "$out/acpi/$blob.ffs" ${extra[@]+"${extra[@]}"} >/dev/null
    python3 tools/woa/dxe-diff.py "$W/src38.fd" "$out/$name.fd" ${unserial[@]+"${unserial[@]}"}
    cat "$out/src38-bootshim.bin" "$out/$name.fd" > "$out/starbite-uefi-$name$SUFFIX.bin"
}
serial=() unserial=()
[ "$KILL_SERIAL" = 0 ] || { serial=(--kill-blank "$SERIAL"); unserial=(--removed "$SERIAL"); }
if [ "$PPTT_STATIC" = 1 ]; then
    # the release's table file with the new PPTT replaces the old one (killed to a blank pad file,
    # which the DXE core's by-name lookups skip); dxe-diff accepts exactly that file as changed
    python3 tools/woa/dxe-replace.py "$rel" "$rel" /dev/null --extract "$ACPIFILE" "$out/acpi/release-tables.ffs" >/dev/null
    python3 -I tools/woa/pptt-patch.py "$out/acpi/release-tables.ffs" "$out/acpi/release-tables-pptt.ffs" --dump "$out/acpi/pptt-s9.aml"
    serial+=(--kill-blank "$ACPIFILE" --add "$out/acpi/release-tables-pptt.ffs"); unserial+=(--replaced "$ACPIFILE")
fi
if [ "$PPTT_OEM" = 1 ]; then
    # the OEM-shaped PPTT (docs/WIN11-24H2-A55.md): rev 1, placeholder caches per core type, A55 UIDs 0..3, M3 4..7
    python3 tools/woa/dxe-replace.py "$rel" "$rel" /dev/null --extract "$ACPIFILE" "$out/acpi/release-tables.ffs" >/dev/null
    python3 -I tools/woa/pptt-gen.py "$out/acpi/pptt-oem.aml"
    python3 -I tools/woa/pptt-patch.py "$out/acpi/release-tables.ffs" "$out/acpi/release-tables-ppttoem.ffs" --table "$out/acpi/pptt-oem.aml"
    serial+=(--kill-blank "$ACPIFILE" --add "$out/acpi/release-tables-ppttoem.ffs"); unserial+=(--replaced "$ACPIFILE")
fi
# the M3 probe / PPTT fix code is linked in only with its flag (LTO drops it otherwise)
flags_ok() {   # flags_ok FFS
    local has
    if grep -aq "=PPTT v%d" "$1"; then has=1; else has=0; fi
    [ "$has" = "$PPTT" ] || { echo "$1: PPTT fix present=$has, wanted $PPTT" >&2; exit 1; }
    if grep -aq "=M3 probe v1" "$1"; then has=1; else has=0; fi
    [ "$has" = "$M3" ] || { echo "$1: M3 probe present=$has, wanted $M3" >&2; exit 1; }
    if grep -aq "=M3 park v2" "$1"; then has=1; else has=0; fi
    [ "$has" = "$M3PARK" ] || { echo "$1: M3 park present=$has, wanted $M3PARK" >&2; exit 1; }
    if grep -aq "=M3 run v2" "$1"; then has=1; else has=0; fi
    [ "$has" = "$M3RUN" ] || { echo "$1: M3 run present=$has, wanted $M3RUN" >&2; exit 1; }
    if grep -aq "=M3 boot v4 armed" "$1"; then has=1; else has=0; fi
    [ "$has" = "$((M3BOOT > 0))" ] || { echo "$1: M3 boot present=$has, wanted $M3BOOT" >&2; exit 1; }
    if [ "$M3BOOT" = 2 ]; then grep -aq "solo: M3 1 only" "$1" || { echo "$1: not the solo build" >&2; exit 1; }; fi
    if [ "$M3BOOT" = 3 ]; then grep -aq "big: M3 1 + parked M3s" "$1" && grep -aq "=C8 v1 big" "$1" || { echo "$1: not the big-core build" >&2; exit 1; }; fi
    if grep -aq "=C8 v1" "$1"; then has=1; else has=0; fi
    [ "$has" = "$((CORES8 > 0))" ] || { echo "$1: cores8 present=$has, wanted $CORES8" >&2; exit 1; }
}
if [ -n "$SOLO" ]; then
    # An experiment alone: src38 + Star2LteSsdtDxe with the chosen flags, no USB module; only the
    # EXTRA_AML blobs (e.g. SBGPU, so Windows keeps its display driver on these boots).
    echo "== $SOLO: Star2LteSsdtDxe alone (PPTT fix $PPTT, M3 probe $M3, M3 park $M3PARK, M3 run $M3RUN, M3 boot $M3BOOT, cores8 $CORES8, PPTT OEM $PPTT_OEM)"
    tools/woa/build-uefi.sh "$UEFI_TARGET" -D STAR2LTE_RAM_STAGE=3 -D STAR2LTE_SSDT_DXE=TRUE \
        -D STARBITE_PPTT_FIX=$PPTT -D STARBITE_M3_PROBE=$M3 -D STARBITE_M3_PARK=$M3PARK -D STARBITE_M3_RUN=$M3RUN -D STARBITE_M3_BOOT=$M3BOOT -D STARBITE_CORES8=$CORES8 | tail -1
    python3 tools/woa/dxe-replace.py "$out/Star2LtePkg.fd" "$out/Star2LtePkg.fd" /dev/null --extract "$SSDTDXE" "$out/$SOLO-ssdt.ffs"
    flags_ok "$out/$SOLO-ssdt.ffs"
    python3 tools/woa/dxe-replace.py "$W/Star2LtePkg-src38.fd" "$rel" "$out/$SOLO.fd" \
        --kill "$RTC" ${serial[@]+"${serial[@]}"} --add "$W/pmic-rtc.ffs" --add "$out/$SOLO-ssdt.ffs" \
        ${extra[@]+"${extra[@]}"} >/dev/null
    python3 tools/woa/dxe-diff.py "$W/src38.fd" "$out/$SOLO.fd" ${unserial[@]+"${unserial[@]}"}
    cat "$out/src38-bootshim.bin" "$out/$SOLO.fd" > "$out/starbite-uefi-$SOLO$SUFFIX.bin"
    (cd "$out" && shasum -a 256 "starbite-uefi-$SOLO$SUFFIX.bin" | tee "usbh-SHA256SUMS-$SOLO$SUFFIX")
    rm -rf tools/woa/__pycache__
    exit 0
fi
image usbh-vbus0 0 0 0 UsbXhc
image usbh-vbus1 0 0 1 UsbXhc
image usb-urs-auto 2 1 0 UsbUrs
image usb-ufn-device 1 2 0 UsbUfn
(cd "$out" && shasum -a 256 $(ls starbite-uefi-usbh-vbus0$SUFFIX.bin starbite-uefi-usbh-vbus1$SUFFIX.bin \
    starbite-uefi-usb-urs-auto$SUFFIX.bin starbite-uefi-usb-ufn-device$SUFFIX.bin 2>/dev/null) \
    acpi/*.aml ${EXTRA_AML:-} > usbh-SHA256SUMS$SUFFIX && cat usbh-SHA256SUMS$SUFFIX)
rm -rf tools/woa/__pycache__
