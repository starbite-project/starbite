#!/usr/bin/env bash
# Build the Starbite "UEFI Shell" store entry: our source build of the woa
# Star2LtePkg UEFI (same SEC/PEI and DXE stack as tools/woa/build-uefi.sh) whose
# BDS starts the edk2 UEFI Shell embedded in the FV instead of Windows, with the
# S9's keys as the console (Volume = command palette, Power = Enter, Bixby = q).
#
#   tools/woa/build-shell-uefi.sh [RAM_STAGE]      (default 0 = bank 0, 711 MiB)
#
# Patches: tools/woa/patches 0001 0002 0003 0005 0018 0022 0023 (no TWRP divert,
# SEC without dc zva, DEBUG -> pstore ring, EndOfDxe, no RamManager, RAM stages,
# PMIC-seeded RTC) + tools/woa/patches-shell/*.patch (Star2LteKeysDxe, BDS shell
# launch, DSC/FDF switch STAR2LTE_BOOT_SHELL, default 0 = no change).
# The release modules (0019-0021) are left out: this build needs our BDS.
# Shell: edk2 ShellPkg.dsc RELEASE, the "all commands" variant (FILE_GUID
# EA4BB293..., acpiview included), placed in the FV under gUefiShellFileGuid.
# edk2: lima ~/work/edk2 at 2938b830 + tools/woa/patches-edk2 (as build-uefi.sh).
# Output (build/woa/shell/): Star2LteShell.fd, Shell-full.efi, bootshim.bin and
# uefishell.uefi = bootshim + FD, the store entry (method uefi, type uefi).
# Nothing here touches the phone.
# Shared lima tree: the build resets ~/work/edk2 with 'git checkout -- .' (as build-uefi.sh does),
# which discards uncommitted edk2 edits of any other workflow. The pgrep guard only covers running
# builds, so the tracked diff is saved first to ~/work/edk2-discarded-<date>.diff when it is not empty.
set -euo pipefail
cd "$(dirname "$0")/../.."
stage=${1:-0}
src=build/woa/fw-shell
out=build/woa/shell
shellrel=Build/Shell/RELEASE_CLANGPDB/AARCH64/Shell_EA4BB293-2D7F-4456-A681-1F22F42CD0BC.efi
rm -rf "$src"
mkdir -p "$out"
cp -R ref/exynos9810-woa/firmware "$src"
for n in 0001 0002 0003 0005 0018 0022 0023; do
    patch -s -d "$src" -p1 < tools/woa/patches/$n-*.patch
done
for p in tools/woa/patches-shell/*.patch; do
    patch -s -d "$src" -p1 < "$p"
done
# Separate edk2 output directory: never clobbers build-uefi.sh's Build/Star2LtePkg.
sed -i '' 's#^\(  OUTPUT_DIRECTORY *= \)Build/Star2LtePkg#\1Build/Star2LteShell#' "$src/Platform/Star2LtePkg/Star2LtePkg.dsc"
grep -q 'Build/Star2LteShell' "$src/Platform/Star2LtePkg/Star2LtePkg.dsc"
limactl shell starbite -- bash -c "
    set -e
    if pgrep -f '[b]uild[.]py|[G]enFds' >/dev/null; then echo 'another edk2 build is running in lima'; exit 1; fi
    cd ~/work/edk2
    [ \"\$(git rev-parse --short=8 HEAD)\" = 2938b830 ] || { echo 'edk2 is not at 2938b830'; exit 1; }
    if ! git diff --quiet; then
        d=~/work/edk2-discarded-\$(date +%Y%m%d-%H%M%S).diff
        git diff > \"\$d\"
        echo \"edk2 tree diff (possibly only the last build's patches-edk2) saved to \$d before git checkout -- .\"
    fi
    git checkout -q -- .
    for p in '$PWD'/tools/woa/patches-edk2/*.patch; do [ -f \"\$p\" ] && git apply \"\$p\"; done
    export WORKSPACE=\$PWD PACKAGES_PATH=\$PWD:'$PWD/$src' CLANG_BIN=/usr/bin/
    source edksetup.sh >/dev/null
    build -a AARCH64 -t CLANGPDB -b RELEASE -p ShellPkg/ShellPkg.dsc -n \$(nproc) > /tmp/shell-build.log 2>&1 ||
        { tail -25 /tmp/shell-build.log; exit 1; }
    [ -s '$shellrel' ]
    rm -rf Build/Star2LteShell
    build -a AARCH64 -t CLANGPDB -b DEBUG -p Platform/Star2LtePkg/Star2LtePkg.dsc \
        -D STAR2LTE_BOOT_SHELL=1 -D STAR2LTE_SHELL_EFI='$shellrel' -D STAR2LTE_RAM_STAGE=$stage \
        -n \$(nproc) > /tmp/shell-uefi-build.log 2>&1 ||
        { grep -n -E ' error|Error [0-9A-F]+:|error:' /tmp/shell-uefi-build.log | head -20; tail -15 /tmp/shell-uefi-build.log; exit 1; }
    clang -target aarch64-none-elf -c '$PWD/$src/tools/bootshim/bootshim.S' -o /tmp/shell-bootshim.o
    llvm-objcopy -O binary /tmp/shell-bootshim.o /tmp/shell-bootshim.bin
"
# virtiofs is read-only from the guest: copy the outputs out through the shell.
rm -f "$out/Star2LteShell.fd" "$out/bootshim.bin" "$out/uefishell.uefi" "$out/Shell-full.efi"
limactl shell starbite -- bash -lc "cat ~/work/edk2/Build/Star2LteShell/DEBUG_CLANGPDB/FV/STAR2LTE_UEFI.fd" </dev/null > "$out/Star2LteShell.fd"
limactl shell starbite -- bash -lc "cat ~/work/edk2/$shellrel" </dev/null > "$out/Shell-full.efi"
limactl shell starbite -- cat /tmp/shell-bootshim.bin </dev/null > "$out/bootshim.bin"
[ "$(stat -f %z "$out/Star2LteShell.fd")" -eq 7340032 ] || { echo "FD copy failed" >&2; exit 1; }
[ "$(stat -f %z "$out/bootshim.bin")" -gt 100 ] || { echo "bootshim copy failed" >&2; exit 1; }
cat "$out/bootshim.bin" "$out/Star2LteShell.fd" > "$out/uefishell.uefi"
# The loader checks the arm64 Image magic "ARMd" at +56 (xnu_boot.c boot_uefi).
[ "$(dd if="$out/uefishell.uefi" bs=1 skip=56 count=4 2>/dev/null)" = ARMd ] || { echo "no ARMd magic" >&2; exit 1; }
# The shell and the modules it needs must be inside the (compressed) DXE volume: check the build
# report for the Shell file GUID, the keys driver (the only input device) and DevicePathDxe.
fvmain=$(limactl shell starbite -- bash -lc "cat ~/work/edk2/Build/Star2LteShell/DEBUG_CLANGPDB/FV/FVMAIN.inf" </dev/null)
for want in 7C04A583 Star2LteKeysDxe DevicePathDxe; do
    grep -q "$want" <<<"$fvmain" || { echo "$want missing from FVMAIN" >&2; exit 1; }
done
(cd "$out" && shasum -a 256 uefishell.uefi Star2LteShell.fd Shell-full.efi bootshim.bin | tee SHA256SUMS)
ls -l "$out"
