#!/bin/bash
# sbdash-mac.sh S2_SERIAL [--stop] [--once N] [--fake-root DIR]: the Mac side of the night dashboard (sbdash.sh).
# Every 2 s: builds the feed from build/installer/logs/night-1115.log and the current substep's log, and pushes it to
# S9 #2's installer environment (/tmp/sbdash/feed, its own tmpfs folder) over its OWN ssh connection (a control master,
# separate from the installer's). When the dashboard is not running there (first start, or the environment
# restarted), it pushes sbdash.sh + usbpeek and starts them nice'd, but only when the environment answers as S2 and
# the night is NOT inside a GPT write (C-gpt) or a card-store write (*-store, the real run): then it waits.
# Read-only on the phone except /tmp/sbdash, the screen and the backlight (see sbdash.sh). Never touches night-s9b.sh.
#   --stop      ends the phone's dashboard (backlight, font and sb-status's screen put back) and this watcher
#   --once N    N rounds, then exit (rehearsals)
# Log: build/installer/logs/sbdash-mac.log. Pid: build/installer/logs/sbdash-mac.pid.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
BUILD=${SBDASH_BUILD:-${REPO%%/.claude/worktrees/*}}
S2=${1:?usage: sbdash-mac.sh S2_SERIAL [--stop] [--once N] [--fake-root DIR]}; shift
STOP= ONCE= FAKE=
while [ $# -gt 0 ]; do case $1 in --stop) STOP=1; shift ;; --once) ONCE=$2; shift 2 ;; --fake-root) FAKE=$(cd "$2" && pwd); shift 2 ;; *) echo "unknown $1"; exit 2 ;; esac; done
ST=${SBINST_STATE:-$BUILD/build/installer}
NIGHT_LOG=${SBDASH_NIGHT_LOG:-$ST/logs/night-1115.log}
NB=${SBDASH_NIGHT_DIR:-$ST/night-s9b}
LOGF=${SBDASH_LOG:-$ST/logs/sbdash-mac.log}
PIDF=${SBDASH_PIDFILE:-$ST/logs/sbdash-mac.pid}
KEY=$ST/env/id_ed25519
PEEK=${SBDASH_USBPEEK:-$ST/usbpeek}
TTY=/dev/tty0
mkdir -p "$(dirname "$LOGF")"
if [ -n "$FAKE" ]; then export PATH="$REPO/tools/installer/fakes:$PATH" FAKE_ROOT=$FAKE; TTY=/tmp/sbdash/screen; fi
CTL=/tmp/sbdash-ssh-$(id -u)-${S2:0:6}
SSH=(ssh -i "$KEY" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
     -o ConnectTimeout=4 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 -o ControlMaster=auto -o "ControlPath=$CTL" -o ControlPersist=60 root@172.16.42.1)
log() { echo "$(date +%H:%M:%S) $*" >> "$LOGF"; }
ph() { "${SSH[@]}" "$@"; }
if [ -n "$STOP" ]; then
	[ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
	ph 'touch /tmp/sbdash/stop' 2>/dev/null && echo "the phone's dashboard is stopping (screen, backlight and font put back)" || echo "the phone did not answer: its dashboard ends with the environment"
	ssh -O exit -o "ControlPath=$CTL" root@172.16.42.1 2>/dev/null
	rm -f "$PIDF"; log "stopped"; exit 0
fi
echo $$ > "$PIDF"
log "watcher started for $S2 (night log $NIGHT_LOG)"
feed() {    # the Mac's view of the night, as key=value + log:/sub: lines
	python3 - "$NIGHT_LOG" "$NB/logs" "$S2" <<'PY'
import hashlib, os, re, sys, time
logp, ldir, serial = sys.argv[1:4]
now = time.time()
lines = open(logp, errors='replace').read().splitlines() if os.path.exists(logp) else []
f = {'phone': 'phone-' + hashlib.sha256(serial.encode()).hexdigest()[:10], 'at': time.strftime('%H:%M:%S')}
st, step, sub, sub_t, night_start = {}, '', '', None, ''
for l in lines:
    m = re.match(r'^######## night-s9b \((\w+)\) (\S+ \S+)', l)
    if m and not night_start:
        night_start = m.group(2)
    m = re.match(r'^### step (\w+) (started|done|failed)', l)
    if m:
        st[m.group(1)] = m.group(2)
        if m.group(2) == 'started':
            step = m.group(1)
    m = re.match(r'^   \[(\d\d:\d\d:\d\d)\] ([A-Za-z0-9-]+):', l)
    if m:
        sub, sub_t = m.group(2), m.group(1)
for k, v in st.items():
    f['st_' + k] = v
names = {'P': "S9 #1's card store", 'C': 'the partition table + One UI /data', 'D': 'One UI 8', 'E': 'Linux distros',
         'F': 'macOS Sequoia', 'G': 'Windows 11', 'H': 'iOS 27', 'ALL': 'all done'}
if st.get('ALL') == 'done':
    f['step'] = 'ALL DONE: S9 #2 has every system; waiting for the morning'
elif step:
    f['step'] = f'{step}: {names.get(step, "")} ({st.get(step, "")})'
f['n_done'] = str(sum(1 for k in 'PCDEFGH' if st.get(k) == 'done'))
f['sub'] = sub
f['phase'] = ('dry run (reads only)' if sub.endswith('-dry') else 'WRITING') if sub else ''
def hms(s):
    s = int(max(s, 0)); return f'{s // 3600}h{s % 3600 // 60:02d}m{s % 60:02d}s' if s >= 3600 else f'{s // 60}m{s % 60:02d}s'
if sub_t:
    t = time.mktime(time.strptime(time.strftime('%Y-%m-%d ') + sub_t, '%Y-%m-%d %H:%M:%S'))
    if t > now + 60:
        t -= 86400
    f['elapsed'] = hms(now - t)
if night_start:
    try:
        f['night_start'] = night_start[11:]
        f['night_elapsed'] = hms(now - time.mktime(time.strptime(night_start, '%Y-%m-%d %H:%M:%S')))
    except ValueError:
        pass
subl = []
sp = os.path.join(ldir, sub + '.txt') if sub else ''
if sp and os.path.exists(sp):
    subl = open(sp, errors='replace').read().replace('\r', '\n').splitlines()
    total = done = rate = None
    for l in subl:
        m = re.search(r'\((\d+) bytes\)', l)
        if m and 'source:' in l:
            total = int(m.group(1))
        m = re.search(r'(\d+) / (\d+) MiB \((\d+) MiB/s', l)
        if m:
            done, tot2, rate = int(m.group(1)) << 20, int(m.group(2)) << 20, int(m.group(3)) << 20
            total = total or tot2
    if total:
        f['total'] = str(total)
        f['done'] = str(min(done or 0, total))
        if rate:
            f['rate'] = str(rate)
            f['eta'] = hms((total - (done or 0)) / rate)
out = [f'{k}={v}' for k, v in f.items()]
out += ['log:' + l for l in lines[-40:]]
out += ['sub:' + l for l in subl[-10:] if l.strip()]
print('\n'.join(out))
PY
}
unsafe() {  # inside a GPT write or a card-store write right now?
	python3 - "$NIGHT_LOG" <<'PY'
import os, re, sys
p = sys.argv[1]
lines = open(p, errors='replace').read().splitlines() if os.path.exists(p) else []
sub, open_step = '', None
for l in lines:
    m = re.match(r'^### step (\w+) (started|done|failed)', l)
    if m:
        open_step = m.group(1) if m.group(2) == 'started' else None
    m = re.match(r'^   \[\d\d:\d\d:\d\d\] ([A-Za-z0-9-]+):', l)
    if m:
        sub = m.group(1)
sys.exit(0 if open_step and (sub == 'C-gpt' or sub.endswith('-store')) else 1)
PY
}
rounds=0
while :; do
	rounds=$((rounds + 1))
	if ph 'echo sbdash-up' 2>/dev/null | grep -q sbdash-up; then
		ser=$(ph 'getprop ro.boot.serialno' 2>/dev/null | tr -d '\r')
		if [ "$ser" != "$S2" ]; then
			log "172.16.42.1 is ${ser:-?}, not $S2: nothing pushed"
		else
			feed > "${LOGF%.log}.feed" 2>/dev/null
			ph 'mkdir -p /tmp/sbdash && cat > /tmp/sbdash/feed.tmp && mv /tmp/sbdash/feed.tmp /tmp/sbdash/feed' < "${LOGF%.log}.feed" 2>/dev/null
			if ! ph '[ -f /tmp/sbdash/pid ] && kill -0 $(cat /tmp/sbdash/pid) 2>/dev/null && echo running' 2>/dev/null | grep -q running; then
				if unsafe; then
					log "the dashboard is not running, but the night is inside a GPT or card-store write: starting it later"
				else
					ph 'cat > /tmp/sbdash/sbdash.sh' < "$HERE/sbdash.sh"
					[ -f "$PEEK" ] && ph 'cat > /tmp/sbdash/usbpeek && chmod 755 /tmp/sbdash/usbpeek' < "$PEEK"
					ph "cd /; if command -v setsid >/dev/null; then S=setsid; else S=; fi; nice -n 19 \$S sh /tmp/sbdash/sbdash.sh /tmp/sbdash $TTY /sys/ /proc/ </dev/null >/dev/null 2>&1 &" 2>/dev/null
					log "dashboard started on $S2"
				fi
			fi
		fi
	fi
	[ -n "$ONCE" ] && [ "$rounds" -ge "$ONCE" ] && break
	sleep 2
done
rm -f "$PIDF"
