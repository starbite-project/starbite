#!/bin/bash
# Build, sign and package the Starbite Windows drivers of track D with the shared kit (tools/win, L0):
# $SBWIN (default ~/sbwin, made by tools/win/setup.sh), tools/win/build.sh, sign.sh, makecat.py.
#
#   starbite/win/build.sh [out-dir]          default: build/win-gpu (git-ignored)
#
# Out: sbthermal.sys (non-PnP service), sbdisplay.{sys,inf,cat} and sbmali.{sys,inf,cat} (PnP packages for
# ACPI\STRB0072, one installed at a time), StarbiteGpu.aml,
# all signed with the Starbite test certificate, plus SHA256SUMS.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/win-gpu}
win=$root/tools/win
mkdir -p "$out"
# The test key lives in the main checkout's git-ignored build/ (not in worktrees).
main=$(cd "$(git -C "$root" rev-parse --git-common-dir)/.." && pwd)
export SB_SIGN_KEY=${SB_SIGN_KEY:-$main/build/woa/ufs-sys/sign/starbite-test.key}
export SB_SIGN_CERT=${SB_SIGN_CERT:-$main/build/woa/ufs-sys/sign/starbite-test.crt}

# Both are plain WDM-style kernel images (GsDriverEntry); sbdisplay is a display miniport through displib.
"$win/build.sh" wdm "$out/sbthermal.sys" "$here/sbthermal/sbthermal.c" "$here/sbthermal/sbth_core.c" "$here/common/sbw_acpm.c"
LIBS="displib.lib ntstrsafe.lib libcntpr.lib" \
    "$win/build.sh" wdm "$out/sbdisplay.sys" "$here/sbdisplay/sbdisplay.c" "$here/sbdisplay/sbdc_core.c"

"$win/build.sh" kmdf "$out/sbmali.sys" "$here/sbmali/sbmali.c" "$here/sbmali/sbm_core.c" "$here/sbmali/sbg_core.c" "$here/common/sbw_acpm.c"

cp "$here/sbdisplay/sbdisplay.inf" "$here/sbmali/sbmali.inf" "$out/"
"$win/sign.sh" "$out/sbthermal.sys" "$out/sbdisplay.sys" "$out/sbmali.sys"
for p in sbdisplay sbmali; do
    python3 "$win/makecat.py" "$out/$p.cat" "$out/$p.inf" "$out/$p.sys" --hwid 'acpi\strb0072'
    "$win/sign.sh" "$out/$p.cat"
done
# The ACPI device as a self-contained SSDT (iasl in the lima VM, which sees this tree at the same path), for the
# shared SSDT-installing firmware DXE (owned by the win-usb-host track; one BOOT flash for all SSDTs).
limactl shell starbite -- bash -c "cd '$out' && cp '$here/acpi/StarbiteGpu.asl' . && iasl -vs -p StarbiteGpu StarbiteGpu.asl >/dev/null && rm StarbiteGpu.asl"
(cd "$out" && shasum -a 256 sbthermal.sys sbdisplay.sys sbdisplay.inf sbdisplay.cat sbmali.sys sbmali.inf sbmali.cat StarbiteGpu.aml > SHA256SUMS && cat SHA256SUMS)
