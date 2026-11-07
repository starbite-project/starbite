#!/usr/bin/env bash
# diag-boot.sh : why One UI 8 (catalog key `android`) does not boot. READ-ONLY, from TWRP over adb, ~3 min.
# Checks the layout starbite-oneui v3 table (docs/ANDROID-ONEUI.md 4), the EROFS images on SYSTEM / VENDOR / ODM
# (SHA-256 of the image range), A16DATA's ext4 (superblock + e2fsck -n), the OS store's android.* files against
# build/oneui-boot/SHA256SUMS, MISC's bootloader message, CACHE's recovery files, and pulls the last boot's logs
# (pstore, last_kmsg, boot guard logcat, tombstones, dropbox, bootstat) without writing anything:
# A16DATA and CACHE are mounted -o ro,noload (no journal replay) only when TWRP has not mounted them.
#   tools/android/oneui/diag-boot.sh [TAG]   -> build/test-boots/<date>-android-diag-TAG/ (summary.txt first)
set -uo pipefail
cd "$(dirname "$0")/../../.."
tag=${1:-loop}
out=build/test-boots/$(date +%Y%m%d-%H%M%S)-android-diag-$tag
mkdir -p "$out"
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
S="$out/summary.txt"
say() { echo "$*" | tee -a "$S"; }
sh() { adb shell "$1" 2>/dev/null </dev/null | tr -d '\r'; }
xo() { adb exec-out "$1" </dev/null; }     # raw bytes; stdin closed (loops below read from heredocs)
pull() { xo "cat $1 2>/dev/null" > "$out/$2" || true; [ -s "$out/$2" ] || rm -f "$out/$2"; }
BN=/dev/block/platform/11120000.ufs/by-name

say "== layout (starbite-oneui v3: start..end in 4 KiB blocks)"
# name expected_start expected_blocks (0 = do not check)
while read -r n st bl; do
    d=$(sh "readlink -f $BN/$n"); s=${d##*/}
    a=$(sh "cat /sys/class/block/$s/start"); z=$(sh "cat /sys/class/block/$s/size")
    a4=$(( ${a:-0} / 8 )); z4=$(( ${z:-0} / 8 ))
    ok=OK; [ "$st" = 0 ] || [ "$a4" = "$st" ] || ok="WRONG start (want $st)"
    [ "$bl" = 0 ] || [ "$z4" = "$bl" ] || ok="$ok WRONG size (want $bl)"
    say "$(printf '%-8s %-6s %9s + %8s  %s' "$n" "$s" "$a4" "$z4" "$ok")"
done <<'EOF'
SYSTEM 68736 1609984
VENDOR 12512000 262144
ODM 12774144 16384
WINDOWS 6744832 5767168
A16DATA 12790528 2825467
CACHE 0 0
EOF

say "== EROFS images (magic at 1024 = e2e1f5e0; SHA-256 of the image range)"
for p in SYSTEM:1567348:7662390c VENDOR:170306:5572b8b7 ODM:9:-; do
    IFS=: read -r n cnt want <<<"$p"
    m=$(xo "dd if=$BN/$n bs=1 skip=1024 count=4 2>/dev/null" | xxd -p)
    h=$(sh "dd if=$BN/$n bs=4096 count=$cnt 2>/dev/null | sha256sum" | cut -c1-64)
    v="sha256 $h"; [ "$want" = - ] || { case "$h" in "$want"*) v="$v OK" ;; *) v="$v MISMATCH (want $want...)" ;; esac; }
    say "$n magic $m  $v"
done
# SYSTEM may hold the 08:45 firstboot rebuild (6419861504 B = 1567349 blocks, dedb499c...) instead
grep -q "^SYSTEM.*MISMATCH" "$S" && h=$(sh "dd if=$BN/SYSTEM bs=4096 count=1567349 2>/dev/null | sha256sum" | cut -c1-64)
case "$h" in dedb499c*) say "SYSTEM = the firstboot rebuild (dedb499c...)" ;; esac

say "== A16DATA ext4"
sm=$(xo "dd if=$BN/A16DATA bs=1 skip=1080 count=2 2>/dev/null" | xxd -p)
say "superblock magic at 1080: $sm (53ef = ext4); first 4 KiB: $(xo "dd if=$BN/A16DATA bs=4096 count=1 2>/dev/null" | xxd -p | tr -d '\n' | cut -c1-64)..."
f2=$(xo "dd if=$BN/A16DATA bs=1 skip=1024 count=4 2>/dev/null" | xxd -p)
[ "$f2" = 1020f5f2 ] && say "A16DATA holds F2FS (TWRP's /data type is f2fs: a TWRP 'Format Data' makes F2FS; One UI wants ext4)"
sh "(dumpe2fs -h $BN/A16DATA || tune2fs -l $BN/A16DATA) 2>&1" > "$out/a16data-sb.txt"
grep -E "state|Block count|Filesystem created|Last mount time|Last write time|Mount count|FS Error count|First error|Last error|error" "$out/a16data-sb.txt" | tee -a "$S"
if ! sh "grep -q ' /data ' /proc/mounts && echo m" | grep -q m; then
    say "e2fsck -fn (read-only):"
    sh "e2fsck -fn $BN/A16DATA 2>&1 | tail -15" | tee -a "$S"
fi

say "== mounts for logs"
dm=$(sh "awk '\$2==\"/data\"{print \$1, \$3}' /proc/mounts")
D=/data
if [ -z "$dm" ]; then
    sh "mkdir -p /tmp/sbd && mount -t ext4 -o ro,noload $BN/A16DATA /tmp/sbd && echo mounted" | grep -q mounted && D=/tmp/sbd || D=
fi
say "A16DATA: ${dm:-not mounted by TWRP} -> logs from ${D:-nowhere (no mount)}"
if [ -n "$D" ]; then
    sh "ls -la $D/local/tmp $D/tombstones $D/misc/bootstat $D/system/dropbox 2>&1" > "$out/data-ls.txt"
    for f in $(sh "ls $D/local/tmp 2>/dev/null | grep starbite"); do pull "$D/local/tmp/$f" "data-$f"; done
    for f in $(sh "ls -t $D/tombstones 2>/dev/null | grep -v pb | head -3"); do pull "$D/tombstones/$f" "tomb-$f"; done
    for f in $(sh "ls -t $D/system/dropbox 2>/dev/null | grep -E 'crash|tombstone|anr|boot|wtf|SYSTEM_' | head -12"); do pull "$D/system/dropbox/$f" "dropbox-$f"; done
    sh "cd $D/misc/bootstat 2>/dev/null && for f in *; do echo \"\$f \$(cat \$f 2>/dev/null | head -c 120)\"; done" > "$out/bootstat.txt"
    sh "ls -la $D/system/users/0 2>&1 | head -5; ls $D 2>&1" >> "$out/data-ls.txt"
    say "boot guard files: $(sh "ls $D/local/tmp 2>/dev/null | grep starbite | tr '\n' ' '")"
    [ "$D" = /tmp/sbd ] && sh "umount /tmp/sbd"
fi
cm=$(sh "awk '\$2==\"/cache\"{print \$1}' /proc/mounts")
C=/cache
[ -n "$cm" ] || { sh "mkdir -p /tmp/sbc && mount -t ext4 -o ro,noload $BN/CACHE /tmp/sbc"; C=/tmp/sbc; }
sh "ls -la $C $C/recovery 2>&1" > "$out/cache-ls.txt"
pull "$C/recovery/command" cache-recovery-command
pull "$C/recovery/last_log" cache-last_log
pull "$C/recovery/last_install" cache-last_install
[ -s "$out/cache-recovery-command" ] && say "PENDING /cache/recovery/command: $(tr '\n' ' ' < "$out/cache-recovery-command")"
[ "$C" = /tmp/sbc ] && sh "umount /tmp/sbc"

say "== MISC bootloader message"
xo "dd if=$BN/MISC bs=2048 count=1 2>/dev/null" > "$out/misc-2k.bin"
say "command: '$(head -c 32 "$out/misc-2k.bin" | tr -d '\0')' recovery: '$(dd if="$out/misc-2k.bin" bs=1 skip=64 count=200 2>/dev/null | tr -d '\0' | tr '\n' ' ')'"

say "== last boot (pstore / last_kmsg)"
for f in $(sh "ls /sys/fs/pstore 2>/dev/null"); do pull "/sys/fs/pstore/$f" "pstore-$f"; done
pull /proc/last_kmsg last_kmsg.txt
pull /proc/reset_reason reset_reason.txt
for f in "$out"/pstore-console* "$out"/last_kmsg.txt; do
    [ -f "$f" ] || continue
    say "-- ${f##*/}: panics / fs errors / init reboots"
    grep -a -E "Kernel panic|EXT4-fs error|EXT4-fs \(sd|erofs|Unable to mount|fs_mgr|reboot|init: .*(fail|abort)|Watchdog|BUG:|Oops|recovery" "$f" | tail -25 | tee -a "$S"
done
say "reset_reason: $(cat "$out/reset_reason.txt" 2>/dev/null)"

say "== OS store: android.* vs build/oneui-boot/SHA256SUMS"
U=$(sh "readlink -f $BN/USERDATA")
xo "dd if=$U bs=4096 skip=4194304 count=7 2>/dev/null" > "$out/store-head.bin"
python3 - "$out/store-head.bin" <<'PY' > "$out/store-android.txt"
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[4096:4104] == b'SBSTORE2', 'no store'
n = struct.unpack_from('<I', d, 4096 + 12)[0]
for i in range(n):
    name, blk, size, crc, typ, fl = struct.unpack_from('<32sQQIHH', d, 3 * 4096 + 64 * i)
    name = name.rstrip(b'\0').decode()
    if name.startswith('android.'):
        print(name, blk, size, f'{crc:08x}')
PY
while read -r name blk size crc; do
    h=$(sh "dd if=$U bs=4096 skip=$((4194304 + blk)) count=$(( (size + 4095) / 4096 )) 2>/dev/null | head -c $size | sha256sum" | cut -c1-64)
    w=$(awk -v n="$name" '$2 == n {print $1}' build/oneui-boot/SHA256SUMS)
    [ "$h" = "$w" ] && r=OK || r="DIFFERS from build/oneui-boot ($w)"
    say "$name $size B crc32 $crc $r"
done < "$out/store-android.txt"
grep -q . "$out/store-android.txt" || say "NO android.* files in the store"
rm -f "$out/store-head.bin"
say "== done: $out"
