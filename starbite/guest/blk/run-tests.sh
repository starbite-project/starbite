#!/bin/bash
# sbblk's QEMU tests (docs/STARBITE-LINUX-APPS.md L4): the guest kernel + sbguest.ko's blk-mq driver against
# sbghost's block server in sbsim, on file-backed devices. One PASS/FAIL line per test:
#   a read       the base EROFS image's known tree matches its md5 list (and the host's md5 of that list)
#   b persist    QEMU run 1 writes a file to the overlay and syncs; a second QEMU run sees it; the host finds the
#                same bytes in the overlay file with debugfs
#   c speed      200 MiB read from sbblk0, 100 MiB written to sbblk1 (QEMU TCG: indicative only)
#   d stress     parallel readers and writers with verification; e2fsck of the overlay file afterwards
#   e fallback   no overlay device, and an overlay device that is not ext4: tmpfs upper + the warning
#   f poll       the read test without doorbells (both sides poll)
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/blk/run-tests.sh [a b c d e f]
# WORK (/mnt/build/sbguest2/blk): images in $WORK/img (mk-test-images.sh), logs in $WORK/out/test-*.log.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export WORK=${WORK:-/mnt/build/sbguest2/blk}
img=$WORK/img
[ -f "$img/base.erofs" ] || OUT=$img "$here/mk-test-images.sh" || exit 1
tree=$(cat "$img/tree.md5.md5")
want=${*:-a b c d e f}
res=$WORK/out/results.txt
mkdir -p "$WORK/out"
: > "$res"
run() {     # run NAME EXTRA [env...]: one QEMU run; the guest's verdict lines go to $res
    local name=$1 extra=$2
    shift 2
    env "$@" BASE="$img/base.erofs" EXTRA="sbblk.tree=$tree $extra" LOG="out/test-$name.log" TAIL=0 \
        "$here/../run-desktop.sh" > "$WORK/out/test-$name.out" 2>&1
    local rc=$?
    grep -a 'sbblk-test: ' "$WORK/out/test-$name.log" | sed 's/.*sbblk-test: //' > "$WORK/out/test-$name.verdict"
    grep -a 'sbsim: run .*sbblk reads' "$WORK/out/test-$name.log" | sed 's/^/    /'
    return $rc
}
verdict() { # verdict LETTER NAME RC [more]: PASS only when QEMU passed, a PASS line is there and no FAIL line
    local l=$1 name=$2 rc=$3 more=${4:-}
    local v=$WORK/out/test-$name.verdict
    if [ "$rc" = 0 ] && grep -q '^PASS' "$v" && ! grep -q '^FAIL' "$v"; then
        echo "($l) PASS $(grep '^PASS' "$v" | tail -1 | cut -d' ' -f2-)$more" | tee -a "$res"
    else
        echo "($l) FAIL $(grep '^FAIL' "$v" | head -1 | cut -d' ' -f2-) [sbsim rc $rc: $(grep -a 'sbsim: RESULT' "$WORK/out/test-$name.log" | cut -c1-120)]$more" | tee -a "$res"
    fi
}
for t in $want; do
    case $t in
    a)
        cp --sparse=always "$img/overlay-empty.img" "$img/overlay-a.img"
        run a "sbblk.test=read" OVERLAY="$img/overlay-a.img" REPO="$img/repo.erofs"
        verdict a a $?
        ;;
    b)
        cp --sparse=always "$img/overlay-empty.img" "$img/overlay-b.img"
        run b1 "sbblk.test=persist sbblk.phase=1" OVERLAY="$img/overlay-b.img"
        rc1=$?
        md5_guest=$(sed -n 's/.*md5 \([0-9a-f]\{32\}\)).*/\1/p' "$WORK/out/test-b1.verdict" | head -1)
        # the host's own look at the overlay file between the runs (a copy: the journal may need a replay)
        cp --sparse=always "$img/overlay-b.img" "$img/overlay-b.chk"
        e2fsck -fy "$img/overlay-b.chk" > "$WORK/out/test-b-fsck.log" 2>&1
        md5_host=$(debugfs -R 'cat /upper/var/sbblk-persist/data' "$img/overlay-b.chk" 2>/dev/null | md5sum | cut -d' ' -f1)
        rm -f "$img/overlay-b.chk"
        run b2 "sbblk.test=persist sbblk.phase=2" OVERLAY="$img/overlay-b.img"
        rc2=$?
        cat "$WORK/out/test-b1.verdict" "$WORK/out/test-b2.verdict" > "$WORK/out/test-b.tmp"   # run 1's lines first
        mv "$WORK/out/test-b.tmp" "$WORK/out/test-b2.verdict"
        [ "$(grep -c '^PASS' "$WORK/out/test-b2.verdict")" = 2 ] || echo "FAIL persist: one of the two runs has no PASS line" >> "$WORK/out/test-b2.verdict"
        [ "$md5_guest" = "$md5_host" ] && [ -n "$md5_guest" ] || echo "FAIL persist: the overlay file holds md5 $md5_host, the guest wrote $md5_guest" >> "$WORK/out/test-b2.verdict"
        verdict b b2 $((rc1 + rc2)) " (two QEMU runs; the host reads the same md5 from the overlay file: $md5_host)"
        ;;
    c)
        cp --sparse=always "$img/overlay-empty.img" "$img/overlay-c.img"
        run c "sbblk.test=speed" OVERLAY="$img/overlay-c.img"
        verdict c c $?
        ;;
    d)
        cp --sparse=always "$img/overlay-empty.img" "$img/overlay-d.img"
        run d "sbblk.test=stress=${STRESS_ROUNDS:-6}" OVERLAY="$img/overlay-d.img" TIMEOUT=3000
        rc=$?
        e2fsck -fn "$img/overlay-d.img" > "$WORK/out/test-d-fsck.log" 2>&1
        fs=$?
        [ $fs = 0 ] || echo "FAIL stress: e2fsck -fn of the overlay file exits $fs" >> "$WORK/out/test-d.verdict"
        verdict d d $rc " (e2fsck -fn of the overlay file afterwards: exit $fs)"
        ;;
    e)
        run e1 "sbblk.test=fallback"
        rc1=$?
        head -c 67108864 /dev/urandom > "$img/overlay-e.img"       # a device that is not ext4
        run e2 "sbblk.test=fallback" OVERLAY="$img/overlay-e.img"
        rc2=$?
        grep -q '^PASS' "$WORK/out/test-e1.verdict" || echo "FAIL fallback: the run without an overlay device did not pass" >> "$WORK/out/test-e2.verdict"
        cat "$WORK/out/test-e1.verdict" | grep '^FAIL' >> "$WORK/out/test-e2.verdict"
        verdict e e2 $((rc1 + rc2)) " (and with no sbblk1 at all: $(grep -c '^PASS' "$WORK/out/test-e1.verdict") PASS)"
        rm -f "$img/overlay-e.img"
        ;;
    f)
        cp --sparse=always "$img/overlay-empty.img" "$img/overlay-f.img"
        run f "sbblk.test=read,persist" OVERLAY="$img/overlay-f.img" REPO="$img/repo.erofs" DB_GUEST=0 DB_HOST=0
        verdict f f $? " (no doorbells: timer poll in the guest, tick poll in the host)"
        ;;
    esac
done
echo "== $(grep -c ') PASS' "$res") PASS, $(grep -c ') FAIL' "$res") FAIL"
