#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build, sign and check the Starbite Display IddCx driver package and its installer tool on the Mac.
#
#   starbite/link/win/idd/build.sh
#
# Output (git-ignored): build/link/idd/
#   pkg/StarbiteDisplay.dll  UMDF 2.33 + IddCx 1.4 driver, ARM64, signed (Starbite test certificate)
#   pkg/StarbiteDisplay.inf  the INF (copied)
#   pkg/StarbiteDisplay.cat  catalog over the two (makecat.py, hwid root\starbitedisplay), signed
#   sbdevnode.exe            root devnode + driver install tool (devcon "install" replacement), signed
#   test/modes-test, test/edid.bin   host check of the EDID and mode table
#   SHA256SUMS
# Checks: PE machine/subsystem, the FxDriverEntryUm export, the imports, the IddCx class binding string,
# signatures, the DLL against the catalog, the host EDID/mode test. Nothing touches the phone.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
win=$repo/tools/win
out=$repo/build/link/idd
pkg=$out/pkg
mkdir -p "$pkg" "$out/test"
. "$win/env.sh"

# 1. host check of the EDID + modes (fails the build if the EDID is malformed)
clang++ -std=c++17 -Wall -Wextra -Werror -I"$here/src" "$here/test/modes-test.cpp" -o "$out/test/modes-test"
"$out/test/modes-test" "$out/test/edid.bin" | tail -1

# 2. the driver. IddCx 1.4 (Windows 10 1903+) is all this driver uses; 22631 has IddCx 1.9.
#    -Wno-duplicate-decl-specifier: WDF_DECLARE_CONTEXT_TYPE in C++ expands to extern "C" ... extern const (MSVC-ok).
CXXFLAGS="-Wno-duplicate-decl-specifier ${CXXFLAGS:-}" IDDCX=1.4 LIBS="d3d11.lib dxgi.lib avrt.lib" \
    "$win/build.sh" umdf "$pkg/StarbiteDisplay.dll" "$here/src/StarbiteDisplay.cpp"
# 3. the installer tool
LIBS="setupapi.lib newdev.lib" "$win/build.sh" exe "$out/sbdevnode.exe" "$here/tools/sbdevnode.c"
rm -rf "$pkg/StarbiteDisplay.dll.obj" "$out/sbdevnode.exe.obj"

cp "$here/StarbiteDisplay.inf" "$pkg/StarbiteDisplay.inf"
"$win/sign.sh" "$pkg/StarbiteDisplay.dll" "$out/sbdevnode.exe"
# The catalog hashes the signed DLL, so it is made after signing.
python3 "$win/makecat.py" "$pkg/StarbiteDisplay.cat" "$pkg/StarbiteDisplay.inf" "$pkg/StarbiteDisplay.dll" \
    --hwid 'root\starbitedisplay'
"$win/sign.sh" "$pkg/StarbiteDisplay.cat"
crt=${SB_SIGN_CERT:-$repo/build/woa/ufs-sys/sign/starbite-test.crt}
osslsigncode verify -CAfile "$crt" -catalog "$pkg/StarbiteDisplay.cat" -in "$pkg/StarbiteDisplay.dll" 2>&1 |
    grep -q 'Signature verification: ok' && echo "StarbiteDisplay.dll: in the catalog, ok" ||
    { echo "StarbiteDisplay.dll: NOT in the catalog"; exit 1; }
# osslsigncode cannot check a flat file against a catalog (not even Inf2Cat's): look for the INF's flat hashes.
python3 - "$pkg/StarbiteDisplay.cat" "$pkg/StarbiteDisplay.inf" <<'PY'
import hashlib, sys
cat, inf = (open(f, 'rb').read() for f in sys.argv[1:])
ok = all(hashlib.new(a, inf).digest() in cat for a in ('sha1', 'sha256'))
print(f'StarbiteDisplay.inf: SHA-1 + SHA-256 flat hashes {"in the catalog, ok" if ok else "NOT in the catalog"}')
sys.exit(0 if ok else 1)
PY

fail=0
pecheck() {  # file subsystem
    local h m s
    h=$("$SB_READOBJ" --file-headers "$1")
    m=$(grep -m1 'Machine:' <<<"$h" | sed 's/.*Machine: //'); s=$(grep -m1 'Subsystem:' <<<"$h" | sed 's/.*Subsystem: //')
    printf '%-20s %8s bytes  %-36s %s\n' "$(basename "$1")" "$(wc -c < "$1" | tr -d ' ')" "$m" "$s"
    printf '    imports: %s\n' "$("$SB_READOBJ" --coff-imports "$1" | sed -n 's/^ *Name: //p' | tr '\n' ' ')"
    grep -q 'IMAGE_FILE_MACHINE_ARM64' <<<"$m" && grep -q "$2" <<<"$s" || { echo "  ^ WRONG"; fail=1; }
}
echo
pecheck "$pkg/StarbiteDisplay.dll" IMAGE_SUBSYSTEM_WINDOWS_GUI
pecheck "$out/sbdevnode.exe" IMAGE_SUBSYSTEM_WINDOWS_CUI
exports=$("$SB_READOBJ" --coff-exports "$pkg/StarbiteDisplay.dll" | sed -n 's/^ *Name: //p' | tr '\n' ' ')
echo "    StarbiteDisplay.dll exports: $exports"
[ "$exports" = "FxDriverEntryUm " ] || { echo "  ^ expected exactly FxDriverEntryUm"; fail=1; }
# The UMDF stub binds the IddCx class extension by name at FxDriverEntryUm (UTF-16 "IddCx" + "WUDFx02000").
python3 - "$pkg/StarbiteDisplay.dll" <<'EOF' || fail=1
import sys
d = open(sys.argv[1], 'rb').read()
for s in ('IddCx', 'WUDFx02000', 'SbState'):
    ok = s.encode('utf-16-le') in d
    print(f'    string {s!r}: {"present" if ok else "MISSING"}')
    if not ok: sys.exit(1)
EOF
# payload/ = what goes to C:\starbite\idd\ on the phone for install route B (INSTALL.md); CRLF for cmd.exe.
rm -rf "$out/payload"; mkdir -p "$out/payload/pkg"
cp "$pkg"/StarbiteDisplay.{dll,inf,cat} "$out/payload/pkg/"
cp "$out/sbdevnode.exe" "$out/payload/"
sed 's/$/\r/' "$here/install.cmd" > "$out/payload/install.cmd"
openssl x509 -in "$crt" -outform DER -out "$out/payload/starbite-test.cer"   # public certificate only
(cd "$out" && shasum -a 256 pkg/StarbiteDisplay.dll pkg/StarbiteDisplay.inf pkg/StarbiteDisplay.cat sbdevnode.exe \
    payload/install.cmd payload/starbite-test.cer > SHA256SUMS)
echo; cat "$out/SHA256SUMS"
exit $fail
