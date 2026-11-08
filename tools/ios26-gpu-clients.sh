#!/usr/bin/env bash
# Which processes get a Metal device (and so wired GPU memory): /private/var/local/mtl2-clients on iOS 26's root
# (tools/metal2/HotMetal2.mm reads it; backboardd always gets one). Phone in TWRP; installed at the next iOS 26
# boot (kernel 0401+). 2026-11-08 T4: backboardd 416 MiB, SpringBoard 136, iconservicesagent 135.
#   tools/ios26-gpu-clients.sh "backboardd SpringBoard"                     (drop iconservicesagent: -135 MiB)
#   tools/ios26-gpu-clients.sh "backboardd SpringBoard iconservicesagent"   (undo: today's list)
set -euo pipefail
cd "$(dirname "$0")/.."
list=${1:?usage: ios26-gpu-clients.sh "backboardd SpringBoard ..."}
out=build/ios26/gpu-clients; mkdir -p "$out"
printf '%s\n' "$list" > "$out/mtl2-clients"
python3 tools/overlay/sboverlay.py "$out/clients.bin" "$out/mtl2-clients=/private/var/local/mtl2-clients:644" | tail -1
tools/overlay/card-overlay.sh "$out/clients.bin"
echo "queued: /var/local/mtl2-clients = '$list' at the next iOS 26 boot (check 'STARLTE OVERLAY: /private/var/local/mtl2-clients ... written 0')"
