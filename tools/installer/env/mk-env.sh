#!/usr/bin/env bash
# The Starbite installer environment IS Starbite Maintenance in installer mode (docs/MAINTENANCE.md,
# docs/INSTALLER.md 13): one Linux environment for the installer, maintenance and rescue. This builds
# SBMAINT (tools/maint/env/mk-maint.sh) and names its parts the way installer bundles expect:
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/installer/env/mk-env.sh [OUTDIR]
#
# OUTDIR (default ~/distros/out/installer-env in the VM): installer.image, installer.initrd, installer.dtb
# (= maint.image, maint.initrd, maint.dtb) + SHA256SUMS. The installer starts it with `sb.installer=1`:
# the init then leaves the microSD unmounted, and merges the key the installer appends to the initrd
# (newc cpio root/.ssh/authorized_keys) with the keys built in. init, sb-byname, getprop and sb-reboot in
# this directory are links to tools/maint/env/. Environment: as mk-maint.sh, plus MAINT_OUT (the SBMAINT
# build directory, default ~/distros/out/maint).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
U=${SUDO_USER:-$(id -un)}
UH=$(getent passwd "$U" | cut -d: -f6)
OUT=${1:-$UH/distros/out/installer-env}
M=${MAINT_OUT:-$UH/distros/out/maint}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
bash "$HERE/../../maint/env/mk-maint.sh" "$M"
mkdir -p "$OUT"
cp "$M/maint.image" "$OUT/installer.image"
cp "$M/maint.initrd" "$OUT/installer.initrd"
cp "$M/maint.dtb" "$OUT/installer.dtb"
total=$(( $(stat -c %s "$OUT/installer.image") + $(stat -c %s "$OUT/installer.initrd") + $(stat -c %s "$OUT/installer.dtb") + 6144 ))
[ $total -le $((0x04ff0000)) ] || { echo "kernel+initrd+dtb = $total bytes > the loader's 0x04ff0000 download buffer" >&2; exit 1; }
chown -R "$U": "$OUT"
(cd "$OUT" && sha256sum installer.image installer.initrd installer.dtb) > "$OUT/SHA256SUMS"
echo "$OUT: $(( total / 1048576 )) MiB of the 80 MiB download buffer (Starbite Maintenance, installer mode)"
