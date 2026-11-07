#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Headless end-to-end tests of the Starbite Link Mac viewer against the fake SBLINK source (TCP, loopback):
#   unit     sbv-test: the viewer session on generated + mutated streams (no sockets)
#   basic    100 frames, MODE 1024x600, synthetic keyboard / pointer / wheel / text, image checksum
#   auto     the same in AUTO mode (USB matching armed, no device attached: falls back to TCP)
#   drop     the source drops the connection after frame 40: the viewer reconnects (1 s) and still passes
#   corrupt  the source sends garbage after frame 40: viewer sends BYE(PROTOCOL), reconnects, passes
#   stall    the source goes silent after frame 40: the viewer declares the link dead (5 s), reconnects
#   busy     a second viewer gets BYE(BUSY) while the first one runs
#   big      1920x1080, 300 frames (throughput)
# Every viewer run checks its own composed image (frame buffer, and the IOSurface the layer shows) against
# the sender's per-frame checksum and exits 0/1; this script also checks the sender's input log.
# Usage: starbite/link/mac/selftest.sh [scenario...]   (run build.sh first). Exit 0 = all passed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
app="$root/build/link/StarbiteLink.app/Contents/MacOS/StarbiteLink"
fs="$root/build/link/mac/fakesender"
work="$root/build/link/mac/selftest"
rm -rf "$work"
mkdir -p "$work"
port=47490
fails=0

expect() {   # expect FILE REGEX WHAT
    if grep -Eq "$2" "$1"; then echo "  ok   $3"; else echo "  FAIL $3 (no /$2/ in $1)"; fails=$((fails + 1)); fi
}

start_sender() {   # start_sender NAME ARGS...
    d="$work/$1"; shift
    mkdir -p "$d"
    port=$((port + 1))
    "$fs" --port $port --crc-file "$d/crc.txt" --log "$d/sender.log" --exit-on-bye "$@" &
    spid=$!
    sleep 0.3
}

stop_sender() {
    sleep 0.3
    kill $spid 2>/dev/null
    wait $spid 2>/dev/null
}

viewer() {   # viewer ARGS...  (in $d, against $port)
    "$app" --selftest --headless --crc-file "$d/crc.txt" --snapshot "$d/snapshot.png" --window-snapshot "$d/window.png" "$@" >"$d/viewer.out" 2>"$d/viewer.err"
    rc=$?
    grep '^SELFTEST' "$d/viewer.out" | sed 's/^/  /'
    if [ $rc -eq 0 ]; then echo "  ok   viewer exit 0 (image matches)"; else echo "  FAIL viewer exit $rc ($d/viewer.err)"; fails=$((fails + 1)); fi
}

input_checks() {
    l="$d/sender.log"
    expect "$l" 'MODE REQUEST 1024x600 -> APPLIED 1024x600' "MODE request applied"
    expect "$l" 'KEY page=0x07 usage=0xe1 down mods=0x[23]02' "left Shift down (HID 0xE1, modifier bit set)"
    expect "$l" 'KEY page=0x07 usage=0x0b down mods=0x[23]02' "H down with Shift"
    expect "$l" 'KEY page=0x07 usage=0x0c up mods=0x[23]02' "I up"
    expect "$l" 'KEY page=0x07 usage=0xe1 up mods=0x[23]00' "left Shift up"
    expect "$l" 'KEY page=0x07 usage=0xe3 down mods=0x[23]08' "Cmd = left GUI (the Windows key)"
    expect "$l" 'PTR x=100 y=200 buttons=0x1 wheel=0,0' "left button down at display pixel (100,200)"
    expect "$l" 'PTR x=100 y=200 buttons=0x0 wheel=0,0' "left button up"
    expect "$l" 'PTR x=300 y=150 buttons=0x2 wheel=0,0' "right button down at (300,150)"
    expect "$l" 'wheel=0,-120' "one wheel notch towards the user = -120"
    expect "$l" 'TEXT "Grüße ✓"' "INPUT_TEXT (UTF-8)"
    expect "$l" 'BYE received reason=0' "viewer BYE(NORMAL) at the end"
}

[ $# -eq 0 ] && set -- unit basic auto drop corrupt stall busy big
for s in "$@"; do
    echo "== $s"
    case $s in
    unit)
        "$root/build/link/mac/sbv-test" 300 >"$work/unit.out" 2>&1; rc=$?
        sed 's/^/  /' "$work/unit.out"
        if [ $rc -eq 0 ]; then echo "  ok   sbv-test"; else echo "  FAIL sbv-test exit $rc"; fails=$((fails + 1)); fi ;;
    basic)
        start_sender basic
        viewer --tcp 127.0.0.1:$port
        stop_sender; input_checks ;;
    auto)
        start_sender auto
        viewer --auto --host 127.0.0.1:$port
        stop_sender; input_checks ;;
    drop)
        start_sender drop --fault drop:40
        viewer --tcp 127.0.0.1:$port
        stop_sender; input_checks
        expect "$d/sender.log" 'FAULT drop after frame 40' "connection dropped by the source"
        expect "$d/viewer.out" '2 session\(s\)' "viewer reconnected (2 sessions)" ;;
    corrupt)
        start_sender corrupt --fault corrupt:40
        viewer --tcp 127.0.0.1:$port
        stop_sender; input_checks
        expect "$d/sender.log" 'BYE received reason=1 text="stream"' "viewer answered garbage with BYE(PROTOCOL)"
        expect "$d/viewer.err" 'bad magic|magic' "viewer saw the bad magic"
        expect "$d/viewer.out" '2 session\(s\)' "viewer reconnected (2 sessions)" ;;
    stall)
        start_sender stall --fault stall:40
        viewer --tcp 127.0.0.1:$port
        stop_sender; input_checks
        expect "$d/viewer.err" 'link dead' "viewer declared the silent link dead"
        expect "$d/viewer.out" '2 session\(s\)' "viewer reconnected (2 sessions)" ;;
    busy)
        start_sender busy
        "$app" --selftest --headless --crc-file "$d/crc.txt" --frames 360 --tcp 127.0.0.1:$port >"$d/viewer.out" 2>"$d/viewer.err" &
        vpid=$!
        sleep 2
        python3 -I - "$port" >"$d/second.out" <<'PY'
import socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3)
b = b""
while len(b) < 20:
    c = s.recv(64)
    if not c: break
    b += c
magic, ver, typ, flags, ln, seq = struct.unpack("<IBBHII", b[:16])
reason = struct.unpack("<I", b[16:20])[0] if len(b) >= 20 else -1
print("second viewer got type=0x%02x len=%d reason=%d closed=%s" % (typ, ln, reason, s.recv(1) == b""))
PY
        wait $vpid; rc=$?
        stop_sender
        sed 's/^/  /' "$d/second.out"
        expect "$d/second.out" 'type=0x3f len=4 reason=4 closed=True' "second viewer: BYE(BUSY) and closed"
        if [ $rc -eq 0 ]; then echo "  ok   first viewer unaffected (exit 0)"; else echo "  FAIL first viewer exit $rc"; fails=$((fails + 1)); fi ;;
    big)
        start_sender big --size 1920x1080
        viewer --tcp 127.0.0.1:$port --frames 300 --mode-test 1920x1080
        stop_sender ;;
    *)
        echo "unknown scenario $s"; fails=$((fails + 1)) ;;
    esac
done
echo
if [ $fails -eq 0 ]; then echo "selftest: ALL PASSED ($work)"; else echo "selftest: $fails FAILED ($work)"; fi
[ $fails -eq 0 ]
