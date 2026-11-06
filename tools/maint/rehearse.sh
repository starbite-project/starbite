#!/usr/bin/env bash
# Rehearses Starbite Maintenance end to end in the Lima VM, without the phone (docs/MAINTENANCE.md 11):
#
#   limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/maint/rehearse.sh [DIR]
#
# DIR (default <main checkout>/build/maint) holds the build (sbmaint.img, known_hosts). The container's
# own initramfs is unpacked into a chroot; its dropbear (the built-in host key, key-only login) listens
# on 127.0.0.1:2222; the UNCHANGED host tools then drive it over ssh through tools/maint/transport/adb
# (SB_TRANSPORT=maint, the pinned host key), with the phone's partitions as plain sparse files named as in
# S9 #1's GPT (+ SBMAINT). No loop devices, no disks: /proc and /sys in the chroot are small fakes (so no
# script can reboot the VM or touch its memory), and sb-reboot, reboot-to, devmem, memdump, reboot are
# recording stubs. What it checks: login, the by-name paths, part-write.py (a zero chunk and a pushed
# chunk, verified), part-backup.sh, metadata-reset.sh with SBMAINT's mke2fs.conf (no orphan_file),
# store-flash.sh + store-pull.sh, sbm update into the SBMAINT slots (A, then B, then a torn B falls back to
# A), push/pull of directories, the job line, sb-protect, sbm logs decoding a fake ramoops/mailbox/POST
# page, the maintenance-environment guard, a wrong host key, and that recovery is refused everywhere.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
MAIN=${MAIN:-${REPO%/.claude/worktrees/*}}
D=${1:-$MAIN/build/maint}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo): chroot" >&2; exit 1; }
[ -s "$D/sbmaint.img" ] && [ -s "$D/known_hosts" ] || { echo "no $D/sbmaint.img + known_hosts (mk-maint.sh)" >&2; exit 1; }
KEY=${KEY:-$MAIN/build/linux/ssh/starbite_ed25519}
[ -s "$KEY" ] || { echo "no ssh key $KEY" >&2; exit 1; }

W=$(mktemp -d /var/tmp/sbm-rehearse.XXXX)
R=$W/root
LOG=$W/log
cleanup() {
	[ -s "$R/run/dropbear.pid" ] && kill "$(cat "$R/run/dropbear.pid")" 2>/dev/null
	sleep 0.5
	for m in "$R/dev" "$R/proc" "$R/sys" "$R/run" "$R/tmp"; do mountpoint -q "$m" && umount -l "$m"; done
	cp "$LOG" "$D/rehearse.log" 2>/dev/null
	rm -rf "$W"
}
trap cleanup EXIT
pass=0 fail=0
ok() { echo "PASS  $*"; pass=$((pass + 1)); }
no() { echo "FAIL  $*"; fail=$((fail + 1)); }
t() {                                      # t NAME CMD...: CMD's exit status decides
	local name=$1; shift
	echo "==== $name: $*" >> "$LOG"
	if "$@" >> "$LOG" 2>&1; then ok "$name"; else no "$name"; tail -5 "$LOG" | sed 's/^/      /'; fi
}

# ---- the chroot: the container's own initramfs ----------------------------------------------------
python3 "$REPO/tools/maint/pack.py" extract "$D/sbmaint.img" "$W/x" > /dev/null
mkdir -p "$R"
(cd "$R" && zstd -dc "$W/x/initrd" | cpio -idm --quiet 2>/dev/null)
[ -x "$R/init" ] && [ -x "$R/usr/sbin/dropbear" ] || { echo "the initramfs did not unpack" >&2; exit 1; }
mount -t tmpfs tmpfs "$R/dev"
mknod -m 666 "$R/dev/null" c 1 3; mknod -m 666 "$R/dev/zero" c 1 5
mknod -m 666 "$R/dev/urandom" c 1 9; mknod -m 666 "$R/dev/random" c 1 8; mknod -m 666 "$R/dev/tty" c 5 0
mount -t tmpfs tmpfs "$R/proc"                 # fake: no sysrq-trigger, no /proc/<pid> of the VM
mkdir -p "$R/proc/net"
: > "$R/proc/mounts"
echo "sb.maint=1 sb.maint.src=rehearsal" > "$R/proc/cmdline"
printf 'MemAvailable:   3000000 kB\n' > "$R/proc/meminfo"
echo "42.00 1.00" > "$R/proc/uptime"
printf 'IP address HW type Flags HW address Mask Device\n' > "$R/proc/net/arp"
mount -t tmpfs tmpfs "$R/sys"
mount -t tmpfs tmpfs "$R/run"
mount -t tmpfs -o mode=1777 tmpfs "$R/tmp"
mkdir -p "$R/run/sbmaint/early" "$R/dev/block/by-name" "$R/sys/class/block" "$R/root/.ssh"
: > "$R/run/sbmaint/log"
printf 'mode=maint\nsrc=rehearsal\nseq=\nfrom=\n' > "$R/run/sbmaint/state"

# recording stubs: nothing in the chroot may reboot the VM or touch physical memory
for s in sb-reboot reboot-to devmem memdump reboot poweroff halt; do
	for p in bin sbin usr/bin usr/sbin usr/local/sbin; do [ -e "$R/$p/$s" ] || [ -L "$R/$p/$s" ] && rm -f "$R/$p/$s"; done
	printf '#!/bin/sh\necho "%s $*" >> /run/sbmaint/stub.log\n' "$s" > "$R/bin/$s"
	chmod 755 "$R/bin/$s"
done
# blockdev on the partition FILES (busybox's for anything else)
RB=$(chroot "$R" /bin/sh -c 'command -v blockdev')
rm -f "$R$RB"
cat > "$R$RB" <<'EOF'
#!/bin/sh
for a; do f=$a; done
if [ -f "$f" ]; then
	case $1 in --getsize64) stat -c %s "$f" ;; --setro|--setrw|--rereadpt|--flushbufs) echo "blockdev $*" >> /run/sbmaint/stub.log ;; *) exec /bin/busybox blockdev "$@" ;; esac
else
	exec /bin/busybox blockdev "$@"
fi
EOF
chmod 755 "$R$RB"

# ---- the phone's partitions: sparse files named as S9 #1's GPT, and their sysfs size/dev/uevent ------
part() {                                     # part INDEX NAME BYTES
	local n=sda$1
	truncate -s "$3" "$R/dev/block/$n"
	ln -s "../$n" "$R/dev/block/by-name/$2"
	mkdir -p "$R/sys/class/block/$n"
	echo $(($3 / 512)) > "$R/sys/class/block/$n/size"
	echo 0 > "$R/sys/class/block/$n/ro"
	printf 'DEVNAME=%s\nPARTN=%s\nPARTNAME=%s\n' "$n" "$1" "$2" > "$R/sys/class/block/$n/uevent"
}
python3 - "$REPO" > "$W/rows" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + '/tools/installer')
from sbinst import ufslayout
for i, p in enumerate(ufslayout.get('starbite-a').parts, 1):
    print(i, p.name)
PY
MIB=1048576
while read -r i name; do
	case $name in
	SYSTEM) sz=$((80 * MIB)) ;; ODM) sz=$((32 * MIB)) ;; VENDOR|A16DATA) sz=$((16 * MIB)) ;;
	USERDATA) sz=$(( (4194304 + 65536) * 4096 )) ;;        # the Apple slot (16 GiB) + a 256 MiB OS store, sparse
	*) sz=$MIB ;;
	esac
	part "$i" "$name" "$sz"
done < "$W/rows"
part 29 SBMAINT $((256 * MIB))                              # the proposal's row (docs/MAINTENANCE.md 4.1)
UD=$R/dev/block/sda25
python3 - "$UD" <<'PY'
import struct, sys
with open(sys.argv[1], 'r+b') as f:                         # an HFS+ volume header in the Apple slot
    f.seek(1024)
    f.write(b'H+' + bytes(38) + struct.pack('>II', 4096, 1000))
PY

# ---- sshd: the build's own dropbear, host key and keys ---------------------------------------------
cp "$R/etc/sbmaint/authorized_keys" "$R/root/.ssh/authorized_keys"
chmod 700 "$R/root" "$R/root/.ssh"; chmod 600 "$R/root/.ssh/authorized_keys"
install -m600 "$KEY" "$W/key"
chroot "$R" /usr/sbin/dropbear -s -g -j -k -p 127.0.0.1:2222 -r /etc/dropbear/dropbear_ed25519_host_key -P /run/dropbear.pid
sleep 1
export SB_TRANSPORT=maint SB_MAINT_HOST=127.0.0.1 SB_MAINT_PORT=2222 SB_MAINT_KEY=$W/key SB_MAINT_KNOWN_HOSTS=$D/known_hosts
export BACKUP_LOG=$W/backups.txt PATH=$REPO/tools/maint/transport:$PATH
ADB=$REPO/tools/maint/transport/adb
SBM=$REPO/tools/maint/sbm
eqs() { [ "$1" = "$2" ] || { echo "got '$1', want '$2'"; return 1; }; }

t "login: key only, the pinned host key" eqs "$(adb shell 'echo ok; cat /etc/sbmaint-release' | head -1)" ok
t "get-state answers recovery" eqs "$(adb get-state)" recovery
t "sbm status" "$SBM" status
t "by-name SYSTEM -> /dev/block/sda18 (as TWRP)" eqs "$(adb shell readlink -f /dev/block/by-name/SYSTEM)" /dev/block/sda18

# part-write.py: chunk 0 zeros (dd from /dev/zero on the phone), chunk 1 pushed gzip'd; whole-image hash
head -c $((64 * MIB)) /dev/zero > "$W/system.img"
head -c $((4 * MIB)) /dev/urandom >> "$W/system.img"
t "part-write.py SYSTEM --dry-run" python3 "$REPO/tools/android/pixel/part-write.py" SYSTEM "$W/system.img" --dry-run
t "part-write.py SYSTEM (real, verified)" python3 "$REPO/tools/android/pixel/part-write.py" SYSTEM "$W/system.img" --state "$W/pw.state"
t "the partition holds the image" cmp -n $((68 * MIB)) "$W/system.img" "$R/dev/block/sda18"
t "part-backup.sh mac SYSTEM (verified)" bash "$REPO/tools/android/pixel/part-backup.sh" mac SYSTEM "$W/bk"
t "part-backup.sh mac ODM" bash "$REPO/tools/android/pixel/part-backup.sh" mac ODM "$W/bk"
t "metadata-reset.sh (SBMAINT's mke2fs)" bash "$REPO/tools/android/pixel/metadata-reset.sh"
feat=$(chroot "$R" dumpe2fs -h /dev/block/sda20 2>/dev/null | sed -n 's/^Filesystem features: *//p')
t "ODM ext4 mountable by Android's 4.9 (no orphan_file)" sh -c "echo '$feat' | grep -qw has_journal && ! echo '$feat' | grep -qw orphan_file"

# the OS store tools
echo "a rehearsal store entry" > "$W/note.txt"
t "sbstore2.py build (a 256 MiB-capacity store)" python3 "$REPO/tools/store/sbstore2.py" build "$W/store.img" "rehearsal.txt=$W/note.txt:text" --capacity 65536
t "store-flash.sh (Apple slot + store checks, read back)" bash "$REPO/tools/store/store-flash.sh" "$W/store.img"
t "store-pull.sh" bash "$REPO/tools/store/store-pull.sh" "$W/pulled.img" ufs
t "pulled store = flashed store" cmp "$W/store.img" "$W/pulled.img"

# sbm update: slot A, then B, then a torn B falls back to A (the loader's choice, pack.py select)
t "sbm update (dry run)" "$SBM" update
t "sbm update --real: slot a" "$SBM" update --real
t "the loader would start slot a" sh -c "python3 '$REPO/tools/maint/pack.py' select '$R/dev/block/sda29' | grep -q 'starts slot a'"
t "sbm update --real: slot b (a untouched)" "$SBM" update --real
t "the loader would start slot b (seq 2)" sh -c "python3 '$REPO/tools/maint/pack.py' select '$R/dev/block/sda29' | grep -q 'starts slot b'"
printf 'torn' | dd of="$R/dev/block/sda29" bs=1 seek=$((128 * MIB + 6 * MIB)) conv=notrunc 2>/dev/null
t "a torn slot b: the loader falls back to a" sh -c "python3 '$REPO/tools/maint/pack.py' select '$R/dev/block/sda29' | grep -q 'starts slot a'"
t "sbm update rewrites the torn slot b" sh -c "'$SBM' update --real | grep -q 'slot b: written and verified (seq 2)'"

# files, the job line, protection, logs
mkdir -p "$W/tree/sub"; echo A > "$W/tree/sub/a.txt"
t "sbm push (directory)" "$SBM" push "$W/tree" /tmp/tree
t "sbm pull (directory)" sh -c "'$SBM' pull /tmp/tree '$W/back' && cmp '$W/tree/sub/a.txt' '$W/back/sub/a.txt'"
t "sbm job" sh -c "'$SBM' job writing SYSTEM 3/9 && grep -q 'writing SYSTEM 3/9' '$R/run/sbmaint/job'"
t "sbm unlock RECOVERY is refused" sh -c "! '$SBM' unlock RECOVERY"
t "sbm unlock EFS" sh -c "'$SBM' unlock EFS && grep -qx EFS '$R/run/sbmaint/allowed'"
python3 - "$R/run/sbmaint/early" <<'PY'
import struct, sys
d = sys.argv[1]
cap = 0x4000 - 12
pm = b'STARBITE: POST quick 312ms fail 0 warn 0\nSTARBITE: boot mac13\n'
z1 = struct.pack('<III', 0x43474244, 0, 0) + bytes(cap)
z2 = bytearray(struct.pack('<III', 0x43474244, len(pm), len(pm)) + pm + bytes(cap - len(pm)))
z2[-32:] = b'SBNEXT2!' + b'maint'.ljust(24, b'\0')
open(f'{d}/ramoops.bin', 'wb').write(z1 + bytes(z2))
page = [0] * 512
page[200] = 0x504F5354 << 32 | 1 << 24 | 13 << 16 | 0 << 8 | 1
page[201] = 312000
page[10] = 0x701
open(f'{d}/loader-log.bin', 'wb').write(struct.pack('<512Q', *page))
open(f'{d}/seclog.bin', 'wb').write(b'\0' * 100 + b'S-Boot 4.0 reset reason: SWRESET\n' + b'\0' * 100)
PY
t "sbm logs (pmsg ring, mailbox, POST, sec_log)" sh -c "'$SBM' logs rehearsal > '$W/logs.out' && grep -q 'boot-next mailbox: \"maint\"' '$W/logs.out' && grep -q 'loader POST: mode 1, 13 checks' '$W/logs.out' && grep -q 'console-pmsg' '$W/logs.out'"
rm -rf "$MAIN"/build/maint/logs/*-rehearsal

# refusals and guards
t "adb reboot recovery is refused" sh -c "! adb reboot recovery"
t "sbm reboot-to recovery is refused" sh -c "! '$SBM' reboot-to recovery"
t "sbm reboot-to a non-catalog key is refused" sh -c "! '$SBM' reboot-to mac13-v1"
t "sbm reboot-to maint (stubbed sb-reboot)" sh -c "'$SBM' reboot-to maint && grep -q 'sb-reboot maint' '$R/run/sbmaint/stub.log'"
mv "$R/etc/sbmaint-release" "$W/rel"
t "not the maintenance environment: refused, nothing run" sh -c "adb shell 'touch /tmp/should-not-exist'; [ \$? = 97 ] && [ ! -e '$R/tmp/should-not-exist' ]"
mv "$W/rel" "$R/etc/sbmaint-release"
echo "sbmaint ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIG4cDd8F5U2Yh9xg5bQkqO0XU9n1Jt0nI1wW3tYqY0aV" > "$W/wrong_known_hosts"
t "a different host key is refused" sh -c "! SB_MAINT_KNOWN_HOSTS='$W/wrong_known_hosts' adb get-state"
t "no stub saw a recovery reboot" sh -c "! grep -qi recovery '$R/run/sbmaint/stub.log'"

echo "result: $pass passed, $fail failed (log: $D/rehearse.log)"
[ $fail = 0 ]
