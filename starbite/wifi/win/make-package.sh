#!/bin/bash
# SbWifiCx (W2) as a PnP package that installs itself at the next Windows boot, the same way as the GPU
# session's starbite/win/make-pnp-package.sh (which passed on the phone): copy the folder to C:\starbite\pnp\
# from TWRP and add the one-shot service "sbpnp" to the offline SYSTEM hive. At the next boot sbpnp (LocalSystem,
# no logon) runs install.cmd once: disables itself, trusts the Starbite test certificate (Root + TrustedPublisher),
# pnputil /add-driver /install (the Net class installer gives the adapter its NetCfgInstanceId and bindings, which
# an offline registry bind cannot), waits for the radio, then logs what Windows sees to C:\starbite\pnp.log.
#
# Optional join: a Wi-Fi profile made from the user's network (never in git):
#   starbite/wifi/win/make-package.sh --profile        asks for the network's name and passphrase on the terminal
# writes wifi.xml (WPA2-PSK/AES, profile name "starbite-test") into the package; install.cmd adds it for all
# users, DELETES the file, connects, and logs only "connected"/"not connected" and a ping summary (no names,
# no addresses).
#
#   starbite/wifi/win/make-package.sh [--profile]       (after starbite/wifi/win/build-cx.sh)
# Out: starbite/out/win/pnp-sbwificx/ = the contents of C:\starbite\pnp\ ; prints the hive command.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
bld=$root/starbite/out/win
main=$(cd "$(git -C "$root" rev-parse --git-common-dir)/.." && pwd)
crt=${SB_SIGN_CERT:-$main/build/woa/ufs-sys/sign/starbite-test.crt}
out=$bld/pnp-sbwificx
rm -rf "$out"
mkdir -p "$out/sbwificx"
cp "$bld/sbwificx.sys" "$bld/sbwificx.inf" "$bld/sbwificx.cat" "$out/sbwificx/"
openssl x509 -in "$crt" -outform der -out "$out/starbite-test.cer"     # the public certificate only

if [ "${1:-}" = --profile ]; then
    umask 077
    read -r -p 'Network name (SSID): ' ssid
    read -r -s -p 'Passphrase (8..63 characters): ' pass; echo
    [ ${#pass} -ge 8 ] && [ ${#pass} -le 63 ] || { echo "the passphrase must be 8..63 characters" >&2; exit 1; }
    esc() { python3 -c 'import sys,html; print(html.escape(sys.argv[1], quote=True))' "$1"; }
    hex=$(printf '%s' "$ssid" | xxd -p | tr -d '\n' | tr a-f A-F)
    cat > "$out/wifi.xml" <<XML
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>starbite-test</name>
  <SSIDConfig><SSID><hex>$hex</hex><name>$(esc "$ssid")</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>auto</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$(esc "$pass")</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
XML
    unset pass
    echo "wifi.xml written (holds the passphrase: never commit it; install.cmd deletes it on the phone)"
fi

{
printf '%s\r\n' \
'@echo off' \
'rem Starbite one-shot PnP install of SbWifiCx (starbite/wifi/win/make-package.sh). Runs once as LocalSystem.' \
'set LOG=C:\starbite\pnp.log' \
'echo ==== %DATE% %TIME% sbpnp start (sbwificx)>> %LOG%' \
'sc config sbpnp start= disabled>> %LOG% 2>&1' \
'certutil -addstore -f Root C:\starbite\pnp\starbite-test.cer>> %LOG% 2>&1' \
'echo certutil Root rc=%ERRORLEVEL%>> %LOG%' \
'certutil -addstore -f TrustedPublisher C:\starbite\pnp\starbite-test.cer>> %LOG% 2>&1' \
'echo certutil TrustedPublisher rc=%ERRORLEVEL%>> %LOG%' \
'pnputil /add-driver C:\starbite\pnp\sbwificx\sbwificx.inf /install>> %LOG% 2>&1' \
'echo pnputil add-driver rc=%ERRORLEVEL%>> %LOG%' \
'pnputil /enum-devices /instanceid "ACPI\STBW4361\0" /drivers>> %LOG% 2>&1' \
'rem the radio comes up in ~1-2 s; WLAN AutoConfig then opens the adapter and scans' \
'ping -n 31 127.0.0.1 >nul' \
'echo ---- netsh wlan show drivers>> %LOG%' \
'netsh wlan show drivers>> %LOG% 2>&1' \
'echo ---- netsh wlan show interfaces (names and addresses left out)>> %LOG%' \
'netsh wlan show interfaces | findstr /v /i /c:"SSID" /c:"address" /c:"Profile">> %LOG% 2>&1' \
'echo ---- networks heard (count only):>> %LOG%' \
'netsh wlan show networks | find /c "SSID ">> %LOG% 2>&1' \
'if not exist C:\starbite\pnp\wifi.xml goto done' \
'netsh wlan add profile filename=C:\starbite\pnp\wifi.xml user=all>> %LOG% 2>&1' \
'echo netsh add profile rc=%ERRORLEVEL%>> %LOG%' \
'del /f /q C:\starbite\pnp\wifi.xml' \
'if exist C:\starbite\pnp\wifi.xml echo WIFI.XML NOT DELETED>> %LOG%' \
'netsh wlan connect name=starbite-test>> %LOG% 2>&1' \
'echo netsh connect rc=%ERRORLEVEL%>> %LOG%' \
'ping -n 31 127.0.0.1 >nul' \
'netsh wlan show interfaces | findstr /i /c:" State">> %LOG% 2>&1' \
'echo ---- ping 1.1.1.1 (summary)>> %LOG%' \
'ping -n 4 1.1.1.1 | findstr /i /c:"Lost">> %LOG% 2>&1' \
':done' \
'echo ==== %DATE% %TIME% sbpnp end>> %LOG%'
} > "$out/install.cmd"
(cd "$out" && find . -type f ! -name SHA256SUMS ! -name wifi.xml | sort | xargs shasum -a 256 > SHA256SUMS)
cat "$out/SHA256SUMS"
[ -f "$out/wifi.xml" ] && echo "(+ wifi.xml: not listed, holds the passphrase)"
cat <<MSG

Package: $out  ->  C:\\starbite\\pnp\\  (TWRP: /mnt/win/starbite/pnp/)
Hive (offline SYSTEM copy, clean hives only):
  tools/win/hive-service.py IN OUT add sbpnp --type 16 --start 2 --oneshot 0 \\
      --image '%SystemRoot%\\System32\\cmd.exe /c C:\\starbite\\pnp\\install.cmd' --display 'Starbite one-shot PnP install'
Read back: C:\\starbite\\pnp.log, C:\\starbite\\sbwificx.txt, Services\\sbpnp\\Start (4), Services\\sbwificx\\Parameters\\GuardBoots.
MSG
