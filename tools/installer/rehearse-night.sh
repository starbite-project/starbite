#!/bin/bash
# tools/installer/rehearse-night.sh [WORKDIR]: tools/installer/night-s9b.sh against two FAKE phones (no phone touched):
# S9 #1 in TWRP (starbite-oneui table, the 128 GB card with a compact home store holding every entry C-H needs, the
# distros on p1, data in its partitions and the Sequoia range) and S9 #2 after steps A/B (its installer environment,
# the STOCK table, an sd64l card with step A's store). Big copies are capped (NIGHT_SIZE_CAP) to keep it to minutes.
# Checks: the whole night (P, C-H) done; S9 #2's table = starbite-oneui; each copy = S9 #1's bytes; A16DATA made;
# the card store's rows and entries, its volatile entries kept; S9 #1 byte-identical; S9 #2's RECOVERY, BOOT, EFS,
# PARAM ... unchanged; a cable pull in step G stops the night ("### step G failed"), the same command resumes it; a
# third run does nothing. WORKDIR (default build/night-rehearsal) is removed unless KEEP=1.
set -uo pipefail
cd "$(dirname "$0")/../.."
REPO=$PWD
W=${1:-build/night-rehearsal}
rm -rf "$W"; mkdir -p "$W"; W=$(cd "$W" && pwd)
export NIGHT_STATE=$W/state NIGHT_LOG=$W/night.log NIGHT_SIZE_CAP=$((192 << 20))
MAIN=${REPO%%/.claude/worktrees/*}                # a worktree's main checkout (build/ lives there)
export NIGHT_CATALOG=${NIGHT_CATALOG:-$MAIN/build/store/catalog-live.toml} NIGHT_CATALOG_A=$W/catalog-a.toml
export NIGHT_BUNDLES=${NIGHT_BUNDLES:-$MAIN/build/installer/night-s9b}
unset ANDROID_SERIAL SBINST_SERIAL FAKE_FAIL SBINST_FAKE_USB_EXTRA
[ -f "$NIGHT_CATALOG" ] || { echo "no $NIGHT_CATALOG (S9 #1's catalog TOML)"; exit 2; }
FP=(python3 -B tools/installer/fakes/fakephone.py)
pass=0 fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
tree_hash() {
	python3 - "$1" <<'PY'
import hashlib, os, sys
h = hashlib.sha256()
for dp, dn, fn in sorted(os.walk(sys.argv[1])):
    dn.sort()
    for f in sorted(fn):
        p = os.path.join(dp, f)
        if os.path.islink(p) or f in ('state', 'usb', 'authorized_keys', 'fail-count'):
            continue
        h.update(p.encode())
        with open(p, 'rb') as fh:
            fd, off, end = fh.fileno(), 0, os.fstat(fh.fileno()).st_size
            while off < end:
                try:
                    off = os.lseek(fd, off, os.SEEK_DATA)
                except OSError:
                    break
                hole = os.lseek(fd, off, os.SEEK_HOLE)
                fh.seek(off)
                while off < hole:
                    b = fh.read(min(1 << 24, hole - off)); h.update(off.to_bytes(8, 'little') + b); off += len(b)
print(h.hexdigest())
PY
}

echo "######## two fake phones"
"${FP[@]}" create "$W/s1" --state recovery --gpt starbite-oneui --card starbite --layout sd128 >/dev/null
"${FP[@]}" create "$W/s2" --state env --gpt stock --card starbite --layout sd64l --card-sectors 124735488 >/dev/null
sed -i.bak 's/^ro.boot.serialno=.*/ro.boot.serialno=S9TWO02/' "$W/s2/props" && rm "$W/s2/props.bak"
mkdir -p "$W/state/env" "$W/in"
ssh-keygen -q -t ed25519 -N '' -C night-rehearsal -f "$W/state/env/id_ed25519"
cp "$W/state/env/id_ed25519.pub" "$W/s2/authorized_keys"
python3 - "$W" "$NIGHT_CATALOG" <<'PY' || { echo "FAIL the fake phones could not be set up"; exit 1; }
# S9 #1: data in its partitions, the distros, the Sequoia range, a compact sd128 home store with the entries.
# S9 #2: step A's card store (a small one with the same shape: a catalog, Starbite OS, the volatile entries).
import os, struct, subprocess, sys, tomllib
w, cat = sys.argv[1], sys.argv[2]
s1, s2 = os.path.join(w, 's1'), os.path.join(w, 's2')
def node(r, n): return os.path.realpath(os.path.join(r, 'dev/block/by-name', n))
def put(path, off, data):
    with open(path, 'r+b') as f: f.seek(off); f.write(data)
for n in ('SYSTEM', 'VENDOR', 'ODM', 'WINESP', 'WINDOWS', 'USERDATA'):
    put(node(s1, n), 0, os.urandom(min(40 << 20, os.path.getsize(node(s1, n)))))
put(os.path.join(s1, 'dev/block/mmcblk0'), 182888448 * 512, os.urandom(50 << 20))
d = os.path.join(s1, 'external_sd/distros'); os.makedirs(d, exist_ok=True)
for f, mib in (('chromeos', 70), ('ubuntu', 90), ('pmos', 30), ('_tools', 5)):
    open(os.path.join(d, f + '.img'), 'wb').write(os.urandom(mib << 20))
live = tomllib.loads(open(cat).read())
mac = next(e for e in live['entry'] if e['key'] == 'mac15')['files'][0]
ind = os.path.join(w, 'in'); os.makedirs(ind, exist_ok=True)
pay = bytearray(0x48 + 608 + 8192); pay[0:8] = b'SWEETXNU'; pay[8:12] = (1).to_bytes(4, 'little')
cmd = b'-v sdroot=lba:182888448,33554432 starlte=1'; pay[0x48:0x48 + len(cmd)] = cmd
files = {mac: bytes(pay)}
for n, kb in (('android.kernel', 900), ('android.ramdisk', 300), ('android.dtb', 60), ('linux.image', 800),
              ('linux.dtb', 20), ('linux.initrd', 400), ('windows-t32s-nocache.uefi', 300), ('windows-big3i.uefi', 300),
              ('ios27-refport-t8.xnu', 500), ('ios.overlay', 100), ('starbite-next-gpu.img', 600)):
    files[n] = os.urandom(kb << 10)
files['android.cmdline'] = b'console=ram'
args = []
for n, b in files.items():
    open(os.path.join(ind, n), 'wb').write(b); args.append(f'{n}={os.path.join(ind, n)}')
sb = os.path.join('tools', 'store', 'sbstore2.py')
img = os.path.join(ind, 's1-store.img')
subprocess.run([sys.executable, sb, 'build', img, '--medium', 'sd', '--home', '--data-at', 'layout', '--catalog', cat,
                '--volatile', 'sbos-state=1048576', *args], check=True, stdout=subprocess.DEVNULL)
c = open(img, 'rb').read()
data_at = struct.unpack_from('<Q', c, 4096 + 80)[0]
p2 = os.path.join(s1, 'dev/block/mmcblk0p2')
put(p2, 0, c[:7 * 4096]); put(p2, data_at * 4096, c[7 * 4096:])
# S9 #2's step-A store (catalog-a.toml: Starbite OS only), on its sd64l p2, with the phone's marks in the volatile entries
a = '[[entry]]\nkey = "starbite-next-gpu"\nname = "Starbite OS Next (GPU)"\nversion = "test"\ncategory = "starbite"\n' \
    'method = "bare"\ntag = "experimental"\nfiles = ["ufs:starbite-next-gpu.img|sd:starbite-next-gpu.img"]\nroot = "none (RAM only)"\n'
open(os.path.join(w, 'catalog-a.toml'), 'w').write(a)
img2 = os.path.join(ind, 's2-store.img')
subprocess.run([sys.executable, sb, 'build', img2, '--medium', 'sd64l', '--home', '--catalog', os.path.join(w, 'catalog-a.toml'),
                '--volatile', 'sbos-state=1048576', f'starbite-next-gpu.img={ind}/starbite-next-gpu.img:image'], check=True,
               stdout=subprocess.DEVNULL)
b2 = bytearray(open(img2, 'rb').read())
n = struct.unpack_from('<8sII', b2, 4096)[2]
for i in range(n):
    name, blk, size, crc, typ, fl = struct.unpack_from('<32sQQIHH', b2, 3 * 4096 + 64 * i)
    if fl & 1:
        m = b'PHONE WROTE ' + name.rstrip(b'\0'); b2[blk * 4096:blk * 4096 + len(m)] = m
put(os.path.join(s2, 'dev/block/mmcblk0p2'), 0, bytes(b2))
# S9 #2's real stock GPT has no backup copy (the last 5 blocks are zeros): the fake gets the same
sda = os.path.join(s2, 'dev/block/sda')
with open(sda, 'r+b') as f:
    f.seek(-5 * 4096, 2); f.write(bytes(5 * 4096))
PY
S1H=$(tree_hash "$W/s1")
prot2() { cat "$W/s2/dev/block/by-name/"{EFS,PARAM,KEYSTORAGE,PERSISTENT,STEADY,RECOVERY,BOOT,DTB,DTBO,BOTA0,BOTA1,UP_PARAM} "$W/s2/dev/block/"sd{b,c,d,e} | shasum -a 256; }
P2=$(prot2)
NIGHT=(bash tools/installer/night-s9b.sh S9ONE01 S9TWO02 --rehearse "$W/s1" "$W/s2")

echo; echo "######## NIGHT_ONLY=\"P E F\" first (the card-only steps; C waits): the GPT stays stock"
GPT0=$(python3 -c "import sys, hashlib; f=open(sys.argv[1],'rb'); h=f.read(6*4096); f.seek(-5*4096,2); print(hashlib.sha256(h+f.read()).hexdigest())" "$W/s2/dev/block/sda")
NIGHT_ONLY="P E F" "${NIGHT[@]}" > "$W/run0.txt" 2>&1 && ok "P E F ran (exit 0)" || { bad "P E F"; tail -20 "$W/night.log"; }
grep -q '^### step E done' "$W/night.log" && grep -q '^### step F done' "$W/night.log" && ! grep -q '^### step C ' "$W/night.log" &&
	grep -q '^### step ONLY(P E F) done' "$W/night.log" && ok "E and F done, C not touched, the ONLY marker" || bad "ONLY markers"
[ "$(python3 -c "import sys, hashlib; f=open(sys.argv[1],'rb'); h=f.read(6*4096); f.seek(-5*4096,2); print(hashlib.sha256(h+f.read()).hexdigest())" "$W/s2/dev/block/sda")" = "$GPT0" ] &&
	ok "S9 #2's GPT untouched by P E F" || bad "the GPT changed"
echo; echo "######## the night, with a cable pull in step G"
FAKE_FAIL='sbsc.chunk of=/dev/block/sda27' FAKE_FAIL_TIMES=1 "${NIGHT[@]}" > "$W/run1.txt" 2>&1
rc=$?; rm -f "$W/s2/fail-count"
[ $rc != 0 ] && ok "the night stopped (exit $rc)" || bad "the night did not stop at the pull"
grep -q '^### step G failed' "$W/night.log" && ok "### step G failed in the log" || { bad "no G failed marker"; tail -20 "$W/night.log"; }
for s in P C D E F; do grep -q "^### step $s done" "$W/night.log" && ok "### step $s done" || bad "step $s not done"; done
NBW=$W/state/night-s9b
# fix 3 (review 515c6928): an interruption between D-store's done marker and its catalog copy must not lose D's rows
# (the last store step before G here: E and F ran first, then C and D)
rm -f "$NBW/done-D"; cp "$NBW/catalog-F-store.toml" "$NBW/catalog-cur.toml"
grep -q 'key = "android"' "$NBW/catalog-cur.toml" && bad "the simulated lost copy did not take" || ok "simulated: D-store done, but its catalog copy lost"
echo; echo "######## the same command again: resumes"
"${NIGHT[@]}" > "$W/run2.txt" 2>&1 && ok "the second run finished (exit 0)" || { bad "second run"; tail -25 "$W/night.log"; }
grep -q 'D-system: done earlier' "$W/night.log" || grep -q '^### step G done' "$W/night.log" && ok "...skipping what was done" || bad "no skip lines"
for s in G H ALL; do grep -q "^### step $s done" "$W/night.log" && ok "### step $s done" || bad "step $s not done"; done
grep -q 'D-store: done earlier' "$W/night.log" && grep -q 'key = "android"' "$NBW/catalog-cur.toml" && ok "...D's catalog copy redone; G and H's store updates kept One UI's row" || bad "lost catalog copy"
# fix 1: the _tools.img probe answers yes / no explicitly; anything else (here: no /external_sd/distros) is neither
probe='if [ -f /external_sd/distros/_tools.img ]; then echo yes; elif [ -d /external_sd/distros ]; then echo no; fi'
grep -qF "$probe" tools/installer/night-s9b.sh && ok "night-s9b.sh uses the explicit probe" || bad "probe text"
a1=$(env PATH="$REPO/tools/installer/fakes:$PATH" FAKE_ROOT="$W/s1" FAKE_SERIAL=S9ONE01 adb -s S9ONE01 shell "$probe" | tr -d '\r')
mv "$W/s1/external_sd/distros/_tools.img" "$W/s1/_tools.keep"
a2=$(env PATH="$REPO/tools/installer/fakes:$PATH" FAKE_ROOT="$W/s1" FAKE_SERIAL=S9ONE01 adb -s S9ONE01 shell "$probe" | tr -d '\r')
mv "$W/s1/external_sd/distros" "$W/s1/distros.keep"
a3=$(env PATH="$REPO/tools/installer/fakes:$PATH" FAKE_ROOT="$W/s1" FAKE_SERIAL=S9ONE01 adb -s S9ONE01 shell "$probe" | tr -d '\r')
mv "$W/s1/distros.keep" "$W/s1/external_sd/distros"; mv "$W/s1/_tools.keep" "$W/s1/external_sd/distros/_tools.img"
[ "$a1/$a2/$a3" = "yes/no/" ] && ok "the probe: yes / no / nothing (the night dies on nothing)" || bad "probe answers $a1/$a2/$a3"
grep -q "\*) die \"could not tell whether S9 #1 has" tools/installer/night-s9b.sh && ok "...and anything but yes/no ends the night" || bad "no die branch"
# fix 2: --no-reboot: a table the kernel does not re-read in place stops the run instead of restarting S9 #2
python3 - <<'PY' && ok "repart: --no-reboot + no in-place re-read -> STOPPED, no adb reboot" || bad "repart no_reboot"
import os, sys, types
sys.path.insert(0, 'tools/installer')
from sbinst import repart
from sbinst.common import Stopped
os.environ['SBINST_ENV_UP'] = '1'
calls = []
r = repart.Repart.__new__(repart.Repart)
r.ui = types.SimpleNamespace(info=lambda *a: None, ok=lambda *a: None, cmd=lambda *a: None)
r.phone = types.SimpleNamespace(sh=lambda *a, **k: '', facts=lambda: None, adb=lambda *a, **k: calls.append(a),
                                wait_for=lambda *a: types.SimpleNamespace(kind='none'))
r.i = types.SimpleNamespace(o=types.SimpleNamespace(no_reboot=True), facts=None)
r.layout = types.SimpleNamespace(name='starbite-oneui')
r.kernel_diff = lambda p, f: ['A16DATA: not as planned']
try:
    r.reboot_and_check(types.SimpleNamespace(entries=[]))
    sys.exit('no Stopped')
except Stopped as e:
    assert 'not restarting' in str(e), e
assert not calls, calls
PY
"${NIGHT[@]}" > "$W/run3.txt" 2>&1 && ! grep -q 'stream to\|update store on' "$W/run3.txt" && ok "a third run does nothing" || bad "third run"

echo; echo "######## S9 #2 afterwards"
python3 - "$W" <<'PY' && ok "S9 #2's GPT is S9 #1's starbite-oneui table" || bad "GPT"
import sys
sys.path.insert(0, 'tools/installer')
from sbinst import gpt, ufslayout
p = sys.argv[1] + '/s2/dev/block/sda'
with open(p, 'rb') as f:
    head = f.read(6 * 4096); f.seek(-5 * 4096, 2); tail = f.read()
d = gpt.read(head, tail, 15616000)
assert d.valid, d.problems
assert not [r for r in gpt.plan(d, ufslayout.get('starbite-oneui')).rows if r.status not in ('same',)], 'differs'
PY
python3 - "$W" <<'PY' && ok "SYSTEM, VENDOR, ODM, WINESP, WINDOWS, USERDATA, the Sequoia slot, the distros = S9 #1's bytes (as far as copied)" || bad "copies"
import os, sys
w, cap = sys.argv[1], 192 << 20
def node(r, n): return os.path.realpath(os.path.join(w, r, 'dev/block/by-name', n))
def rd(p, off, n):
    with open(p, 'rb') as f: f.seek(off); return f.read(n)
for n in ('SYSTEM', 'VENDOR', 'WINESP', 'WINDOWS', 'USERDATA'):
    assert rd(node('s1', n), 0, cap) == rd(node('s2', n), 0, cap), n
assert rd(node('s1', 'ODM'), 0, 1 << 40) == rd(node('s2', 'ODM'), 0, 1 << 40)
assert rd(w + '/s1/dev/block/mmcblk0', 182888448 * 512, cap) == rd(w + '/s2/dev/block/mmcblk0', 2105344 * 512, cap)
for f in ('chromeos', 'ubuntu', 'pmos', '_tools'):
    assert open(f'{w}/s1/external_sd/distros/{f}.img', 'rb').read() == open(f'{w}/s2/external_sd/distros/{f}.img', 'rb').read(), f
PY
grep -q 'oneui-data: verified on the phone.*TYPE ext4 LABEL data' "$W/state/night-s9b/logs/C-gpt.txt" && grep -q 'TYPE="ext4" LABEL="data"' "$W/s2/blkid/sda28" && ok "A16DATA made in step C (mke2fs ... -L data; blkid: ext4 data)" || bad "A16DATA"
python3 - "$W" <<'PY' && ok "S9 #2's card store: every row and entry, the Sequoia payload at 2105344, the phone's volatile bytes, every CRC" || bad "card store"
import importlib.util, struct, sys, tempfile
w = sys.argv[1]
spec = importlib.util.spec_from_file_location('sb', 'tools/store/sbstore2.py'); sb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sb)
sys.path.insert(0, 'tools/store'); import cardlayout
d = open(w + '/s2/dev/block/mmcblk0p2', 'rb').read(1100 << 20)
used = struct.unpack_from('<8sIIQQ', d, 4096)[4]
with tempfile.NamedTemporaryFile(suffix='.img') as t:
    t.write(d[:used * 4096]); t.flush()
    e = {n: (b, fl) for n, _, b, fl in sb.read_store(t.name, with_flags=True)}
    keys = set(sb.catalog_keys(e['catalog'][0]))
need = {'starbite-next-gpu', 'android', 'chromeos', 'linux-ubuntu', 'linux-pmos', 'linux-shell', 'mac15', 'win11', 'win11-big', 'ios27'}
assert need <= keys, need - keys
for n in ('android.kernel', 'android.cmdline', 'linux.image', 'windows-big3i.uefi', 'ios27-refport-t8.xnu', 'ios.overlay'):
    assert n in e, n
mac = [n for n in e if n.startswith('xnu-macos15')][0]
assert cardlayout.sdroot_of(cardlayout.payload_cmdline(e[mac][0])) == (2105344, 33554432)
assert e['sbos-state'][0].startswith(b'PHONE WROTE sbos-state') and e['crashlog'][0].startswith(b'PHONE WROTE crashlog')
PY
[ "$(tree_hash "$W/s1")" = "$S1H" ] && ok "S9 #1 byte-identical (only read)" || bad "S9 #1 changed"
[ "$(prot2)" = "$P2" ] && ok "S9 #2's RECOVERY, BOOT, EFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, DTB, DTBO, BOTA, UP_PARAM, sdb-sde unchanged" || bad "a protected area changed"
[ "$(cat "$W/s2/state")" = env ] && ok "S9 #2 still in its installer environment (no reboot)" || bad "S9 #2 state $(cat "$W/s2/state")"
echo; echo "--- the night log (rehearsal) ---"; grep -E '^###' "$W/night.log"
echo; echo "######## result: $pass passed, $fail failed"
[ "${KEEP:-0}" = 1 ] || [ "$fail" != 0 ] || rm -rf "$W"
[ "$fail" = 0 ]
