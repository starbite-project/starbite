#!/usr/bin/env bash
# Ubuntu 26.04 LTS root for the S9, as an ext4 image for the card (/distros/ubuntu.img on the exFAT
# p1). Runs in the arm64 Lima VM (native chroot):
#   limactl shell starbite -- bash -lc 'cd ~/distros && sudo /Users/dev/project-starbite/tools/linux/distros/mkroot-ubuntu.sh'
# Input: ubuntu-26.04-root.tar (mmdebstrap, packages in NOTES.md). Login: user starbite, PIN in
# build/linux/ubuntu-login.txt (git-ignored, made on first run), SSH key build/linux/ssh.
set -euo pipefail
P=/Users/dev/project-starbite
TAR=${TAR:-ubuntu-26.04-root.tar}
IMG=${IMG:-ubuntu.img}
SIZE=${SIZE:-10G}
R=$(mktemp -d /var/tmp/ubuntu-root.XXXX)
trap 'umount -R "$R" 2>/dev/null || true' EXIT
tar -xpf "$TAR" -C "$R"
PIN_FILE=$P/build/linux/ubuntu-login.txt
[ -s "$PIN_FILE" ] || printf 'user starbite\npin %06d\n' $(( $(od -An -N4 -tu4 /dev/urandom) % 1000000 )) > "$PIN_FILE"
PIN=$(awk '/^pin/ {print $2}' "$PIN_FILE")

echo starbite-ubuntu > "$R/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 starbite-ubuntu\n' > "$R/etc/hosts"
: > "$R/etc/fstab"                                    # the initramfs mounts / (tools/linux/init)
mount --bind /dev "$R/dev"; mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"
cp /etc/resolv.conf "$R/etc/resolv.conf"
chroot "$R" /bin/bash -e <<CH
ln -sf /usr/share/zoneinfo/Europe/Helsinki /etc/localtime
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen; locale-gen >/dev/null; echo LANG=en_US.UTF-8 > /etc/default/locale
id starbite >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo,video,audio,input,render,netdev,plugdev starbite
echo "starbite:$PIN" | chpasswd
passwd -l root >/dev/null
mkdir -p /etc/greetd
printf '[terminal]\nvt = 1\n\n[default_session]\ncommand = "agreety --cmd phosh-session"\nuser = "_greetd"\n\n[initial_session]\ncommand = "phosh-session"\nuser = "starbite"\n' > /etc/greetd/config.toml
systemctl enable greetd NetworkManager ssh >/dev/null 2>&1
systemctl disable systemd-networkd-wait-online NetworkManager-wait-online >/dev/null 2>&1 || true
systemctl set-default graphical.target >/dev/null
# the internal UFS (Android, iOS, Windows) never shows up in Files
printf 'SUBSYSTEM=="block", KERNEL=="sd*", ENV{UDISKS_IGNORE}="1"\n' > /etc/udev/rules.d/90-starbite-ufs.rules
# phone scaling for the 1440 x 2960 panel
mkdir -p /etc/phosh; printf '[output:DSI-1]\nscale = 3\n' > /etc/phosh/phoc.ini
CH
install -d -m700 -o 1000 -g 1000 "$R/home/starbite/.ssh"
install -m600 -o 1000 -g 1000 "$P/build/linux/ssh/starbite_ed25519.pub" "$R/home/starbite/.ssh/authorized_keys"
rm -f "$R/etc/resolv.conf"; ln -s ../run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"
umount -R "$R/dev" "$R/proc" "$R/sys" 2>/dev/null || true
rm -f "$IMG"; truncate -s "$SIZE" "$IMG"
mkfs.ext4 -q -L ubuntu_root -d "$R" "$IMG"
rm -rf "$R"
echo "$IMG: $(du -h --apparent-size "$IMG" | cut -f1) ext4 (label ubuntu_root), $(du -h "$IMG" | cut -f1) used"
