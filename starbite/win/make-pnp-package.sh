#!/bin/bash
# A PnP driver package that installs itself offline-prepared, with no typing on the phone's Windows
# (docs/WIN11-GPU.md 4.3): copy the folder to C:\starbite\pnp\ from TWRP and add the one-shot service
# "sbpnp" to the offline SYSTEM hive. At the next boot the service (LocalSystem, no logon needed) runs
# install.cmd once: it disables itself first, trusts the Starbite test certificate (Root +
# TrustedPublisher), runs pnputil /add-driver /install, lists the device, and logs to C:\starbite\pnp.log.
#
#   starbite/win/make-pnp-package.sh sbmali|sbdisplay [build-dir]   (after starbite/win/build.sh)
#
# Out: <build-dir>/pnp-<driver>/  = the contents of C:\starbite\pnp\ ; prints the hive command.
set -euo pipefail
drv=$1
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
bld=${2:-$root/build/win-gpu}
case "$drv" in sbmali|sbdisplay) ;; *) echo "sbmali or sbdisplay" >&2; exit 1 ;; esac
main=$(cd "$(git -C "$root" rev-parse --git-common-dir)/.." && pwd)
crt=${SB_SIGN_CERT:-$main/build/woa/ufs-sys/sign/starbite-test.crt}
out=$bld/pnp-$drv
rm -rf "$out"
mkdir -p "$out/$drv"
cp "$bld/$drv.sys" "$bld/$drv.inf" "$bld/$drv.cat" "$out/$drv/"
# The public certificate only (DER); the key never leaves build/woa/ufs-sys/sign/.
openssl x509 -in "$crt" -outform der -out "$out/starbite-test.cer"
printf '%s\r\n' \
'@echo off' \
'rem Starbite one-shot PnP install (starbite/win/make-pnp-package.sh). Runs once as LocalSystem.' \
'set LOG=C:\starbite\pnp.log' \
'echo ==== %DATE% %TIME% sbpnp start>> %LOG%' \
'sc config sbpnp start= disabled>> %LOG% 2>&1' \
'certutil -addstore -f Root C:\starbite\pnp\starbite-test.cer>> %LOG% 2>&1' \
'echo certutil Root rc=%ERRORLEVEL%>> %LOG%' \
'certutil -addstore -f TrustedPublisher C:\starbite\pnp\starbite-test.cer>> %LOG% 2>&1' \
'echo certutil TrustedPublisher rc=%ERRORLEVEL%>> %LOG%' \
"pnputil /add-driver C:\\starbite\\pnp\\$drv\\$drv.inf /install>> %LOG% 2>&1" \
'echo pnputil add-driver rc=%ERRORLEVEL%>> %LOG%' \
'pnputil /enum-devices /instanceid "ACPI\STRB0072\0" /drivers>> %LOG% 2>&1' \
'echo ==== %DATE% %TIME% sbpnp end>> %LOG%' > "$out/install.cmd"
(cd "$out" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
cat "$out/SHA256SUMS"
cat <<MSG

Package: $out  ->  C:\\starbite\\pnp\\  (TWRP: /mnt/win/starbite/pnp/)
Hive (offline SYSTEM copy, clean hives only):
  tools/win/hive-service.py IN OUT add sbpnp --type 16 --start 2 --oneshot 0 \\
      --image '%SystemRoot%\\System32\\cmd.exe /c C:\\starbite\\pnp\\install.cmd' --display 'Starbite one-shot PnP install'
Read back: C:\\starbite\\pnp.log, Services\\sbpnp\\Start (4 after it ran), Services\\$drv\\State.
MSG
