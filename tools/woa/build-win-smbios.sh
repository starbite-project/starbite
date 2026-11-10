#!/usr/bin/env bash
# Windows UEFI images with SMBIOS (Task Manager's CPU name): docs/WIN11-GPU.md section 6.39.
#
#   tools/woa/build-win-smbios.sh [OUT_DIR]       default OUT_DIR: build/win/stage of the main checkout
#   BASE=path/to/windows-xxx.uefi tools/woa/build-win-smbios.sh
#
# Every image is BASE (default the win11 row's GPU firmware build/win/stage/windows-t13b.uefi: bootshim
# 384 bytes + FD) plus one added DXE file and nothing else: Star2LteSmbiosDxe (tools/woa/modules-smbios):
# the table (types 0, 1, 3, 4 and, in the full image, 7) and the SMBIOS 3.0 and 2.1 entry points as UEFI
# configuration tables. Not edk2's SmbiosDxe: with it the compressed DXE volume is 0x9C0 bytes over the
# release's fixed slot (0x51000) even with SerialDxe blanked; nothing in UEFI needs the SMBIOS protocol.
# The table builder is host-tested first (modules-smbios/test/run.sh: dmidecode --from-dump).
# It comes from our edk2 build (patches 0001,0002,0005,0018-0023 like the USB track's modules, plus 0030,
# RELEASE: no serial DEBUG output, smallest), are extracted from its FD and added to BASE's DXE volume with
# dxe-replace.py; BASE's SEC/PEI, bootshim and every other DXE file stay byte for byte (dxe-diff.py checks:
# outer FV identical, only that file added). The cache records' names ("L1 Instruction") in the module
# prove the type-7 switch reached it.
# Out:  windows-t32s.uefi          types 0, 1, 3, 4, 7
#       windows-t32s-nocache.uefi  types 0, 1, 3, 4 (no cache records: the PPTT with the same caches
#                                  bugchecked Windows 0x7E, unexplained; this one isolates that)
#       windows-t32s.SHA256SUMS
# The edk2 tree in the VM (~/work/edk2) is shared: don't run two UEFI builds at once.
set -euo pipefail
cd "$(dirname "$0")/../.."
MAIN=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
base=${BASE:-$MAIN/build/win/stage/windows-t13b.uefi}
out=${1:-$MAIN/build/win/stage}
PLATDXE=c4ce8369-79b3-4382-e763-8799826cce01
[ -f "$base" ] || { echo "missing $base" >&2; exit 1; }
[ "$(stat -f %z "$base")" = 7340416 ] || { echo "$base: not bootshim (384) + 7 MiB FD" >&2; exit 1; }
[ -d ref/exynos9810-woa ] || { echo "ref/exynos9810-woa missing (in a worktree: ln -s $MAIN/ref ref)" >&2; exit 1; }
if limactl shell starbite -- pgrep -f 'build -a AARCH64' >/dev/null; then
    echo "another edk2 build is running in the VM; try again later" >&2; exit 1
fi
tools/woa/modules-smbios/test/run.sh
w=build/woa/smbios
rm -rf "$w"; mkdir -p "$w" "$out"
head -c 384 "$base" > "$w/bootshim.bin"
tail -c +385 "$base" > "$w/base.fd"

echo "== identity: BASE's DXE volume rebuilt with nothing changed"
python3 tools/woa/dxe-replace.py "$w/base.fd" "$w/base.fd" "$w/identity.fd" >/dev/null
if cmp -s "$w/identity.fd" "$w/base.fd"; then echo "identity: byte-identical"; else
    python3 tools/woa/dxe-diff.py "$w/base.fd" "$w/identity.fd" || { echo "identity rebuild differs in content" >&2; exit 1; }
    echo "identity: same files (compressed bytes differ)"
fi

export PATCHES='tools/woa/patches/000[125]-*.patch tools/woa/patches/001[89]-*.patch tools/woa/patches/002[0-3]-*.patch tools/woa/patches/0030-*.patch'
image() {   # image NAME TYPE7
    local name=$1 t7=$2
    echo "== $name: Star2LteSmbiosDxe (type 7: $t7)"
    tools/woa/build-uefi.sh RELEASE -D STAR2LTE_RAM_STAGE=3 -D STAR2LTE_SMBIOS_DXE=TRUE -D STARBITE_SMBIOS_TYPE7=$t7 \
        > "$w/$name-uefi.log" 2>&1 || { tail -25 "$w/$name-uefi.log" >&2; exit 1; }
    tail -1 "$w/$name-uefi.log"
    python3 tools/woa/dxe-replace.py build/woa/Star2LtePkg.fd build/woa/Star2LtePkg.fd /dev/null --extract "$PLATDXE" "$w/$name-plat.ffs"
    # the switch reached the module: the cache records' names are in the binary only with type 7
    grep -aq "Samsung Exynos 9810" "$w/$name-plat.ffs" || { echo "$name-plat.ffs: no CPU name string" >&2; exit 1; }
    if grep -aq "L1 Instruction" "$w/$name-plat.ffs"; then has=1; else has=0; fi
    [ "$has" = "$t7" ] || { echo "$name-plat.ffs: type 7 present=$has, wanted $t7" >&2; exit 1; }
    python3 tools/woa/dxe-replace.py "$w/base.fd" "$w/base.fd" "$w/$name.fd" \
        --add "$w/$name-plat.ffs" | grep -E "added|inner FV|DXE file"
    python3 tools/woa/dxe-diff.py "$w/base.fd" "$w/$name.fd"
    cat "$w/bootshim.bin" "$w/$name.fd" > "$out/$name.uefi"
    cmp -n 384 "$base" "$out/$name.uefi"
}
image windows-t32s 1
image windows-t32s-nocache 0
(cd "$out" && shasum -a 256 windows-t32s.uefi windows-t32s-nocache.uefi windows-t13b.uefi > windows-t32s.SHA256SUMS 2>/dev/null ||
    shasum -a 256 windows-t32s.uefi windows-t32s-nocache.uefi > windows-t32s.SHA256SUMS; cat windows-t32s.SHA256SUMS)
