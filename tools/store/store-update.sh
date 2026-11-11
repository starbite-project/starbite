#!/usr/bin/env bash
# Update the phone's OS store from TWRP: pull the store the installed loader reads (tools/store/live-store.sh: the
# card once BOOT is the card-only loader, else the UFS store at USERDATA +16 GiB), rebuild it with --import and the
# given sbstore2.py build arguments, write it back, read back. Before the release, a HOME card store is then
# rebuilt from the new UFS store, so the card copy stays complete (docs/STORE-ON-CARD.md). The one-call replacement for
# "store-pull.sh + sbstore2.py build --medium ufs + store-flash.sh".
#
#   tools/store/store-update.sh WORKDIR [sbstore2.py build arguments, without OUT, --medium, --import]
#   e.g. store-update.sh build/store/upd --catalog build/store/catalog-live.toml --drop oldkey --omit old.img \
#            --volatile sbos-state=1048576 starbite-next.img=build/store/sbn.img:image
#
# Put NAME=FILE[:TYPE] arguments after all --options (argparse). Prints "OS store v2 written (...)" on success
# (the line next.sh / sbtest.sh / rowswap.sh grep for). WORKDIR keeps live.img (the pull) and new.img until the
# next run (about twice the store size: ~1 GB).
set -euo pipefail
cd "$(dirname "$0")/../.."
W=${1:?usage: store-update.sh WORKDIR [sbstore2.py build args]}; shift
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
for a in "$@"; do case "$a" in --medium*|--import*|--home|--data-at*|--capacity*) echo "store-update.sh sets $a itself"; exit 1 ;; esac; done
mkdir -p "$W"
rm -f "$W/live.img" "$W/new.img"
live=$(tools/store/live-store.sh)
card_home=no
[ "$(adb exec-out "dd if=/dev/block/mmcblk0p2 bs=1 skip=$((4096 + 64)) count=8 2>/dev/null")" = SBSTHOME ] && card_home=yes
if [ "$live" = card ]; then
    [ "$card_home" = yes ] || { echo "BOOT is card-only but the card holds no HOME store: refusing"; exit 1; }
    tools/store/store-pull.sh "$W/live.img" sd | tail -1
    python3 -I -B tools/store/sbstore2.py build "$W/new.img" --medium sd --home --data-at layout --import "$W/live.img" "$@" | tail -1
    tools/store/card-store.sh flash "$W/new.img" | tail -2
    echo "OS store v2 written (card): $(python3 -I -B tools/store/sbstore2.py header "$W/new.img" | head -1)"
else
    tools/store/store-pull.sh "$W/live.img" ufs | tail -1
    python3 -I -B tools/store/sbstore2.py build "$W/new.img" --medium ufs --import "$W/live.img" "$@" | tail -1
    tools/store/store-flash.sh "$W/new.img" | tail -1
    echo "OS store v2 written (ufs, the installed loader's): $(python3 -I -B tools/store/sbstore2.py header "$W/new.img" | head -1)"
    if [ "$card_home" = yes ]; then
        # the card copy follows (the release needs it complete): the same entries, the card's settings kept
        rm -f "$W/live.img"
        python3 -I -B tools/store/sbstore2.py build "$W/card.img" --medium sd --home --data-at layout --import "$W/new.img" | tail -1
        tools/store/card-store.sh flash "$W/card.img" | tail -1
        rm -f "$W/card.img"
        echo "card copy updated to the same entries"
    fi
fi
