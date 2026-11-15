#!/bin/bash
# night-s9b.sh S1_SERIAL S2_SERIAL [--rehearse FAKE_S1 FAKE_S2]
# S9 #2's steps C-H in one unattended run (shared notes installer-s9b-space-plan.md 3, 5; docs/INSTALLER.md 15-17):
#   P  S9 #1's card store pulled (store-pull.sh, read only) and the entries C-H need taken out of it
#   C  the UFS partition table = S9 #1's (starbite-oneui; --repartition; S9 #2's stock /data goes: the user's O1), and
#      in the same installer run One UI's /data made fresh on A16DATA (the installer marks it fresh at the GPT step)
#   D  One UI 8: SYSTEM, VENDOR, ODM streamed from S9 #1; android.* + the row into the card store
#   E  ChromiumOS, Ubuntu, postmarketOS (+ _tools.img): card p1 files streamed; linux.* + the rows
#   F  macOS Sequoia: S9 #1's 16 GiB card range into the sd64l mac15 slot; the payload retargeted; the row
#   G  Windows 11 (+ big cores): WINESP, WINDOWS streamed; the two .uefi files + rows
#   H  iOS 27: USERDATA streamed (18.75 GiB); its payload + ios.overlay + the row
# Every write is a dry run first, then the real one with its typed line piped. S9 #1 (TWRP, adb -s S1) is only ever
# read; S9 #2 is written only through its installer environment (ssh) by starbite-install, stream-copy.py and
# store-update.py, which refuse RECOVERY, BOOT, EFS, CPEFS, PARAM ... Nothing here reboots either phone: no One UI
# start tonight (and no other). S9 #2 stays in its installer environment at the end.
# Stops at the first failure ("### step X failed: ..." in the log). Run it again to resume: finished steps and
# sub-steps are skipped (done markers), and an interrupted copy or write resumes inside its tool.
# NIGHT_ONLY="P E F": only those steps (the others are left as they are, done or not).
# Log: build/installer/logs/night-1115.log: each command's proof lines + "### step X done/failed" for a watcher.
# Rehearsal: --rehearse FAKE_S1 FAKE_S2 (tools/installer/fakes, tools/installer/rehearse-night.sh).
# NIGHT_REPO = the checkout whose tools run (default: this script's ../..); build/ (state, logs, bundles, catalog)
# is always the main checkout's (NIGHT_BUILD), so a copy in build/installer/ can run a worktree's tools.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${NIGHT_REPO:-$(cd "$HERE/../.." && pwd)}       # the tools (a checkout with tools/installer, tools/store)
cd "$REPO" || exit 2
BUILD=${NIGHT_BUILD:-${REPO%%/.claude/worktrees/*}}    # build/ lives in the main checkout
S1=${1:?usage: night-s9b.sh S1_SERIAL S2_SERIAL [--rehearse FAKE_S1 FAKE_S2]}
S2=${2:?usage: night-s9b.sh S1_SERIAL S2_SERIAL [--rehearse FAKE_S1 FAKE_S2]}
shift 2
MODE=real F1= F2=
if [ "${1:-}" = --rehearse ]; then MODE=rehearse; F1=$(cd "$2" && pwd); F2=$(cd "$3" && pwd); fi
ST=${NIGHT_STATE:-$BUILD/build/installer}
export SBINST_STATE=$ST
NB=${NIGHT_DIR:-$ST/night-s9b}                # work: logs, entries, catalogs, done markers
BUN=${NIGHT_BUNDLES:-$BUILD/build/installer/night-s9b}   # bundle-c (the GPT + One UI's fresh /data), prebuilt
LOG=${NIGHT_LOG:-$ST/logs/night-1115.log}
CAT_LIVE=${NIGHT_CATALOG:-$BUILD/build/store/catalog-live.toml}
CAT_A=${NIGHT_CATALOG_A:-$ST/bundle-s9b/store/catalog.toml}
CAP=${NIGHT_SIZE_CAP:-}                       # rehearsal only: copy this many bytes of each big source
ONLY=${NIGHT_ONLY:-}                          # e.g. "P E F": only these steps (E, F write only the card, not UFS)
want() { [ -z "$ONLY" ] || case " $ONLY " in *" $1 "*) true ;; *) false ;; esac; }
mkdir -p "$NB/logs" "$NB/entries" "$(dirname "$LOG")"
PID=$(python3 -c 'import hashlib,sys;print("phone-"+hashlib.sha256(sys.argv[1].encode()).hexdigest()[:10])' "$S2")
SI=(python3 -B tools/installer/starbite-install --serial "$S2" --state-dir "$ST")
SC=(python3 -B tools/installer/stream-copy.py --from-serial "$S1" --to-serial "$S2" --state-dir "$ST")
SU=(python3 -B tools/installer/store-update.py --serial "$S2" --state-dir "$ST")
if [ $MODE = rehearse ]; then
	R_SI=(--rehearse --yes --fake-root "$F2") R_SC=(--rehearse --yes --fake-src "$F1" --fake-dst "$F2")
	R_SU=(--rehearse --yes --fake-root "$F2") D_SI=(--dry-run --yes --fake-root "$F2")
	D_SC=(--dry-run --fake-src "$F1" --fake-dst "$F2") D_SU=(--dry-run --fake-root "$F2")
else
	R_SI=(--real) R_SC=(--real) R_SU=(--real) D_SI=(--dry-run) D_SC=(--dry-run) D_SU=(--dry-run)
fi
STEP=-
now() { date +%H:%M:%S; }
log() { echo "$*" | tee -a "$LOG"; }
mark() { log "### step $1 $2 ($(now))"; }
die() { mark "$STEP" "failed: $*"; log "   the phones stay as they are; run the same command again to resume"; exit 1; }
done_p() { [ -e "$NB/done-$1" ]; }
set_done() { : > "$NB/done-$1"; }
# run NAME ANSWERS -- COMMAND...: full output to $NB/logs/NAME.txt, proof lines to the log; a failure ends the night
run() {
	local name=$1 answers=$2; shift 3
	local out=$NB/logs/$name.txt rc
	log "   [$(now)] $name: $(printf '%q ' "$@" | sed -e "s#$REPO/##g" -e 's#PATH=[^ ]* ##' | cut -c1-240)"
	printf "$answers" | "$@" > "$out" 2>&1
	rc=$?
	grep -E '^(ok|WARN battery|STOPPED|REFUSED)|written [0-9]|both phones agree|battery  ' "$out" | grep -v PENDING |
		sed 's/^/      /' | tee -a "$LOG" > /dev/null
	[ $rc = 0 ] || die "$name exited $rc ($(grep -E '^(STOPPED|REFUSED)' "$out" | head -1 | cut -c1-200))"
}
# twice: the dry run, then the real one
twice() {     # SUBSTEP ANSWERS-REAL TOOL -- ARGS...
	local sub=$1 ans=$2 tool=$3; shift 4
	done_p "$sub" && { log "   $sub: done earlier"; return; }
	case $tool in
	si) run "$sub-dry" 'y\ny\n' -- "${SI[@]}" "$@" "${D_SI[@]}"; run "$sub" "$ans" -- "${SI[@]}" "$@" "${R_SI[@]}" --no-reboot ;;
	sc) run "$sub-dry" '' -- "${SC[@]}" "$@" "${D_SC[@]}"; run "$sub" "$ans" -- "${SC[@]}" "$@" "${R_SC[@]}" ;;
	su) run "$sub-dry" '' -- "${SU[@]}" "$@" "${D_SU[@]}"; run "$sub" "$ans" -- "${SU[@]}" "$@" "${R_SU[@]}" ;;
	esac
	set_done "$sub"
}
cap() { [ -n "$CAP" ] && echo --size && echo "$CAP"; }      # rehearsal only
stream() {    # SUBSTEP SOURCE TARGET [--size N]
	local sub=$1 src=$2 tgt=$3; shift 3
	twice "$sub" "stream to $PID\n" sc -- --source "$src" --target "$tgt" "$@"
}
# the card store: the rows S9 #2 has + the step's rows (from S9 #1's catalog, roots moved to sd64l) + entry files
store() {     # SUBSTEP "KEY ..." NAME ...
	local sub=$1 keys=$2; shift 2
	if done_p "$sub"; then      # also after an interruption between the done marker and the copy below
		[ -f "$NB/catalog-$sub.toml" ] && cp "$NB/catalog-$sub.toml" "$NB/catalog-cur.toml"
		log "   $sub: done earlier"; return
	fi
	[ -f "$NB/catalog-cur.toml" ] || cp "$CAT_A" "$NB/catalog-cur.toml"
	python3 - "$NB/catalog-cur.toml" "$CAT_LIVE" "$keys" "$NB/catalog-$sub.toml" <<'PY' || die "catalog for $sub"
import re, sys
sys.path.insert(0, 'tools/store')
import cardlayout
cur, live, keys, out = open(sys.argv[1]).read(), open(sys.argv[2]).read(), sys.argv[3].split(), sys.argv[4]
have = set(re.findall(r'(?m)^key\s*=\s*"([^"]+)"', cur))
blocks = re.split(r'(?m)^\[\[entry\]\]\s*$', live)[1:]
add = []
for k in keys:
    if k in have:
        continue
    b = next((b for b in blocks if re.search(rf'(?m)^key\s*=\s*"{re.escape(k)}"', b)), None)
    if b is None:
        sys.exit(f'no row {k} in {sys.argv[2]}')
    add.append('\n[[entry]]' + b.rstrip() + '\n')
text = cur.rstrip() + '\n' + ''.join(add)
open(out, 'w').write(cardlayout.catalog_text(text, 'sd64l'))
PY
	local files=() n
	for n in "$@"; do
		[ -f "$NB/entries/$n" ] || die "store entry $n was not in S9 #1's card store"
		files+=("$n=$NB/entries/$n")
	done
	twice "$sub" "update store on $PID\n" su -- --catalog "$NB/catalog-$sub.toml" "${files[@]}"
	cp "$NB/catalog-$sub.toml" "$NB/catalog-cur.toml"
}

log ""
log "######## night-s9b ($MODE) $(date '+%F %T'): S9 #1 = source (TWRP, read only), S9 #2 = $PID (installer environment)"

if [ $MODE = rehearse ]; then           # adb for S9 #1 (read only): the real one, or the fake for FAKE_S1
	ADBENV=(env PATH="$REPO/tools/installer/fakes:$PATH" FAKE_ROOT="$F1" FAKE_SERIAL="$S1" ANDROID_SERIAL="$S1")
else
	ADBENV=(env ANDROID_SERIAL="$S1")
fi

# ---- P: S9 #1's card store, read only
STEP=P
if want P && ! done_p P; then
	mark P started
	[ "$("${ADBENV[@]}" adb -s "$S1" get-state 2>/dev/null)" = recovery ] || die "S9 #1 ($S1) is not in TWRP"
	run P-pull '' -- "${ADBENV[@]}" bash tools/store/store-pull.sh "$NB/s1-store.img" sd
	CAT_LIVE_PATH=$CAT_LIVE python3 - "$NB/s1-store.img" "$NB/entries" <<'PY' || die "taking the entries out of S9 #1's store"
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location('sb', 'tools/store/sbstore2.py'); sb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sb)
want = ['android.kernel', 'android.ramdisk', 'android.cmdline', 'android.dtb', 'linux.image', 'linux.dtb', 'linux.initrd',
        'windows-t32s-nocache.uefi', 'windows-big3i.uefi', 'ios27-refport-t8.xnu', 'ios.overlay']
import re, tomllib
live = tomllib.loads(open(os.environ.get('CAT_LIVE_PATH')).read())
mac = next(e for e in live['entry'] if e['key'] == 'mac15')['files'][0]
want.append(mac)
open(os.path.join(sys.argv[2], '..', 'mac15-file'), 'w').write(mac)
got = {n: b for n, _, b in sb.read_store(sys.argv[1])}           # every CRC checked
miss = [n for n in want if n not in got]
if miss:
    sys.exit('not in S9 #1\'s card store: ' + ' '.join(miss))
for n in want:
    open(os.path.join(sys.argv[2], n), 'wb').write(got[n])
print(f'{len(want)} entries taken out')
PY
	rm -f "$NB/s1-store.img"
	set_done P
	mark P done
fi
MAC15=$(cat "$NB/mac15-file" 2>/dev/null)
[ -n "$MAC15" ] || die "no entries from S9 #1 (step P has not run)"

# ---- C: the partition table
STEP=C
if want C && ! done_p C; then
	mark C started
	twice C-gpt "y\ny\ninstall $PID\nrepartition $PID\n" si -- --bundle "$BUN/bundle-c" --repartition
	grep -q 'the kernel sees the starbite-oneui table\|skip  UFS partition table' "$NB/logs/C-gpt.txt" || die "no proof of the new table in the log"
	grep -q 'oneui-data: verified on the phone.*TYPE ext4 LABEL data\|keep .*One UI 8 /data' "$NB/logs/C-gpt.txt" ||
		die "no proof that One UI's /data was made (A16DATA)"
	set_done C; mark C done
fi

# ---- D: One UI 8
STEP=D
if want D && ! done_p D; then
	mark D started
	stream D-system part:SYSTEM part:SYSTEM $(cap)
	stream D-vendor part:VENDOR part:VENDOR $(cap)
	stream D-odm part:ODM part:ODM
	store D-store "android" android.kernel android.ramdisk android.cmdline android.dtb
	set_done D; mark D done
fi

# ---- E: the Linux distros on p1
STEP=E
if want E && ! done_p E; then
	mark E started
	for f in chromeos ubuntu pmos; do stream "E-$f" "file:/distros/$f.img" "file:/distros/$f.img"; done
	keys="chromeos linux-ubuntu linux-pmos"
	if done_p E-tools; then
		tools=yes
	else       # an explicit answer: an adb hiccup or an unmounted card must not read as "S9 #1 has none"
		tools=$("${ADBENV[@]}" adb -s "$S1" shell 'if [ -f /external_sd/distros/_tools.img ]; then echo yes; elif [ -d /external_sd/distros ]; then echo no; fi' 2>/dev/null | tr -d '\r')
	fi
	case $tools in
	yes) stream E-tools file:/distros/_tools.img file:/distros/_tools.img; keys="$keys linux-shell" ;;
	no) log "   E-tools: S9 #1 has /external_sd/distros but no _tools.img: the hidden Starbite shell row is left out" ;;
	*) die "could not tell whether S9 #1 has /external_sd/distros/_tools.img (adb said '${tools}'; is p1 mounted in TWRP?)" ;;
	esac
	store E-store "$keys" linux.image linux.dtb linux.initrd
	set_done E; mark E done
fi

# ---- F: macOS Sequoia
STEP=F
if want F && ! done_p F; then
	mark F started
	stream F-root card:lba:182888448+33554432 card:root:mac15 $(cap)
	if ! done_p F-payload; then
		python3 - "$NB/entries/$MAC15" <<'PY' || die "retargeting the Sequoia payload to sd64l"
import sys
sys.path.insert(0, 'tools/store')
import cardlayout
p = sys.argv[1]
blob = open(p, 'rb').read()
if cardlayout.sdroot_of(cardlayout.payload_cmdline(blob) or '') != (2105344, 33554432):
    open(p, 'wb').write(cardlayout.retarget_payload(blob, 'sd64l', 'mac15'))
print('payload sdroot:', cardlayout.sdroot_of(cardlayout.payload_cmdline(open(p, 'rb').read())))
PY
		set_done F-payload
	fi
	store F-store "mac15" "$MAC15"
	set_done F; mark F done
fi

# ---- G: Windows 11 (+ the big cores)
STEP=G
if want G && ! done_p G; then
	mark G started
	stream G-esp part:WINESP part:WINESP $(cap)
	stream G-windows part:WINDOWS part:WINDOWS $(cap)
	store G-store "win11 win11-big" windows-t32s-nocache.uefi windows-big3i.uefi
	set_done G; mark G done
fi

# ---- H: iOS 27
STEP=H
if want H && ! done_p H; then
	mark H started
	stream H-userdata part:USERDATA part:USERDATA $(cap)
	store H-store "ios27" ios27-refport-t8.xnu ios.overlay
	set_done H; mark H done
fi

STEP=end
if [ -n "$ONLY" ]; then
	mark "ONLY($ONLY)" "done: S9 #2 stays in its installer environment (no reboot tonight)"
else
	mark ALL "done: S9 #2 has C-H; it stays in its installer environment (no reboot tonight)"
fi
exit 0
