#!/bin/sh
# sbgw with a real Linux guest: in the VM, as root. A network namespace "sbguest" is the guest (Linux's own stack,
# busybox udhcpc, ping, nslookup, curl); its NIC is the TAP sbg0, whose other end is host/gw-tap (sbgw + sbnet).
# gw-tap's uplinks: the TAP sbtap0 (Ethernet, as Wi-Fi: 10.77.0.2/24, Linux 10.77.0.1) and the TUN sbtun0 (raw IP,
# as mobile data: 10.78.0.2, MTU 1400); Linux forwards and masquerades both to the Internet. Everything added is
# removed at the end (the devices, the namespace, /etc/netns/sbguest, two MASQUERADE rules, ip_forward).
#   limactl shell starbite -- sudo sh starbite/wifi/host/run-gw-netns.sh
#   GW_BIG=<url>: the large download (default: 25 MB from speed.cloudflare.com)
#   GW_NICE=-10: the gateway, the local server and curl at this priority (a busy VM otherwise sets the speed)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
OUT=${OUT:-"$here/out/gwns"}
mkdir -p "$OUT"
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -I"$core" -o "$OUT/gw-tap" "$core/gw.c" "$core/net.c" "$here/gw-tap.c"
big=${GW_BIG:-"https://speed.cloudflare.com/__down?bytes=25000000"}
dns=$(resolvectl status 2>/dev/null | sed -n 's/.*Current DNS Server: //p' | head -1)
[ -n "$dns" ] || dns=$(resolvectl status 2>/dev/null | sed -n 's/.*DNS Servers: //p' | head -1 | cut -d' ' -f1)
dns=${dns:-1.1.1.1}
fwd=$(sysctl -n net.ipv4.ip_forward)
fails=0
gwpid=; srvpid=
cleanup() {
    [ -n "$gwpid" ] && kill -TERM "$gwpid" 2>/dev/null && sleep 0.5
    [ -n "$srvpid" ] && kill "$srvpid" 2>/dev/null
    ip netns exec sbguest ip link del sbg0 2>/dev/null || true
    ip netns del sbguest 2>/dev/null || true
    ip link del sbg0 2>/dev/null || true
    ip link del sbtap0 2>/dev/null || true
    ip link del sbtun0 2>/dev/null || true
    iptables -t nat -D POSTROUTING -s 10.77.0.0/24 ! -d 10.77.0.0/24 -j MASQUERADE 2>/dev/null || true
    iptables -t nat -D POSTROUTING -s 10.78.0.0/24 ! -d 10.78.0.0/24 -j MASQUERADE 2>/dev/null || true
    sysctl -qw net.ipv4.ip_forward="$fwd"
    rm -rf /etc/netns/sbguest
}
trap cleanup EXIT
ok() { echo "gw-netns: ok   $*"; }
bad() { echo "gw-netns: FAIL $*"; fails=$((fails + 1)); }
G() { nice -n "${GW_NICE:--10}" ip netns exec sbguest "$@"; }

ip tuntap add dev sbtap0 mode tap
ip tuntap add dev sbtun0 mode tun
ip tuntap add dev sbg0 mode tap
ip addr add 10.77.0.1/24 dev sbtap0 && ip link set sbtap0 up
ip addr add 10.78.0.1 peer 10.78.0.2 dev sbtun0 && ip link set sbtun0 mtu 1400 up
sysctl -qw net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s 10.77.0.0/24 ! -d 10.77.0.0/24 -j MASQUERADE
iptables -t nat -A POSTROUTING -s 10.78.0.0/24 ! -d 10.78.0.0/24 -j MASQUERADE
ip netns add sbguest
# the guest's resolver: plain DNS from /etc/resolv.conf (not the host's systemd-resolved over its socket)
mkdir -p /etc/netns/sbguest
sed 's/^hosts:.*/hosts: files dns/' /etc/nsswitch.conf > /etc/netns/sbguest/nsswitch.conf
: > /etc/netns/sbguest/resolv.conf
# a local bulk file behind the NAT (the NAT's own speed, without the Internet's)
head -c 67108864 /dev/zero > "$OUT/bulk.bin"
nice -n "${GW_NICE:--10}" python3 -m http.server 7004 --bind 10.77.0.1 --directory "$OUT" >/dev/null 2>&1 & srvpid=$!
rm -f "$OUT/ready"
nice -n "${GW_NICE:--10}" "$OUT/gw-tap" sbg0 sbtap0 sbtun0 "$dns" "$OUT/ready" > "$OUT/gw-tap.log" 2>&1 & gwpid=$!
i=0; while [ ! -f "$OUT/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
ip link set sbg0 netns sbguest
G ip link set lo up
G ip link set sbg0 up
sleep 1

# 1. DHCP (busybox udhcpc; its script keeps what it got)
cat > "$OUT/udhcpc.sh" <<EOF
#!/bin/sh
case "\$1" in bound|renew)
    ip addr flush dev "\$interface"
    ip addr add "\$ip/\$mask" dev "\$interface"
    ip route replace default via "\$router" dev "\$interface"
    echo "\$ip \$mask \$router \$dns \$lease" > "$OUT/lease" ;;
esac
EOF
chmod +x "$OUT/udhcpc.sh"
rm -f "$OUT/lease"
t=$(date +%s%N)
G busybox udhcpc -i sbg0 -n -q -f -t 5 -T 2 -s "$OUT/udhcpc.sh" >/dev/null 2>&1 || true
t=$(( ($(date +%s%N) - t) / 1000000 ))
lease=$(cat "$OUT/lease" 2>/dev/null || true)
if [ "$lease" = "10.0.2.15 24 10.0.2.2 10.0.2.3 86400" ]; then ok "DHCP: 10.0.2.15/24, router 10.0.2.2, DNS 10.0.2.3, 1 day ($t ms)"
else bad "DHCP: got '$lease'"; fi
echo "nameserver 10.0.2.3" > /etc/netns/sbguest/resolv.conf

# 2. pings: the gateway, then through both NATs to the Internet
if r=$(G ping -c 3 -i 0.2 -W 2 10.0.2.2 2>&1); then ok "ping 10.0.2.2: $(echo "$r" | tail -1)"; else bad "ping 10.0.2.2"; fi
if r=$(G ping -c 3 -i 0.2 -W 2 1.1.1.1 2>&1); then ok "ping 1.1.1.1 through the NAT: $(echo "$r" | tail -1)"
else echo "gw-netns: note ping 1.1.1.1 failed (the VM's own NAT may not carry ICMP): $(echo "$r" | tail -2 | head -1)"; fi
if r=$(G ping -c 2 -W 2 10.77.0.1 2>&1); then ok "ping 10.77.0.1 (the uplink's router) through the NAT"; else bad "ping 10.77.0.1"; fi

# 3. DNS through 10.0.2.3 (UDP, and the guest's own resolver through nsswitch)
if r=$(G busybox nslookup example.com 10.0.2.3 2>&1) && echo "$r" | grep -q "Address.*[0-9]\.[0-9]"; then
    ok "DNS: nslookup example.com via 10.0.2.3"
else bad "DNS: nslookup: $r"; fi
if r=$(G getent ahostsv4 example.com 2>&1) && [ -n "$r" ]; then ok "DNS: getent (glibc's resolver, /etc/resolv.conf 10.0.2.3)"
else bad "DNS: getent"; fi

curl_stat() { # url: "code bytes MiB/s seconds"
    G curl -sS -o /dev/null --max-time "${2:-120}" -w '%{http_code} %{size_download} %{speed_download} %{time_total}' "$1"
}
mib() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }
phase() { # name
    if r=$(curl_stat https://example.com/ 30 2>&1) && [ "${r%% *}" = 200 ]; then ok "$1: HTTPS example.com: status 200, ${r#* }"
    else bad "$1: HTTPS example.com: $r"; fi
    if r=$(curl_stat http://10.77.0.1:7004/bulk.bin 2>&1) && [ "$(echo "$r" | cut -d' ' -f2)" = 67108864 ]; then
        ok "$1: 64 MiB from 10.77.0.1 through the NAT: $(mib "$(echo "$r" | cut -d' ' -f3)") MiB/s"
    else bad "$1: local 64 MiB: $r"; fi
    if r=$(curl_stat "$big" 2>&1) && [ "${r%% *}" = 200 ]; then
        ok "$1: $(mib "$(echo "$r" | cut -d' ' -f2)") MiB over HTTPS from the Internet: $(mib "$(echo "$r" | cut -d' ' -f3)") MiB/s"
    else bad "$1: the large download: $r"; fi
}

# 4. HTTPS and the downloads on the Wi-Fi-like uplink
phase "Wi-Fi uplink (Ethernet, MTU 1500)"

# 5. mobile data: the switch resets what was open; then the same through a 1400 MTU (the MSS clamp)
kill -USR1 "$gwpid"; sleep 0.3
if r=$(G ping -c 2 -W 2 10.78.0.1 2>&1); then ok "mobile uplink: ping its router"; else bad "mobile uplink: ping"; fi
if r=$(G busybox nslookup example.com 10.0.2.3 2>&1) && echo "$r" | grep -q "Address.*[0-9]\.[0-9]"; then
    ok "mobile uplink: DNS"; else bad "mobile uplink: DNS: $r"; fi
phase "mobile uplink (raw IP, MTU 1400)"
# a connection open across a switch: reset at once, not hanging
# (no --limit-rate: curl would sleep through the reset; the 64 MiB take far longer than the 1.5 s before the switch)
G curl -sS -o /dev/null --max-time 60 http://10.77.0.1:7004/bulk.bin > "$OUT/switch.out" 2>&1 & cpid=$!
sleep 1.5
open=$(G ss -Htn state established '( dport = :7004 )' | wc -l)
t=$(date +%s%N)
kill -USR2 "$gwpid"
wait "$cpid" 2>/dev/null && rc=0 || rc=$?
t=$(( ($(date +%s%N) - t) / 1000000 ))
left=$(G ss -Htn state established '( dport = :7004 )' | wc -l)
if [ "$rc" -ne 0 ] && [ "$open" -eq 1 ] && [ "$left" -eq 0 ] && [ "$t" -lt 3000 ]; then
    ok "switch to Wi-Fi mid-download: the guest's connection reset, curl ended after $t ms ($(tr -d '\n' < "$OUT/switch.out"))"
else bad "switch mid-download: curl rc $rc after $t ms, connections open before $open, after $left"; fi

# 6. no uplink: connects fail at once
kill -HUP "$gwpid"; sleep 0.3
t=$(date +%s%N)
r=$(G curl -sS -o /dev/null --max-time 20 http://203.0.113.1/ 2>&1) && rc=0 || rc=$?
t=$(( ($(date +%s%N) - t) / 1000000 ))
if [ "$rc" -ne 0 ] && [ "$t" -lt 2000 ]; then ok "no uplink: curl failed in $t ms ($r)"; else bad "no uplink: rc $rc after $t ms"; fi
kill -USR2 "$gwpid"; sleep 0.3
if r=$(curl_stat https://example.com/ 30 2>&1) && [ "${r%% *}" = 200 ]; then ok "Wi-Fi back: HTTPS example.com"; else bad "Wi-Fi back: $r"; fi

kill -TERM "$gwpid"; wait "$gwpid" 2>/dev/null || true; gwpid=
grep -v '^gw-tap: gw: \|^gw-tap: net: ' "$OUT/gw-tap.log" || true
echo "gw-netns: $fails failed"
[ "$fails" -eq 0 ]
