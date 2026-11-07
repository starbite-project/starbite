#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Starbite Link: the whole end-to-end check on the Mac, no phone involved.
#   1. builds every part: proto (runs its tests), the Mac app + fake sender, sblinkd (host tests run, and the
#      Windows ARM64 sblinkd.exe), the Starbite Display IddCx driver package, and these e2e tools
#   2. the Mac app's own selftest against the fake sender over TCP (mac/selftest.sh: frames, image checksum,
#      input, drop/corrupt/stall reconnects, BUSY, 1080p)
#   3. restart: the fake sender PROCESS is killed (SIGKILL) mid-stream and started again; the app reconnects
#   4. the Mac app against the REAL sender code (sblinkd-host): HELLO, MODE, injected input arrive
#   5. encbench: CPU per frame of the sender's encoder on a static desktop with a dragged window
#   6. live: sblinkd (drag scene) -> TCP -> viewbench (the app's session code): frames/s, Mbit/s, sender
#      CPU, input during frames, exact final image, and zero traffic once the desktop is static
# Usage: starbite/link/e2e/check.sh [--no-build]     Output: build/link/e2e/check/  Exit 0 = all passed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
link="$root/starbite/link"
B="$root/build/link"
work="$B/e2e/check"
rm -rf "$work"
mkdir -p "$work"
fails=0
ok()   { echo "  ok   $*"; }
bad()  { echo "  FAIL $*"; fails=$((fails + 1)); }
step() { echo; echo "== $*"; }
cputime() {   # cputime PID -> seconds of user+sys CPU so far
    ps -o time= -p "$1" 2>/dev/null | awk -F: '{ if (NF == 3) print $1 * 3600 + $2 * 60 + $3; else print $1 * 60 + $2 }'
}

if [ "${1:-}" != "--no-build" ]; then
    step "build"
    for b in "proto/build.sh" "mac/build.sh" "win/sender/build.sh" "win/idd/build.sh" "e2e/build.sh"; do
        log="$work/build-$(echo "$b" | tr / -).log"
        if sh "$link/$b" >"$log" 2>&1; then ok "$b"; else bad "$b ($log)"; fi
    done
    grep -E '^PASSED|FAILED' "$work/build-proto-build.sh.sh.log" 2>/dev/null | tail -1 | sed 's/^/       proto: /'
    grep -E 'checks, [0-9]+ failed' "$work/build-win-sender-build.sh.sh.log" 2>/dev/null | tail -1 | sed 's/^/       sender host tests: /'
    grep -E '^PE:' "$work/build-win-sender-build.sh.sh.log" 2>/dev/null | cut -c1-60 | sed 's/^/       sblinkd.exe: /'
    grep -E 'catalog, ok|ok \(0 failures\)' "$work/build-win-idd-build.sh.sh.log" 2>/dev/null | head -2 | sed 's/^/       idd: /'
fi

step "Mac app selftest against the fake sender (TCP)"
if sh "$link/mac/selftest.sh" >"$work/selftest.log" 2>&1; then ok "mac/selftest.sh: $(tail -1 "$work/selftest.log")"
else bad "mac/selftest.sh ($work/selftest.log)"; grep FAIL "$work/selftest.log" | head; fi
grep -E 'SELFTEST PASS' "$work/selftest.log" | sed 's/^ */       /'

APP="$B/StarbiteLink.app/Contents/MacOS/StarbiteLink"
step "reconnect after the sender process is killed and restarted"
d="$work/restart"; mkdir -p "$d"; P=47581
"$B/mac/fakesender" --port $P --crc-file "$d/crc.txt" --log "$d/sender1.log" & S1=$!
sleep 0.3
"$APP" --selftest --headless --crc-file "$d/crc.txt" --tcp 127.0.0.1:$P --frames 400 --timeout 40 >"$d/viewer.out" 2>"$d/viewer.err" & V=$!
sleep 3
kill -9 $S1; wait $S1 2>/dev/null
sleep 2
"$B/mac/fakesender" --port $P --crc-file "$d/crc.txt" --log "$d/sender2.log" --exit-on-bye & S2=$!
wait $V; rc=$?
sleep 0.3; kill $S2 2>/dev/null; wait $S2 2>/dev/null
grep '^SELFTEST PASS' "$d/viewer.out" | sed 's/^/       /'
[ $rc -eq 0 ] && ok "viewer exit 0, image matches the restarted sender" || bad "viewer exit $rc ($d)"
grep -q '2 session(s)' "$d/viewer.out" && ok "2 sessions (reconnected to the new process)" || bad "no reconnect ($d)"
grep -q 'KEY page=0x07 usage=0x0b down' "$d/sender1.log" && ok "input reached sender 1" || bad "no input at sender 1"
grep -q 'MODE REQUEST 1024x600 -> APPLIED' "$d/sender2.log" && ok "MODE asked again from sender 2" || bad "no MODE at sender 2"

step "Mac app against the real sender code (sblinkd-host, synthetic desktop)"
d="$work/app-sblinkd"; mkdir -p "$d"; P=47582
: >"$d/crc.txt"   # sblinkd-host keeps no checksums: the selftest cannot pass, it is run for the interop
"$B/sender/sblinkd-host" --tcp $P --synth-auto 30 --print-input --log "$d/sender.log" >/dev/null 2>&1 & S=$!
sleep 0.3
"$APP" --selftest --headless --crc-file "$d/crc.txt" --tcp 127.0.0.1:$P --frames 60 --timeout 6 >"$d/viewer.out" 2>"$d/viewer.err"
kill $S; wait $S 2>/dev/null
grep -q 'HELLO from "Starbite S9"' "$d/viewer.err" && ok "app got HELLO from sblinkd (EXTEND: $(grep -c 'extended display' "$d/viewer.err") times)" || bad "no HELLO ($d)"
grep -q 'MODE APPLIED 1024x600' "$d/viewer.err" && ok "MODE 1024x600 applied by sblinkd" || bad "no MODE APPLIED ($d)"
grep -q 'inject: kind 1 code 0x23' "$d/sender.log" && ok "keys injected by sblinkd (H = scan 0x23)" || bad "no key injected ($d/sender.log)"
grep -q 'inject: kind 3' "$d/sender.log" && ok "text injected (KEYEVENTF_UNICODE)" || echo "  note no text inject line"
n=$(grep -c 'link dead' "$d/sender.log")
[ "$n" -eq 0 ] && ok "no 'link dead' while input arrived" || echo "  WARN sblinkd declared the link dead $((n / 2)) time(s) right after input (session.c race; see README)"
grep -E 'session end' "$d/sender.log" | sed 's/^.*session end/       session end/' | head -3

step "encoder CPU per frame (sender pipeline, this Mac)"
"$B/e2e/encbench" >"$work/encbench.txt" 2>&1 && ok "encbench: every frame decoded back exactly" || bad "encbench ($work/encbench.txt)"
sed 's/^/       /' "$work/encbench.txt"
taskpolicy -c background "$B/e2e/encbench" >"$work/encbench-ecore.txt" 2>&1
echo "       efficiency cores (taskpolicy -c background):"
grep -E '^drag ' "$work/encbench-ecore.txt" | sed 's/^/       /'

step "live: sblinkd (drag scene) -> TCP -> viewbench"
for size in 1280x720 1920x1080; do
    for snd in sblinkd-drag-fix sblinkd-drag; do
        [ -x "$B/e2e/$snd" ] || continue
        d="$work/live-$size-$snd"; mkdir -p "$d"; P=$((P + 1))
        ihz=10; [ $snd = sblinkd-drag ] && ihz=0   # the unfixed sender drops the link on input (race)
        SBLINK_SCENE_STOP=13 SBLINK_SCENE_HASH="$d/hash.txt" "$B/e2e/$snd" --tcp $P --synth-auto 60 --synth-size $size \
            --print-input --log "$d/sender.log" >/dev/null 2>&1 & S=$!
        sleep 0.5
        c0=$(cputime $S)
        "$B/e2e/viewbench" --tcp 127.0.0.1:$P --seconds 10 --input-hz $ihz --hash-file "$d/hash.txt" >"$d/view.out" 2>"$d/view.err"; rc=$?
        c1=$(cputime $S)
        kill $S; wait $S 2>/dev/null
        frames=$(sed -n 's/.*session end ([^)]*): \([0-9]*\) frames.*/\1/p' "$d/sender.log" | tail -1)
        echo "  -- $size, $snd (input $ihz/s)"
        sed 's/^/       /' "$d/view.out"
        grep -E 'stats:' "$d/sender.log" | head -1 | sed 's/^.*stats:/       sender stats:/'
        awk -v a="$c0" -v b="$c1" -v f="${frames:-0}" 'BEGIN { printf "       sender process CPU: %.2f s for %d frames = %.2f ms/frame (includes drawing the scene)\n", b - a, f, f ? (b - a) * 1000 / f : 0 }'
        [ $rc -eq 0 ] && ok "$size $snd: frames, exact final image" || bad "$size $snd: viewbench exit $rc ($d)"
        if [ $ihz -gt 0 ]; then
            k=$(grep -c 'inject: kind 1 code 0x1e' "$d/sender.log")
            [ "$k" -gt 0 ] && ok "input during frames: $k key events injected, no drop" || bad "no input arrived ($d)"
        fi
        grep -q 'static desktop for 3 s: 0 frames, 0 bytes of frames' "$d/view.out" && ok "static desktop: 0 frames sent" || bad "frames while static ($d)"
    done
done

echo
if [ $fails -eq 0 ]; then echo "e2e check: ALL PASSED ($work)"; else echo "e2e check: $fails FAILED ($work)"; fi
[ $fails -eq 0 ]
