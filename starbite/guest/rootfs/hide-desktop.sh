#!/bin/bash
# Hide helper .desktop files from sbwl's catalog (the home screen) in a root: dpkg-divert keeps the package's file
# (renamed *.desktop.distrib, which no launcher reads) and a copy with NoDisplay=true takes its place, so package
# upgrades do not bring the entry back.   hide-desktop.sh ROOT
set -euo pipefail
R=$1
HIDE=(
    foot-server       # foot's server mode (systemd socket activation): not an app
    footclient        # "Foot Client": needs a foot server; the Terminal is foot.desktop
    pcmanfm-desktop-pref    # PCManFM's X11 desktop settings: no desktop to set here
)
for id in "${HIDE[@]}"; do
    f=/usr/share/applications/$id.desktop
    [ -f "$R$f" ] || [ -f "$R$f.distrib" ] || continue
    if [ ! -f "$R$f.distrib" ]; then
        sudo chroot "$R" dpkg-divert --quiet --local --rename --divert "$f.distrib" --add "$f"
    fi
    sed -e '/^NoDisplay=/d' -e 's/^\[Desktop Entry\]$/[Desktop Entry]\nNoDisplay=true/' "$R$f.distrib" |
        sudo tee "$R$f" >/dev/null
done
echo "hide-desktop: hidden ${HIDE[*]}"
