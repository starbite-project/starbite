#!/usr/bin/env bash
# Which OS store the phone's installed loader uses (docs/STORE-ON-CARD.md), from TWRP: prints "card" or "ufs".
#   card  BOOT's payload command line has " sbstore=card" (the card-only BOOT, store-to-card release step D)
#   ufs   otherwise: every BOOT before that reads the UFS store at USERDATA +16 GiB and ITS settings, even when the
#         card holds a HOME store (those loaders do not know the HOME flag)
# The leading space matters: the card-only loader's own code contains "sbstore=card" too, only the cmdline
# has it after a space.
set -euo pipefail
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "live-store.sh: phone not in TWRP" >&2; exit 1; }
n=$(adb shell "dd if=/dev/block/by-name/BOOT bs=1048576 2>/dev/null | grep -a -c ' sbstore=card'; true" | tr -d '\r')
case "$n" in ''|*[!0-9]*) echo "live-store.sh: cannot read BOOT ($n)" >&2; exit 1 ;; esac
if [ "$n" -gt 0 ]; then echo card; else echo ufs; fi
