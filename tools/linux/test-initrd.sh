#!/usr/bin/env bash
# Check a built linux.initrd in the VM: unpack it to a temporary directory, chroot in, and check
# that /init parses, the tools the init calls exist and run, and the logic test passes with the
# initrd's own busybox.
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/linux/test-initrd.sh [INITRD]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
RD=${1:-$UH/distros/out/linux.initrd}
W=$(mktemp -d /var/tmp/sb-rdtest.XXXX)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/r"
case $(head -c4 "$RD" | od -An -tx1 | tr -d ' ') in
28b52ffd) zstd -dc "$RD" ;;
1f8b*) gzip -dc "$RD" ;;
*) cat "$RD" ;;
esac | (cd "$W/r" && cpio -id --quiet)
cp -r "$HERE/test-init.sh" "$HERE/init" "$HERE/reboot-to" "$HERE/initrd" "$W/r/tmp/"
mkdir -p "$W/r/tmp/distros"
[ -f "$HERE/distros/sb-integrate.sh" ] && cp "$HERE/distros/sb-integrate.sh" "$W/r/tmp/distros/"
fail=0
chk() { if chroot "$W/r" /bin/sh -c "$2" >/dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
chk "/init is the repo's"          "cmp /init /tmp/init"
chk "/dev/console node"            "[ -c /dev/console ]"
for a in sh devmem udhcpd telnetd watchdog switch_root hexdump setsid mkfifo blockdev ifconfig killall od cmp; do
	chk "applet $a" "command -v $a"
done
chk "e2fsck runs"                  "e2fsck -V"
chk "resize2fs runs"               "resize2fs 2>&1 | grep -q Usage"
chk "fsck.exfat runs"              "fsck.exfat -V 2>&1 | grep -qi exfatprogs"
chk "losetup has --direct-io"      "/sbin/losetup --help 2>&1 | grep -q direct-io"
chk "evtest present"               "command -v evtest"
chk "Wi-Fi firmware"               "[ -s /lib/firmware/brcm/brcmfmac4361-pcie.bin ] && [ -s /lib/firmware/brcm/brcmfmac4361-pcie.txt ] && [ -s /lib/firmware/brcm/brcmfmac4361-pcie.clm_blob ]"
chk "reboot-to dry run"            "reboot-to -n linux-ubuntu | grep -q 0x21325458"
chk "busybox.static is static"     "[ -x /bin/busybox.static ]"
chk "sb-tools present"             "[ -x /bin/sb-tools ]"
if chroot "$W/r" /bin/sh /tmp/test-init.sh > "$W/log" 2>&1; then
	echo "ok   test-init.sh inside the initrd ($(tail -1 "$W/log"))"
else
	echo "FAIL test-init.sh inside the initrd:"; grep -v '^ok' "$W/log"; fail=1
fi
echo "unpacked: $(du -sh "$W/r" | cut -f1), packed: $(( $(stat -c %s "$RD") / 1024 )) KiB"
exit $fail
