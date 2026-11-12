#!/usr/bin/python3
"""A stand-in for sbwl's side of /run/sbwl.sock, and the checks of test-sbpkg.sh.

    fake-sbwl.py SOCKET SHMFILE ROOT

It listens on SOCKET (sbwl is the server), reads the store table from SHMFILE (the file sbpkg maps instead of
/dev/sbshm) with the seqlock rules of sbshm.h, sends "op IDX OP SEQ" lines like sbwl does for SBG_MSG_PKG, and
checks the "pkg"/"store"/"rescan" lines, the table's flags and the files under ROOT (the chroot)."""
import os
import socket
import struct
import sys
import time

SOCK, SHM, ROOT = sys.argv[1:4]
STORE_OFF, ICON_OFF, ICON_BYTES, ICON_FIRST = 0x1020000, 0x1040000, 160 * 160 * 4, 64
PKG_FMT = "<40s32s96s16s32s8I"           # struct sbshm_pkg, 248 bytes
assert struct.calcsize(PKG_FMT) == 248
F = {"INSTALLED": 1, "SYSTEM": 2, "TOO_BIG": 4, "NO_SPACE": 8, "FEATURED": 16}
ST = ["IDLE", "QUEUED", "INSTALLING", "REMOVING", "FAILED"]
OP_INSTALL, OP_REMOVE, OP_CANCEL = 1, 2, 3
fails = []


def check(ok, what):
    print(f"test: {'PASS' if ok else 'FAIL'} {what}", flush=True)
    if not ok:
        fails.append(what)


def table():
    """a whole copy under the seqlock"""
    with open(SHM, "rb") as f:
        for _ in range(1000):
            f.seek(STORE_OFF)
            s1 = struct.unpack("<I", f.read(4))[0]
            if s1 & 1:
                time.sleep(0.005)
                continue
            f.seek(STORE_OFF)
            raw = f.read(64 + 160 * 248)
            f.seek(STORE_OFF)
            s2 = struct.unpack("<I", f.read(4))[0]
            if s1 == s2:
                break
        seq, n, tot, free, repo_ok = struct.unpack("<5I", raw[:20])
        pk = []
        for i in range(n):
            v = struct.unpack(PKG_FMT, raw[64 + i * 248: 64 + (i + 1) * 248])
            s = lambda b: b.split(b"\0", 1)[0].decode()
            pk.append(dict(i=i, pkg=s(v[0]), name=s(v[1]), summary=s(v[2]), cat=s(v[3]), err=s(v[4]), dl_kb=v[5],
                           inst_kb=v[6], ram_mb=v[7], flags=v[8], state=v[9], pct=v[10], icon=v[11]))
        return dict(seq=seq, n=n, total_kb=tot, free_kb=free, repo_ok=repo_ok, pkg=pk)


def flags(p):
    return "|".join(k for k, b in F.items() if p["flags"] & b) or "-"


def icon_nonzero(idx):
    with open(SHM, "rb") as f:
        f.seek(ICON_OFF + idx * ICON_BYTES)
        b = f.read(ICON_BYTES)
    return sum(1 for k in range(3, len(b), 4) if b[k]) if b else 0


class Conn:
    def __init__(self, c):
        self.c, self.buf, self.lines, self.all = c, b"", [], []

    def pump(self, timeout):
        self.c.settimeout(timeout)
        try:
            d = self.c.recv(4096)
        except socket.timeout:
            return
        if not d:
            raise SystemExit("test: FAIL sbpkg hung up")
        self.buf += d
        while b"\n" in self.buf:
            l, self.buf = self.buf.split(b"\n", 1)
            l = l.decode()
            print(f"  <- {l}", flush=True)
            self.lines.append(l)
            self.all.append(l)

    def wait(self, pred, secs):
        t = time.time() + secs
        while time.time() < t:
            for i, l in enumerate(self.lines):
                if pred(l):
                    del self.lines[: i + 1]
                    return l
            self.pump(0.2)
        return None

    def send(self, l):
        print(f"  -> {l}", flush=True)
        self.c.sendall((l + "\n").encode())


def run_op(c, key, op, secs, expect_state="IDLE"):
    t = table()
    p = next(x for x in t["pkg"] if x["pkg"] == key)
    c.lines.clear()
    t0 = time.time()
    first = len(c.all)
    c.send(f"op {p['i']} {op} {t['seq']}")
    done = c.wait(lambda l: l.startswith(f"pkg {p['i']} ") and l.split()[2] in ("0", "4") and
                  (l.split()[2] == "4" or l.split()[3] == "100"), secs)
    dt = time.time() - t0
    seen = [l for l in c.all[first:] if l.startswith(f"pkg {p['i']} ")]
    states = [ST[int(l.split()[2])] + ":" + l.split()[3] for l in seen]
    print(f"test: {key} op {op}: {len(seen)} pkg lines in {dt:.1f} s: {' '.join(states[:6])}"
          f"{' ... ' + ' '.join(states[-3:]) if len(states) > 9 else ' '.join(states[6:])}", flush=True)
    end = done and ST[int(done.split()[2])]
    return end, seen, dt, table()["pkg"][p["i"]]


def main():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK)
    os.chmod(SOCK, 0o666)
    srv.listen(1)
    srv.settimeout(60)
    print("test: fake sbwl listening on", SOCK, flush=True)
    c = Conn(srv.accept()[0])
    first = c.wait(lambda l: l.startswith("store "), 120)
    check(first is not None, f"sbpkg connected and sent '{first}'")
    time.sleep(1)                                       # the start-up repair / apt-get update publish
    c.pump(0.5)
    t = table()
    check(t["seq"] % 2 == 0 and t["seq"] > 0, f"store seq {t['seq']} even")
    check(t["repo_ok"] == 1 and t["n"] > 10, f"repo_ok {t['repo_ok']}, {t['n']} entries")
    print(f"test: overlay {t['free_kb'] // 1024} MB free of {t['total_kb'] // 1024} MB", flush=True)
    for p in t["pkg"]:
        print(f"test:   [{p['i']:2d}] {p['pkg']:20s} {p['cat']:10s} dl {p['dl_kb'] // 1024:4d} MB inst "
              f"{p['inst_kb'] // 1024:4d} MB ram {p['ram_mb']:4d} MB icon {p['icon']} ({icon_nonzero(p['icon'])} px) "
              f"{flags(p)}", flush=True)
    by = {p["pkg"]: p for p in t["pkg"]}
    check(by["papers"]["flags"] & F["SYSTEM"] and by["papers"]["flags"] & F["INSTALLED"], "papers (base) SYSTEM|INSTALLED")
    check(not by["firefox-esr"]["flags"] & (F["INSTALLED"] | F["SYSTEM"]), "firefox-esr not installed")
    check(all(icon_nonzero(p["icon"]) > 1000 for p in t["pkg"] if p["icon"] != 0xFFFFFFFF),
          "every icon in the pool has pixels")
    big = [p["pkg"] for p in t["pkg"] if p["flags"] & F["TOO_BIG"]]
    print(f"test: TOO_BIG: {big}", flush=True)

    # ---- install firefox-esr ----
    free0 = t["free_kb"]
    c.lines.clear()
    end, seen, dt, p = run_op(c, "firefox-esr", OP_INSTALL, 900)
    check(end == "IDLE" and p["flags"] & F["INSTALLED"], f"firefox-esr installed in {dt:.0f} s, flags {flags(p)}")
    pcts = [int(l.split()[3]) for l in seen if l.split()[2] == "2"]
    check(len(pcts) >= 3 and pcts == sorted(pcts), f"INSTALLING progress rises: {pcts[:3]}..{pcts[-2:]}")
    check(c.wait(lambda l: l == "rescan", 5) is not None, "rescan sent after the install")
    desk = os.path.join(ROOT, "usr/share/applications/firefox-esr.desktop")
    check(os.path.exists(desk), "usr/share/applications/firefox-esr.desktop is there")
    amap = open(os.path.join(ROOT, "run/sbpkg/apps.tsv")).read()
    check("\tfirefox-esr\tfirefox-esr" in amap, "run/sbpkg/apps.tsv maps firefox-esr.desktop to its entry")
    free1 = table()["free_kb"]
    print(f"test: overlay free {free0 // 1024} -> {free1 // 1024} MB (inst_kb {p['inst_kb'] // 1024} MB)", flush=True)
    check(free0 - free1 > p["inst_kb"] * 0.6, "the install took overlay space")

    # ---- the queue: two installs, the second cancelled while queued; a stale op ----
    t = table()
    a, b = by["gnome-sudoku"]["i"], by["gnome-mines"]["i"]
    c.lines.clear()
    c.send(f"op {a} {OP_INSTALL} {t['seq']}")
    c.send(f"op {b} {OP_INSTALL} {t['seq']}")
    q = c.wait(lambda l: l.startswith(f"pkg {b} 1 "), 10)
    check(q is not None, "the second install is QUEUED")
    c.send(f"op {b} {OP_CANCEL} {table()['seq']}")
    cn = c.wait(lambda l: l.startswith(f"pkg {b} 0 "), 10)
    check(cn is not None, "cancel of the queued install -> IDLE")
    d = c.wait(lambda l: l.startswith(f"pkg {a} 0 100") or l.startswith(f"pkg {a} 4 "), 300)
    t = table()
    check(d is not None and t["pkg"][a]["flags"] & F["INSTALLED"] and not t["pkg"][b]["flags"] & F["INSTALLED"],
          "gnome-sudoku installed, gnome-mines not")
    c.lines.clear()
    c.send(f"op {a} {OP_REMOVE} 0")
    c.pump(2)
    check(not any(l.startswith(f"pkg {a} ") for l in c.lines), "an op with a stale seq is ignored")
    end, seen, dt, p = run_op(c, "gnome-sudoku", OP_REMOVE, 300)
    check(end == "IDLE" and not p["flags"] & F["INSTALLED"], f"gnome-sudoku removed in {dt:.0f} s")

    # ---- a system app cannot be removed ----
    end, seen, dt, p = run_op(c, "papers", OP_REMOVE, 30)
    check(end == "FAILED" and p["err"], f"remove papers refused: FAILED '{p['err']}'")

    # ---- remove firefox-esr: the space comes back ----
    end, seen, dt, p = run_op(c, "firefox-esr", OP_REMOVE, 600)
    check(end == "IDLE" and not p["flags"] & F["INSTALLED"], f"firefox-esr removed in {dt:.0f} s")
    check(not os.path.exists(desk), "firefox-esr.desktop is gone")
    free2 = table()["free_kb"]
    print(f"test: overlay free after remove {free2 // 1024} MB (before install {free0 // 1024} MB)", flush=True)
    check(free2 >= free0 - 16 * 1024, "the space came back (within 16 MB: apt lists, dpkg's database)")

    # ---- optional: more entries, install + remove each (TEST_EXTRA="libreoffice-writer gimp") ----
    for key in os.environ.get("TEST_EXTRA", "").split():
        f0 = table()["free_kb"]
        end, seen, dt, p = run_op(c, key, OP_INSTALL, 1200)
        check(end == "IDLE" and p["flags"] & F["INSTALLED"], f"{key} installed in {dt:.0f} s")
        apps = [l.split("\t")[2].strip() for l in open(os.path.join(ROOT, "run/sbpkg/apps.tsv")) if l.split("\t")[1] == key]
        check(bool(apps), f"{key}: .desktop files {apps}")
        end, seen, dt, p = run_op(c, key, OP_REMOVE, 600)
        check(end == "IDLE" and not p["flags"] & F["INSTALLED"] and table()["free_kb"] >= f0 - 16 * 1024,
              f"{key} removed in {dt:.0f} s, space back")

    print(f"test: {'ALL PASS' if not fails else 'FAILED: ' + '; '.join(fails)}", flush=True)
    sys.exit(1 if fails else 0)


main()
