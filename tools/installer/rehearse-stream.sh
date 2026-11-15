#!/bin/bash
# tools/installer/rehearse-stream.sh [WORKDIR]: tools/installer/stream-copy.py between TWO fake phones (no phone, no
# adb server, nothing real touched): S9 #1 in TWRP (the source, read only) and S9 #2 in the installer environment (the
# target), both on the starbite-oneui UFS table; S9 #1 has the 128 GB card (sd128), S9 #2 a 64 GB one (sd64l).
#   refusals (targets outside the allowlist or outside a root slot, the source not in TWRP, another environment's
#   serial, a phone in Download mode, the same serial twice, a write command on the source side, no dry run);
#   a dry run (nothing changes); a partition copy with a cable pull and the resume; a card range into the Sequoia
#   slot; a card file; a source that changed between two runs (refused, then --restart); the target journal; the
#   source phone byte-identical at the end, the target's protected partitions too.
# About 3 minutes; WORKDIR (default build/stream-rehearsal) is removed unless KEEP=1.
set -uo pipefail
cd "$(dirname "$0")/../.."
W=${1:-build/stream-rehearsal}
rm -rf "$W"; mkdir -p "$W"
W=$(cd "$W" && pwd)
export SBINST_STATE=$W/state
unset ANDROID_SERIAL SBINST_SERIAL FAKE_OTHER_PHONE SBINST_FAKE_USB_EXTRA FAKE_FAIL
FP=(python3 -B tools/installer/fakes/fakephone.py)
SC=(python3 -B tools/installer/stream-copy.py --from-serial S9ONE01 --to-serial S9TWO02 --state-dir "$W/state"
    --fake-src "$W/s1" --fake-dst "$W/s2")
pass=0 fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
run() {     # RC-WANTED DESCRIPTION -- COMMAND...
	local want=$1 desc=$2; shift 3
	"$@" > "$W/last.txt" 2>&1
	local rc=$?
	grep -E '^(ok|REFUSED|STOPPED)' "$W/last.txt" | sed 's/^/     /' | head -4
	if [ "$rc" = "$want" ]; then ok "$desc (exit $rc)"; else bad "$desc (exit $rc, wanted $want)"; sed 's/^/     | /' "$W/last.txt" | tail -8; fi
}
has() { if grep -qE -- "$1" "$W/last.txt"; then ok "$2"; else bad "$2 (no /$1/)"; fi; }
tree_hash() {   # every byte of a fake phone except the state files (sparse files by their data extents)
	python3 - "$1" <<'PY'
import hashlib, os, sys
h = hashlib.sha256()
for dp, dn, fn in sorted(os.walk(sys.argv[1])):
    dn.sort()
    for f in sorted(fn):
        p = os.path.join(dp, f)
        if os.path.islink(p) or f in ('state', 'usb', 'authorized_keys', 'fail-count'):
            continue
        h.update(p.encode())
        with open(p, 'rb') as fh:
            fd, off, end = fh.fileno(), 0, os.fstat(fh.fileno()).st_size
            while off < end:
                try:
                    off = os.lseek(fd, off, os.SEEK_DATA)
                except OSError:
                    break
                hole = os.lseek(fd, off, os.SEEK_HOLE)
                fh.seek(off)
                while off < hole:
                    b = fh.read(min(1 << 24, hole - off)); h.update(off.to_bytes(8, 'little') + b); off += len(b)
print(h.hexdigest())
PY
}
same() {   # SRC SRC_OFF DST DST_OFF LEN: the byte ranges are equal
	python3 - "$@" <<'PY'
import sys
a, ao, b, bo, n = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
with open(a, 'rb') as fa, open(b, 'rb') as fb:
    fa.seek(ao); fb.seek(bo)
    while n:
        k = min(n, 1 << 24)
        if fa.read(k) != fb.read(k):
            sys.exit(1)
        n -= k
PY
}

echo "######## two fake phones: S9 #1 (TWRP, sd128 card) and S9 #2 (the installer environment, sd64l card)"
"${FP[@]}" create "$W/s1" --state recovery --gpt starbite-oneui --card starbite --layout sd128 >/dev/null
"${FP[@]}" create "$W/s2" --state env --gpt starbite-oneui --card starbite --layout sd64l --card-sectors 124735488 >/dev/null
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=S9TWO02/' "$W/s2/props" && rm "$W/s2/props.bak"
mkdir -p "$W/state/env"
ssh-keygen -q -t ed25519 -N '' -C stream-rehearsal -f "$W/state/env/id_ed25519"
cp "$W/state/env/id_ed25519.pub" "$W/s2/authorized_keys"
python3 - "$W/s1" <<'PY'
# S9 #1's content: SYSTEM with data, a hole and data; the Sequoia slot (sd128 182888448) with data; a Linux file
import os, sys
r = sys.argv[1]
def put(path, off, data):
    with open(path, 'r+b') as f:
        f.seek(off); f.write(data)
sysn = os.path.realpath(os.path.join(r, 'dev/block/by-name/SYSTEM'))
put(sysn, 0, os.urandom(100 << 20))
put(sysn, 200 << 20, os.urandom(10 << 20))             # chunks 1-2 stay zero, chunk 3 has data
put(os.path.join(r, 'dev/block/mmcblk0'), 182888448 * 512, os.urandom(70 << 20))
d = os.path.join(r, 'external_sd/distros'); os.makedirs(d, exist_ok=True)
open(os.path.join(d, 'test.img'), 'wb').write(os.urandom(100 << 20) + bytes(30 << 20))
PY
S1=$(tree_hash "$W/s1")
prot() { cat "$W/s2/dev/block/by-name/"{EFS,PARAM,KEYSTORAGE,PERSISTENT,STEADY,RECOVERY,BOOT,DTB,DTBO} | shasum -a 256; }
P2=$(prot)

echo; echo "######## refusals"
run 2 "target RECOVERY" -- "${SC[@]}" --source part:SYSTEM --target part:RECOVERY
has 'only SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, USERDATA' "...only the allowlist"
run 2 "target BOOT" -- "${SC[@]}" --source part:SYSTEM --target part:BOOT
run 2 "target EFS" -- "${SC[@]}" --source part:EFS --target part:EFS
run 2 "target card store area (sd64l sector 8192)" -- "${SC[@]}" --source card:lba:182888448+143360 --target card:lba:8192
has 'not inside one root slot' "...not inside a root slot"
run 2 "target card p1 (sector 35790848)" -- "${SC[@]}" --source card:lba:182888448+143360 --target card:lba:35790848
run 2 "target file outside /distros, /starbite" -- "${SC[@]}" --source file:/distros/test.img --target file:/etc/passwd
run 2 "the same serial twice" -- python3 -B tools/installer/stream-copy.py --from-serial S9TWO02 --to-serial S9TWO02 --state-dir "$W/state" --fake-src "$W/s1" --fake-dst "$W/s2" --source part:ODM --target part:ODM
SBINST_FAKE_USB_EXTRA=04e8:685d run 2 "a phone in Download mode" -- "${SC[@]}" --source part:ODM --target part:ODM
has 'Download mode' "...says why"
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=OTHER/' "$W/s2/props"
run 2 "another phone's environment on 172.16.42.1" -- "${SC[@]}" --source part:ODM --target part:ODM
mv "$W/s2/props.bak" "$W/s2/props"
python3 -B tools/installer/fakes/fakephone.py set-state "$W/s1" menu
run 2 "the source not in TWRP" -- "${SC[@]}" --source part:ODM --target part:ODM
has 'not in TWRP' "...says so"
python3 - "$W/s1" <<'PY'
import sys; sys.path.insert(0, 'tools/installer/fakes'); import fakephone; fakephone.write_state(sys.argv[1], 'recovery')
PY
python3 - <<'PY' && ok "the source side refuses any command off the read-only list (dd of=, rm, mount, >)" || bad "source guard"
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sc', 'tools/installer/stream-copy.py'); m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
s = m.Source('X', {})
for c in ('dd if=/dev/zero of=/dev/block/sda18', 'rm -f /x', 'mount -o remount,rw /system', 'echo hi > /dev/block/sda3',
          'dd if=/dev/block/sda18 bs=1048576 skip=0 count=1 2>/dev/null | head -c 5 > /tmp/x'):
    try:
        s.sh(c); sys.exit(1)
    except m.Refused:
        pass
PY
run 2 "--rehearse without a dry run" -- "${SC[@]}" --source part:SYSTEM --target part:SYSTEM --size $((220 << 20)) --rehearse --yes
has 'no dry run of this copy' "...asks for it"

echo; echo "######## SYSTEM (220 MiB of it: data, two zero chunks, data) with a cable pull"
B2=$(tree_hash "$W/s2")
run 0 "dry run" -- "${SC[@]}" --source part:SYSTEM --target part:SYSTEM --size $((220 << 20))
has '4 chunks of 64 MiB' "4 chunks"
[ "$(tree_hash "$W/s2")" = "$B2" ] && ok "dry run: S9 #2 byte-identical" || bad "dry run changed S9 #2"
FAKE_FAIL='sbsc.chunk of=' FAKE_FAIL_TIMES=1 run 3 "a cable pull at the first data write" -- "${SC[@]}" --source part:SYSTEM --target part:SYSTEM --size $((220 << 20)) --rehearse --yes
rm -f "$W/s2/fail-count"
ls "$W"/state/phone-*/stream/*.json >/dev/null 2>&1 && ok "the state file is there" || bad "no state file"
run 0 "the same command again: resumes and finishes" -- "${SC[@]}" --source part:SYSTEM --target part:SYSTEM --size $((220 << 20)) --rehearse --yes
has 'both phones agree' "whole range: both phones agree"
same "$(python3 -c "import os;print(os.path.realpath('$W/s1/dev/block/by-name/SYSTEM'))")" 0 "$(python3 -c "import os;print(os.path.realpath('$W/s2/dev/block/by-name/SYSTEM'))")" 0 $((220 << 20)) && ok "S9 #2's SYSTEM = S9 #1's, byte for byte" || bad "SYSTEM differs"
python3 - "$W/state" <<'PY' && ok "the target journal records the copy (source, size, sha256)" || bad "journal"
import glob, json, sys
j = json.load(open(glob.glob(sys.argv[1] + '/phone-*/journal.json')[0]))
r = j['steps']['stream:part:SYSTEM']
assert r['status'] == 'done' and r['size'] == 220 << 20 and len(r['sha256']) == 64 and r['source'] == 'part:SYSTEM'
PY

echo; echo "######## macOS Sequoia slot: S9 #1's card range -> S9 #2's sd64l root slot mac15"
run 0 "dry run" -- "${SC[@]}" --source card:lba:182888448+143360 --target card:root:mac15
has 'root slot mac15 2105344\+33554432' "the target is the mac15 slot"
FAKE_FAIL='sbsc.chunk of=' FAKE_FAIL_TIMES=1 run 3 "stopped midway" -- "${SC[@]}" --source card:lba:182888448+143360 --target card:root:mac15 --rehearse --yes
rm -f "$W/s2/fail-count"
python3 -c "
f=open('$W/s1/dev/block/mmcblk0','r+b'); f.seek(182888448*512); f.write(b'changed!')"
run 2 "the source changed before the resume: refused" -- "${SC[@]}" --source card:lba:182888448+143360 --target card:root:mac15 --rehearse --yes
has 'the source changed' "...says so"
run 0 "--restart copies it again" -- "${SC[@]}" --source card:lba:182888448+143360 --target card:root:mac15 --rehearse --yes --restart
same "$W/s1/dev/block/mmcblk0" $((182888448 * 512)) "$W/s2/dev/block/mmcblk0" $((2105344 * 512)) $((70 << 20)) && ok "S9 #2's Sequoia slot = S9 #1's range" || bad "card range differs"
S1=$(tree_hash "$W/s1")       # the source's new bytes (the test changed them on purpose)

echo; echo "######## a Linux file on p1"
run 0 "dry run" -- "${SC[@]}" --source file:/distros/test.img --target file:/distros/test.img
run 0 "the copy" -- "${SC[@]}" --source file:/distros/test.img --target file:/distros/test.img --rehearse --yes
cmp -s "$W/s1/external_sd/distros/test.img" "$W/s2/external_sd/distros/test.img" && ok "the file is identical" || bad "file differs"
[ ! -e "$W/s2/external_sd/distros/test.img.part" ] && ok "no .part left behind" || bad ".part left"

echo; echo "######## at the end"
[ "$(tree_hash "$W/s1")" = "$S1" ] && ok "S9 #1 (the source) byte-identical through every copy" || bad "the source changed"
[ "$(prot)" = "$P2" ] && ok "S9 #2's EFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, RECOVERY, BOOT, DTB, DTBO unchanged" || bad "a protected partition changed"

echo; echo "######## result: $pass passed, $fail failed"
[ "${KEEP:-0}" = 1 ] || rm -rf "$W"
[ "$fail" = 0 ]
