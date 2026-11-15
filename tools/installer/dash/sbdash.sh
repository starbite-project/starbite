#!/bin/sh
# sbdash.sh DIR TTY [SYSROOT] [PROCROOT] [DIM_AFTER_S] [DIM_RAMP_S]: the night install's live dashboard on the installer environment's screen
# (fbcon on tty0), redrawn every second. Started and fed by tools/installer/dash/sbdash-mac.sh from the Mac.
# READ ONLY on the phone: it reads /sys, /proc, dmesg and DIR/feed (the Mac's view of the night log); it writes only
# DIR (its own tmpfs folder), TTY, the backlight's brightness (the slow dim) and the console font. No block device is
# opened; nothing of the installer's (/tmp/sbsc.*, /tmp/sbsu.*, /tmp/sbinst-*) is touched.
#   DIR/feed       from the Mac: key=value lines (step, sub, done, total, rate, eta, ...) + "log:" / "sub:" lines
#   DIR/peek       usbpeek's samples of raw usb0 frames (DIR/usbpeek, started here if present)
#   DIR/stop       touch it to end: the backlight, the font and sb-status's screen are put back
# Screen: the 16x32 font (90 columns on the 1440 px panel), ANSI colours only (bars and sparklines are coloured
# cells, so no font glyph beyond ASCII is needed). After 30 min with no new step and no touch, the backlight ramps
# down over 10 min to 6 % of what it was; a new step or a touch brings it back at once. A kernel without a backlight
# class (S9 #2's environment, 2026-11-15) gets a saver instead: a black screen with one dim status line that moves
# every minute (no static image on the OLED panel), until a touch or a new step.
D=${1:-/tmp/sbdash}
T=${2:-/dev/tty0}
SYS=${3:-/sys/}
PROC=${4:-/proc/}
DIM_AFTER=${5:-1800}            # idle seconds before the backlight starts to drop
DIM_RAMP=${6:-600}              # seconds the drop takes (to 6 % of the original)
FONTS=/usr/share/sbmaint
mkdir -p "$D" || exit 1
echo $$ > "$D/pid"
rm -f "$D/stop"
H="$D/hist"; : > "$H"
now() { date +%s; }
# ---- the backlight (the only sysfs write besides the screen)
BL=$(ls -d ${SYS}class/backlight/* 2>/dev/null | head -1)
BL_ORIG=$(cat "$BL/brightness" 2>/dev/null)
BL_SET=$BL_ORIG
bl() { [ -n "$BL" ] && [ "$1" != "$BL_SET" ] && echo "$1" > "$BL/brightness" 2>/dev/null && BL_SET=$1; }
# ---- sb-status (the environment's own status screen) pauses while this one owns tty0
SBS=$(ps -o pid,args 2>/dev/null | awk '/sb-status --loop/ && !/awk/ { print $1 }')
[ -n "$SBS" ] && kill -STOP $SBS 2>/dev/null
if [ -c "$T" ]; then
	command -v setfont >/dev/null && setfont -C "$T" "$FONTS/font-16x32.psf" 2>/dev/null
	printf '\033[?25l\033[2J' > "$T"
fi
cleanup() {
	bl "$BL_ORIG"
	[ -n "$TOUCHPID" ] && kill "$TOUCHPID" 2>/dev/null
	[ -n "$PEEKPID" ] && kill "$PEEKPID" 2>/dev/null
	if [ -c "$T" ]; then
		command -v setfont >/dev/null && setfont -C "$T" "$FONTS/font-32x64.psf" 2>/dev/null
		printf '\033[0m\033[2J\033[H' > "$T"
	fi
	[ -n "$SBS" ] && kill -CONT $SBS 2>/dev/null
	rm -f "$D/pid"
	exit 0
}
trap cleanup INT TERM HUP
# ---- a touch wakes the screen: the touchscreen's input events are read (and dropped) in the background
TOUCHPID=
EV=$(awk 'BEGIN{RS=""} /[Tt]ouch|s6sy|sec_ts|fts|ABS=[0-9a-f]*6.....+/ { if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit } }' \
	${PROC}bus/input/devices 2>/dev/null)
if [ -n "$EV" ] && [ -c "/dev/input/$EV" ]; then
	( while :; do dd if="/dev/input/$EV" of=/dev/null bs=64 count=1 2>/dev/null && now > "$D/touch"; sleep 1; done ) &
	TOUCHPID=$!
fi
PEEKPID=
if [ -x "$D/usbpeek" ]; then nice -n 19 "$D/usbpeek" usb0 "$D/peek" 3 >/dev/null 2>&1 & PEEKPID=$!; fi
LAST_ACT=$(now) LAST_STEP=
while [ ! -e "$D/stop" ]; do
	t=$(now)
	# ---- what the phone says (read only)
	N=${SYS}class/net/usb0/statistics
	usb=$(cat $N/rx_bytes $N/tx_bytes $N/rx_packets $N/tx_packets $N/rx_errors $N/tx_errors $N/rx_dropped $N/tx_dropped 2>/dev/null | tr '\n' ' ')
	udc=$(cat ${SYS}class/udc/*/state 2>/dev/null | head -1)
	spd=$(cat ${SYS}class/udc/*/current_speed 2>/dev/null | head -1)
	mtu=$(cat ${SYS}class/net/usb0/mtu 2>/dev/null)
	carrier=$(cat ${SYS}class/net/usb0/carrier 2>/dev/null)
	disk=$(awk '$3 ~ /^(sda[0-9]*|mmcblk0(p[0-9]+)?)$/ { printf "%s:%s:%s:%s:%s:%s ", $3, $4, $6, $8, $10, $12 }' ${PROC}diskstats 2>/dev/null)
	bat=; for b in ${SYS}class/power_supply/*; do
		[ "$(cat $b/type 2>/dev/null)" = Battery ] || continue
		bat="$(cat $b/capacity 2>/dev/null) $(cat $b/voltage_now 2>/dev/null) $(cat $b/current_now 2>/dev/null) $(cat $b/temp 2>/dev/null) $(cat $b/status 2>/dev/null)"
		break
	done
	therm=$(cat ${SYS}class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1)
	load=$(cut -d' ' -f1-3 ${PROC}loadavg 2>/dev/null)
	mem=$(awk '/^MemAvailable/ { print int($2 / 1024) }' ${PROC}meminfo 2>/dev/null)
	up=$(cut -d. -f1 ${PROC}uptime 2>/dev/null)
	dmesg 2>/dev/null | tail -n 7 > "$D/dmesg.tmp" && mv "$D/dmesg.tmp" "$D/dmesg"
	echo "$t $usb $disk" >> "$H"
	tail -n 120 "$H" > "$H.tmp" && mv "$H.tmp" "$H"
	# ---- the dim: idle = no new step and no touch for 30 min
	step=$(sed -n 's/^step=//p' "$D/feed" 2>/dev/null)
	[ "$step" != "$LAST_STEP" ] && { LAST_STEP=$step; LAST_ACT=$t; }
	tt=$(cat "$D/touch" 2>/dev/null); [ -n "$tt" ] && [ "$tt" -gt "$LAST_ACT" ] && LAST_ACT=$tt
	idle=$((t - LAST_ACT))
	if [ -n "$BL_ORIG" ]; then
		if [ $idle -lt $DIM_AFTER ]; then bl "$BL_ORIG"
		else
			f=$(( (idle - DIM_AFTER) * 94 / DIM_RAMP )); [ $f -gt 94 ] && f=94
			bl $(( BL_ORIG * (100 - f) / 100 ))
		fi
	fi
	cols=$(stty size < "$T" 2>/dev/null | awk '{ print $2 }'); rows=$(stty size < "$T" 2>/dev/null | awk '{ print $1 }')
	if [ -z "$BL" ] && [ $idle -ge $DIM_AFTER ]; then          # no backlight to dim: the saver
		r=$(( (t / 60) % ${rows:-90} + 1 )); c=$(( (t / 60) * 7 % 30 + 1 ))
		msg=$(awk -F= '/^(step|sub|at)=/ { v[$1] = $2 } /^done=/ { d = $2 } /^total=/ { tt = $2 }
			END { printf "STARBITE %s  %s  %s%s  tap to wake", v["at"], v["step"], v["sub"], (tt + 0 > 0 ? sprintf("  %.0f%%", d * 100 / tt) : "") }' "$D/feed" 2>/dev/null)
		printf '\033[0m\033[2J\033[%d;%dH\033[2;37m%s\033[0m' "$r" "$c" "$(echo "$msg" | cut -c1-$(( ${cols:-90} - c )))" > "$T" 2>/dev/null
		cp "$T" "$D/frame.tmp" 2>/dev/null
		sleep 1
		continue
	fi
	# ---- one frame
	awk -v W="${cols:-90}" -v HROWS="${rows:-90}" -v NOW="$t" -v USB="$usb" -v UDC="$udc" -v SPD="$spd" -v MTU="$mtu" \
	    -v CAR="$carrier" -v BAT="$bat" -v THERM="$therm" -v LOAD="$load" -v MEM="$mem" -v UP="$up" -v IDLE="$idle" -v DIMA="$DIM_AFTER" \
	    -v BLS="$BL_SET" -v BLO="$BL_ORIG" -v FEED="$D/feed" -v HIST="$H" -v DMESG="$D/dmesg" -v PEEK="$D/peek" '
	function c(code, s) { return "\033[" code "m" s "\033[0m" }
	function pad(s, n) { s = substr(s, 1, n); while (length(s) < n) s = s " "; return s }
	function hb(b) { b = b + 0; if (b >= 1073741824) return sprintf("%.2f GiB", b / 1073741824); if (b >= 1048576) return sprintf("%.1f MiB", b / 1048576); if (b >= 1024) return sprintf("%.1f KiB", b / 1024); return b " B" }
	function rate(b) { return hb(b) "/s" }
	function line(s) { if (nl < HROWS - 1) { out = out s "\033[K\n"; nl++ } }
	function rule(t) { line(c("1;36", "== " t " " substr("==========================================================================================", 1, W - length(t) - 5))) }
	function bar(frac, n, col,   k, s) { frac = frac + 0; if (frac < 0) frac = 0; if (frac > 1) frac = 1; k = int(frac * n + 0.5); s = sprintf("%" k "s", ""); return c(col, s) c("100", sprintf("%" (n - k) "s", "")) }
	# a sparkline block: rows x n colored cells from the series v[1..m]
	function spark(v, m, rowsn, col, label,   i, j, mx, s, h, start) {
		mx = 1; for (i = 1; i <= m; i++) if (v[i] > mx) mx = v[i]
		start = m - (W - 14); if (start < 1) start = 1
		for (j = rowsn; j >= 1; j--) {
			s = (j == rowsn ? pad(label, 12) : (j == 1 ? pad(rate(mx) " max", 12) : pad("", 12))) "  "
			for (i = start; i <= m; i++) { h = v[i] / mx * rowsn; s = s (h >= j - 0.5 && v[i] > 0 ? c(col, " ") : " ") }
			line(s)
		}
	}
	BEGIN {
		nl = 0; out = "\033[H"
		while ((getline l < FEED) > 0) { if (l ~ /^log:/) logs[++nlog] = substr(l, 5); else if (l ~ /^sub:/) subs[++nsub] = substr(l, 5); else { k = index(l, "="); if (k) f[substr(l, 1, k - 1)] = substr(l, k + 1) } }
		m = 0; while ((getline l < HIST) > 0) { m++; na = split(l, a, " "); ht[m] = a[1]; rx[m] = a[2]; tx[m] = a[3]
			wr[m] = 0; rd[m] = 0; for (i = 10; i <= na; i++) { split(a[i], d, ":"); if (d[1] == "sda" || d[1] == "mmcblk0") { rd[m] += d[3]; wr[m] += d[5] } } }
		for (i = 2; i <= m; i++) { dt = ht[i] - ht[i - 1]; if (dt < 1) dt = 1
			srx[i - 1] = (rx[i] - rx[i - 1]) / dt; stx[i - 1] = (tx[i] - tx[i - 1]) / dt; swr[i - 1] = (wr[i] - wr[i - 1]) * 512 / dt; srd[i - 1] = (rd[i] - rd[i - 1]) * 512 / dt
			if (srx[i - 1] < 0) srx[i - 1] = 0; if (stx[i - 1] < 0) stx[i - 1] = 0; if (swr[i - 1] < 0) swr[i - 1] = 0; if (srd[i - 1] < 0) srd[i - 1] = 0 }
		ns = m - 1
		clock = f["at"] != "" ? f["at"] : "--:--:--"          # the Mac clock (the phone clock is not set)
		line(c("1;97;44", pad(" STARBITE INSTALLER  -  S9 #2  -  the night of 15 November", W - 10) pad(clock, 10)))
		line(c("2", pad("phone " (f["phone"] != "" ? f["phone"] : "?") "   env up " int(UP / 60) " min   load " LOAD "   free RAM " MEM " MiB   feed " (f["at"] != "" ? f["at"] : "none yet"), W)))
		rule("STEPS")
		split("P C D E F G H", sk, " "); split("store|GPT+dat|One UI|Linux|Sequoia|Windows|iOS 27", sn, "|")
		s = ""; for (i = 1; i <= 7; i++) { st = f["st_" sk[i]]; col = st == "done" ? "1;30;42" : st == "failed" ? "1;97;41" : st == "started" ? "1;30;43" : "37;100"
			s = s c(col, " " sk[i] " " pad(sn[i], 7) " ") " " }
		line(s)
		line(c("1", "now: ") c("1;93", f["step"] != "" ? f["step"] : "waiting for the night log") "   " c("1", "substep: ") c("1;96", f["sub"]) "   " f["phase"])
		if (f["total"] + 0 > 0) {
			fr = (f["done"] + 0) / (f["total"] + 0)
			line(bar(fr, W - 9, "42") sprintf(" %5.1f%%", fr * 100))
			line(pad(hb(f["done"]) " of " hb(f["total"]) "   " (f["rate"] + 0 > 0 ? rate(f["rate"]) : "") "   ETA " (f["eta"] != "" ? f["eta"] : "?") "   elapsed " f["elapsed"], W))
		} else line(c("2", "(no byte counts for this substep: dry runs, store updates and the GPT print none)"))
		if (f["night_elapsed"] != "") line(c("2", "night: started " f["night_start"] ", running " f["night_elapsed"] "   steps done " f["n_done"] "/7"))
		rule("USB  (CDC-ECM on the dwc3 device controller)")
		split(USB, u, " ")
		line(sprintf("UDC %s  speed %s  carrier %s  MTU %s", c(UDC == "configured" ? "1;32" : "1;31", UDC != "" ? UDC : "?"), SPD != "" ? SPD : "?", CAR, MTU))
		line(sprintf("rx %s in %d pkts (err %d drop %d)   tx %s in %d pkts (err %d drop %d)", hb(u[1]), u[3], u[5], u[7], hb(u[2]), u[4], u[6], u[8]))
		if (ns > 0) line(sprintf("now: rx %s   tx %s", rate(srx[ns]), rate(stx[ns])))
		spark(srx, ns, 5, "46", "Mac -> phone")
		spark(stx, ns, 3, "45", "phone -> Mac")
		rule("RAW usb0 FRAMES  (sampled every 3 s, read only)")
		np = 0; while ((getline l < PEEK) > 0 && np < 3) { np++; line(c("32", substr(l, 1, W))) }
		if (!np) line(c("2", "(no sample yet)"))
		rule("STORAGE I/O  (/proc/diskstats)")
		spark(swr, ns, 4, "41", "writes")
		spark(srd, ns, 2, "44", "reads")
		# per-device rates from the last two samples
		if (m >= 2) {
			cmd = "tail -n 2 " HIST; i = 0; while ((cmd | getline l) > 0) { i++; L[i] = l }; close(cmd)
			n0 = split(L[1], a0, " "); n1 = split(L[2], a1, " "); dt = a1[1] - a0[1]; if (dt < 1) dt = 1
			for (i = 10; i <= n0; i++) { split(a0[i], d, ":"); r0[d[1]] = d[3]; w0[d[1]] = d[5] }
			s = ""; cnt = 0
			for (i = 10; i <= n1; i++) { split(a1[i], d, ":"); if (d[1] == "sda" || d[1] == "mmcblk0") continue
				wr1 = (d[5] - w0[d[1]]) * 512 / dt; rd1 = (d[3] - r0[d[1]]) * 512 / dt
				if (wr1 > 0 || rd1 > 0 || d[6] > 0) { s = s sprintf("%s r %s w %s q%d   ", d[1], rate(rd1), rate(wr1), d[6]); cnt++ } }
			line(cnt ? substr(s, 1, W) : c("2", "(all partitions idle)"))
		}
		rule("PHONE")
		split(BAT, bt, " ")
		line(sprintf("battery %s%% %s  %.2f V  %+d mA  %.1f C   SoC temp %.1f C   backlight %s/%s%s", bt[1] != "" ? bt[1] : "?", bt[5],
		     bt[2] / 1e6, bt[3] / 1000, bt[4] / 10, THERM / 1000, BLS, BLO, IDLE >= DIMA ? c("33", "  (dimmed: tap to wake)") : ""))
		rule("NIGHT LOG  (build/installer/logs/night-1115.log on the Mac)")
		for (i = (nlog > 14 ? nlog - 13 : 1); i <= nlog; i++) { l = logs[i]; col = l ~ /failed|STOPPED|REFUSED/ ? "1;31" : l ~ /^### .*done/ ? "1;32" : l ~ /^###/ ? "1;33" : l ~ /^      ok/ ? "32" : "37"; line(c(col, substr(l, 1, W))) }
		rule("SUBSTEP OUTPUT  (" f["sub"] ")")
		for (i = (nsub > 8 ? nsub - 7 : 1); i <= nsub; i++) line(substr(subs[i], 1, W))
		rule("KERNEL  (dmesg)")
		while ((getline l < DMESG) > 0) line(c("2", substr(l, 1, W)))
		while (nl < HROWS - 1) line("")
		line(c("2", pad("read-only dashboard: nothing here writes to the phone storage.  stop: the Mac, sbdash-mac.sh --stop", W)))
		printf "%s", out
	}' > "$D/frame.tmp" 2>/dev/null && cat "$D/frame.tmp" > "$T" 2>/dev/null
	sleep 1
done
cleanup
