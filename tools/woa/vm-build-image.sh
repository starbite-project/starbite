#!/usr/bin/env bash
# W4: build the Windows volume in the "Starbite Windows" UTM VM (tools/woa/vm/build-image.ps1)
# for the WINESP/WINDOWS geometry and GUIDs in build/woa/gpt-new/primary.bin.
#   tools/woa/vm-build-image.sh            push drivers + script, start, follow C:\sb\build.log
# Commands run as SYSTEM through the UTM guest agent (utmctl exec, which does not wait).
set -euo pipefail
cd "$(dirname "$0")/../.."
vm="Starbite Windows"
iso='C:\Users\starbite\Downloads\22631.7584_arm64_en-us_professional_deafdbcf_convert\22631.7584.260912-1040.23H2_NI_RELEASE_SVC_IM_CLIENTPRO_OEMRET_A64FRE_EN-US.ISO'

read -r disk esp_off esp_bytes esp_guid win_off win_bytes win_guid < <(python3 - build/woa/gpt-new/primary.bin <<'EOF'
import sys, struct, uuid
b = open(sys.argv[1], 'rb').read(); S = 4096
disk = uuid.UUID(bytes_le=b[S + 56:S + 72])
parts = {}
for i in range(128):
    e = b[2 * S + i * 128:2 * S + (i + 1) * 128]
    name = e[56:128].decode('utf-16le').rstrip('\0')
    first, last = struct.unpack('<QQ', e[32:48])
    parts[name] = (first * S, (last - first + 1) * S, uuid.UUID(bytes_le=e[16:32]))
e, w = parts['WINESP'], parts['WINDOWS']
print('{%s}' % disk, e[0], e[1], '{%s}' % e[2], w[0], w[1], '{%s}' % w[2])
EOF
)
disk_bytes=$(( $(python3 -c "import struct;b=open('build/woa/gpt-new/primary.bin','rb').read();print(struct.unpack('<Q',b[4096+32:4096+40])[0]+1)") * 4096 ))
echo "disk $disk ($disk_bytes) WINESP $esp_off+$esp_bytes $esp_guid WINDOWS $win_off+$win_bytes $win_guid"

utmctl exec "$vm" --cmd cmd.exe /c 'mkdir C:\sb\drivers' >/dev/null 2>&1 || true
utmctl file push "$vm" 'C:\sb\drivers.zip' < build/woa-release/drivers.zip
utmctl file push "$vm" 'C:\sb\build-image.ps1' < tools/woa/vm/build-image.ps1
cat > "${TMPDIR:-/tmp}/sb-start.ps1" <<EOF
Expand-Archive -Force C:\sb\drivers.zip C:\sb\drivers
Remove-Item -Force C:\sb\build.log -ErrorAction SilentlyContinue
& C:\sb\build-image.ps1 -Iso '$iso' -DiskBytes $disk_bytes -EspOffset $esp_off -EspBytes $esp_bytes \`
    -WinOffset $win_off -WinBytes $win_bytes -DiskGuid '$disk' -EspGuid '$esp_guid' -WinGuid '$win_guid'
EOF
utmctl file push "$vm" 'C:\sb\start.ps1' < "${TMPDIR:-/tmp}/sb-start.ps1"
utmctl exec "$vm" --cmd powershell.exe -ExecutionPolicy Bypass -File 'C:\sb\start.ps1' >/dev/null

seen=0
while sleep 10; do
    log=$(utmctl file pull "$vm" 'C:\sb\build.log' 2>/dev/null | tr -d '\r' | sed '1s/^\xEF\xBB\xBF//') || continue
    printf '%s\n' "$log" | tail -n +$((seen + 1))
    seen=$(printf '%s\n' "$log" | wc -l)
    printf '%s' "$log" | grep -q 'SB-BUILD-DONE' && exit 0
    printf '%s' "$log" | grep -q 'SB-BUILD-FAIL' && exit 1
done
