#!/bin/sh
# Host-side test of the initrd logic (tools/linux/init with SB_TEST=1, tools/linux/reboot-to -n),
# under the same static busybox the initrd uses. In the VM:
#   limactl shell starbite -- /mnt/build/out/busybox-arm64 sh /Users/dev/project-starbite/tools/linux/test-init.sh
# Nothing is mounted and nothing outside a temporary directory is written.
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0 n=0
ok() {                                              # ok NAME GOT WANT
	n=$((n + 1))
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2', want '$3'"; fail=$((fail + 1)); fi
}

for f in "$D/init" "$D/reboot-to" "$D/initrd/sb-tools" "$D/distros/sb-integrate.sh"; do
	[ -f "$f" ] || continue
	sh -n "$f"; ok "syntax ${f#$D/}" $? 0
done

SB_TEST=1
. "$D/init"

# ---- sb_parse: DTB bootargs first, the entry's cmdline after; the last value wins -------------
cl() { printf '%s\n' "$*" > "$T/cmdline"; sb_parse "$T/cmdline"; echo "$SB_ROOT|$SB_INIT|$SB_RW|$SB_SHELL|$SB_WDT"; }
B='console=ramoops-1 console=tty0 loglevel=4 panic=10 maxcpus=8 s9p_vendor_m3_defaults'
ok "no sb.* -> ask"              "$(cl $B)"                                   "ask||1|0|0"
ok "ubuntu entry"                "$(cl $B sb.root=/distros/ubuntu.img)"       "/distros/ubuntu.img||1|0|0"
ok "shell entry"                 "$(cl $B sb.root=none)"                      "none||1|0|0"
ok "bring-up entry"              "$(cl loglevel=7 keep_bootcon sb.root=ask)"  "ask||1|0|0"
ok "last value wins"             "$(cl sb.root=a sb.root=/distros/b.img)"     "/distros/b.img||1|0|0"
ok "all options"                 "$(cl sb.root=x sb.init=/bin/sh sb.rw=0 sb.shell=1 sb.wdt=180)" "x|/bin/sh|0|1|180"
ok "bad wdt -> 0"                "$(cl sb.wdt=12x)"                           "ask||1|0|0"
ok "huge wdt -> 0"               "$(cl sb.wdt=999999)"                        "ask||1|0|0"
ok "relative sb.init dropped"    "$(cl sb.init=bin/sh)"                       "ask||1|0|0"
ok "sb.init with .. dropped"     "$(cl sb.init=/../bin/sh)"                   "ask||1|0|0"
ok "sb.rw=1 / junk"              "$(cl sb.rw=yes sb.shell=2)"                 "ask||1|0|0"
ok "empty cmdline file"          "$(: > "$T/cmdline"; sb_parse "$T/cmdline"; echo $SB_ROOT)" "ask"
ok "missing cmdline file"        "$(sb_parse "$T/nope"; echo $SB_ROOT)"      "ask"
ok "sb.session ignored here"     "$(cl sb.root=ubuntu sb.session=gnome)"      "ubuntu||1|0|0"

# ---- sb_img_path -------------------------------------------------------------------------
ip() { sb_img_path "$1" || echo FAIL; }
ok "path"            "$(ip /distros/ubuntu.img)"   /distros/ubuntu.img
ok "bare name"       "$(ip ubuntu)"                /distros/ubuntu.img
ok "name.img"        "$(ip debian-phosh.img)"      /distros/debian-phosh.img
ok "legacy pmOS"     "$(ip /starbite-linux.img)"   /starbite-linux.img
ok "none"            "$(ip none)"                  FAIL
ok "ask"             "$(ip ask)"                   FAIL
ok "empty"           "$(ip '')"                    FAIL
ok "dotdot"          "$(ip /distros/../x.img)"     FAIL
ok "subdir"          "$(ip /distros/a/b.img)"      FAIL
ok "other dir"       "$(ip /backup/x.img)"         FAIL
ok "_tools"          "$(ip _tools)"                FAIL
ok "_kernel path"    "$(ip /distros/_kernel.img)"  FAIL
ok "space"           "$(ip 'a b')"                 FAIL
ok "semicolon"       "$(ip 'a;reboot')"            FAIL
ok "hidden"          "$(ip .x)"                    FAIL
ok "bare .img"       "$(ip .img)"                  FAIL

# ---- sb_list on a fake card ----------------------------------------------------------------
C=$T/card
mkdir -p "$C/distros/_kernel" "$C/distros/sub.img"
: > "$C/distros/ubuntu.img"; : > "$C/distros/debian-phosh.img"; : > "$C/distros/_tools.img"
: > "$C/distros/notes.txt"; : > "$C/distros/bad name.img"; : > "$C/starbite-linux.img"
ok "sb_list" "$(sb_list "$C" | tr '\n' ' ')" "/distros/debian-phosh.img /distros/ubuntu.img /starbite-linux.img "
ok "sb_list empty" "$(sb_list "$T/none" | tr '\n' ' ')" ""

# ---- sb_pick_init / sb_mods_dir ---------------------------------------------------------------
R=$T/root
mkdir -p "$R/usr/lib/systemd" "$R/usr/sbin" "$R/usr/lib/modules"
ln -s usr/lib "$R/lib"; ln -s usr/sbin "$R/sbin"
ok "no init" "$(SB_INIT=; sb_pick_init "$R" || echo none)" none
: > "$R/usr/lib/systemd/systemd"; chmod 755 "$R/usr/lib/systemd/systemd"
ok "systemd only" "$(SB_INIT=; sb_pick_init "$R")" /usr/lib/systemd/systemd
ln -s /lib/systemd/systemd "$R/usr/sbin/init"         # absolute link: only valid inside the root
ok "/sbin/init link" "$(SB_INIT=; sb_pick_init "$R")" /sbin/init
ok "sb.init missing" "$(SB_INIT=/bin/bash; sb_pick_init "$R" || echo none)" none
ok "sb.init present" "$(SB_INIT=/usr/lib/systemd/systemd; sb_pick_init "$R")" /usr/lib/systemd/systemd
ok "mods usrmerge" "$(sb_mods_dir "$R" 7.1.0-starbite)" "$R/usr/lib/modules/7.1.0-starbite"
mkdir -p "$T/r2/lib"
ok "mods plain" "$(sb_mods_dir "$T/r2" 7.1.0-starbite)" "$T/r2/lib/modules/7.1.0-starbite"

# ---- selector items and actions (no hardware) ------------------------------------------------
ITEMS=$(sb_list "$C"; printf '@shell\n@card\n@menu\n')     # what items_build makes from /sd
NITEMS=$(echo "$ITEMS" | grep -c .)
SEL=0
ok "items" "$NITEMS" 6
act @up;   ok "up wraps" "$SEL" 5
act @down; ok "down wraps" "$SEL" 0
act @down; act @down; ok "down x2" "$(item $SEL)" /starbite-linux.img
ok "label distro" "$(label /distros/debian-phosh.img)" debian-phosh
ok "label shell" "$(label @shell | cut -c1-14)" "Starbite shell"
act 'x;y'; ok "bad FIFO name" "$STATUS" "'x;y' is not a distro name"
ok "why 23" "$(why 23 /distros/x.img)" "/distros/x.img is not on the card"

# ---- card_into_root: the exFAT root-only, only shared/ for the user (mount is stubbed) ---------
SB_TMP=$T
CARD_OWNER=$(id -u)
mount() { echo "mount $*" >> "$T/mnt.log"; [ "${MOUNT_FAIL:-}" != "$1" ]; }
cr() {                                              # cr ROOT: return code | mount calls
	: > "$T/mnt.log"
	card_into_root "$1"; r=$?
	echo "$r|$(sed "s|$1||g" "$T/mnt.log" | tr '\n' ';')"
}
NR1=$T/nr1; mkdir -p "$NR1/var/lib" "$NR1/media"
ok "card: moved root-only, share bound" "$(cr "$NR1")" \
	"0|mount --move /sd /var/lib/starbite/card;mount --bind /var/lib/starbite/card/shared /media/starbite-sd/shared;"
ok "card: /var/lib/starbite 0700" "$(stat -c %a "$NR1/var/lib/starbite")" 700
NR2=$T/nr2; mkdir -p "$NR2/var/lib/starbite/card"; chmod 755 "$NR2/var/lib/starbite"
cr "$NR2" >/dev/null
ok "card: a 0755 dir is made 0700" "$(stat -c %a "$NR2/var/lib/starbite")" 700
NR3=$T/nr3; mkdir -p "$NR3/var/lib" "$NR3/elsewhere"; ln -s ../../elsewhere "$NR3/var/lib/starbite"
ok "card: symlinked dir refused" "$(cr "$NR3")" "1|"
NR4=$T/nr4; mkdir -p "$NR4/var/lib"
ok "card: failed move -> 2" "$(MOUNT_FAIL=--move cr "$NR4")" "2|mount --move /sd /var/lib/starbite/card;"
NR5=$T/nr5; mkdir -p "$NR5/var/lib" "$NR5/media/starbite-sd"; ln -s / "$NR5/media/starbite-sd/shared"
ok "card: symlinked share not bound" "$(cr "$NR5")" "0|mount --move /sd /var/lib/starbite/card;"
if [ "$(id -u)" != 0 ]; then                        # read-only stand-in (root ignores modes)
	NR6=$T/nr6; mkdir -p "$NR6/var/lib"; chmod 555 "$NR6/var/lib"
	ok "card: no dir, no write -> stays hidden" "$(cr "$NR6")" "1|"
	chmod 755 "$NR6/var/lib"
fi
unset -f mount

# ---- tty_holders: the shells shells_stop kills (fake /proc) ------------------------------------
P=$T/proc
for x in 123:/dev/ttyGS0 124:/dev/pts/3 125:/dev/null 126:/dev/tty0 $$:/dev/pts/0; do
	mkdir -p "$P/${x%%:*}/fd"; ln -s "${x#*:}" "$P/${x%%:*}/fd/0"
done
ln -s /dev/pts/1 "$P/125/fd/7"
ok "tty_holders" "$(tty_holders "$P" | sort -n | tr '\n' ' ')" "123 124 125 "

# ---- wdt_conf: sb.wdt is the only switch -------------------------------------------------------
WR=$T/wr; mkdir -p "$WR/etc/systemd/system"
wdt_conf "$WR" 45
ok "wdt on: drop-in" "$(grep '^RuntimeWatchdogSec' "$WR$WDT_CONF")" "RuntimeWatchdogSec=45s"
wdt_conf "$WR" 0
ok "wdt off: drop-in removed" "$([ -e "$WR$WDT_CONF" ] && echo left || echo gone)" gone
ok "wdt off: no drop-in is fine" "$(wdt_conf "$WR" 0; echo $?)" 0
OR=$T/openrc; mkdir -p "$OR/etc"
ok "wdt on, not systemd: nothing" "$(wdt_conf "$OR" 45; echo $?; ls "$OR/etc")" 0

# ---- reboot-to: the SBNEXT2! mailbox (shim/starlte/menu.c boot_next) ----------------------
RT=$D/reboot-to
ok "reboot-to linux-ubuntu" "$(sh "$RT" -n linux-ubuntu | tr '\n' ';')" \
"devmem 0xFED17FE8 32 0x756e696c;devmem 0xFED17FEC 32 0x62752d78;devmem 0xFED17FF0 32 0x75746e75;devmem 0xFED17FF4 32 0x00000000;devmem 0xFED17FF8 32 0x00000000;devmem 0xFED17FFC 32 0x00000000;devmem 0xFED17FE0 32 0x454E4253;devmem 0xFED17FE4 32 0x21325458;devmem 0x1406080C 32 0x12345670;"
ok "reboot-to menu" "$(sh "$RT" -n menu)" "devmem 0x1406080C 32 0x12345670"
ok "reboot-to 23 chars" "$(sh "$RT" -n abcdefghijklmnopqrstuvw | sed -n 6p)" "devmem 0xFED17FFC 32 0x00777675"
ok "reboot-to 24 chars" "$(sh "$RT" -n abcdefghijklmnopqrstuvwx 2>/dev/null; echo $?)" 2
ok "reboot-to bad key" "$(sh "$RT" -n 'a b' 2>/dev/null; echo $?)" 2
ok "reboot-to no key" "$(sh "$RT" -n 2>/dev/null; echo $?)" 2
ok "reboot-to: 9 writes" "$(sh "$RT" -n linux | wc -l | tr -d ' ')" 9
if [ "$(id -u)" != 0 ]; then                        # never run for real as root: it would reboot
	ok "reboot-to as a user refuses" "$(sh "$RT" linux-ubuntu 2>&1; echo $?)" \
		"reboot-to: needs root for /dev/mem (sudo reboot-to linux-ubuntu)
1"
	ok "reboot-to -w as a user refuses" "$(sh "$RT" -w linux 2>/dev/null; echo $?)" 1
fi
ln -s "$RT" "$T/reboot-linux"
ok "reboot-linux = linux" "$(sh "$T/reboot-linux" -n | sed -n 1p)" "devmem 0xFED17FE8 32 0x756e696c"

# Decode what reboot-to writes back into the key the menu reads (bytes 8..30, little-endian).
dec() {
	sh "$RT" -n "$1" | while read -r _ a _ v; do
		case $a in 0xFED17FE[89A-F]|0xFED17FF*) ;; *) continue ;; esac
		v=${v#0x}; printf "\\$(printf %o 0x${v#??????})\\$(printf %o 0x$(echo $v | cut -c5-6))\\$(printf %o 0x$(echo $v | cut -c3-4))\\$(printf %o 0x$(echo $v | cut -c1-2))"
	done | tr -d '\000'
}
for k in linux linux-shell linux-ubuntu-gnome linux-bringup; do
	ok "round trip $k" "$(dec $k)" "$k"
done

echo "$((n - fail))/$n passed"
[ $fail -eq 0 ]
