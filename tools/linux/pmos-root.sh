#!/usr/bin/env bash
# Run a command as root on the phone's postmarketOS over USB SSH: doas, with
# the PIN read from build/linux/pmos-login.txt (git-ignored) and answered by
# expect, so it never shows up in a command line or log. Prints the command's
# output; exits with its status.
#   tools/linux/pmos-root.sh 'blockdev --getro /dev/sda25'
set -euo pipefail
cd "$(dirname "$0")/../.."
pin=$(grep -o '[0-9]\{6\}' build/linux/pmos-login.txt)
remote="doas sh -c $(printf '%q' "$*"); echo __rc=\$?"
PIN="$pin" RCMD="$remote" expect -c '
	log_user 0
	set timeout 3600
	spawn -noecho ssh -tt -q -i build/linux/ssh/starbite_ed25519 -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o BatchMode=yes user@172.16.42.1 \
		$env(RCMD)
	expect -re "(?i)password.*: " { send -- "$env(PIN)\r" }
	expect -re "\r?\n"
	log_user 1
	expect eof
	' | tr -d '\r' | awk '/^__rc=/{rc=substr($0,6); next} {print} END{exit rc+0}'
