#!/bin/sh
# 1.2-alpha20: Calculator and Notes on the host OS. Calculator: 12 + 7 x 2 = 38 (chained), 1 / 3, 5 / 0, a decimal,
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
CALC="t:382,2485 w:600"
NOTES="t:1057,2485 w:600"
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
echo "apps test: $fails failed"
[ "$fails" = 0 ]
