#!/usr/bin/env bash
# Plan H5 (KC link test), host only. Reproduces build/kc-test/results/:
#  1. macOS 15.6 (24G84) M1 kernelcache from the UniversalMac IPSW (one ~28 MiB member via remotezip),
#     unwrapped with kmutil emit-macho;
#  2. apfs.kext's imports recovered from the prelinked fileset (fileset-imports.py) for apfs and its
#     whole OSBundleLibraries closure;
#  3. each import checked against our kernel's exports / KPI symbol sets (check-imports.py);
#  4. instruction census (PAC / LSE / LDAPR) of the closure.
# Big intermediates (the .kc, the im4p) are deleted at the end unless KEEP=1.
# Apple binaries stay in build/ (gitignored), never in git.
set -euo pipefail
cd "$(dirname "$0")/../.."
KERNEL=${KERNEL:-out/kernel.development.starlte-11417}
W=build/kc-test; I=$W/ipsw-15.6; R=$W/results
mkdir -p "$I" "$R"
U=https://updates.cdn-apple.com/2025SummerFCS/fullrestores/082-08674/51294E4D-A273-44BE-A280-A69FC347FB87/UniversalMac_15.6_24G84_Restore.ipsw
[ -f "$I/boot-15.6-mac13g.kc" ] || {
    [ -f "$I/kernelcache.release.mac13g" ] || python3 -I ref/ios-herolte/tools/remotezip.py "$U" kernelcache.release.mac13g "$I"
    kmutil emit-macho -B "$PWD/$I/kernelcache.release.mac13g" --no-variant-extension
    mv /tmp/KMUtilProducts/BootKernelCollection.kc "$I/boot-15.6-mac13g.kc"; rmdir /tmp/KMUtilProducts || true
}
KC=$I/boot-15.6-mac13g.kc
python3 -I tools/kc/fileset-imports.py "$KC" --symbolsets "$I/symbolsets-15.6.plist"
tag=$(basename "$KERNEL" | sed 's/^kernel\.development\.starlte-//')
for b in com.apple.filesystems.apfs com.apple.iokit.IOStorageFamily com.apple.security.AppleImage4 \
         com.apple.kec.corecrypto com.apple.kext.CoreTrust com.apple.iokit.IOCryptoAcceleratorFamily \
         com.apple.driver.AppleEffaceableStorage com.apple.iokit.CoreAnalyticsFamily; do
    s=${b##*.}
    python3 -I tools/kc/fileset-imports.py "$KC" "$b" "$I/$s-15.6-imports.json"
    printf '%s vs %s: ' "$s" "$tag"
    python3 -I tools/kc/check-imports.py "$I/$s-15.6-imports.json" "$KERNEL" "$I/symbolsets-15.6.plist" "$R/$s-15.6-vs-$tag"
done
[ -n "${KEEP:-}" ] || rm -f "$KC" "$I/kernelcache.release.mac13g"
