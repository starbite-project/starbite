#!/bin/bash
# rehearse-part-write.sh : run part-write.py end to end against a FAKE phone in lima (a fake `adb` that
# runs TWRP's shell commands locally, with /dev/block/* and /tmp redirected into a temp dir, and the
# "partition" a 320 MiB file). Checks: refusals (wrong name, wrong by-name link, image too big,
# mounted), dry-run writes nothing, a real write verifies, an interrupted run resumes, zero chunks
# really clear stale data. Uses about 1 GiB in a temp dir under $HOME and removes it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(mktemp -d "$HOME/pwfake.XXXXXX")   # not under /tmp: fake-adb rewrites /tmp/; trap 'rm -rf "$R"' EXIT
mkdir -p "$R/bin" "$R/dev/block/by-name" "$R/tmp" "$R/proc"
: > "$R/proc/mounts"
head -c $((320 << 20)) /dev/urandom > "$R/dev/block/sda18"           # stale data everywhere
ln -s "$R/dev/block/sda18" "$R/dev/block/by-name/SYSTEM"
cp "$HERE/fake-adb" "$R/bin/adb"
export FAKE_ROOT=$R
chmod +x "$R/bin/adb"
export PATH="$R/bin:$PATH"
cd "$R"
python3 -I - "$R/img" <<'PY'
import os, sys
with open(sys.argv[1], 'wb') as f:
    f.write(os.urandom(64 << 20)); f.write(bytes(128 << 20)); f.write(os.urandom(40 << 20))   # 232 MiB
PY
ok() { echo "PASS $*"; }
python3 -I "$HERE/part-write.py" BOOT img --dry-run >/dev/null 2>&1 && exit 1 || ok "refuses BOOT"
python3 -I "$HERE/part-write.py" VENDOR img --dry-run >/dev/null 2>&1 && exit 1 || ok "refuses a missing/wrong by-name link"
head -c $((400 << 20)) /dev/zero > big.img
{ python3 -I "$HERE/part-write.py" SYSTEM big.img --dry-run 2>&1 || true; } | grep -q 'REFUSED: image' && ok "refuses an image larger than the partition"; rm big.img
echo "$R/dev/block/sda18 /system ext4 ro 0 0" > "$R/proc/mounts"
{ python3 -I "$HERE/part-write.py" SYSTEM img --dry-run 2>&1 || true; } | grep -q 'is mounted' && ok "refuses a mounted partition"; : > "$R/proc/mounts"
before=$(sha256sum dev/block/sda18 | cut -d' ' -f1)
python3 -I "$HERE/part-write.py" SYSTEM img --dry-run | tail -2
[ "$(sha256sum dev/block/sda18 | cut -d' ' -f1)" = "$before" ] && ok "dry run wrote nothing"
FAIL_AT=128 python3 -I "$HERE/part-write.py" SYSTEM img --state st >/dev/null 2>&1 && exit 1 || ok "interrupted at chunk 2 (simulated)"
[ "$(wc -l < st)" = 2 ] && ok "state has 2 finished chunks"
python3 -I "$HERE/part-write.py" SYSTEM img --state st | tail -1
cmp -n $((232 << 20)) img dev/block/sda18 && ok "partition == image (zero chunks cleared the stale data)"
[ ! -e st ] && ok "state removed after the verified end"
gzip -1 -k img && python3 -I "$HERE/part-write.py" SYSTEM img.gz | tail -1 && ok ".gz input"
