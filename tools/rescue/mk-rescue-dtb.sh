#!/bin/bash
# Project Starbite: rescue.dtb, the daily S9 DTB with USB host mode (docs/RESCUE-USB-HOST.md).
#
#   mk-rescue-dtb.sh exynos9810-starlte.dtb rescue.dtb      (in the Lima VM: needs fdtput, dtc)
#
# tools/linux/build-kernel.sh runs it after every build, next to linux-old.dtb. The input must be a
# DTB of the same build as the kernel it will boot with (its usb-otg-vbus node and the drivers that
# read the properties below came in together). Exactly these properties change, nothing else:
#   /soc/usb@10c00000                     dr_mode "peripheral" -> "otg", usb-role-switch,
#                                         role-switch-default-mode "host": dwc3 starts as a host
#                                         and /sys/class/usb_role/*/role can still pick "device"
#                                         (the rescue init's Volume Down serial shell)
#   .../charger@69/usb-otg-vbus           status "disabled" -> "okay": max77705_charger registers
#                                         the OTG boost regulator
#   .../muic@25                           starbite,usb-role "host": s9p-max77705-muic drives VBUS
#                                         from the Type-C state
#   /chosen bootargs                      loglevel=4 -> loglevel=7: the dwc3/xHCI/OTG/MUIC lines
#                                         and the USB enumeration reach tty0 and ramoops
#                                         (console=ramoops-1), readable from TWRP afterwards
# The initrd placeholders and CMA stay as they are (check-kernel.py dtb --usb-host).
# RESCUE_EXTRA_ARGS (environment) is appended to the bootargs: the store path's command line is
# fixed ("sb.rescue=1"), so a test variant is a second DTB, e.g. for the first hardware test
#   RESCUE_EXTRA_ARGS="s9p_max77705_muic.vbus=2" mk-rescue-dtb.sh IN.dtb rescue-vbus-forced.dtb
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 IN.dtb OUT.dtb" >&2; exit 2; }
IN=$1 OUT=$2
USB=/soc/usb@10c00000
OTG=/soc/i2c@14360000/charger@69/usb-otg-vbus
MUIC=/soc/i2c@14360000/muic@25

for t in fdtput fdtget dtc; do
	command -v $t >/dev/null || { echo "missing $t" >&2; exit 1; }
done
[ "$(fdtget -t s "$IN" $USB dr_mode)" = peripheral ] || { echo "$IN: $USB dr_mode is not peripheral" >&2; exit 1; }
[ "$(fdtget -t s "$IN" $OTG status)" = disabled ] || { echo "$IN: no disabled $OTG (DTB older than the OTG driver?)" >&2; exit 1; }
fdtget -t x "$IN" $MUIC vbus-supply >/dev/null || { echo "$IN: $MUIC has no vbus-supply" >&2; exit 1; }

cp "$IN" "$OUT.part"
fdtput -t s "$OUT.part" $USB dr_mode otg
fdtput -t s "$OUT.part" $USB usb-role-switch ""
fdtput -t s "$OUT.part" $USB role-switch-default-mode host
fdtput -t s "$OUT.part" $OTG status okay
fdtput -t s "$OUT.part" $MUIC starbite,usb-role host
ARGS=$(fdtget -t s "$IN" /chosen bootargs)
case " $ARGS " in *" loglevel=4 "*) ;; *) echo "$IN: bootargs lack loglevel=4: $ARGS" >&2; rm -f "$OUT.part"; exit 1 ;; esac
fdtput -t s "$OUT.part" /chosen bootargs \
	"$(echo "$ARGS" | sed 's/\bloglevel=4\b/loglevel=7/')${RESCUE_EXTRA_ARGS:+ $RESCUE_EXTRA_ARGS}"

# 9 lines of a dts diff: dr_mode, status and bootargs changed (2 each), 3 properties added
N=$(diff <(dtc -q -I dtb -O dts "$IN") <(dtc -q -I dtb -O dts "$OUT.part") | grep -c '^[<>]' || true)
[ "$N" = 9 ] || { echo "rescue DTB differs in $N lines, expected 9" >&2; rm -f "$OUT.part"; exit 1; }
mv "$OUT.part" "$OUT"
echo "$OUT: rescue DTB (USB host, OTG VBUS) from $IN"
