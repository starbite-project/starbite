#!/usr/bin/env bash
# W4: write the VM-built Windows disk (tools/woa/vm-build-image.sh) to the phone from TWRP.
#   tools/woa/write-windows.sh build/woa/starbite-win.vhdx
# The VHDX mirrors sda (4Kn, same WINESP/WINDOWS offsets), so the volumes go over unchanged.
# tools/woa/vhdx.py reads it (qemu refuses our 4Kn VHDX). For each partition, the ranges the
# VHDX holds payload for are pushed over adb (256 MiB pieces staged in /tmp); every other range is zero in the VHDX and is
# zero-filled on the phone itself (fast, no USB). Each piece is checked by sha256 on both sides.
set -euo pipefail
cd "$(dirname "$0")/../.."
vhdx=$1
gpt=build/woa/gpt-new/primary.bin
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
adb shell cat /proc/mounts | grep -qE 'sda2[67] ' && { echo "sda26/sda27 mounted"; exit 1; }
MiB=1048576 PIECE=$((256 * 1048576))
stage=$(mktemp "${TMPDIR:-/tmp}/sb-piece.XXXXXX"); trap 'rm -f "$stage"' EXIT

read -r esp_off esp_bytes win_off win_bytes < <(python3 - "$gpt" <<'EOF'
import sys, struct
b = open(sys.argv[1], 'rb').read(); S = 4096; p = {}
for i in range(128):
    e = b[2 * S + i * 128:2 * S + (i + 1) * 128]
    f, l = struct.unpack('<QQ', e[32:48]); p[e[56:128].decode('utf-16le').rstrip('\0')] = (f * S, (l - f + 1) * S)
print(*p['WINESP'], *p['WINDOWS'])
EOF
)
for x in "26 $esp_off $esp_bytes" "27 $win_off $win_bytes"; do
    set -- $x
    read -r st sz < <(adb shell "echo \$(cat /sys/block/sda/sda$1/start) \$(cat /sys/block/sda/sda$1/size)" | tr -d '\r')
    [ "$((st * 512)) $((sz * 512))" = "$2 $3" ] ||   # 512-byte sectors; the phone's sh is 32-bit
        { echo "sda$1 does not match build/woa/gpt-new"; exit 1; }
done

# check N POFF OFF LEN: sha256 of VHDX bytes [POFF+OFF, +LEN) == sdaN bytes [OFF, +LEN)
check() {
    local a b
    a=$(python3 tools/woa/vhdx.py sha "$vhdx" $(( $2 + $3 )) "$4")
    b=$(adb shell "dd if=/dev/block/sda$1 bs=$MiB skip=$(( $3 / MiB )) count=$(( $4 / MiB )) 2>/dev/null | sha256sum" | cut -c1-64)
    [ "$a" = "$b" ] || { echo; echo "sda$1 +$(( $3 / MiB )) MiB x$(( $4 / MiB )): sha256 mismatch"; exit 1; }
}
# send N POFF OFF LEN: stream VHDX bytes to sdaN at OFF, in PIECE-sized verified pieces
send() {
    local n=$1 poff=$2 off=$3 end=$(( $3 + $4 )) k
    while [ "$off" -lt "$end" ]; do
        k=$(( end - off < PIECE ? end - off : PIECE ))
        # Staged in TWRP's RAM: adb exec-in can drop the tail of a long stream (seen: last ~350 KB
        # of a 256 MiB piece missing), adb push cannot.
        python3 tools/woa/vhdx.py cat "$vhdx" $(( poff + off )) "$k" > "$stage"
        adb push "$stage" /tmp/sb-piece >/dev/null
        adb shell "dd if=/tmp/sb-piece of=/dev/block/sda$n bs=$MiB seek=$(( off / MiB )) conv=notrunc 2>/dev/null; rm /tmp/sb-piece"
        check "$n" "$poff" "$off" "$k"
        off=$(( off + k )); sent=$(( sent + k / MiB ))
        printf '\r  sda%s: %d MiB sent' "$n" "$sent"
    done
}
# zero N OFF LEN: zero-fill sdaN [OFF, +LEN) on the phone
zero() {
    adb shell "dd if=/dev/zero of=/dev/block/sda$1 bs=$MiB seek=$(( $2 / MiB )) count=$(( $3 / MiB )) conv=notrunc 2>/dev/null"
    zeroed=$(( zeroed + $3 / MiB ))
}

for x in "26 $esp_off $esp_bytes WINESP" "27 $win_off $win_bytes WINDOWS"; do
    set -- $x
    n=$1 poff=$2 len=$3 sent=0 zeroed=0 pos=0
    runs=$(python3 tools/woa/vhdx.py present "$vhdx" "$poff" "$len")
    payload=$(printf '%s\n' "$runs" | awk '{t+=$2} END {print t/1048576}')
    echo "$4 (sda$n): $(( len / MiB )) MiB, payload $(printf '%s\n' "$runs" | awk '{t+=$2} END {print t/1048576}') MiB in $(printf '%s\n' "$runs" | grep -c .) runs"
    # fd 3: the adb calls inside would otherwise eat the run list from stdin
    while read -r s k <&3; do
        [ -n "$s" ] || continue
        [ "$s" -gt "$pos" ] && zero "$n" "$pos" $(( s - pos ))
        send "$n" "$poff" "$s" "$k"
        pos=$(( s + k ))
    done 3<<<"$runs"
    [ "$pos" -lt "$len" ] && zero "$n" "$pos" $(( len - pos ))
    echo; echo "  sda$n: $sent MiB streamed and verified, $zeroed MiB zero-filled"
    [ $(( sent + zeroed )) -eq $(( len / MiB )) ] && [ "$sent" -eq "$payload" ] ||
        { echo "sda$n: covered $((sent + zeroed)) of $((len / MiB)) MiB, sent $sent of $payload"; exit 1; }
done
# Final spot checks: both boot sectors and the NTFS backup boot sector (last 4 KiB) via the last MiB.
check 26 "$esp_off" 0 "$MiB"
check 27 "$win_off" 0 "$MiB"
check 27 "$win_off" $(( win_bytes - MiB )) "$MiB"
adb shell sync
echo "Windows written and verified"
