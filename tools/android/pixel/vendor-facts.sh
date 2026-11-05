#!/bin/bash
# vendor-facts.sh VENDOR.img : read-only facts about an Android vendor image that decide whether a GSI fits
# (props, fstab, SELinux versions, VINTF level, keymaster). Runs in lima (debugfs). Writes nothing.
set -euo pipefail
V=${1:?vendor.img}
cat_() { debugfs -R "cat $1" "$V" 2>/dev/null; }
echo "== $V  sha256 $(sha256sum "$V" | cut -c1-16)…"
echo "-- props"
for f in /build.prop /default.prop /etc/build.prop; do
  cat_ "$f" | grep -E 'security_patch|keymaster|api_level|vndk|first_api|build.version.(sdk|release)|treble|zygote|fingerprint|abilist|board' || true
done
echo "-- fstab"
cat_ /etc/fstab.samsungexynos9810 | grep -v '^#' | grep -v '^$' || true
echo "-- selinux"
debugfs -R 'ls /etc/selinux' "$V" 2>/dev/null | tr -s ' ' '\n' | grep -E '\.' | sort -u || true
echo -n "plat_sepolicy_vers.txt: "; cat_ /etc/selinux/plat_sepolicy_vers.txt; echo
echo "-- vintf manifest target-level / kernel"
cat_ /etc/vintf/manifest.xml | grep -o -E 'target-level="[0-9]+"|<kernel[^>]*>' || true
debugfs -R 'ls /etc/vintf/manifest' "$V" 2>/dev/null | tr -s ' ' '\n' | grep -E '\.xml' | sort -u | head -80 || true
echo "-- keymaster / keymint / gatekeeper services"
debugfs -R 'ls /bin/hw' "$V" 2>/dev/null | tr -s ' ' '\n' | grep -i -E 'keymaster|keymint|gatekeeper|security|drm|radio|camera' | sort -u || true
echo "-- init rc mentioning keymaster props"
for rc in $(debugfs -R 'ls /etc/init' "$V" 2>/dev/null | tr -s ' ' '\n' | grep -E '\.rc$' | sort -u); do
  cat_ "/etc/init/$rc" | grep -i -E 'keymaster|security_patch' | sed "s|^|$rc: |" || true
done
