#!/usr/bin/env bash
# Post-mortem of the last Windows run, read from TWRP (phone in recovery).
# Loads the reference project's READ-ONLY rwd1_evidence_reader.ko (prebuilt for
# TWRP's 4.9.118-g36e6ddff22b9 kernel; it maps the retained PRAM records and
# exposes them in debugfs, no MMIO writes), saves the raw blobs plus the pmsg
# ring, unloads the module and decodes everything:
#   PMU RST_STAT   what reset the SoC last: 0x20000000 software, 0x01000000 CPU
#                  cluster 0 hardware watchdog (Rwd1Constants.h)
#   RWD1 record    0xFED13D80, the firmware/driver watchdog ownership record
#   P3 'SMP1'      0xFED13E80, the release firmware's SMP startup record
#   witness 'WWR1' 0xFED17F70, what Windows saw: CPUs, usable pages, ranges
#   unattend/arm   0xFED17F60, 'UAR1' driver auto-return token (should be 0)
#   KEP            0xFED17FB0, firmware handoff marker area
#   tools/woa/twrp-postmortem.sh NAME     -> build/woa/postmortem-NAME/
set -euo pipefail
cd "$(dirname "$0")/../.."
name=${1:?usage: twrp-postmortem.sh NAME}
out=build/woa/postmortem-$name
mkdir -p "$out"
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
ko=ref/exynos9810-woa/tools/twrp-winre/modules/rwd1_evidence_reader.ko
adb push "$ko" /tmp/rwd1_evidence_reader.ko >/dev/null
adb shell 'mount -t debugfs none /sys/kernel/debug 2>/dev/null; rmmod rwd1_evidence_reader 2>/dev/null; insmod /tmp/rwd1_evidence_reader.ko' \
    || { echo "insmod failed"; adb shell 'dmesg | tail -3'; exit 1; }
for f in meta rwd1-first rwd1-second p3-record windows-witness windows-arm kep-record highbank-meta highbank-first; do
    adb exec-out cat "/sys/kernel/debug/rwd1-evidence/$f" > "$out/$f.bin"
done
adb exec-out cat /sys/fs/pstore/pmsg-ramoops-0 > "$out/pmsg.bin"
adb shell 'rmmod rwd1_evidence_reader; rm -f /tmp/rwd1_evidence_reader.ko'
python3 - "$out" <<'PY'
import struct, sys
from pathlib import Path
out = Path(sys.argv[1])
def words(name, n=None):
    b = (out / f'{name}.bin').read_bytes()
    w = list(struct.unpack_from('<%dI' % (len(b) // 4), b))
    return w if n is None else w[:n]
def tag(v):
    s = v.to_bytes(4, 'little')
    return s.decode() if all(32 <= c < 127 for c in s) else f'0x{v:08x}'
stable, rst, cookie, _ = words('meta', 4)
rst_name = {0x20000000: 'SOFTWARE reset (SWRESET)', 0x01000000: 'cluster-0 HARDWARE WATCHDOG'}.get(rst, 'other')
print(f'RST_STAT  0x{rst:08x}  {rst_name}   (reads stable={stable}, rwd1 cookie 0x{cookie:08x})')
r = words('rwd1-second')
if all(w == 0xffffffff for w in r):
    print('RWD1      never written (all 0xFF): the firmware does not keep this record,')
    print('          so the UFS driver cannot take watchdog ownership (NoPetReason=WATCHDOG_INVALID)')
elif all(w == 0 for w in r):
    print('RWD1      cleared (all zero)')
else:
    st = {0x10:'BOOTSHIM_ARMED',0x20:'SEC_ACTIVE',0x30:'DXE_ACTIVE',0x40:'BDS_ACTIVE',0x50:'EBS_ARMED',0x60:'WINDOWS_OWNED',0x70:'P3_POST_HANDOFF',0x80:'SHUTDOWN_PENDING',0x90:'CONTROLLED_STOP',0xA0:'RECOVERY_PENDING',0xB0:'TWRP_ACK',0xE0:'FATAL_NO_PET'}
    print(f'RWD1      magic {tag(r[0])} gen {r[2]} state 0x{r[4]:02x} {st.get(r[4],"?")} owner {r[5]} phase {r[6]} reason {r[7]} rst 0x{r[8]:08x} detail {r[9]} heartbeat {r[10]}')
p = words('p3-record')
if p[0] == 0x31504d53:
    s = p[5] & 0xff
    print(f'P3 SMP1   version 0x{p[1]:08x} generation 0x{p[2]:08x} seq {p[3]} state 0x{s:02x} '
          f'{ {0x60:"PREBRANCH (not acknowledged by the driver)",0x70:"WINDOWS_ACK"}.get(s,"?") } one-shot-armed={bool(p[5] & 0x10000)}')
else:
    print(f'P3        magic {tag(p[0])}')
w = words('windows-witness')
if w[0] == 0x31525757:
    pages = w[4] | (w[5] << 32)
    fl = w[7]
    flags = [n for b, n in ((1,'early'),(2,'init'),(4,'tick'),(8,'memok'),(16,'armed')) if fl & b]
    print(f'witness   WWR1: {w[1]} of {w[2]} processors active, {w[3]} group(s); usable memory {pages} pages = {pages*4//1024} MiB in {w[6]} ranges; flags {"|".join(flags)}; tick {w[8]}')
else:
    print(f'witness   magic {tag(w[0])} (no Windows witness)')
a = words('windows-arm', 4)
print(f'unattend  {tag(a[0])} sec {a[1]} ack {tag(a[3])}' if a[0] else 'unattend  no auto-return token (good)')
k = words('kep-record', 4)
print(f'KEP area  {" ".join(f"{x:08x}" for x in k)}')
pm = (out / 'pmsg.bin').read_bytes()
text = pm.decode('latin-1')
tags = [t for t in ('=UFSENTRY','=UFSINIT','=UFSNORMAL','=UFSWRIT','=UFSBUGCHECK','=UFSSRBSHUT','=UFSSTOPNORM','=UFSREBOOT','=UFSAUTO','=UFSCLEANRESUME','STARBITE: latches') if t in text]
print(f'pmsg      {len(pm)} bytes; =UFSWRIT x{text.count("=UFSWRIT")}; present: {" ".join(tags)}')
if len(pm) >= 15300:
    print('          (ring is at its 0x3F00 cap: records after the write flood are lost)')
PY
echo "raw blobs in $out"
