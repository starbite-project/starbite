#!/bin/bash
# sepolicy-check.sh SYSTEM.img VENDOR.img [OUTDIR] [EXTRA.cil ...]
# Offline replay of what Android's init does at every boot on this phone (no precompiled_sepolicy on VENDOR,
# so init compiles the split policy itself; selinux.cpp LoadSplitPolicy): secilc over the GSI's
# plat + system_ext + product policy and mappings for the vendor's version, and the vendor's
# plat_pub_versioned.cil + vendor_sepolicy.cil (+ any EXTRA.cil, e.g. our vendor additions).
# A compile error here = init's "Failed to compile split policy" = reboot loop on the phone.
# Policy version 30 = the maximum of Linux 4.9. Lima, read-only on both images.
set -euo pipefail
S=${1:?system.img}; V=${2:?vendor.img}; O=${3:-$(mktemp -d /tmp/sepol.XXXXXX)}; shift 3 || shift $#
EXTRA=("$@")
SECILC=${SECILC:-secilc}
mkdir -p "$O/sys" "$O/ven"
get() { debugfs -R "dump $2 $3" "$1" >/dev/null 2>&1; [ -s "$3" ] || { rm -f "$3"; return 1; }; }
ver=$(debugfs -R 'cat /etc/selinux/plat_sepolicy_vers.txt' "$V" 2>/dev/null | tr -d '[:space:]')
gver=$(debugfs -R 'cat /etc/selinux/genfs_labels_version.txt' "$V" 2>/dev/null | tr -d '[:space:]' || true)
echo "vendor sepolicy version $ver, genfs labels $gver"
args=()
get "$S" /system/etc/selinux/plat_sepolicy.cil "$O/sys/plat.cil"; args+=("$O/sys/plat.cil")
get "$S" "/system/etc/selinux/mapping/$ver.cil" "$O/sys/map.cil" || { echo "GSI has no mapping for $ver"; exit 2; }
args+=("$O/sys/map.cil")
get "$S" "/system/etc/selinux/mapping/$ver.compat.cil" "$O/sys/compat.cil" && args+=("$O/sys/compat.cil")
[ -n "$gver" ] && get "$S" "/system/etc/selinux/plat_sepolicy_genfs_$gver.cil" "$O/sys/genfs.cil" && args+=("$O/sys/genfs.cil")
for p in system_ext product; do
  get "$S" "/system/$p/etc/selinux/${p}_sepolicy.cil" "$O/sys/$p.cil" && args+=("$O/sys/$p.cil")
  get "$S" "/system/$p/etc/selinux/mapping/$ver.cil" "$O/sys/$p-map.cil" && args+=("$O/sys/$p-map.cil")
  get "$S" "/system/$p/etc/selinux/mapping/$ver.compat.cil" "$O/sys/$p-compat.cil" && args+=("$O/sys/$p-compat.cil")
done
get "$V" /etc/selinux/plat_pub_versioned.cil "$O/ven/pub.cil"; args+=("$O/ven/pub.cil")
get "$V" /etc/selinux/vendor_sepolicy.cil "$O/ven/vendor.cil"; args+=("$O/ven/vendor.cil")
for e in "${EXTRA[@]}"; do args+=("$e"); done
printf '  %s\n' "${args[@]}"
set +e
"$SECILC" -m -M true -G -N -c 30 "${args[@]}" -o "$O/policy" -f /dev/null 2>"$O/secilc.err"
rc=$?
set -e
tail -20 "$O/secilc.err"
if [ $rc = 0 ]; then echo "COMPILES: $O/policy ($(stat -c %s "$O/policy") bytes)"; else echo "FAILS (rc $rc): $O/secilc.err"; fi
exit $rc
