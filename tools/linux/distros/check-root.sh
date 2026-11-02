#!/bin/sh
# check-root.sh IMG: read-only check that a distro image carries the Starbite integration
# (sb-integrate.sh). In the VM:  limactl shell starbite -- sudo sh .../check-root.sh ~/distros/ubuntu.img
set -u
IMG=${1:?check-root.sh IMG}
R=$(mktemp -d /tmp/sb-check.XXXX)
mount -o ro,loop,noload "$IMG" "$R" || exit 1
trap 'umount "$R"; rmdir "$R"' EXIT
fail=0
c() { if ( cd "$R" && eval "$2" ) >/dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
c "init present"                 "[ -L sbin/init ] || [ -x sbin/init ]"
c "user 10000:10000"             "grep -q '^user:x:10000:10000:' etc/passwd"
c "user in video,render,input"   "for g in video render input; do grep -q \"^\$g:.*[:,]user\" etc/group || exit 1; done"
c "user has a password"          "grep '^user:' etc/shadow | cut -d: -f2 | grep -q '^\\$'"
c "root locked"                  "grep '^root:' etc/shadow | cut -d: -f2 | grep -q '^!'"
c "ssh key"                      "[ -s home/user/.ssh/authorized_keys ]"
c "no host keys baked in"        "! ls etc/ssh/ssh_host_* 2>/dev/null | grep -q ."
c "sshd keys made at boot"       "[ -x usr/local/sbin/sb-ssh-keys ] && { ls etc/systemd/system/multi-user.target.wants/sb-ssh-keys.service || [ -x etc/local.d/50-starbite.start ]; }"
c "sshd enabled, not socket"     "ls etc/systemd/system/multi-user.target.wants/ssh*.service || ls etc/runlevels/default/sshd"
c "no ssh.socket"                "! ls etc/systemd/system/sockets.target.wants/ssh*.socket"
c "sshd on every address"        "! grep -hs '^[[:space:]]*ListenAddress' etc/ssh/sshd_config etc/ssh/sshd_config.d/*.conf | grep -q ."
c "reboot-to + busybox"          "[ -x usr/local/sbin/reboot-to ] && [ -x usr/local/lib/starbite/busybox ]"
c "Wi-Fi firmware"               "[ -s usr/lib/firmware/brcm/brcmfmac4361-pcie.bin ] || [ -s lib/firmware/brcm/brcmfmac4361-pcie.bin ]"
c "usb0 networkd + marker"       "grep -q DHCPServer=yes etc/systemd/network/10-usb0.network && [ -e etc/starbite/usb0-server ]"
c "networkd enabled"             "ls etc/systemd/system/*/systemd-networkd.service"
c "NM leaves usb0 alone"         "grep -q interface-name:usb0 etc/NetworkManager/conf.d/90-starbite-usb0.conf"
c "ModemManager masked"          "[ \"\$(readlink etc/systemd/system/ModemManager.service)\" = /dev/null ]"
c "watchdog off unless sb.wdt"   "grep -qx RuntimeWatchdogSec=off etc/systemd/system.conf.d/50-starbite-watchdog.conf && grep -qx RebootWatchdogSec=off etc/systemd/system.conf.d/50-starbite-watchdog.conf && ! grep -rqs '^RuntimeWatchdogSec=[1-9]' etc/systemd/system.conf etc/systemd/system.conf.d"
c "no watchdog-on drop-in"       "[ ! -e etc/systemd/system.conf.d/60-starbite-watchdog-on.conf ]"
c "card dir root-only 0700"      "[ \"\$(stat -c %a:%u:%g var/lib/starbite)\" = 700:0:0 ] && [ -d var/lib/starbite/card ] && [ ! -L var/lib/starbite ]"
c "share mountpoint"             "[ -d media/starbite-sd/shared ] && [ ! -L media/starbite-sd/shared ]"
c "~/Card -> the share"          "[ \"\$(readlink home/user/Card)\" = /media/starbite-sd/shared ]"
c "card hidden from udisks"      "grep -q 'KERNEL==\"mmcblk0\*\", ENV{UDISKS_IGNORE}=\"1\"' etc/udev/rules.d/90-starbite.rules"
c "fstrim masked"                "[ \"\$(readlink etc/systemd/system/fstrim.timer)\" = /dev/null ] && [ \"\$(readlink etc/systemd/system/fstrim.service)\" = /dev/null ]"
c "reboot-to shutdown hook"      "[ -x usr/lib/systemd/system-shutdown/starbite-next ] && grep -q 'reboot-to -w' usr/lib/systemd/system-shutdown/starbite-next"
c "serial getty ttyGS0"          "ls etc/systemd/system/*/serial-getty@ttyGS0.service"
c "session unit enabled"         "[ -L etc/systemd/system/graphical.target.wants/starbite-session.service ]"
c "default graphical"            "readlink etc/systemd/system/default.target | grep -q graphical"
c "greetd not the session"       "[ ! -e etc/systemd/system/display-manager.service ]"
c "phosh scale 3"                "grep -A1 'output:DSI-1' etc/phosh/phoc.ini | grep -q 'scale = 3'"
c "AFBC env"                     "grep -qx PAN_MAX_AFBC_PACKING_RATIO=0 etc/environment"
c "GPU floor rule"               "grep -q 455000000 etc/udev/rules.d/90-starbite.rules"
c "phone setup enabled"          "[ -L etc/systemd/system/sysinit.target.wants/sb-phone-setup.service ]"
c "no kernel installed"          "! ls boot/vmlinu* usr/lib/modules/*/kernel 2>/dev/null | grep -q ."
c "kernel pin"                   "grep -q 'Pin-Priority: -1' etc/apt/preferences.d/90-starbite-no-kernel"
c "fstab has no root"            "! grep -v '^#' etc/fstab | grep -q ' / '"
c "machine-id empty"             "[ ! -s etc/machine-id ]"
c "resolv.conf"                  "[ -L etc/resolv.conf ] || [ -f etc/resolv.conf ]"
c "no policy-rc.d left"          "[ ! -e usr/sbin/policy-rc.d ]"
c "sessions: phosh"              "[ -x usr/bin/phosh-session ]"
echo "sessions: $(for b in phosh-session gnome-session lomiri-session; do [ -x "$R/usr/bin/$b" ] && printf '%s ' "$b"; done)"
cat "$R/etc/starbite/integrated"
df -h "$R" | tail -1
exit $fail
