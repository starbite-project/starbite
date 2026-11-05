#!/usr/bin/env bash
# Rebuild the ext4 test fixtures of the installer's no-password check (tests/fixtures/*.ext4.gz).
# Linux with e2fsprogs (on the Mac: limactl shell starbite -- bash tools/installer/tests/mk-ext4-fixtures.sh).
# Each image: /etc/shadow with user "user" (empty password / a PIN hash / locked), optionally
# /etc/starbite/login, /etc/many/ with 300 files (a hashed directory), /etc/big.bin (several hundred KiB) + its sha256.
set -euo pipefail
F=$(cd "$(dirname "$0")" && pwd)/fixtures
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cd "$W"
mk() {   # NAME SHADOW-FIELD LOGIN
	rm -rf s
	mkdir -p s/etc/starbite s/etc/many
	printf 'root:!:20000:0:99999:7:::\nuser:%s:20000:0:99999:7:::\n' "$2" > s/etc/shadow
	[ -z "$3" ] || printf '%s\n' "$3" > s/etc/starbite/login
	for i in $(seq -w 0 299); do echo "file $i" > "s/etc/many/f$i"; done
	printf 'starbite test line %.0s\n' $(seq 1 16000) > s/etc/big.bin
	sha256sum s/etc/big.bin | cut -d' ' -f1 > s/etc/big.sha256
	rm -f "$1.ext4"
	truncate -s 8M "$1.ext4"
	mke2fs -q -t ext4 -L test -E root_owner=0:0 -d s "$1.ext4"
	gzip -9 -n -c "$1.ext4" > "$F/$1.ext4.gz"
}
mk linux-nopin '' none
mk linux-pin '$6$salt$abcdefghijklmnop' ''
mk linux-locked '!' ''
ls -l "$F"
