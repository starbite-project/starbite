#!/usr/bin/env python3
"""The store daemon in a guest-like root (run inside tools/vm-chroot.sh's trixie root, real apt and network):
a plain file as /dev/sbshm, a fake sbwl on a unix socket sending the shell's op lines. PASS / FAIL lines.
    vm-chroot.sh run python3 -I tests/vm-daemon-test.py CATALOG_DIR [ID:METHOD ...]"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import sbstore  # noqa: E402

cat = sys.argv[1] if sys.argv[1].endswith(".json") else os.path.abspath(sys.argv[1])
jobs = sys.argv[2:] or ["neovim:release", "gnome-mines:distro", "fzf:release"]
d = tempfile.mkdtemp()
shm = os.path.join(d, "shm")
with open(shm, "wb") as f:
    f.truncate(sbstore.SBSHM_BLK_OFF)
sockp = os.path.join(d, "sbwl.sock")
srv = socket.socket(socket.AF_UNIX)
srv.bind(sockp)
srv.listen(1)
env = dict(os.environ, SBSTORE_SHM=shm, SBSTORE_SOCK=sockp, SBSTORE_CATALOG=cat)
env.setdefault("SBSTORE_MIRROR", "/nonexistent")
env.setdefault("SBSTORE_REPO", "/nonexistent")
daemon = os.environ.get("SBSTORE_BIN") or os.path.join(HERE, "..", "sbstore.py")
p = subprocess.Popen([sys.executable, "-I", daemon, "daemon"], env=env,
                     stderr=open(os.path.join(d, "daemon.log"), "w"))
srv.settimeout(60)
c, _ = srv.accept()
c.settimeout(5)
buf = b""
fails = 0


def lines(until, t):
    global buf
    end = time.time() + t
    seen = []
    while time.time() < end:
        try:
            b = c.recv(4096)
            buf += b
        except socket.timeout:
            pass
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            l = l.decode()
            seen.append(l)
            if until(l):
                return l, seen
    return None, seen


def table():
    with open(shm, "rb") as f:
        f.seek(sbstore.SBSHM_STORE_OFF)
        hdr = struct.unpack("<16I", f.read(64))
        pk = [struct.unpack(sbstore.PKG_FMT, f.read(248)) for _ in range(hdr[1])]
        f.seek(sbstore.SBSHM_STORE3_OFF)
        h3 = struct.unpack("<16I", f.read(64))
        p3 = [struct.unpack(sbstore.PKG3_FMT, f.read(256)) for _ in range(h3[2])]
    return hdr, pk, h3, p3


def say(ok, msg):
    global fails
    fails += 0 if ok else 1
    print(("PASS " if ok else "FAIL ") + msg, flush=True)


l, _ = lines(lambda l: l.startswith("store "), 120)
say(l is not None, "daemon published the table (%s)" % l)
seq = int(l.split()[1]) if l else 0
hdr, pk, h3, p3 = table()
names = [x[0].rstrip(b"\0").decode() for x in pk]
say(hdr[1] == len(names) and h3[0] == sbstore.STORE3_MAGIC, "%d entries, store3 magic ok, net_ok=%d" % (hdr[1], h3[3]))
for job in jobs:
    aid, m = job.split(":")
    if aid not in names:
        say(False, "%s not in the table" % aid)
        continue
    i = names.index(aid)
    t0 = time.time()
    c.sendall(b"op %d %d %d\n" % (i, 1 | (sbstore.M_NUM[m] << 8), seq))
    l, seen = lines(lambda l: l.startswith("pkg %d 0 100" % i) or l.startswith("pkg %d 4" % i), 1800)
    phases = sorted({s.split()[2] for s in seen if s.startswith("pkg %d " % i)})
    time.sleep(1)
    hdr, pk, h3, p3 = table()
    ok = l is not None and l.startswith("pkg %d 0 100" % i) and pk[i][8] & sbstore.F_INSTALLED
    say(ok, "%s by %s in %.0f s (states seen %s; installed %s, ver %s, err %r)" % (
        aid, m, time.time() - t0, phases, bool(pk[i][8] & 1), p3[i][16].rstrip(b"\0").decode(),
        pk[i][4].rstrip(b"\0").decode()))
    say("rescan" in seen, "%s: rescan sent" % aid)
    c.sendall(b"op %d 2 %d\n" % (i, seq))
    l, seen = lines(lambda l: l.startswith("pkg %d 0 100" % i) or l.startswith("pkg %d 4" % i), 600)
    time.sleep(1)
    hdr, pk, h3, p3 = table()
    say(l is not None and not pk[i][8] & 1, "%s removed" % aid)
p.terminate()
p.wait(10)
print(open(os.path.join(d, "daemon.log")).read()[-1500:])
print("RESULT %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
