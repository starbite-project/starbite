#!/usr/bin/env bash
# Host side of docs/WINPE-ARM64-PLAN.md (wimlib in the lima VM; the ADK cannot run on macOS).
#
#   tools/woa/winpe/winpe-host.sh info WIM              images in a WIM (boot.wim / Winre.wim)
#   tools/woa/winpe/winpe-host.sh versions WIM [INDEX]  winload/ntoskrnl/hal versions in an image
#                                                       (the firmware is matched to 22621.7582)
#   tools/woa/winpe/winpe-host.sh build SRC OUT [DRIVERS_DIR]
#       SRC = a Windows 11 ARM64 ISO or a boot.wim. Exports image 1 (WinPE) as a bootable
#       LZX WIM, adds X:\Windows\System32\startnet.cmd (wpeinit + drvload of every INF under
#       X:\Starbite\Drivers) and DRIVERS_DIR as X:\Starbite\Drivers; with an ISO also takes
#       \boot\boot.sdi. OUT gets boot.wim, boot.sdi (ISO only), versions.txt, SHA256SUMS.
#       This is the route without DISM (drivers load at startnet time, not boot-start);
#       starbite-winpe.cmd on the phone's Windows is the DISM route.
#
# Paths must be under this repository (the VM sees it at the same path, read-only); work
# files go to the guest's ~/winpe-work and are deleted afterwards. Nothing touches the phone.
set -euo pipefail
cd "$(dirname "$0")/../../.."
here=$PWD
vm() { limactl shell starbite -- bash -lc "$1" </dev/null; }
abs() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
cmd=${1:-}
case $cmd in
info)
    vm "wimlib-imagex info '$(abs "$2")'" ;;
versions)
    wim=$(abs "$2"); idx=${3:-1}
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/winpe-ver.XXXXXX")
    vm "set -e; rm -rf ~/winpe-ver; mkdir -p ~/winpe-ver
        for f in /Windows/System32/Boot/winload.efi /Windows/System32/ntoskrnl.exe /Windows/System32/hal.dll; do
            wimlib-imagex extract '$wim' $idx \$f --dest-dir=\$HOME/winpe-ver --no-acls >/dev/null 2>&1 || true
        done"
    for f in winload.efi ntoskrnl.exe hal.dll; do
        vm "cat ~/winpe-ver/$f 2>/dev/null || true" > "$tmp/$f"
    done
    vm "rm -rf ~/winpe-ver"
    (cd "$tmp" && python3 -I "$here/tools/woa/winpe/pever.py" winload.efi ntoskrnl.exe hal.dll)
    rm -rf "$tmp" ;;
build)
    src=$(abs "$2"); out=$3; drv=${4:-}
    [ -n "$drv" ] && drv=$(abs "$drv")
    mkdir -p "$out"
    vm "set -e
        w=~/winpe-work; rm -rf \$w; mkdir -p \$w/add/Windows/System32 \$w/add/Starbite/Drivers \$w/iso
        case '$src' in
        *.iso|*.ISO)
            sudo mount -o loop,ro '$src' \$w/iso
            trap 'sudo umount \$w/iso 2>/dev/null || true' EXIT
            bw=\$w/iso/sources/boot.wim; cp \$w/iso/boot/boot.sdi \$w/boot.sdi ;;
        *) bw='$src' ;;
        esac
        wimlib-imagex info \"\$bw\" | grep -E '^(Index|Name|Description|Architecture|Build|Service Pack Build|Total Bytes)'
        wimlib-imagex export \"\$bw\" 1 \$w/boot.wim --boot --compress=LZX >/dev/null
        printf 'wpeinit\r\nfor /r X:\\\\Starbite\\\\Drivers %%%%i in (*.inf) do drvload \"%%%%i\"\r\necho Starbite WinPE (Galaxy S9)\r\n' \
            > \$w/add/Windows/System32/startnet.cmd
        [ -n '$drv' ] && cp -R '$drv'/. \$w/add/Starbite/Drivers/
        wimlib-imagex update \$w/boot.wim 1 --command=\"add \$w/add /\"
        wimlib-imagex info \$w/boot.wim 1 | grep -E '^(Index|Name|Boot Index|Total Bytes)'
        wimlib-imagex verify \$w/boot.wim >/dev/null 2>&1 && echo \"boot.wim verified\""
    vm "cat ~/winpe-work/boot.wim" > "$out/boot.wim"
    vm "test -f ~/winpe-work/boot.sdi && cat ~/winpe-work/boot.sdi || true" > "$out/boot.sdi"
    [ -s "$out/boot.sdi" ] || rm -f "$out/boot.sdi"
    vm "rm -rf ~/winpe-work"
    "$0" versions "$out/boot.wim" 1 | tee "$out/versions.txt"
    (cd "$out" && shasum -a 256 boot.wim $( [ -f boot.sdi ] && echo boot.sdi ) | tee SHA256SUMS) ;;
*)
    sed -n 2,17p "$0"; exit 1 ;;
esac
