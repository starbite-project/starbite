#!/bin/sh
# Project Starbite: sample rescue script (tools/rescue/usb/rescue.sh), copied to
# /starbite-rescue/rescue.sh of a rescue USB disk by tools/rescue/mk-rescue-usb.sh.
#
# The rescue ramdisk (tools/rescue/init) runs it with the disk mounted read-only and
#   RESCUE_ROOT  the mount point        RESCUE_DIR  $RESCUE_ROOT/starbite-rescue
#   RESCUE_DEV   the block device       RESCUE_LOG  the ramdisk's log (/run/rescue.log)
# Output goes to the screen. It does three things and writes nothing to the phone:
#   1. prints system information (kernel, memory, battery, USB, storage);
#   2. saves that, the kernel log and the ramdisk log to starbite-rescue/logs/ on this disk
#      (the only write: a short read-write remount of this disk);
#   3. waits 60 s for a key: Volume Up = reboot to TWRP, Volume Down = reboot to Download mode,
#      Power = back to the rescue screen. Reboots go through sb-reboot (USB VBUS off first).
PATH=/bin:/sbin:/usr/bin:/usr/sbin
GPA0_DAT=0x14050024                                # bit 3 Vol up, bit 4 Vol down (active low)
GPA2_DAT=0x14050064                                # bit 4 Power (active low)

key_down() {
	case $1 in
	up) a=$GPA0_DAT; m=0x08 ;;
	down) a=$GPA0_DAT; m=0x10 ;;
	power) a=$GPA2_DAT; m=0x10 ;;
	esac
	v=$(devmem $a 32 2>/dev/null) || return 1
	[ $(( v & m )) -eq 0 ]
}

rd() { cat "$1" 2>/dev/null | tr -d '\0'; }

sysinfo() {
	echo "== Starbite rescue: system information =="
	echo "model:    $(rd /proc/device-tree/model)"
	echo "kernel:   $(uname -r) ($(uname -v))"
	echo "cmdline:  $(rd /proc/cmdline)"
	echo "ramdisk:  $(rd /etc/starbite-rescue)"
	echo "uptime:   $(cut -d' ' -f1 /proc/uptime) s"
	awk '/^MemTotal|^MemAvailable/ {printf "memory:   %s %d MiB\n", $1, $2 / 1024}' /proc/meminfo
	for b in /sys/class/power_supply/*; do
		[ -e "$b/capacity" ] || [ -e "$b/online" ] || continue
		echo "power:    ${b##*/}: capacity $(rd $b/capacity) status $(rd $b/status)" \
			"online $(rd $b/online) voltage_now $(rd $b/voltage_now)"
	done
	for r in /sys/class/usb_role/*/role; do
		[ -r "$r" ] && echo "usb role: $(rd $r)"
	done
	for r in /sys/class/regulator/*; do
		[ "$(rd $r/name)" = usb-otg-vbus ] && echo "usb vbus: $(rd $r/state)"
	done
	for u in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
		[ -e "$u/idVendor" ] || continue
		echo "usb dev:  ${u##*/} $(rd $u/idVendor):$(rd $u/idProduct) $(rd $u/manufacturer) $(rd $u/product) ($(rd $u/speed) Mb/s)"
	done
	echo "rescue disk: $RESCUE_DEV"
	echo "block devices (MiB):"
	awk 'NR > 2 && $4 !~ /[0-9]$/ || $4 ~ /^mmcblk[0-9]+$/ {printf "  %-10s %8d\n", $4, $3 / 1024}' /proc/partitions
}

say() { echo "$*"; echo "<7>rescue.sh: $*" > /dev/kmsg 2>/dev/null; }

printf '\033[2J\033[H'
sysinfo

# 2. the logs, on this disk
stamp=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)-up$(cut -d. -f1 /proc/uptime)
if mount -o remount,rw "$RESCUE_ROOT" 2>/dev/null; then
	d=$RESCUE_DIR/logs
	mkdir -p "$d"
	{
		sysinfo
		echo
		echo "== dmesg =="
		dmesg
		echo
		echo "== $RESCUE_LOG =="
		cat "$RESCUE_LOG" 2>/dev/null
	} > "$d/rescue-$stamp.txt" 2>&1
	sync
	mount -o remount,ro "$RESCUE_ROOT"
	say "Log saved: starbite-rescue/logs/rescue-$stamp.txt"
else
	say "This disk is read-only: no log saved"
fi

# 3. what next
echo
say "Volume Up: reboot to TWRP"
say "Volume Down: reboot to Download mode"
say "Power: back to the rescue screen (or wait 60 s)"
i=0
while [ $i -lt 300 ]; do
	if key_down up; then
		sb-reboot twrp
	elif key_down down; then
		sb-reboot download
	elif key_down power; then
		while key_down power; do sleep 0.1; done    # let go, or the init reads it as "reboot"
		break
	fi
	sleep 0.2
	i=$((i + 1))
done
exit 0
