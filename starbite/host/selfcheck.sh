#!/bin/sh
# Engineer OS host self-checks (docs/ENGINEER-OS.md section 2.1). Run on the Mac from the repo root:
#   sh starbite/host/selfcheck.sh
# 1. builds Engineer OS twice in the Lima VM and requires identical images (build.sh --repro: the
#    unit tests, undefined symbol / relocation / pre-relocation GOT checks, the ARMv8.0 check of the
#    worker-core code, the image header and size budget, the build-host path scan and the
#    relocation test run in each build);
# 2. the keys-only tour on the host preview: every row of every tab opened with Volume and Power
#    only, each page drawn and fully presented, and holding Power always back to the Dashboard;
# 3. the Lab tour: every Lab test started with keys and stopped with Power held (its result page
#    must show), a tier 1 write end to end, the M3 prompt cancelled, an unconfirmed write refused.
# Nothing touches the phone. Prints the image's sha256 at the end.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
limactl shell --workdir "$root" starbite sh starbite/engineer/build.sh --repro
for t in tour labtour; do
    log="$root/starbite/out/$t.log"
    if ! sh "$root/starbite/host/preview.sh" "$t" > "$log" 2>&1; then
        grep -E 'FAIL|CRASH|tour:' "$log" >&2 || tail -20 "$log" >&2
        echo "selfcheck: the $t FAILED (full log: starbite/out/$t.log)" >&2
        exit 1
    fi
    tail -1 "$log"
done
echo "selfcheck: all passed"
cat "$root/starbite/out/engineer.img.sha256"
