#!/bin/sh
# Host test of kern/usbctl.c (USB Control: the handshake, pairing, the session, its commands) with BearSSL's ECDSA
# verify and P-256 keys and signatures from the Mac's openssl. Run from anywhere; V=1 traces the log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sb=$(cd "$here/../../.." && pwd)
brs="$sb/wifi/tls/bearssl"
T=$(mktemp -d "${TMPDIR:-/tmp}/sbos-usbctl-test.XXXXXX")
export T
openssl ecparam -name prime256v1 -genkey -noout -out "$T/k0.pem" 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out "$T/k1.pem" 2>/dev/null
br=""
for f in $(cat "$brs/FILES"); do br="$br $brs/src/$f"; done
cc -O1 -g -std=gnu11 -w -I"$brs/inc" -I"$brs/src" -c $br 2>/dev/null || true
mv ./*.o "$T/" 2>/dev/null || true
cc -O1 -g -std=gnu11 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -Werror -DSB_HOST -DSBOS_KERNEL -I"$sb/libsb" -I"$here/.." -I"$sb/sbos/sdk" \
    -I"$sb/utility" -I"$sb/guest/sbos" -I"$brs/inc" -c "$here/../usbctl.c" -o "$T/usbctl.o"
cc -O1 -g -std=gnu11 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -Werror -DSB_HOST -DSBOS_KERNEL -I"$sb/libsb" -I"$here/.." -I"$sb/sbos/sdk" \
    -I"$sb/utility" -I"$sb/guest/sbos" -I"$brs/inc" -c "$here/usbctl-test.c" -o "$T/test.o"
cc -O1 -g -std=gnu11 -w -DSB_HOST -I"$sb/libsb" -c "$sb/libsb/str.c" -o "$T/str.o"
cc -o "$T/usbctl-test" "$T"/*.o
rc=0
"$T/usbctl-test" || rc=$?
rm -rf "$T"
exit $rc
