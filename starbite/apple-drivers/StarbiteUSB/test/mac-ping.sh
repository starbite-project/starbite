#!/bin/sh
# Mac side of the M11 phone test (PHONE-TEST.md): wait for the phone's NCM adapter, then ping the phone.
#   sh starbite/apple-drivers/StarbiteUSB/test/mac-ping.sh [WAIT_S]      (default 70)
# Read-only on the Mac: no sudo, no settings changed, nothing sent but pings on the new interface.
# Always ends within WAIT_S + 15 s, so an unattended watcher is never held by a Mac that did not accept the device.
# Exit 0: ping answered. 2: the USB device never appeared. 3: it appeared but macOS made no network interface
# (accessory approval pending, or no NCM driver bound). 4: interface there but not active / no link-local.
# 5: no answer to ping.
set -u
wait_s=${1:-70}
t0=$(date +%s)
left() { echo $(( wait_s - ($(date +%s) - t0) )); }
usb_seen=0; ifc=""
while [ "$(left)" -gt 0 ]; do
    if [ "$usb_seen" = 0 ] && ioreg -p IOUSB -w0 | grep -q "Starbite S9 network"; then
        usb_seen=1; echo "USB: 'Starbite S9 network (NCM)' is on the bus after $(( $(date +%s) - t0 )) s"
    fi
    for i in $(ifconfig -l); do
        if ifconfig "$i" 2>/dev/null | grep -q "ether 02:53:42:"; then ifc=$i; fi
    done
    [ -n "$ifc" ] && break
    sleep 1
done
if [ -z "$ifc" ]; then
    if [ "$usb_seen" = 0 ]; then echo "FAIL 2: no Starbite NCM device on USB within $wait_s s"; exit 2; fi
    echo "FAIL 3: the USB device is there but macOS made no interface for it:"
    ioreg -p IOUSB -w0 -l -n "Starbite S9 network (NCM)" | grep -E "UsbEnumerationState|registered|matched|idVendor|idProduct|bcdDevice|USB Serial" | head -12
    echo "(a new accessory waits for 'Allow accessory to connect' on the Mac's screen)"
    exit 3
fi
mac=$(ifconfig "$ifc" | awk '/ether/ {print $2}')
echo "interface $ifc, host MAC $mac, after $(( $(date +%s) - t0 )) s"
# the phone's MAC is the host's with bit 0 of the last byte clear; its link-local is the modified EUI-64 of that
IFS=: read -r a b c d e f <<EOF2
$mac
EOF2
f=$(printf '%02x' $(( 0x$f & 0xfe )))
addr=$(printf 'fe80::%x:%sff:fe%s:%s%s' $(( (0x$a ^ 2) * 256 + 0x$b )) "$c" "$d" "$e" "$f")
while [ "$(left)" -gt 0 ]; do
    if ifconfig "$ifc" | grep -q "status: active" && ifconfig "$ifc" | grep -q "inet6 fe80"; then break; fi
    sleep 1
done
if ! ifconfig "$ifc" | grep -q "inet6 fe80"; then
    echo "FAIL 4: $ifc has no link-local address (link not active, or the interface is not configured):"
    ifconfig "$ifc"
    exit 4
fi
echo "ping6 $addr%$ifc"
if ping6 -c 5 -i 0.3 "$addr%$ifc"; then
    echo "PASS: the phone answers ping over NCM ($ifc)"
    exit 0
fi
echo "FAIL 5: no answer; interface counters:"
netstat -I "$ifc" -b | head -3
exit 5
