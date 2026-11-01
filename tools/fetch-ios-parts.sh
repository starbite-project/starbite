#!/usr/bin/env bash
# Fetch the iOS pieces the root image needs, straight from Apple's CDN (the
# URL ipsw.me lists), without the whole IPSW: BuildManifest, OS / SystemOS /
# AppOS disk images and the device tree. Same selection as the herolte
# ios-root workflow. Default: iPhone X (iPhone10,3 / D22AP) iOS 16.7.16.
# iPadOS 18 (iPad 7th gen):
#   DEVICE=iPad7,11 VERSION=18.7.11 BOARD=j171ap OUT=build/ipsw-18 tools/fetch-ios-parts.sh
# Only some parts (iPadOS 26 shared cache survey, A12 iPad 8):
#   DEVICE=iPad11,6 VERSION=26.7.2 BOARD=j171aap PARTS=systemos OUT=build/ipsw-26 tools/fetch-ios-parts.sh
# iOS 18 images come as .aea (encrypted); tools/decrypt-ios-parts.sh opens them.
# Apple files stay in build/ (gitignored), never in a repository.
set -euo pipefail
cd "$(dirname "$0")/.."
DEVICE=${DEVICE:-iPhone10,3} VERSION=${VERSION:-16.7.16} BOARD=${BOARD:-d22ap}
out=${OUT:-build/ipsw}
PARTS=${PARTS:-os systemos appos devicetree}
mkdir -p "$out"
RZ=ref/ios-herolte/tools/remotezip.py

url=$(curl -sf "https://api.ipsw.me/v4/device/$DEVICE?type=ipsw" | python3 -c "
import json, sys
fw = [f for f in json.load(sys.stdin)['firmwares'] if f['version'] == '$VERSION']
print(fw[0]['url'])")
case "$url" in https://updates.cdn-apple.com/*|https://appldnld.apple.com/*) ;; *)
    echo "unexpected IPSW host: $url"; exit 1 ;; esac
echo "IPSW: $url"
[ -f "$out/BuildManifest.plist" ] || python3 "$RZ" "$url" BuildManifest.plist "$out"
python3 - "$out/BuildManifest.plist" "$BOARD" > "$out/members.txt" <<'PY'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], 'rb'))
ident = next(i for i in m['BuildIdentities']
             if i['Info']['DeviceClass'].lower() == sys.argv[2]
             and 'Erase' in i['Info'].get('Variant', ''))
want = {'OS': 'os', 'Cryptex1,SystemOS': 'systemos', 'Cryptex1,AppOS': 'appos',
        'DeviceTree': 'devicetree'}
for key, name in want.items():
    print(name, ident['Manifest'][key]['Info']['Path'])
PY
cat "$out/members.txt"
while read -r name path; do
    case " $PARTS " in *" $name "*) ;; *) continue ;; esac
    case "$path" in *.dmg.aea) ext=dmg.aea ;; *) ext=${path##*.} ;; esac
    dest="$out/$name.$ext"
    [ -f "$dest" ] && { echo "have $dest"; continue; }
    python3 "$RZ" "$url" "$path" "$out"
    mv "$out/$path" "$dest"
done < "$out/members.txt"
ls -la "$out"
