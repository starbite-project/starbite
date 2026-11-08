#!/usr/bin/env bash
# Sign Windows binaries / catalogs with the Starbite test certificate (embedded Authenticode, SHA-256).
#
#   tools/win/sign.sh FILE...        sign each .sys/.dll/.exe/.cat in place, then verify it against our cert
#   tools/win/sign.sh -v FILE...     verify only
#
# The key and certificate are the ones Windows on the S9 already accepts under testsigning (NOTES.md
# 2026-10-27: the re-signed Exynos9810Ufs.sys boots): self-signed "CN=Starbite Test Signing", CA:TRUE,
# EKU codeSigning, valid 2026-10-26 .. 2036-10-23, SHA-1 thumbprint 9F0C3A3346BAF11DABF4FC2EC001FC6C4034493C.
# They live in build/woa/ufs-sys/sign/ (build/ is git-ignored; never commit or print the key).
# Override with SB_SIGN_KEY / SB_SIGN_CERT. No timestamp (test signing does not need one; nothing leaves the Mac).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
key=${SB_SIGN_KEY:-$repo/build/woa/ufs-sys/sign/starbite-test.key}
crt=${SB_SIGN_CERT:-$repo/build/woa/ufs-sys/sign/starbite-test.crt}
verify_only=0
[ "${1:-}" = -v ] && { verify_only=1; shift; }
[ $# -ge 1 ] || { sed -n '2,12p' "$0"; exit 2; }
[ -r "$crt" ] || { echo "certificate not found: $crt"; exit 1; }
[ "$verify_only" = 1 ] || [ -r "$key" ] || { echo "key not found: $key"; exit 1; }

verify() {
    local log
    log=$(osslsigncode verify -CAfile "$crt" -in "$1" 2>&1) || true
    if grep -q 'Signature verification: ok' <<<"$log" &&
       grep -q 'Subject: /\?CN=Starbite Test Signing' <<<"$log"; then
        echo "$1: signed by CN=Starbite Test Signing, $(grep -m1 'Calculated message digest' <<<"$log" | sed 's/.*: //' | cut -c1-16)..."
    else
        echo "$1: VERIFY FAILED"; grep -E 'digest|verification|Error|error' <<<"$log" | head -6; return 1
    fi
}

for f in "$@"; do
    if [ "$verify_only" = 0 ]; then
        tmp=$(mktemp "$f.sign.XXXXXX")
        # an already-signed file is re-signed from scratch (remove-signature first; a .cat has no "unsigned" form)
        case $f in
        *.cat) cp "$f" "$tmp.in" ;;
        *) osslsigncode remove-signature -in "$f" -out "$tmp.in" >/dev/null 2>&1 || cp "$f" "$tmp.in" ;;
        esac
        rm -f "$tmp"
        osslsigncode sign -h sha256 -certs "$crt" -key "$key" -n "Starbite" -in "$tmp.in" -out "$tmp" >/dev/null
        rm -f "$tmp.in"; mv "$tmp" "$f"
    fi
    verify "$f"
done
