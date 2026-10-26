#!/bin/sh
# Run on the phone's bring-up Linux (fetched from the Mac's http server on
# 172.16.42.2:8643, serving build/linux): clock, apk.static (checked against
# the hash the Mac got over HTTPS), Alpine + postmarketOS signing keys, and a
# RAM tools root (/tmp/tools) with e2fsprogs and exfatprogs.
#   sh phone-prep.sh EPOCH APK_SHA256
set -u
date -u -s "@$1" >/dev/null
export http_proxy=http://172.16.42.2:3128
M=http://172.16.42.2:8643
APKV=apk-tools-static-3.0.8-r0.apk
mkdir -p /tmp/apk /tmp/keys
if [ ! -x /tmp/apk/sbin/apk.static ]; then
	(cd /tmp/apk && wget -q "http://dl-cdn.alpinelinux.org/alpine/edge/main/aarch64/$APKV") || exit 1
	got=$(sha256sum "/tmp/apk/$APKV" | cut -c1-64)
	[ "$got" = "$2" ] || { echo "apk-tools-static hash mismatch: $got"; exit 1; }
	(cd /tmp/apk && tar -xzf "$APKV" 2>/dev/null)
fi
for k in $(http_proxy= wget -q -O - "$M/keys/" | grep -o 'href="[^"]*\.pub"' | cut -d'"' -f2); do
	n=$(echo "$k" | sed 's/%40/@/g')
	[ -f "/tmp/keys/$n" ] || http_proxy= wget -q -O "/tmp/keys/$n" "$M/keys/$k"
done
T=/tmp/tools
if [ ! -x $T/usr/sbin/fsck.exfat ]; then
	mkdir -p $T/etc/apk/keys
	cp /tmp/keys/*.pub $T/etc/apk/keys/
	R=http://dl-cdn.alpinelinux.org/alpine/edge
	/tmp/apk/sbin/apk.static --root $T --initdb -X $R/main -X $R/community --no-interactive \
		add exfatprogs e2fsprogs >/dev/null 2>&1 || { echo "tools install failed"; exit 1; }
fi
echo "prep ok: $(date -u), $(ls /tmp/keys | wc -l) keys, tools in $T"
