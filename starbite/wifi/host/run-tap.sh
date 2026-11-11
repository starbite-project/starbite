#!/bin/sh
# The phone's stack (sbnet, sbtcp, netio, sbhttp, BearSSL) against Linux through a TAP device: in the VM, as
# root. It adds the TAP sbtap0 (10.77.0.1/24 on Linux's side), turns on forwarding and one MASQUERADE rule for
# 10.77.0.0/24 for the live part, and undoes all three at the end.
#   limactl shell starbite -- sudo sh starbite/wifi/host/run-tap.sh [live]     TAP_LOSS=0.05: drop 5% each way
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
OUT=${OUT:-"$here/out/tap"}
mkdir -p "$OUT"
br="$core/tls/bearssl"
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I$core -I$br/inc"
brflags="-std=gnu11 -O1 -g -w -fsanitize=address,undefined -I$br/inc -I$br/src -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_GETENTROPY=0 -DBR_RDRAND=0"
objs=""
for f in $(cat "$br/FILES" "$br/FILES.test"); do
    o="$OUT/br_$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    [ "$o" -nt "$br/src/$f" ] || gcc $brflags -c "$br/src/$f" -o "$o"
    objs="$objs $o"
done
# shellcheck disable=SC2086
gcc $flags -o "$OUT/tap-test" "$core/net.c" "$core/tcp.c" "$core/netio.c" "$core/http.c" "$core/tls/ta.c" \
    "$here/tap-test.c" $objs
d="$here/out/http-pki"
[ -f "$d/ca.der" ] || { echo "run-tap: make the test CA first (sh host/run-http.sh on the Mac)" >&2; exit 1; }
dns=$(resolvectl status 2>/dev/null | sed -n 's/.*Current DNS Server: //p' | head -1)
[ -n "$dns" ] || dns=$(resolvectl status 2>/dev/null | sed -n 's/.*DNS Servers: //p' | head -1 | cut -d' ' -f1)
fwd=$(sysctl -n net.ipv4.ip_forward)
ip tuntap add dev sbtap0 mode tap
cleanup() {
    kill "$s1" "$s2" "$s3" 2>/dev/null || true
    iptables -t nat -D POSTROUTING -s 10.77.0.0/24 ! -d 10.77.0.0/24 -j MASQUERADE 2>/dev/null || true
    sysctl -qw net.ipv4.ip_forward="$fwd"
    ip link del sbtap0 2>/dev/null || true
}
s1=; s2=; s3=
trap cleanup EXIT
ip addr add 10.77.0.1/24 dev sbtap0
ip link set sbtap0 up
sysctl -qw net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s 10.77.0.0/24 ! -d 10.77.0.0/24 -j MASQUERADE
python3 "$here/bulk-serve.py" 10.77.0.1 7001 & s1=$!
SBHTTP_BIND=10.77.0.1 python3 "$here/http-serve.py" 7002 & s2=$!
SBHTTP_BIND=10.77.0.1 python3 "$here/http-serve.py" 7003 --tls "$d/srv.pem" "$d/srv.key" & s3=$!
sleep 1
"$OUT/tap-test" sbtap0 "${dns:-1.1.1.1}" 7001 7002 7003 "$d/ca.der" ${1:-}
