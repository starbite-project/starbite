#!/bin/sh
# 1.2-alpha20/21: Calculator, Notes and Modem on the host OS. Calculator: 12 + 7 x 2 = 38 (chained), 1 / 3, 5 / 0, a decimal,
# Delete, C. Notes: a new note saved, its count kept across a restart (the state file), deleted with a hold.
# Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: $3 ($2)"; fails=$((fails + 1)); else echo "ok: $3"; fi; }
ST=/tmp/sbos-apps-state.bin
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 ${STATE:+SBOS_HOST_STATE=$ST} "$out/sbos-host" "$out" /dev/null "$@" q 2>&1; }
UNLOCK="w:300 g:1150,2310,0,700 w:400"
CALC="t:1057,2129 w:600"
NOTES="t:382,2520 w:600"
# keys: columns x 224 552 884 1212, rows y 988 1328 1668 2008
K1="t:224,1668 w:120"; K2="t:552,1668 w:120"; K3="t:884,1668 w:120"; K5="t:552,1328 w:120"; K7="t:224,988 w:120"
K0="t:224,2008 w:120"; KP="t:552,2008 w:120"; KEQ="t:884,2008 w:120"; KADD="t:1212,2008 w:120"
KMUL="t:1212,1328 w:120"; KDIV="t:1212,988 w:120"; KSUB="t:1212,1668 w:120"; KC="t:380,2332 w:120"; KDEL="t:916,2332 w:120"
r=$(run $UNLOCK $CALC $K1 $K2 $KADD $K7 $KMUL $K2 $KEQ \
    $KC $K1 $KDIV $K3 $KEQ \
    $KC $K5 $KDIV $K0 $KEQ \
    $KC $K1 $KP $K5 $KMUL $K2 $KEQ \
    $KC $K7 $K7 $KDEL $KSUB $K1 $K0 $KEQ \
    $KC $K2 $KMUL $KEQ)
want "$r" "calc: = 38$" "12 + 7 x 2 = 38 (chained, left to right)"
want "$r" "calc: = 0.333333$" "1 / 3 = 0.333333"
want "$r" "calc: = error$" "5 / 0 is an error"
want "$r" "calc: = 3$" "1.5 x 2 = 3"
want "$r" "calc: = -3$" "77, Delete, - 10 = -3"
want "$r" "calc: = 4$" "2 x = repeats the number (4)"
never "$r" "CRASH\|app fault\|EL0 fault" "no app fault"
limactl shell starbite rm -f $ST
KB_H="t:860,2434 w:150"; KB_I="t:1070,2246 w:150"; KB_DONE="t:1245,2810 w:600"
r=$(STATE=1 run $UNLOCK $NOTES t:720,636 w:600 $KB_H $KB_I $KB_DONE t:720,904 w:600 $KB_I $KB_DONE w:2500)
want "$r" "notes: saved, 1" "a new note saved"
want "$r" "notes: saved, 2" "a second note saved"
r=$(STATE=1 run $UNLOCK $NOTES)
want "$r" "notes: loaded 2" "both notes kept across a restart"
r=$(STATE=1 run $UNLOCK $NOTES t:720,636 w:600 g:720,1100,0,1500 w:2500)
want "$r" "notes: saved, 1" "a hold deletes one"
limactl shell starbite rm -f $ST
# 1.2-alpha21: Modem (the host's fake modem): only the kernel's confirmation hold starts it; Cancel does not; status
# GETs; Stop parks and lifts the storage hold; a guard parks it (a notification); Heat parks it first; a save waits
# for the hold to lift; a self-test run never starts it.
MODEM="t:382,2129 w:600"; START="t:720,972 w:500"; HOLDSTART="g:720,940,0,1900 w:600"; STOP="t:720,1448 w:600"
r=$(run $UNLOCK $MODEM $START t:720,1210 w:600 w:2000)
never "$r" "modem: online" "Cancel on the confirmation: no start"
r=$(run $UNLOCK $MODEM $START $HOLDSTART w:12000 $STOP w:500)
want "$r" "storage held: the modem runs" "storage writes held while it runs"
want "$r" "modem: online, receive only" "the hold starts it"
want "$r" "modem: +3 s: SIM absent" "the status GETs (3 s, then every 10 s)"
want "$r" "modem: +13 s:" "the second status GET"
never "$r" "PLMN [0-9]" "no PLMN in any log line"
want "$r" "modem: stopped (Stopped): parked" "Stop parks it"
want "$r" "storage hold lifted" "and lifts the storage hold"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_MODEM_GUARD=4 "$out/sbos-host" "$out" /dev/null \
    $UNLOCK $MODEM $START $HOLDSTART w:6000 q 2>&1)
want "$r" "modem: a guard parked the CP" "a guard parks it"
want "$r" "storage hold lifted" "the hold lifted after the guard"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_MODEM_GUARD=3 "$out/sbos-host" "$out" /dev/null \
    $UNLOCK $MODEM $START $HOLDSTART w:5000 h:b:900 w:400 t:700,50 w:600 t:720,632 w:800 s:notif-open q 2>&1)
want "$r" "notif: modem: Modem parked" "the park posts a notification"
want "$r" "ui: a notification opened its app" "a tap on it opens the Modem app (1.2-alpha22)"
r=$(run $UNLOCK $MODEM $START $HOLDSTART w:2000 h:b:900 w:300 t:1057,663 w:600 t:720,670 w:600 g:720,1070,0,1900 w:2000)
want "$r" "modem: stopped (Heat started): parked" "Heat parks the modem first"
limactl shell starbite rm -f $ST
r=$(STATE=1 run $UNLOCK $MODEM $START $HOLDSTART w:1000 h:b:900 w:300 $NOTES t:720,636 w:600 $KB_H $KB_I $KB_DONE w:9000 \
    h:b:900 w:300 $MODEM $STOP w:7000)
want "$r" "settings: the save waits: storage writes are held" "a save waits while the modem runs"
never "$r" "slot write failed" "no failed slot write"
want "$r" "settings: saved (half" "and is written after Stop"
limactl shell starbite rm -f $ST
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_SELFTEST=1 SBOS_HOST_FILES="$root/starbite/sbos/examples" \
    "$out/sbos-host" "$out" /dev/null w:40000 q 2>&1)
want "$r" "app modem" "the self test opens the Modem app"
never "$r" "modem: online" "the self test never starts the modem"
# 1.2-alpha21: Files (SBOS_HOST_FILES = starbite/sbos/examples): the root, a folder, a text file, Up; read only
FILES="t:1057,2520 w:800"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_FILES="$root/starbite/sbos/examples" \
    "$out/sbos-host" "$out" /dev/null $UNLOCK $FILES t:720,800 w:500 t:720,1000 w:500 t:720,1112 w:500 t:540,750 w:500 q 2>&1)
want "$r" "files: a folder of 2 items" "Files: the card's root (2 items)"
want "$r" "files: a folder of 1 items" "Files: a folder opened"
want "$r" "files: a text file shown, bytes 74" "Files: a text file shown"
never "$r" "readme\|notes/" "Files: no file name logged"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 "$out/sbos-host" "$out" /dev/null $UNLOCK $FILES q 2>&1)
want "$r" "files: the folder could not be read" "Files: no card is said, not a crash"
# 1.2-alpha22: Heat > Last session (the heat log): none yet in a fresh run; after 20 s of High, its samples
HEAT="t:1057,663 w:600"; DOWN="g:720,2400,-1500,600 w:600 g:720,2400,-1500,600 w:600 g:720,2400,-1500,600 w:600"; LAST="t:720,2780 w:800"   # alpha24: three drags (the guard's rows)
r=$(run $UNLOCK $HEAT $DOWN $LAST)
want "$r" "heat: the last session's log shown: samples 0" "Heat log: none before the first session"
r=$(run $UNLOCK $HEAT t:720,670 w:400 g:720,1070,0,1900 w:20000 t:220,670 w:1500 $DOWN $LAST)
want "$r" "heat: the last session's log shown: samples [12][0-9]" "Heat log: the session's samples (one a second)"
echo "apps test: $fails failed"
[ "$fails" = 0 ]
