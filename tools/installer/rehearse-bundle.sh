#!/bin/bash
# tools/installer/rehearse-bundle.sh BUNDLE [WORKDIR]: one REAL bundle (made for a stock phone: BOOT + the
# installer environment + the card, no [ufs] layout) through the whole first-install flow against a FAKE
# stock S9 with a blank 64 GB card (tools/installer/fakes/): no phone, no adb server, nothing real touched.
# The steps are the runbook's (shared notes installer-s9b-runbook.md), all with --serial:
#   dry run in stock Android -> adb -s reboot download -> dry run in Download mode (PIT, the gate) ->
#   the flash (BOOT via the fake heimdall, loader fastboot, the environment, then the dry-run gate) ->
#   --backup-only (every file checked here) -> dry run in the environment -> the install -> again: nothing to do.
# Exit 0 only if every step ends as the runbook expects. WORKDIR (default: a temporary directory) is removed
# unless KEEP=1.
set -uo pipefail
cd "$(dirname "$0")/../.."
B=$(cd "${1:?usage: rehearse-bundle.sh BUNDLE [WORKDIR]}" && pwd)
R=${2:-$(mktemp -d)}
rm -rf "$R"; mkdir -p "$R"
export SBINST_STATE=$R/state                       # the fake run's own journal, backups and ssh key
unset ANDROID_SERIAL SBINST_SERIAL FAKE_OTHER_PHONE SBINST_FAKE_USB_EXTRA
SI=(python3 -B tools/installer/starbite-install)
python3 -B tools/installer/fakes/fakephone.py create "$R/ph" --state android --stock --gpt stock --card blank \
	--card-sectors 124735488 >/dev/null
C=(--bundle "$B" --fake-root "$R/ph" --state-dir "$R/state" --serial FAKE0000SERIAL)
fail=0
step() {    # WANT NAME -- COMMAND...
	local want=$1 n=$2; shift 3
	"$@" > "$R/out-${n// /-}.txt" 2>&1
	local rc=$?
	if [ "$rc" = "$want" ]; then echo "PASS $n (exit $rc)"; else echo "FAIL $n (exit $rc, wanted $want)"; fail=1; fi
	grep -E "^(REFUSED|STOPPED|FAIL)|written [0-9]" "$R/out-${n// /-}.txt" | sed 's/^/     /'
}
ud=$(shasum -a 256 < "$R/ph/dev/block/by-name/USERDATA"); rec=$(shasum -a 256 < "$R/ph/dev/block/by-name/RECOVERY")
efs=$(cat "$R/ph/dev/block/by-name/"{EFS,CPEFS,PARAM,KEYSTORAGE,PERSISTENT,STEADY,DTB,DTBO} | shasum -a 256)
step 0 "dry run, stock Android" -- "${SI[@]}" "${C[@]}" --dry-run --yes
FAKE_ROOT=$R/ph tools/installer/fakes/adb -s FAKE0000SERIAL reboot download
step 0 "dry run, Download mode (PIT)" -- "${SI[@]}" "${C[@]}" --dry-run --yes
step 2 "the flash, then the dry-run gate" -- "${SI[@]}" "${C[@]}" --rehearse --yes
step 0 "backup only" -- "${SI[@]}" "${C[@]}" --rehearse --yes --backup-only
step 0 "check the backup" -- "${SI[@]}" --check-backup "$(ls -d "$R"/state/phone-*/backups/* | head -1)"
step 0 "dry run, environment" -- "${SI[@]}" "${C[@]}" --dry-run --yes
step 0 "install" -- "${SI[@]}" "${C[@]}" --rehearse --yes --no-reboot
step 0 "again" -- "${SI[@]}" "${C[@]}" --rehearse --yes --no-reboot
grep -q '; 0 write commands ran' "$R/out-again.txt" || { echo "FAIL the second install still wrote"; fail=1; }
[ "$(shasum -a 256 < "$R/ph/dev/block/by-name/USERDATA")" = "$ud" ] || { echo "FAIL USERDATA changed"; fail=1; }
[ "$(shasum -a 256 < "$R/ph/dev/block/by-name/RECOVERY")" = "$rec" ] || { echo "FAIL RECOVERY changed"; fail=1; }
[ "$(cat "$R/ph/dev/block/by-name/"{EFS,CPEFS,PARAM,KEYSTORAGE,PERSISTENT,STEADY,DTB,DTBO} | shasum -a 256)" = "$efs" ] ||
	{ echo "FAIL a protected partition changed"; fail=1; }
python3 tools/store/sbstore2.py header "$R/ph/dev/block/mmcblk0p2" | sed 's/^/     card store: /' | head -1
[ "$fail" = 0 ] && echo "RESULT: the bundle passed every runbook step on a fake stock S9" || echo "RESULT: FAILED (logs in $R)"
[ "${KEEP:-0}" = 1 ] || [ "$fail" != 0 ] || rm -rf "$R"
exit $fail
