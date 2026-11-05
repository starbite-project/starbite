#!/usr/bin/env bash
# tools/installer/rehearse.sh [WORKDIR]: the Starbite installer end to end against FAKE phones and
# FAKE disks (tools/installer/fakes/): no phone, no adb server, no real disk is touched. macOS or Linux.
#   - bundle check, refusals (missing / tampered file, protected destination, no dry run, --yes --real,
#     wrong model, small card, low battery, foreign card)
#   - a dry run (writes nothing: every fake file hashed before and after)
#   - a rehearsal with a simulated cable pull in the middle of SYSTEM, then the resumed run
#   - the same run again: nothing to do, 0 write commands
#   - protected areas byte-identical, every target == its image, backups verified
#   - the main phone without a journal: everything kept, nothing replaced
#   - starting from Download mode (TWRP via fake heimdall) and from the loader's fastboot
#   - the rescue USB stick: refusals for internal / huge / disk-image disks, a dry run, a write + read-back
# About 300 MB in WORKDIR (default build/installer-rehearsal; sparse files), removed at the end unless KEEP=1.
# Long runs are shown in summary (headers, plan rows, ok/WARN/REFUSED/STOPPED lines); their full logs
# are in WORKDIR/state*/logs/.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO"
W=${1:-build/installer-rehearsal}
rm -rf "$W"
mkdir -p "$W"
SI=(python3 -B tools/installer/starbite-install)
FP=(python3 -B tools/installer/fakes/fakephone.py)
B=$W/bundle
pass=0 fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
hdr() { echo; echo "######## $*"; }
brief() {   # the lines that tell the story of a run (WARN lines only with BRIEF_WARN=1)
	grep -E '^(== The plan|== Dry run fin|ok   |WARN |FAIL |REFUSED|STOPPED|   [0-9]+ +(host|verify|write|skip|keep|resume|REPLACE|BLOCKED) |   \| (Uploading|RECOVERY upload|\(fake)|   found: |   \$ (heimdall|fastboot|adb reboot|diskutil|sudo dd|python3 -I tools/android/pixel/part-write.py)|   written [0-9])' |
		grep -vE "^WARN ${BRIEF_WARN:+NEVER}" | grep -vE '^ok   (keep|skip): ' |
		sed -E 's/  +$//; s/ {3,}/  /g'
}
run() {     # RC-WANTED DESCRIPTION -- COMMAND...: runs, shows the brief, checks the exit status
	local want=$1 desc=$2
	shift 3
	local out rc
	out=$("$@" 2>&1)
	rc=$?
	printf '%s\n' "$out" | sed -E 's/  +$//; s/ {3,}/  /g' > "$W/last.txt"
	printf '%s\n' "$out" | brief
	if [ "$rc" = "$want" ]; then ok "$desc (exit $rc)"; else bad "$desc (exit $rc, wanted $want)"; fi
}
has() {     # PATTERN DESCRIPTION: the last run's full output contains PATTERN
	if grep -qE -- "$1" "$W/last.txt"; then ok "$2"; else bad "$2 (no /$1/)"; fi
}
fakehash() {   # every byte of a fake phone (sparse files hashed by content)
	python3 - "$1" <<'PY'
import hashlib, os, sys
h = hashlib.sha256()
for dp, dn, fn in sorted(os.walk(sys.argv[1])):
    dn.sort()
    for f in sorted(fn):
        p = os.path.join(dp, f)
        if os.path.islink(p) or f in ('state', 'usb'):
            continue
        h.update(p.encode())
        with open(p, 'rb') as fh:            # only the data extents: the fake UFS and card are sparse
            fd, off, end = fh.fileno(), 0, os.fstat(fh.fileno()).st_size
            while off < end:
                try:
                    off = os.lseek(fd, off, os.SEEK_DATA)
                except OSError:
                    break
                hole = os.lseek(fd, off, os.SEEK_HOLE)
                fh.seek(off)
                while off < hole:
                    b = fh.read(min(1 << 24, hole - off))
                    h.update(off.to_bytes(8, 'little') + b)
                    off += len(b)
print(h.hexdigest())
PY
}
protected() {  # EFS..DTBO, the GPT head/tail and the boot LUNs of a fake phone
	python3 - "$1" <<'PY'
import hashlib, os, sys
r = sys.argv[1]; d = os.path.join(r, 'dev', 'block')
names = ['EFS', 'CPEFS', 'PARAM', 'UP_PARAM', 'KEYSTORAGE', 'PERSISTENT', 'STEADY', 'BOTA0', 'BOTA1', 'BOTA2', 'DTB', 'DTBO']
h = hashlib.sha256()
for n in names:
    h.update(open(os.path.join(d, 'by-name', n), 'rb').read())
with open(os.path.join(d, 'sda'), 'rb') as f:
    h.update(f.read(4 << 20)); f.seek(-5 * 4096, 2); h.update(f.read())
for n in ('sdb', 'sdc', 'sdd', 'sde'):
    h.update(open(os.path.join(d, n), 'rb').read())
print(h.hexdigest())
PY
}

hdr "1. a rehearsal bundle (fake images; real OS stores and ext4 roots) and a fake phone (blank 128 GB card)"
python3 -B tools/installer/fakes/mkbundle.py "$B" | tail -1
"${FP[@]}" create "$W/phone" --card blank
P=$W/phone ST=$W/state
COMMON=(--bundle "$B" --fake-root "$P" --state-dir "$ST")

hdr "2. the bundle against its manifest"
run 0 "bundle verified" -- "${SI[@]}" --verify-bundle "$B" --state-dir "$ST"
has 'bundle verified: 14 items; optional items not in it: windows, ios26-root' "14 items present, 2 optional ones absent"

hdr "3. refusals before anything is written"
cp -R "$B" "$W/b-missing"; rm "$W/b-missing/android/vendor.img"
run 2 "a missing file is refused" -- "${SI[@]}" --verify-bundle "$W/b-missing" --state-dir "$ST"
has 'android-vendor: missing file android/vendor.img' "names the missing file"
cp -R "$B" "$W/b-tampered"; printf 'X' | dd of="$W/b-tampered/boot/starbite-boot.img" bs=1 seek=100 conv=notrunc 2>/dev/null
run 2 "a changed file is refused" -- "${SI[@]}" --verify-bundle "$W/b-tampered" --state-dir "$ST"
has 'boot: boot/starbite-boot.img sha256' "names the file whose hash differs"
cp -R "$B" "$W/b-efs"; printf '\n[[item]]\nkey = "efs"\nfile = "boot/starbite-boot.img"\nsize = 4096\nsha256 = "%064d"\ndest = "part:EFS"\n' 0 >> "$W/b-efs/installer-manifest.toml"
run 2 "a manifest that names EFS is refused" -- "${SI[@]}" --verify-bundle "$W/b-efs" --state-dir "$ST"
has 'EFS is a protected area' "says EFS is protected"
rm -rf "$W/b-missing" "$W/b-tampered" "$W/b-efs"
run 2 "--rehearse before a dry run is refused" -- "${SI[@]}" "${COMMON[@]}" --rehearse --yes
has 'no finished dry run' "asks for the dry run first"
run 2 "--yes with --real is refused" -- "${SI[@]}" --bundle "$B" --real --yes
run 2 "--fake-root with --real is refused" -- "${SI[@]}" "${COMMON[@]}" --real

hdr "4. dry run (fresh install: --replace-all): reads the phone, prints every command, writes nothing"
before=$(fakehash "$P")
BRIEF_WARN=1 run 0 "dry run" -- "${SI[@]}" "${COMMON[@]}" --dry-run --yes --replace-all
[ "$(fakehash "$P")" = "$before" ] && ok "dry run: the fake phone is byte-identical" || bad "dry run changed the fake phone"
has 'nothing was written \([0-9]+ write commands printed\)' "dry run printed its write commands"
has 'part-write.py SYSTEM build/installer-rehearsal/bundle/android/system.img --state' "exact command for SYSTEM shown"

hdr "5. rehearsal with a cable pull in the middle of SYSTEM (chunk 1 of 3)"
prot=$(protected "$P")
rm -f "$P/fail-count"
FAKE_FAIL='seek=64 ' run 3 "stopped by the simulated cable pull" -- "${SI[@]}" "${COMMON[@]}" --rehearse --yes --replace-all --no-reboot
has 'STOPPED: command failed' "the failure is reported as STOPPED (resumable)"
ls "$ST"/phone-*/chunks/android-system-*.state >/dev/null 2>&1 && ok "SYSTEM chunk state kept: $(cat "$ST"/phone-*/chunks/android-system-*.state | tr '\n' ' ')" || bad "no chunk state"

hdr "6. the same command again: resumes"
run 0 "resumed run finished" -- "${SI[@]}" "${COMMON[@]}" --rehearse --yes --replace-all --no-reboot
has '7  resume  Android 17 system' "SYSTEM resumed (not restarted)"
has 'skip  Back up the protected areas  this computer' "the finished backup is not taken again"

hdr "7. and again: nothing left to do"
run 0 "third run" -- "${SI[@]}" "${COMMON[@]}" --rehearse --yes --no-reboot
has 'written 0, skipped 11, kept 8; 0 write commands ran' "0 write commands"

hdr "8. checks on the fake phone"
[ "$(protected "$P")" = "$prot" ] && ok "EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, GPT head/tail, sdb-sde: byte-identical" || bad "a protected area changed"
python3 - "$B" "$P" <<'PY' && ok "every written target reads back as its image" || bad "a target differs from its image"
import hashlib, os, sys, tomllib
b, p = sys.argv[1:]
m = tomllib.loads(open(os.path.join(b, 'installer-manifest.toml')).read())
dev = os.path.join(p, 'dev', 'block')
bad = 0
for it in m['item']:
    f = it.get('file')
    if not f or not os.path.exists(os.path.join(b, f)):
        continue
    d, size = it['dest'], it['size']
    if d.startswith('part:'):
        path, off = os.path.join(dev, 'by-name', d[5:]), 0
    elif d.startswith('slot:'):
        path, off = os.path.join(dev, 'by-name', 'USERDATA'), int(d.split('@')[1])
    elif d.startswith('card:lba:'):
        path, off = os.path.join(dev, 'mmcblk0'), int(d[9:]) * 512
    elif d.startswith('card:file:'):
        path, off = os.path.join(p, 'external_sd') + d[10:], 0
    else:
        continue   # stores: checked by the installer (all but the phone's settings block)
    with open(path, 'rb') as fh:
        fh.seek(off)
        got = hashlib.sha256(fh.read(size)).hexdigest()
    print(f'  {it["key"]:<16} {d:<32} {"ok" if got == it["sha256"] else "DIFFERS"}')
    bad += got != it['sha256']
sys.exit(bad)
PY
n=$(cat "$ST"/phone-*/backups/*/backups.txt | wc -l | tr -d ' ')
[ "$n" = 24 ] && ok "backup: 24 areas, each hashed on the phone and on this computer" || bad "backup has $n lines"
python3 - "$ST" <<'PY' && ok "journal: every present item done, ODM and A16DATA fresh" || bad "journal"
import glob, json, sys
j = json.load(open(glob.glob(sys.argv[1] + '/phone-*/journal.json')[0]))
it = j['items']
print('  ' + ', '.join(f'{k}={v["status"]}' for k, v in sorted(it.items())))
assert it['android-data']['fresh'] and it['android-metadata']['fresh']
assert all(v['status'] == 'done' for k, v in it.items() if k not in ('windows', 'ios26-root'))
PY

hdr "9. the main phone without a journal (installed some other way): nothing replaced"
run 0 "dry run with a fresh state directory" -- "${SI[@]}" --bundle "$B" --fake-root "$P" --state-dir "$W/state-nojournal" --dry-run --yes
has 'keep  macOS Ventura root' "data on the card is kept"
has 'keep  Android /data' "Android data is kept"
has 'skip  Starbite loader \+ menu \(BOOT\)' "identical BOOT is skipped"
has 'has data: kept \(--replace mac13-root overwrites it\)' "says how to replace on purpose"

hdr "10. a phone in Download mode (no TWRP yet): TWRP via heimdall, then the dry run gate"
"${FP[@]}" create "$W/phone-dl" --state download --card starbite >/dev/null
DL=(--bundle "$B" --fake-root "$W/phone-dl" --state-dir "$W/state-dl")
run 0 "dry run in Download mode: shows the heimdall step, plans without phone checks" -- "${SI[@]}" "${DL[@]}" --dry-run --yes
has 'heimdall flash --RECOVERY .*twrp-starbite.img --no-reboot' "the exact heimdall command"
run 2 "rehearsal: TWRP flashed and booted, then the gate asks for a dry run" -- "${SI[@]}" "${DL[@]}" --rehearse --yes
has 'TWRP is up' "TWRP came up after the (fake) key combination"
run 0 "dry run in TWRP" -- "${SI[@]}" "${DL[@]}" --dry-run --yes --replace-all
run 0 "rehearsal completes" -- "${SI[@]}" "${DL[@]}" --rehearse --yes --replace-all --no-reboot
has 'written 1[0-9], skipped' "installed"

hdr "11. a phone in the Starbite loader's fastboot"
"${FP[@]}" create "$W/phone-fb" --state fastboot --card starbite >/dev/null
run 0 "dry run: no reboot in a dry run" -- "${SI[@]}" --bundle "$B" --fake-root "$W/phone-fb" --state-dir "$W/state-fb" --dry-run --yes
has 'fastboot reboot recovery  \[dry run: not run\]' "shows fastboot reboot recovery"
run 2 "rehearsal: reboots to TWRP, then the gate" -- "${SI[@]}" --bundle "$B" --fake-root "$W/phone-fb" --state-dir "$W/state-fb" --rehearse --yes
has 'found: fastboot-starbite \(starbite-fb1\)' "detected the Starbite loader's fastboot"

hdr "12. preflight refusals: another phone model, a 32 GB card, a flat battery, a card with another layout"
rm -rf "$W/phone-x"; "${FP[@]}" create "$W/phone-x" --card starbite >/dev/null
sed -i.bak 's/^ro.product.model=.*/ro.product.model=SM-G965F/; s/^ro.product.device=.*/ro.product.device=star2lte/' "$W/phone-x/props"
run 2 "an S9+ (SM-G965F) is refused" -- "${SI[@]}" --bundle "$B" --fake-root "$W/phone-x" --state-dir "$W/state-x" --dry-run --yes
has 'model  SM-G965F / star2lte  FAIL' "names the model"
rm -rf "$W/phone-x"; "${FP[@]}" create "$W/phone-x" --card blank --card-sectors 62521344 --battery 20 >/dev/null
run 2 "a 32 GB card and 20 % battery are refused" -- "${SI[@]}" --bundle "$B" --fake-root "$W/phone-x" --state-dir "$W/state-x" --dry-run --yes
has 'microSD: no card, or smaller than 64 GB' "the card is too small"
has 'battery: below 50%' "the battery is too low"
rm -rf "$W/phone-x"; "${FP[@]}" create "$W/phone-x" --card foreign >/dev/null
run 2 "a card with another layout is not erased without --erase-card" -- "${SI[@]}" --bundle "$B" --fake-root "$W/phone-x" --state-dir "$W/state-x" --dry-run --yes
has 'BLOCKED  microSD partition table' "the card step is BLOCKED"
run 0 "with --erase-card the plan erases it" -- "${SI[@]}" --bundle "$B" --fake-root "$W/phone-x" --state-dir "$W/state-x" --dry-run --yes --erase-card
has 'REPLACE  microSD partition table' "the card step is REPLACE"

hdr "13. rescue USB stick (fake disks; no real disk)"
FD=$W/disks
python3 - "$FD" <<'PY'
import os, plistlib, sys
d = sys.argv[1]
os.makedirs(os.path.join(d, 'diskutil')); os.makedirs(os.path.join(d, 'dev'))
info = {
 'root': {'ParentWholeDisk': 'disk3', 'APFSPhysicalStores': [{'APFSPhysicalStore': 'disk0s2'}]},
 'disk0': {'WholeDisk': True, 'Internal': True, 'BusProtocol': 'Apple Fabric', 'TotalSize': 251000193024, 'MediaName': 'APPLE SSD'},
 'disk4': {'WholeDisk': True, 'Internal': False, 'Removable': True, 'BusProtocol': 'USB', 'TotalSize': 32017047552, 'MediaName': 'SanDisk Ultra Fit'},
 'disk5': {'WholeDisk': True, 'Internal': False, 'BusProtocol': 'USB', 'TotalSize': 1000204886016, 'MediaName': 'Samsung PSSD T7'},
 'disk6': {'WholeDisk': True, 'Internal': False, 'BusProtocol': 'Disk Image', 'VirtualOrPhysical': 'Virtual', 'TotalSize': 67108864, 'MediaName': 'Disk Image'},
}
for k, v in info.items():
    open(os.path.join(d, 'diskutil', f'info-{k}.plist'), 'wb').write(plistlib.dumps(v))
with open(os.path.join(d, 'dev', 'disk4'), 'wb') as f:
    f.truncate(80 << 20)                       # stands in for the stick (the image fits in it)
PY
IMG=$W/rescue.img
if [ -f "${RESCUE_IMG:-}" ]; then cp "$RESCUE_IMG" "$IMG"; echo "rescue image: $(basename "$RESCUE_IMG")"
else head -c $((64 << 20)) /dev/zero > "$IMG"; printf 'SB-RESCUE stand-in' | dd of="$IMG" conv=notrunc 2>/dev/null; echo "rescue image: a 64 MiB stand-in"; fi
export SBINST_FAKE_DISKS=$FD
run 2 "the Mac's internal disk is refused" -- "${SI[@]}" --make-rescue-usb /dev/disk0 --image "$IMG" --rehearse --yes --state-dir "$ST"
has 'running system' "...because it holds the running system"
run 2 "a 1 TB USB SSD is refused" -- "${SI[@]}" --make-rescue-usb /dev/disk5 --image "$IMG" --rehearse --yes --state-dir "$ST"
has 'larger than 256 GiB' "...because it is larger than 256 GiB"
run 2 "a disk image is refused" -- "${SI[@]}" --make-rescue-usb /dev/disk6 --image "$IMG" --rehearse --yes --state-dir "$ST"
run 2 "a partition name is refused" -- "${SI[@]}" --make-rescue-usb /dev/disk4s1 --image "$IMG" --rehearse --yes --state-dir "$ST"
run 0 "dry run on the 32 GB stick shows the disk and the commands" -- "${SI[@]}" --make-rescue-usb /dev/disk4 --image "$IMG" --dry-run --state-dir "$ST"
has 'sudo dd if=.* of=/dev/rdisk4 bs=1m' "the exact dd command (raw device)"
run 0 "rehearsal: typed name, written, read back" -- "${SI[@]}" --make-rescue-usb /dev/disk4 --image "$IMG" --rehearse --yes --state-dir "$ST"
python3 - "$IMG" "$FD/dev/disk4" <<'PY' && ok "the fake stick starts with the image" || bad "fake stick differs"
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read(len(a))
sys.exit(a != b)
PY
unset SBINST_FAKE_DISKS

hdr "result: $pass passed, $fail failed"
[ "${KEEP:-0}" = 1 ] || rm -rf "$W"
[ "$fail" = 0 ]
