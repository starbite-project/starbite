#!/bin/sh
# Run inside the new postmarketOS root (chroot) on the phone: what
# pmbootstrap install would do, plus Starbite specifics.
#   chroot /mnt/root /bin/sh /tmp/pmos-setup.sh   (password in /tmp/pmos-pass)
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
U=user
echo starbite-s9 > /etc/hostname
printf '127.0.0.1\tlocalhost starbite-s9\n::1\t\tlocalhost starbite-s9\n' > /etc/hosts
ln -sf /usr/share/zoneinfo/Europe/Helsinki /etc/localtime

# User 10000 (tinydm autologins this UID into Plasma Mobile), pmOS groups.
id $U >/dev/null 2>&1 || adduser -D -u 10000 -s /bin/sh $U
for g in audio input netdev plugdev video wheel feedbackd seat; do
	addgroup -S $g 2>/dev/null || true
	addgroup $U $g 2>/dev/null || true
done
printf '%s:%s\n' $U "$(cat /tmp/pmos-pass)" | chpasswd
passwd -l root >/dev/null
mkdir -p /etc/doas.d
echo 'permit persist :wheel' > /etc/doas.d/20-wheel.conf
chmod 600 /etc/doas.d/20-wheel.conf

# SSH from the Mac (key in build/linux/ssh on the Mac).
install -d -m 700 -o 10000 -g 10000 /home/$U/.ssh
install -m 600 -o 10000 -g 10000 /tmp/starbite_ed25519.pub /home/$U/.ssh/authorized_keys

# USB network: the Starbite initramfs owns usb0 (172.16.42.1, its udhcpd keeps
# serving the Mac after switch_root, as postmarketOS's own initramfs does).
mkdir -p /etc/NetworkManager/conf.d
printf '[keyfile]\nunmanaged-devices=interface-name:usb0\n' > /etc/NetworkManager/conf.d/50-starbite-usb0.conf

# Internal UFS (iOS, Windows, Android, EFS) is read-only from the initramfs;
# keep it out of the desktop's disk list as well.
mkdir -p /etc/udev/rules.d
echo 'SUBSYSTEM=="block", KERNEL=="sd[a-z]*", ENV{UDISKS_IGNORE}="1"' > /etc/udev/rules.d/90-starbite-ufs.rules

# What pmOS tools read about the device.
mkdir -p /usr/share/deviceinfo
cat > /usr/share/deviceinfo/deviceinfo <<EOD
deviceinfo_format_version="0"
deviceinfo_name="Samsung Galaxy S9 (Project Starbite)"
deviceinfo_manufacturer="Samsung"
deviceinfo_codename="starbite-s9"
deviceinfo_year="2018"
deviceinfo_arch="aarch64"
deviceinfo_chassis="handset"
deviceinfo_keyboard="false"
deviceinfo_external_storage="true"
deviceinfo_screen_width="1440"
deviceinfo_screen_height="2960"
EOD
echo "setup done"
