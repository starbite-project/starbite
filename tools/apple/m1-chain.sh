#!/bin/sh
# Apple-on-S9 milestone test as a chained test loader (no BOOT write), docs/APPLE-S9-DRIVERS.md Part 5:
#   sh tools/apple/m1-chain.sh PAYLOAD.xnu OUT.img [WDT_S]
#   SHIM_REV=HEAD sh tools/apple/m1-chain.sh ...      build the loader from a commit (others may be editing shim/)
#   sh tools/apple/m1-chain.sh --read OUT.img          a crumb reader: prints the last test run's breadcrumbs
#                                                      (ramoops page 0xFED13000) into pmsg, then reboots normally
# 1. builds the loader (shim/starlte, from a private copy of the tree) with -DSTAR_CHAIN_AUTOBOOT: it boots the
#    payload appended to it at once (no POST, no menu, no launch record);
# 2. appends PAYLOAD at its 4 KiB-aligned _image_end and sets image_size (as tools/starlte-pack-boot.py --append);
# 3. packs it behind the menuchain stub (tools/menu/chain) with a dead-man of WDT_S seconds (default 60): the stub
#    arms cluster 1's watchdog before anything else and nothing feeds it (the kernel must run with sbstop=).
# The BOOT loader starts OUT.img from a store row (method "bare", wdt = 0, wdt_feed = "own", no_launch = true).
set -eu
cd "$(dirname "$0")/../.."
payload=$1 out=$2 wdt=${3:-60}
[ "$payload" = --read ] || head -c 8 "$payload" | grep -q SWEETXNU || { echo "$payload: not a SWEETXNU payload"; exit 1; }
work=build/apple-chain-$$          # a new folder per run: the VM's view of a just-deleted one can be stale
rm -rf "$work"; mkdir -p "$work"
if [ -n "${SHIM_REV:-}" ]; then              # SHIM_REV=HEAD: the committed loader, not the working tree
    git archive "$SHIM_REV" shim | tar -x -C "$work"
else
    cp -R shim "$work/shim"
fi
rm -rf "$work/shim/starlte/out"
limactl shell starbite -- env EXTRA_CFLAGS=-DSTAR_CHAIN_AUTOBOOT sh "$PWD/$work/shim/starlte/build.sh" | tail -1
if [ "$payload" = --read ]; then
    cp "$work/shim/starlte/out/xnu_boot.Image" "$work/loader+payload.Image"
else
python3 - "$work/shim/starlte/out/xnu_boot.Image" "$payload" "$work/loader+payload.Image" <<'PY'
import struct, sys
k = bytearray(open(sys.argv[1], 'rb').read())
p = open(sys.argv[2], 'rb').read()
assert k[56:60] == b'ARMd'
k += bytes(-len(k) % 4096)
k += p
struct.pack_into('<Q', k, 16, (len(k) + 0x10000 + 0xfff) & ~0xfff)
open(sys.argv[3], 'wb').write(k)
print(f'{sys.argv[3]}: loader + payload, {len(k)} bytes')
PY
fi
sh tools/menu/chain/build.sh "$work/loader+payload.Image" "$out" --wdt "$wdt"
rm -rf "$work"
