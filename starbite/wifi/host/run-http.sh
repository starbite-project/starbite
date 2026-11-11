#!/bin/sh
# sbhttp's host test: BearSSL (build/bearssl-src, the pinned commit) + http.c + tls/ta.c against
# host/http-serve.py (plain and TLS, with a throw-away test CA), on the host's sockets.
#   sh starbite/wifi/host/run-http.sh            SBHTTP_LIVE=1: real sites too   SBHTTP_VERBOSE=1: sbhttp's log
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
: "${CC:=cc}"
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
br="$core/tls/bearssl"
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I$core -I$br/inc"
# BearSSL as vendored (its own warnings are not ours); the same config as on the phone
brflags="-std=gnu11 -O1 -g -w -fsanitize=address,undefined -I$br/inc -I$br/src -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_GETENTROPY=0 -DBR_RDRAND=0"
objs=""
for f in $(cat "$br/FILES" "$br/FILES.test"); do
    o="$OUT/br_$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    [ "$o" -nt "$br/src/$f" ] || $CC $brflags -c "$br/src/$f" -o "$o"
    objs="$objs $o"
done
# shellcheck disable=SC2086
$CC $flags -o "$OUT/http-test" "$core/http.c" "$core/tls/ta.c" "$here/http-test.c" $objs
d="$OUT/http-pki"
mkdir -p "$d"
if [ ! -f "$d/ca.der" ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out "$d/ca.key" 2>/dev/null
    openssl req -x509 -new -key "$d/ca.key" -subj "/CN=sbhttp test CA" -days 3650 -out "$d/ca.pem" \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign" 2>/dev/null
    openssl x509 -in "$d/ca.pem" -outform DER -out "$d/ca.der"
    openssl ecparam -name prime256v1 -genkey -noout -out "$d/srv.key" 2>/dev/null
    openssl req -new -key "$d/srv.key" -subj "/CN=localhost" -out "$d/srv.csr" 2>/dev/null
    printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$d/ext.cnf"
    openssl x509 -req -in "$d/srv.csr" -CA "$d/ca.pem" -CAkey "$d/ca.key" -CAcreateserial -days 3650 \
        -extfile "$d/ext.cnf" -out "$d/srv.pem" 2>/dev/null
fi
p1=$((20000 + $$ % 20000)); p2=$((p1 + 1))
python3 "$here/http-serve.py" $p1 & s1=$!
python3 "$here/http-serve.py" $p2 --tls "$d/srv.pem" "$d/srv.key" & s2=$!
trap 'kill $s1 $s2 2>/dev/null' EXIT
sleep 1
"$OUT/http-test" $p1 $p2 "$d/ca.der"
