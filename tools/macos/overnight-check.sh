#!/bin/bash
# overnight-check.sh STEP KERNEL_LOG [SECONDS]: a pass/stop verdict for an unattended macOS run, for a chain script.
#   STEP: s6n | s6p | s6w | ab (S6s, S6t, S6l, S6m) | soak (S6r);  SECONDS = the payload's sweetreboot (default from the log)
# Prints one line "CHECK <step>: PASS|SOFT|UNSAFE ..." plus the numbers it looked at. Exit code:
#   0 PASS    go on
#   1 SOFT    the run did not show what it should; the root is fine: go on, but skip runs that build on this one
#   2 UNSAFE  the desktop did not come up, WindowServer crashed or the kernel panicked: stop the chain
#             (after S6p or later: run tools/macos/s6p-undo.sh, then leave xnu-macos15-s5zday.xnu on the row)
# It reads only the log; it never touches the phone.
set -u
step=${1:?step}; L=${2:?kernel log}; want=${3:-}
[ -s "$L" ] || { echo "CHECK $step: UNSAFE no log at $L"; exit 2; }
T=$(mktemp); trap 'rm -f "$T"' EXIT
LC_ALL=C tr -d '\000' < "$L" > "$T"
n() { LC_ALL=C grep -c -E "$1" "$T"; }
armed=$(LC_ALL=C sed -n -E 's/.*sweetreboot timer ([0-9]+) s armed.*/\1/p' "$T" | head -1)
want=${want:-${armed:-480}}
tops=$(n 'STARLTE TOP \(')
reached=$(n 'sweetreboot=[0-9]+ reached')
panic=$(n 'panic\(cpu|Kernel panic|SWEET PANIC')
ws=$(n 'CRASH WindowServer\[')
wsup=$(n 'SBMTL\[[0-9]+ WindowServer\]')
gpufail=$(n 'chain: FAILED')
sderr=$(n 'STARBITE SD: .*(error|timeout|CRC)')
nc=$(n 'CRASH NotificationCent\[')
apps=$(LC_ALL=C grep -o -E 'CRASH [A-Za-z_ ]+\[' "$T" | sort | uniq -c | sort -rn | head -8 | awk '{printf "%s x%s, ", $3, $1}' | sed 's/\[//g')
info="tops $tops (want >= $((want / 10 - 12))), reboot timer reached $reached, panics $panic, WindowServer crashes $ws, GPU failed jobs $gpufail, SD errors $sderr, NotificationCenter crashes $nc; crashes: ${apps:-none}"
verdict=PASS; why=
unsafe() { verdict=UNSAFE; why="$why $1;"; }
soft() { [ "$verdict" = UNSAFE ] || verdict=SOFT; why="$why $1;"; }
[ "$panic" -eq 0 ] || unsafe "kernel panic"
[ "$wsup" -gt 0 ] || unsafe "WindowServer never got its Metal device (no desktop)"
[ "$ws" -eq 0 ] || unsafe "WindowServer crashed"
[ "$tops" -ge $((want / 10 - 12)) ] || unsafe "the run ended early"
[ "$reached" -ge 1 ] || soft "no sweetreboot line at the end (how did it get back to TWRP?)"
[ "$gpufail" -le 1 ] || soft "$gpufail failed GPU jobs"
case $step in
s6n)
	pad=$(n 'SWEET HID: pad test [0-9]+')
	mon=$(n 'CRASH (Finder|System Settings|Dock|TextEdit|Calculator)\[')
	wake=$(n 'a touch while the display sleeps')
	info="$info | pad test lines $pad, app crashes after scrolls $mon, wake-on-touch lines $wake"
	[ "$pad" -ge 12 ] || soft "the scripted gestures did not run ($pad lines)"
	[ "$mon" -eq 0 ] || soft "an app crashed during the scripted gestures"
	;;
s6p)
	iss=$(n 'SWEET Sandbox: extension [0-9]+ issued')
	done_=$(n 'SBDIAG done')
	books=$(n 'SBDIAG app Books: running')
	running=$(n 'SBDIAG app [A-Za-z]+: running')
	ca=$(n 'CRASH coreaudiod\[')
	capid=$(n 'SBDIAG coreaudiod: [0-9]+')
	ica=$(n 'CRASH iconservicesagen')
	info="$info | extensions issued lines $iss, SBDIAG done $done_, apps running $running of 7 (Books $books), coreaudiod up $capid / crashes $ca, iconservicesagent crashes $ica"
	[ "$iss" -ge 1 ] || soft "no sandbox extension was issued (is sweetsbext=1 in the payload?)"
	# (S6p 11-16: "log show" runs past the reboot timer, so SBDIAG done is reported, not required)
	apps7=$(n 'SBDIAG app [A-Za-z]+: (running|NOT running)')
	[ "$apps7" -ge 7 ] || soft "the diagnostics did not reach the app checks"
	[ "$nc" -le 3 ] || soft "NotificationCenter still crash-loops ($nc)"
	[ "$ica" -le 2 ] || soft "iconservicesagent crash-loops with the Metal device ($ica)"
	;;
s6w)
	pad=$(n 'SWEET HID: pad test [0-9]+')
	mon=$(n 'CRASH (Finder|System Settings|Dock|TextEdit|Calculator)\[')
	lp=$(n 'pad long press = right click')
	kb=$(n 'four-finger tap = keyboard (shown|hidden)')
	wake=$(n 'a touch while the display sleeps')
	info="$info | pad test lines $pad, long presses $lp, keyboard toggles $kb, app crashes $mon, wake-on-touch lines $wake"
	[ "$pad" -ge 24 ] || soft "the scripted gestures did not all run ($pad lines)"
	[ "$lp" -ge 2 ] || soft "the long press did not fire"
	[ "$kb" -ge 4 ] || soft "the four-finger tap did not toggle the keyboard"
	[ "$mon" -eq 0 ] || soft "an app crashed during the scripted gestures"
	;;
ab|soak)
	sub=$(LC_ALL=C sed -n -E 's/.*STARLTE GPU CALLS[^:]*: submit ([0-9]+) x.*/\1/p' "$T" | awk '{s += $1} END {print s + 0}')
	info="$info | GPU submits $sub"
	[ "$sub" -ge 1500 ] || soft "little GPU work to compare ($sub submits)"
	;;
*) echo "CHECK $step: unknown step"; exit 1 ;;
esac
echo "CHECK $step: $verdict$why $info"
case $verdict in PASS) exit 0 ;; SOFT) exit 1 ;; *) exit 2 ;; esac
