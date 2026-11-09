#!/usr/bin/env bash
# iOS 26 wallpaper: PosterBoard logs "Data store could not be created for an unknown reason" on every boot. Its
# store is /private/var/mobile/Library/Application Support/PRBPosterExtensionDataStore (0777 501:501, exists).
# Phone in TWRP; kernel 0406+ (overlay rename op).
#   tools/ios26-poster-store.sh --pull [NAME]   read only: listing + every file <= 4 MiB -> build/ios26/pb/NAME/
#                                          (NAME default store; NAME.txt is the listing)
#   tools/ios26-poster-store.sh --aside    queue a rename of the store to PRBPosterExtensionDataStore.starbite-old
#                                          (PosterBoard then has to create a fresh one at the next boot)
#   tools/ios26-poster-store.sh --undo     queue the rename back (only if no new store was created meanwhile:
#                                          the kernel's rename refuses an existing destination)
#   tools/ios26-poster-store.sh --version FROM TO   rename the store's version folder (T28: 59 -> 61)
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
STORE="/private/var/mobile/Library/Application Support/PRBPosterExtensionDataStore"
OLD="$STORE.starbite-old"
D=build/ios26/pb
mkdir -p "$D"
case "${1:-}" in
--pull)
    N=${2:-store}
    case "$N" in */*|.*|'') echo "bad NAME"; exit 1;; esac
    python3 tools/ios26-find.py '*' "$STORE" --depth 10 > "$D/$N.txt"
    wc -l < "$D/$N.txt" | sed 's/^/entries: /'
    rm -rf "${D:?}/$N"; mkdir -p "$D/$N"
    # lines: "file MODE UID:GID SIZE /full/path" (path may hold spaces)
    awk '$1=="file" && $4 > 0 && $4 <= 4194304 { $1=$2=$3=$4=""; sub(/^ +/, ""); print }' "$D/$N.txt" |
    while IFS= read -r p; do
        rel=${p#"$STORE"/}
        mkdir -p "$D/$N/$(dirname "$rel")"
        HFS_CARD=1 python3 tools/overlay/hfs-read.py "$p" "$D/$N/$rel" </dev/null >/dev/null || echo "could not read $p"
    done
    du -sh "$D/$N"; echo "listing: $D/$N.txt";;
--aside)
    python3 tools/overlay/sboverlay.py "$D/aside.bin" "rename:$STORE=$OLD" | tail -1
    tools/overlay/card-overlay.sh "$D/aside.bin"; echo "queued: store moved aside at the next boot";;
--undo)
    python3 tools/overlay/sboverlay.py "$D/undo.bin" "rename:$OLD=$STORE" | tail -1
    tools/overlay/card-overlay.sh "$D/undo.bin"; echo "queued: store back at the next boot";;
--version)
    # 2026-11-10 T27: PosterBoard looks for store version 61 (then 60, 58), finds only 59 (what a fresh store got
    # in T19) and never creates 61. --version FROM TO renames the version folder (undo: --version TO FROM).
    from=${2:?FROM}; to=${3:?TO}
    case "$from$to" in *[!0-9]*) echo "versions are numbers"; exit 1;; esac
    python3 tools/overlay/sboverlay.py "$D/version.bin" "rename:$STORE/$from=$STORE/$to" | tail -1
    tools/overlay/card-overlay.sh "$D/version.bin"; echo "queued: $from -> $to at the next boot";;
*)
    sed -n '2,12p' "$0"; exit 1;;
esac
