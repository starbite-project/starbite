#!/bin/sh
# bcmw's SAE and AKM 8 four-way handshake against hostap's (host/sae-interop.c says what is checked). hostap (BSD) is
# a reference only, never in git: a checkout in build/hostap-src (git clone https://w1.fi/hostap.git), built here on
# OpenSSL (Homebrew's on the Mac, the system's in the VM).
#   sh starbite/wifi/host/run-sae-interop.sh        HOSTAP=<dir> OPENSSL=<prefix> CC=gcc
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core="$here/.."
root=$(cd "$here/../../.." && pwd)
: "${CC:=cc}"
OUT=${OUT:-"$here/out"}
mkdir -p "$OUT"
hp=${HOSTAP:-}
for c in "$root/build/hostap-src" "$root/../../../build/hostap-src"; do
    if [ -z "$hp" ] && [ -f "$c/src/common/sae.c" ]; then hp=$(cd "$c" && pwd); fi
done
[ -n "$hp" ] || { echo "sae-interop: no hostap checkout (build/hostap-src): git clone https://w1.fi/hostap.git build/hostap-src" >&2; exit 1; }
ssl=${OPENSSL:-}
if [ -z "$ssl" ]; then
    for c in /opt/homebrew/opt/openssl@3 /opt/homebrew/opt/openssl /usr/local/opt/openssl@3 /usr; do
        if [ -f "$c/include/openssl/evp.h" ]; then ssl=$c; break; fi
    done
fi
[ -n "$ssl" ] || { echo "sae-interop: no OpenSSL headers (OPENSSL=<prefix>)" >&2; exit 1; }
br="$core/tls/bearssl"
s="$hp/src"
# hostap's files as they are (their warnings are not ours), the configuration its SAE needs
hdefs="-DCONFIG_SAE -DCONFIG_ECC -DCONFIG_SHA256 -DCONFIG_SHA384 -DCONFIG_SHA512 -DCONFIG_CRYPTO_INTERNAL_NO -DCONFIG_IEEE80211R -DCONFIG_NO_RANDOM_POOL -DOPENSSL_API_COMPAT=0x10100000L"
hflags="-std=gnu11 -O1 -g -w -fsanitize=address,undefined -I$s -I$s/utils -I$ssl/include $hdefs"
hsrc="$s/common/sae.c $s/common/dragonfly.c $s/common/wpa_common.c $s/common/ieee802_11_common.c $s/crypto/crypto_openssl.c
      $s/crypto/sha256-prf.c $s/crypto/sha384-prf.c $s/crypto/sha512-prf.c $s/crypto/sha256-kdf.c $s/crypto/sha384-kdf.c
      $s/crypto/sha512-kdf.c $s/crypto/sha1-prf.c $s/crypto/dh_groups.c
      $s/utils/common.c $s/utils/wpabuf.c $s/utils/wpa_debug.c $s/utils/os_unix.c"
d="$OUT/hostap-$(basename "$CC")"
mkdir -p "$d"
[ "$(cat "$d/flags" 2>/dev/null)" = "$hflags" ] || rm -f "$d"/*.o
echo "$hflags" > "$d/flags"
hobjs=""
for f in $hsrc; do
    o="$d/$(basename "$f" .c).o"
    [ "$o" -nt "$f" ] || $CC $hflags -c "$f" -o "$o"
    hobjs="$hobjs $o"
done
brflags="-std=gnu11 -O1 -g -w -fsanitize=address,undefined -I$br/inc -I$br/src -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_GETENTROPY=0 -DBR_RDRAND=0 -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0"
brout="$OUT/br-$(basename "$CC")"
mkdir -p "$brout"
brobjs=""
for f in $(cat "$br/FILES"); do
    o="$brout/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    [ "$o" -nt "$br/src/$f" ] || $CC $brflags -c "$br/src/$f" -o "$o"
    brobjs="$brobjs $o"
done
# ours with our warnings; the test program with hostap's headers
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$core" -I"$br/inc" -c "$core/sae.c" -o "$d/our-sae.o"
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$core" -c "$core/wpa.c" -o "$d/our-wpa.o"
$CC -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -I"$s" -I"$s/utils" \
    -I"$ssl/include" $hdefs -c "$here/sae-interop.c" -o "$d/sae-interop.o"
# shellcheck disable=SC2086
$CC -fsanitize=address,undefined -o "$OUT/sae-interop" "$d/sae-interop.o" "$d/our-sae.o" "$d/our-wpa.o" $hobjs $brobjs \
    -L"$ssl/lib" -lcrypto
"$OUT/sae-interop"
