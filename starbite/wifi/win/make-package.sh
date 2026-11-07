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

# install.cmd: its first action is a log line (W2 note: sbpnp died twice within 3 s without a log). It finds its
# files next to itself (%~dp0), so it also works tapped from anywhere ("Run as administrator"); run by hand it
# shows its progress and pauses at the end; run by the sbpnp service (session 0, no SESSIONNAME) it stays silent.
cat > "$out/install.cmd.lf" <<'CMD'
@echo off
if not exist C:\starbite mkdir C:\starbite
>>C:\starbite\pnp.log echo ==== %DATE% %TIME% sbwificx install.cmd started, session "%SESSIONNAME%"
setlocal
set LOG=C:\starbite\pnp.log
set PKG=%~dp0
set INTERACTIVE=
if defined SESSIONNAME set INTERACTIVE=1
call :say "Starbite Wi-Fi install: package folder %PKG%"
net session >nul 2>&1
if errorlevel 1 (
    call :say "NOT AN ADMINISTRATOR: press and hold install.cmd, then Run as administrator"
    goto done
)
sc query sbpnp >nul 2>&1 && sc config sbpnp start= disabled>> %LOG% 2>&1
call :say "trusting the Starbite test certificate"
certutil -addstore -f Root "%PKG%starbite-test.cer">> %LOG% 2>&1
call :say "certutil Root rc=%ERRORLEVEL%"
certutil -addstore -f TrustedPublisher "%PKG%starbite-test.cer">> %LOG% 2>&1
call :say "certutil TrustedPublisher rc=%ERRORLEVEL%"
call :say "installing the driver with pnputil"
pnputil /add-driver "%PKG%sbwificx\sbwificx.inf" /install>> %LOG% 2>&1
call :say "pnputil add-driver rc=%ERRORLEVEL%"
pnputil /enum-devices /instanceid "ACPI\STBW4361\0" /drivers>> %LOG% 2>&1
call :say "waiting 30 s for the radio and WLAN AutoConfig"
ping -n 31 127.0.0.1 >nul
>>%LOG% echo ---- netsh wlan show drivers
netsh wlan show drivers>> %LOG% 2>&1
>>%LOG% echo ---- netsh wlan show interfaces, names and addresses left out
netsh wlan show interfaces | findstr /v /i /c:"SSID" /c:"address" /c:"Profile">> %LOG% 2>&1
>>%LOG% echo ---- networks heard, count only:
netsh wlan show networks | find /c "SSID ">> %LOG% 2>&1
if not exist "%PKG%wifi.xml" goto done
call :say "adding the Wi-Fi profile and joining"
netsh wlan add profile filename="%PKG%wifi.xml" user=all>> %LOG% 2>&1
call :say "netsh add profile rc=%ERRORLEVEL%"
del /f /q "%PKG%wifi.xml"
if exist "%PKG%wifi.xml" call :say "WIFI.XML NOT DELETED: delete it by hand, it holds the passphrase"
netsh wlan connect name=starbite-test>> %LOG% 2>&1
call :say "netsh connect rc=%ERRORLEVEL%, waiting 30 s"
ping -n 31 127.0.0.1 >nul
netsh wlan show interfaces | findstr /i /c:" State">> %LOG% 2>&1
>>%LOG% echo ---- ping 1.1.1.1, summary
ping -n 4 1.1.1.1 | findstr /i /c:"Lost">> %LOG% 2>&1
:done
call :say "==== %DATE% %TIME% install.cmd end; the details are in C:\starbite\pnp.log"
if defined INTERACTIVE pause
endlocal
exit /b 0

:say
>>%LOG% echo %~1
if defined INTERACTIVE echo %~1
goto :eof
CMD
sed 's/$/\r/' "$out/install.cmd.lf" > "$out/install.cmd"
rm -f "$out/install.cmd.lf"
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
