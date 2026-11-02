#!/bin/sh
# sb-integrate.sh NAME systemd|openrc [PAYLOAD]: make a distro root a Starbite S9 root
# (docs/LINUX-REBUILD-PLAN.md 3.4). Shared by every distro; run INSIDE the root's chroot, as root,
# with /dev /proc /sys bound. POSIX sh (pmOS/Alpine have only busybox sh). Safe to run again.
#
# PAYLOAD (default: the directory next to this script, "payload/") holds what the chroot cannot
# see; tools/linux/distros/mk-payload.sh makes it:
#   busybox               static busybox (devmem for reboot-to)
#   reboot-to             tools/linux/reboot-to
#   firmware/brcm/*       BCM4361 Wi-Fi + BT firmware under their final names
#   authorized_keys       optional: build/linux/ssh/starbite_ed25519.pub
#   login.txt             optional: "pin NNNNNN" (build/linux/<name>-login.txt, git-ignored)
#
# What it does (each block says why):
#   user "user" uid/gid 10000 (the initrd mounts the card exFAT uid=10000, but root-only under
#   /var/lib/starbite; the user gets only its shared/ on /media/starbite-sd/shared) in video input render
#   audio netdev plugdev wheel (+sudo); root locked; sudo/doas for wheel; SSH key; sshd on every
#   address (not socket-activated) with host keys made on the phone before it starts (sb-ssh-keys)
#   usb0 172.16.42.1/24 served by the distro (systemd-networkd DHCPServer, or busybox udhcpd on
#   OpenRC), never touched by NetworkManager or ModemManager, and nothing resets the gadget
#   GPU devfreq floor 455 MHz, PAN_MAX_AFBC_PACKING_RATIO=0, integer scale 3, no UDisks for the UFS
#   no watchdog unless the entry says sb.wdt=N (the initrd then turns systemd's feeder on), no
#   fstrim (it would reach the card's exFAT), reboot-to's mailbox written by a systemd-shutdown hook,
#   no suspend, power key does not shut down, charger 500 mA, M3 floor 1066 MHz
#   never a distro kernel, bootloader or initramfs generator
set -eu
NAME=${1:?usage: sb-integrate.sh NAME systemd|openrc [PAYLOAD]}
INITSYS=${2:?usage: sb-integrate.sh NAME systemd|openrc [PAYLOAD]}
PL=${3:-$(cd "$(dirname "$0")" && pwd)/payload}
SB_USER=user
SB_ID=10000
SB_TZ=${SB_TZ:-Europe/Helsinki}
case $INITSYS in systemd|openrc) ;; *) echo "init system: systemd or openrc" >&2; exit 2 ;; esac
case $NAME in ""|*[!a-z0-9-]*) echo "NAME: lower case letters, digits, '-'" >&2; exit 2 ;; esac
[ -d "$PL" ] || { echo "no payload directory $PL" >&2; exit 1; }
say() { echo "sb-integrate: $*"; }
have() { command -v "$1" >/dev/null 2>&1; }
# usr-merged roots: /lib may be a link to usr/lib
LIBDIR=/lib; [ -L /lib ] && LIBDIR=/usr/lib
mkdir -p /etc/starbite

# ---- identity -------------------------------------------------------------------------------
echo "starbite-$NAME" > /etc/hostname
printf '127.0.0.1 localhost\n127.0.1.1 starbite-%s\n::1 localhost ip6-localhost ip6-loopback\n' "$NAME" > /etc/hosts
if [ -e "/usr/share/zoneinfo/$SB_TZ" ]; then
	ln -sf "/usr/share/zoneinfo/$SB_TZ" /etc/localtime
	echo "$SB_TZ" > /etc/timezone
fi
if [ -f /etc/locale.gen ] && have locale-gen; then
	sed -i 's/^# *en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen
	locale-gen >/dev/null 2>&1 || true
	echo LANG=en_US.UTF-8 > /etc/locale.conf
	[ -d /etc/default ] && echo LANG=en_US.UTF-8 > /etc/default/locale
fi
# machine-id: empty = systemd makes one on the first boot and commits it (the root is rw)
if [ "$INITSYS" = systemd ]; then
	: > /etc/machine-id
	rm -f /var/lib/dbus/machine-id
	ln -sf /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true
fi

# ---- the user -------------------------------------------------------------------------------
grp_add() {                                         # grp_add NAME [GID]
	getent group "$1" >/dev/null && return 0
	if have groupadd; then groupadd ${2:+-g "$2"} "$1"; else addgroup ${2:+-g "$2"} "$1"; fi
}
grp_add "$SB_USER" "$SB_ID"
grp_add wheel
if ! id "$SB_USER" >/dev/null 2>&1; then
	if have useradd; then useradd -m -u "$SB_ID" -g "$SB_ID" -s /bin/bash "$SB_USER" 2>/dev/null ||
		useradd -m -u "$SB_ID" -g "$SB_ID" -s /bin/sh "$SB_USER"
	else adduser -D -u "$SB_ID" -G "$SB_USER" -s /bin/sh "$SB_USER"; fi
fi
[ "$(id -u "$SB_USER"):$(id -g "$SB_USER")" = "$SB_ID:$SB_ID" ] || { echo "$SB_USER is not $SB_ID:$SB_ID" >&2; exit 1; }
for g in video input render audio netdev plugdev wheel sudo dialout feedbackd; do
	getent group "$g" >/dev/null || continue
	if have usermod; then usermod -aG "$g" "$SB_USER"; else addgroup "$SB_USER" "$g" 2>/dev/null || true; fi
done
PIN=
[ -f "$PL/login.txt" ] && PIN=$(awk '/^pin/ {print $2}' "$PL/login.txt")
if [ -n "$PIN" ]; then echo "$SB_USER:$PIN" | chpasswd; else say "no PIN in the payload: $SB_USER has no password set"; fi
passwd -l root >/dev/null 2>&1 || true
if [ -d /etc/sudoers.d ]; then
	printf '%%wheel ALL=(ALL:ALL) ALL\n' > /etc/sudoers.d/10-starbite
	chmod 440 /etc/sudoers.d/10-starbite
fi
if [ -d /etc/doas.d ]; then echo 'permit persist :wheel' > /etc/doas.d/10-starbite.conf
elif have doas; then grep -q ':wheel' /etc/doas.conf 2>/dev/null || echo 'permit persist :wheel' >> /etc/doas.conf; fi
H=$(getent passwd "$SB_USER" | cut -d: -f6)
if [ -s "$PL/authorized_keys" ]; then
	install -d -m700 -o "$SB_ID" -g "$SB_ID" "$H/.ssh"
	install -m600 -o "$SB_ID" -g "$SB_ID" "$PL/authorized_keys" "$H/.ssh/authorized_keys"
fi
mkdir -p "$H/.config"
chown "$SB_ID:$SB_ID" "$H/.config"

# ---- Starbite tools: reboot-to (+ the old name), static busybox for devmem ---------------------
install -d /usr/local/sbin /usr/local/lib/starbite
install -m755 "$PL/busybox" /usr/local/lib/starbite/busybox
install -m755 "$PL/reboot-to" /usr/local/sbin/reboot-to
ln -sf reboot-to /usr/local/sbin/reboot-linux
# systemd: reboot-to leaves the key in /run/starbite/next and this hook writes the mailbox right
# before the kernel reboots. The mailbox (0xFED17FE0) is the last 32 bytes of the ramoops console
# ring, so kernel output during a long shutdown could overwrite it if it were written earlier.
if [ "$INITSYS" = systemd ]; then
	install -d "$LIBDIR/systemd/system-shutdown"
	cat > "$LIBDIR/systemd/system-shutdown/starbite-next" <<'EOF'
#!/bin/sh
# Starbite (sb-integrate.sh): run by systemd-shutdown right before reboot(2) ($1 = reboot).
[ "$1" = reboot ] || exit 0
[ -s /run/starbite/next ] || exit 0
exec /usr/local/sbin/reboot-to -w "$(cat /run/starbite/next)"
EOF
	chmod 755 "$LIBDIR/systemd/system-shutdown/starbite-next"
fi

# ---- firmware (brcmfmac is built in, and so is its Wi-Fi firmware: CONFIG_EXTRA_FIRMWARE; this
# copy serves a kernel built without it, and Bluetooth) ------------------------------------------
if [ -d "$PL/firmware/brcm" ]; then
	install -d "$LIBDIR/firmware/brcm"
	cp "$PL"/firmware/brcm/* "$LIBDIR/firmware/brcm/"
fi

# ---- filesystems: the initrd mounts /, the card and its share (tools/linux/init card_into_root) --
# The whole exFAT (backup/, windows/, the iOS image, this root's own image) sits root-only under
# /var/lib/starbite (0700); the desktop user sees only shared/, bound on /media/starbite-sd/shared
# and linked as ~/Card. Made here too so a read-only boot (sb.rw=0) finds them.
printf '# /, /var/lib/starbite/card (the card, root only) and /media/starbite-sd/shared (its shared/)\n# are mounted by the Starbite initrd (tools/linux/init card_into_root)\n' > /etc/fstab
install -d -m700 -o 0 -g 0 /var/lib/starbite
install -d -m755 /var/lib/starbite/card /media/starbite-sd/shared
mkdir -p /lib/modules 2>/dev/null || true
if [ -L "$H/Card" ] || [ ! -e "$H/Card" ]; then ln -sfn /media/starbite-sd/shared "$H/Card"; chown -h "$SB_ID:$SB_ID" "$H/Card"; fi

# ---- environment + udev -----------------------------------------------------------------------
touch /etc/environment
sed -i '/^PAN_MAX_AFBC_PACKING_RATIO=/d;/^PAN_MESA_DEBUG=/d' /etc/environment
echo PAN_MAX_AFBC_PACKING_RATIO=0 >> /etc/environment
mkdir -p /etc/udev/rules.d
cat > /etc/udev/rules.d/90-starbite.rules <<'EOF'
# Starbite (sb-integrate.sh). The internal UFS (Android, iOS, Windows) never shows up in Files,
# nor does the card itself (its exFAT is root-only; the user's part is /media/starbite-sd/shared).
SUBSYSTEM=="block", KERNEL=="sd*", ENV{UDISKS_IGNORE}="1"
SUBSYSTEM=="block", KERNEL=="mmcblk0*", ENV{UDISKS_IGNORE}="1"
# GPU clock floor 455 MHz (docs/LINUX-REBUILD-PLAN.md 1.3): no 260 MHz step under load.
ACTION=="add", SUBSYSTEM=="devfreq", KERNEL=="17500000.gpu", ATTR{min_freq}="455000000"
# Touchscreen and keys are plain evdev: readable by the seat.
SUBSYSTEM=="input", KERNEL=="event*", TAG+="uaccess"
EOF

# ---- phone setup at boot: charger limit, M3 floor, no idle-only voltage points ----------------
cat > /usr/local/sbin/sb-phone-setup <<'EOF'
#!/bin/sh
# Starbite (sb-integrate.sh): from the port's s9p-stability and s9p-touch-power.
# No idle-only low-voltage points (the phone froze about once a day with them on).
for p in /sys/module/s9p_cpufreq/parameters/idle_floor /sys/module/s9p_g3d/parameters/step_volt; do
	[ -w "$p" ] && echo N > "$p"
done
# M3 cluster floor 1066 MHz (governor-tested; 400 MHz / 700 mV is not).
p=/sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq
[ -w "$p" ] && echo 1066000 > "$p"
# GPU floor, again here in case the devfreq device came before udev's rule.
for d in /sys/class/devfreq/*gpu*; do [ -w "$d/min_freq" ] && echo 455000000 > "$d/min_freq"; done
# Charger input 500 mA (the port's limit for this board).
C=/sys/class/power_supply/max77705-charger
i=0
while [ "$i" -lt 40 ] && [ ! -e "$C/input_current_limit" ]; do i=$((i + 1)); sleep 0.25; done
[ -e "$C/input_current_limit" ] && echo 500000 > "$C/input_current_limit"
exit 0
EOF
chmod 755 /usr/local/sbin/sb-phone-setup

# ---- NetworkManager never touches usb0; polkit lets netdev use NetworkManager --------------------
if [ -d /etc/NetworkManager ]; then
	mkdir -p /etc/NetworkManager/conf.d
	printf '[keyfile]\nunmanaged-devices=interface-name:usb0\n' > /etc/NetworkManager/conf.d/90-starbite-usb0.conf
	rm -f /etc/NetworkManager/conf.d/usb0-managed.conf /etc/NetworkManager/system-connections/usb0.nmconnection
fi
if [ -d /etc/polkit-1 ] || [ -d /usr/share/polkit-1 ]; then
	mkdir -p /etc/polkit-1/rules.d
	cat > /etc/polkit-1/rules.d/50-starbite-nm.rules <<'EOF'
// Starbite (sb-integrate.sh): the phone's user runs Wi-Fi without a password prompt.
polkit.addRule(function(action, subject) {
	if (action.id.indexOf("org.freedesktop.NetworkManager.") == 0 &&
	    (subject.isInGroup("netdev") || subject.isInGroup("wheel")))
		return polkit.Result.YES;
});
EOF
fi

# ---- never a distro kernel, bootloader or initramfs generator ---------------------------------
if have apt-get; then
	cat > /etc/apt/preferences.d/90-starbite-no-kernel <<'EOF'
# Starbite: the kernel, its modules and the boot chain come from Starbite (linux.image in the
# UFS store, /distros/_kernel/modules-*.img on the card). Never install the distro's.
Package: linux-image* linux-modules* linux-headers* linux-generic* linux-signed* linux-firmware* linux-raspi* linux-*-generic* flash-kernel grub* shim* u-boot* raspi-firmware
Pin: release *
Pin-Priority: -1
EOF
	for p in linux-image-generic linux-generic linux-image-arm64 flash-kernel; do apt-mark hold "$p" >/dev/null 2>&1 || true; done
	if [ -d /etc/initramfs-tools ]; then
		sed -i 's/^update_initramfs=.*/update_initramfs=no/' /etc/initramfs-tools/update-initramfs.conf 2>/dev/null || true
	fi
	# a deb that sneaks a kernel in anyway finds no place to put it
	mkdir -p /etc/kernel/postinst.d
fi
if [ -f /etc/pacman.conf ]; then
	grep -q '^IgnorePkg.*linux-aarch64' /etc/pacman.conf ||
		sed -i 's/^#\?IgnorePkg *=.*/IgnorePkg = linux-aarch64 linux-firmware uboot-*/' /etc/pacman.conf
fi
if [ -f /etc/dnf/dnf.conf ]; then
	grep -q '^exclude=' /etc/dnf/dnf.conf || echo 'exclude=kernel* grub2* shim* linux-firmware*' >> /etc/dnf/dnf.conf
fi

# ---- sshd: its own host keys made before every start, sshd itself on every address -------------
# The image carries no host keys (the build host's are deleted: every root makes its own), so
# something on the phone has to make them before sshd runs. Ubuntu's sshd-keygen.service only runs
# when systemd calls the boot a first boot, and an ExecStartPre=-ssh-keygen -A drop-in runs after
# the unit's own ExecStartPre=sshd -t, which already fails without keys: sshd then failed in a
# restart loop, the start limit stopped ssh.socket too, and port 22 refused connections.
# sb-ssh-keys runs before sshd on every boot (systemd unit, or OpenRC local.d) and makes any key that
# is missing or empty. No socket activation (ssh.socket fails along with its service); no
# ListenAddress: sshd binds the wildcard, so it serves usb0 (172.16.42.1) and Wi-Fi whenever they
# come up, without waiting for either.
if have sshd || [ -x /usr/sbin/sshd ]; then
	rm -f /etc/ssh/ssh_host_*
	cat > /usr/local/sbin/sb-ssh-keys <<'EOF'
#!/bin/sh
# Starbite (sb-integrate.sh): sshd host keys, made on the first boot (the image carries none) and
# again whenever one is missing or empty (a hard reset during the first boot). Runs before sshd.
[ -x /usr/bin/ssh-keygen ] || exit 0
mkdir -p /etc/ssh
for k in /etc/ssh/ssh_host_*_key; do
	[ -e "$k" ] || continue
	[ -s "$k" ] && [ -s "$k.pub" ] || rm -f "$k" "$k.pub"
done
/usr/bin/ssh-keygen -A && sync
EOF
	chmod 755 /usr/local/sbin/sb-ssh-keys
	mkdir -p /etc/ssh/sshd_config.d
	# (first value wins in sshd_config and Include comes first; ListenAddress lines add up instead,
	# so any active one in the distro's files is commented out)
	cat > /etc/ssh/sshd_config.d/10-starbite.conf <<'EOF'
# Starbite (sb-integrate.sh): port 22 on every address (no ListenAddress anywhere), so sshd serves
# usb0 172.16.42.1 and Wi-Fi without waiting for them. Root is locked; "user" logs in with the
# SSH key (build/linux/ssh/starbite_ed25519) or the PIN.
Port 22
AddressFamily any
PermitRootLogin no
PubkeyAuthentication yes
EOF
	for f in /etc/ssh/sshd_config /etc/ssh/sshd_config.d/*.conf; do
		[ -f "$f" ] && sed -i 's/^\([[:space:]]*ListenAddress[[:space:]]\)/#Starbite (sb-integrate.sh): \1/' "$f"
	done
fi

# ---- init-system specific -----------------------------------------------------------------------
if [ "$INITSYS" = systemd ]; then
	mkdir -p /etc/systemd/system /etc/systemd/network /etc/systemd/system.conf.d \
		/etc/systemd/logind.conf.d /etc/systemd/sleep.conf.d
	# usb0: the distro serves the Mac's single lease; the initrd's udhcpd stops at switch_root
	cat > /etc/systemd/network/10-usb0.network <<'EOF'
# Starbite (sb-integrate.sh): the USB gadget made by the initrd (CDC-ECM, the Mac is 172.16.42.2)
[Match]
Name=usb0

[Link]
RequiredForOnline=no

[Network]
Address=172.16.42.1/24
DHCPServer=yes
DHCP=no
IPv6AcceptRA=no
LinkLocalAddressing=no
ConfigureWithoutCarrier=yes
KeepConfiguration=yes

[DHCPServer]
PoolOffset=2
PoolSize=1
EmitDNS=no
EmitRouter=no
EmitNTP=no
EOF
	echo "systemd-networkd 10-usb0.network" > /etc/starbite/usb0-server
	# Watchdog off. sb.wdt=N in the catalog entry is the one switch: the initrd then arms the S3C2410
	# and writes 60-starbite-watchdog-on.conf (RuntimeWatchdogSec=N), removed again when sb.wdt=0.
	# The watchdog reset path is not verified yet (plan 4.3: it may land in upload mode).
	cat > /etc/systemd/system.conf.d/50-starbite-watchdog.conf <<'EOF'
[Manager]
# Starbite (sb-integrate.sh): no hardware watchdog unless the catalog entry passes sb.wdt=N; the
# initrd then writes 60-starbite-watchdog-on.conf (tools/linux/init wdt_conf).
RuntimeWatchdogSec=off
RebootWatchdogSec=off
KExecWatchdogSec=off
EOF
	rm -f /etc/systemd/system.conf.d/60-starbite-watchdog-on.conf
	cat > /etc/systemd/logind.conf.d/50-starbite.conf <<'EOF'
[Login]
# Starbite: the power key wakes the panel; it must not shut down. No lid, no suspend.
HandlePowerKey=ignore
HandlePowerKeyLongPress=poweroff
HandleSuspendKey=ignore
HandleLidSwitch=ignore
IdleAction=ignore
EOF
	cat > /etc/systemd/sleep.conf.d/50-starbite.conf <<'EOF'
[Sleep]
# Starbite: the big cores never come back from power-down on this port.
AllowSuspend=no
AllowHibernation=no
AllowSuspendThenHibernate=no
AllowHybridSleep=no
EOF
	cat > /etc/systemd/system/sb-phone-setup.service <<'EOF'
[Unit]
Description=Starbite: S9 clock floors and charger limit
DefaultDependencies=no
After=systemd-udevd.service
Before=basic.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/sb-phone-setup
RemainAfterExit=yes

[Install]
WantedBy=sysinit.target
EOF
	sc() { systemctl "$@" >/dev/null 2>&1 || true; }
	sc enable sb-phone-setup.service systemd-networkd.service serial-getty@ttyGS0.service
	[ -e /usr/lib/systemd/system/NetworkManager.service ] || [ -e /lib/systemd/system/NetworkManager.service ] &&
		sc enable NetworkManager.service
	# sshd (see the sshd block above): Debian/Ubuntu ssh.service, Arch/Fedora/openSUSE sshd.service
	ssh_unit=
	for s in ssh.service sshd.service; do
		for d in /usr/lib/systemd/system /lib/systemd/system; do
			# a link is an alias (Ubuntu: sshd.service -> ssh.service)
			[ -f "$d/$s" ] && [ ! -L "$d/$s" ] && [ -z "$ssh_unit" ] && ssh_unit=$s
		done
	done
	if [ -n "$ssh_unit" ]; then
		cat > /etc/systemd/system/sb-ssh-keys.service <<'EOF'
[Unit]
Description=Starbite: sshd host keys (first boot, or any one missing)
After=local-fs.target
Before=ssh.service sshd.service sshd@.service ssh.socket sshd.socket sshd-keygen.service sshdgenkeys.service sshd-keygen.target
ConditionPathIsReadWrite=/etc

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/sb-ssh-keys

[Install]
WantedBy=multi-user.target
EOF
		# sshd itself listens: no socket activation (a failing service took ssh.socket down with it)
		for s in ssh.socket sshd.socket; do
			rm -f "/etc/systemd/system/sockets.target.wants/$s" "/etc/systemd/system/ssh.service.requires/$s" \
				"/etc/systemd/system/sshd.service.requires/$s"
			[ -e "/usr/lib/systemd/system/$s" ] || [ -e "/lib/systemd/system/$s" ] || continue
			sc disable "$s"
			sc mask "$s"
		done
		# an earlier mkroot-ubuntu.sh drop-in (its ssh-keygen ran after sshd -t, i.e. never); Ubuntu's
		# socket migration drop-in
		grep -qs 'ssh-keygen -A' /etc/systemd/system/ssh.service.d/10-keys.conf &&
			rm -f /etc/systemd/system/ssh.service.d/10-keys.conf
		rm -f /etc/systemd/system/ssh.service.d/00-socket.conf /etc/systemd/system/ssh.socket.d/addresses.conf
		mkdir -p "/etc/systemd/system/$ssh_unit.d"
		cat > "/etc/systemd/system/$ssh_unit.d/50-starbite.conf" <<'EOF'
# Starbite (sb-integrate.sh): host keys first; keep trying instead of giving up after 5 failures
[Unit]
Wants=sb-ssh-keys.service
After=sb-ssh-keys.service
StartLimitIntervalSec=0

[Service]
Restart=always
RestartSec=5
RestartPreventExitStatus=
EOF
		sc enable sb-ssh-keys.service "$ssh_unit"
	fi
	# no gadget resets, no modem, no BT/firmware updates/first-boot wizard, no wait for usb0
	for s in ModemManager.service bluetooth.service fwupd.service fwupd-refresh.timer \
		systemd-firstboot.service usb-gadget.service usbgadget.service android-tools-adbd.service \
		systemd-networkd-wait-online.service NetworkManager-wait-online.service \
		hwclock.service systemd-hwclock.service apt-daily-upgrade.timer unattended-upgrades.service \
		snapd.service snapd.socket snapd.seeded.service fstrim.timer fstrim.service; do
		sc mask "$s"
	done
	# (fstrim: with no fstab entries it trims everything in mountinfo, the card's exFAT too; after a
	# hard reset a damaged exFAT bitmap could then discard live data of backup/, windows/ or iOS)
	# the old phosh.service (User=1000) and getty on tty1 under the session are not used
	sc disable phosh.service
else
	# OpenRC: busybox feeds the watchdog; local.d runs the phone setup and serves usb0
	mkdir -p /etc/local.d /etc/conf.d
	cat > /etc/local.d/50-starbite.start <<'EOF'
#!/bin/sh
# Starbite (sb-integrate.sh)
/usr/local/sbin/sb-phone-setup &
# usb0 keeps the initrd's address; serve the Mac's lease unless something already does
ifconfig usb0 172.16.42.1 netmask 255.255.255.0 up 2>/dev/null
pidof udhcpd unudhcpd >/dev/null || /usr/local/lib/starbite/busybox udhcpd /etc/starbite/udhcpd.conf
# sshd (the sshd init script ran earlier, and makes keys too): when it is not up, e.g. its "need net"
# failed with nothing providing net, host keys and sshd here (it binds every address: usb0 too)
if [ -x /usr/sbin/sshd ] && ! pidof sshd >/dev/null; then
	/usr/local/sbin/sb-ssh-keys
	rc-service sshd start >/dev/null 2>&1
	pidof sshd >/dev/null || /usr/sbin/sshd
fi
# watchdog only when the entry passed sb.wdt=N, N > 0 (the last value wins; the initrd armed it
# with the same N, at least 30 s): fed every 10 s. sb.wdt=0 or none: never touched.
w=0
for a in $(cat /proc/cmdline); do case $a in sb.wdt=*) w=${a#sb.wdt=} ;; esac; done
case $w in ""|*[!0-9]*|?????*) w=0 ;; esac
if [ "$w" -gt 0 ] && [ -c /dev/watchdog ] && ! pidof watchdog >/dev/null; then
	[ "$w" -lt 30 ] && w=30
	m=$(cat /sys/class/watchdog/watchdog0/max_timeout 2>/dev/null)
	[ -n "$m" ] && [ "$m" -gt 0 ] && [ "$w" -gt "$m" ] && w=$m
	/usr/local/lib/starbite/busybox watchdog -t 10 -T "$w" /dev/watchdog
fi
exit 0
EOF
	chmod 755 /etc/local.d/50-starbite.start
	printf 'start 172.16.42.2\nend 172.16.42.2\ninterface usb0\nlease_file /run/udhcpd.usb0.leases\noption subnet 255.255.255.0\noption lease 86400\n' \
		> /etc/starbite/udhcpd.conf
	have rc-update && { rc-update add local default >/dev/null 2>&1 || true; }
	for s in usb-signaller modemmanager bluetooth hwclock swclock; do
		have rc-update && { rc-update del "$s" default >/dev/null 2>&1 || true; rc-update del "$s" boot >/dev/null 2>&1 || true; }
	done
	# no fstrim (see the systemd block): drop a periodic one if the distro ships it
	rm -f /etc/periodic/*/fstrim /etc/cron.*/fstrim 2>/dev/null || true
	# sshd (see the sshd block above): its init script makes missing keys (ssh-keygen -A) itself
	if [ -x /usr/sbin/sshd ] && [ -e /etc/init.d/sshd ]; then
		have rc-update && { rc-update add sshd default >/dev/null 2>&1 || true; }
	fi
	if [ -f /etc/inittab ] && ! grep -q ttyGS0 /etc/inittab; then
		echo 'ttyGS0::respawn:/sbin/getty -L 115200 ttyGS0 vt100' >> /etc/inittab
	fi
fi

# ---- the graphical session (systemd distros with Phosh, GNOME or Lomiri) ------------------------
# One unit, starbite-session.service (the shape of Debian's phosh.service: a logind session on
# tty7 for "user", autologin; the lock screen asks for the PIN). Which shell runs is chosen per
# boot by sb.session= in the catalog entry's cmdline, so one image serves several catalog entries.
if [ "$INITSYS" = systemd ] && { have phosh-session || have gnome-session || have lomiri-session; }; then
	mkdir -p /usr/local/bin
	cat > /usr/local/bin/sb-session <<'EOF'
#!/bin/sh
# Starbite (sb-integrate.sh): the session named by sb.session= on the kernel command line (the
# catalog entry), else /etc/starbite/session, else phosh. phosh | gnome | lomiri.
s=
for a in $(cat /proc/cmdline); do case $a in sb.session=*) s=${a#sb.session=} ;; esac; done
[ -n "$s" ] || s=$(cat /etc/starbite/session 2>/dev/null)
have() { command -v "$1" >/dev/null 2>&1; }
case $s in
gnome)
	if have gnome-session; then
		sess=gnome
		[ -f /usr/share/gnome-session/sessions/ubuntu.session ] && sess=ubuntu
		export XDG_SESSION_DESKTOP=$sess
		[ $sess = ubuntu ] && export XDG_CURRENT_DESKTOP=ubuntu:GNOME GNOME_SHELL_SESSION_MODE=ubuntu ||
			export XDG_CURRENT_DESKTOP=GNOME
		exec gnome-session --session=$sess
	fi ;;
lomiri)
	if have lomiri-session; then
		export XDG_CURRENT_DESKTOP=Lomiri XDG_SESSION_DESKTOP=lomiri GRID_UNIT_PX=24   # 3 x 8 px
		exec lomiri-session
	fi ;;
esac
export XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_DESKTOP=phosh
exec phosh-session
EOF
	chmod 755 /usr/local/bin/sb-session
	cat > /etc/systemd/system/starbite-session.service <<EOF
[Unit]
Description=Starbite graphical session (sb.session=: phosh, gnome, lomiri)
Conflicts=getty@tty1.service
After=getty@tty1.service systemd-user-sessions.service plymouth-quit-wait.service rc-local.service
After=dbus.socket
Wants=dbus.socket
Before=graphical.target
ConditionPathExists=/dev/tty0
OnFailure=getty@tty1.service

[Service]
Environment=XDG_SESSION_TYPE=wayland
ExecStart=/usr/local/bin/sb-session
ExecStartPost=+-chvt 7
TimeoutStartSec=30
User=$SB_USER
PAMName=login
WorkingDirectory=~
Restart=always
RestartSec=5s
TTYPath=/dev/tty7
TTYReset=yes
TTYVHangup=yes
TTYVTDisallocate=yes
StandardInput=tty-fail
StandardOutput=journal
StandardError=journal
UtmpIdentifier=tty7
UtmpMode=user

[Install]
WantedBy=graphical.target
EOF
	sc enable starbite-session.service
	for s in greetd.service gdm.service gdm3.service lightdm.service sddm.service display-manager.service; do sc disable "$s"; done
	rm -f /etc/systemd/system/display-manager.service
	sc set-default graphical.target
fi

# ---- integer scale 3 (1440 x 2960 panel, 570 ppi) ------------------------------------------------
if [ -f /usr/share/phosh/phoc.ini ] || have phoc; then
	mkdir -p /etc/phosh
	[ -f /etc/phosh/phoc.ini ] || { [ -f /usr/share/phosh/phoc.ini ] && cp /usr/share/phosh/phoc.ini /etc/phosh/phoc.ini; } || : > /etc/phosh/phoc.ini
	sed -i '/^# Starbite scale/,$d' /etc/phosh/phoc.ini
	# DSI-1 is the s9p-decon connector name (check with wlr-randr on the first boot)
	printf '# Starbite scale (sb-integrate.sh)\n[output:DSI-1]\nscale = 3\n' >> /etc/phosh/phoc.ini
fi

{
	echo "name=$NAME"
	echo "init=$INITSYS"
	echo "user=$SB_USER uid=$SB_ID"
	echo "integrated=$(date -u +%FT%TZ)"
} > /etc/starbite/integrated
say "done: starbite-$NAME ($INITSYS), user $SB_USER ($SB_ID)"
