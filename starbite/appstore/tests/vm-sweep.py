#!/usr/bin/env python3
"""Every method of every recipe, checked for real in the trixie arm64 root without installing anything
(tools/vm-chroot.sh fresh root; network): distro -> `apt-get -s install` resolves; source (debian) -> `apt-get -s
build-dep` resolves and `apt-get source --print-uris` finds it; source (upstream) -> the tarball URL answers (HEAD);
release -> the asset resolves and its URL answers (HEAD), the checksum source is found.
    vm-chroot.sh run python3 -I tests/vm-sweep.py CATALOG_DIR [ID...]"""
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import sbstore  # noqa: E402

os.environ.setdefault("SBSTORE_REPO", "/nonexistent")
cat = sbstore.load_catalog(os.path.abspath(sys.argv[1]))
ids = sys.argv[2:] or sorted(cat)
src = "/etc/apt/sources.list.d/sweep-src.sources"
with open(src, "w") as f:
    f.write(sbstore.DEB_SRC)
subprocess.run(["apt-get", "-q", "update"], capture_output=True)
res = {"PASS": 0, "FAIL": 0}


def say(ok, msg):
    res["PASS" if ok else "FAIL"] += 1
    print("%s %s" % ("PASS" if ok else "FAIL", msg), flush=True)


def head(url):
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": sbstore.C.ua})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, int(r.headers.get("Content-Length") or 0)
    except urllib.error.HTTPError as e:
        return e.code, 0
    except Exception as e:  # noqa: BLE001
        return str(e)[:60], 0


for aid in ids:
    a = cat[aid]
    for m in sbstore.app_methods(a):
        try:
            if m == "distro":
                pk = a["distro"]["apt"] + (a.get("runtime") or {}).get("apt", [])
                r = subprocess.run(["apt-get", "-s", "-q", "install", "--no-install-recommends"] + a["distro"]["apt"],
                                   capture_output=True, text=True)
                n = r.stdout.count("\nInst ")
                say(r.returncode == 0, "%s distro: %s (%d packages to install)" % (
                    aid, " ".join(a["distro"]["apt"]), n) + ("" if r.returncode == 0 else " " + r.stdout[-200:] + r.stderr[-200:]))
            elif m == "source" and a["source"].get("system") == "debian":
                pkg = a["source"].get("package") or a["distro"]["apt"][0]
                r = subprocess.run(["apt-get", "-s", "-q", "build-dep", pkg], capture_output=True, text=True)
                n = r.stdout.count("\nInst ")
                r2 = subprocess.run(["apt-get", "-q", "source", "--print-uris", pkg], capture_output=True, text=True)
                dsc = [l for l in r2.stdout.splitlines() if ".dsc'" in l]
                ok = r.returncode == 0 and r2.returncode == 0 and bool(dsc)
                say(ok, "%s source (Debian): build-dep %d packages, %s" % (
                    aid, n, dsc[0].split()[1] if dsc else "NO SOURCE " + r2.stderr[-150:] + r.stderr[-150:]))
            elif m == "source":
                rel = sbstore.resolve(a, "source")
                url = rel.get("tarball") or a["source"].get("git")
                st, n = head(url) if rel.get("tarball") else ("git", 0)
                deps = a["source"].get("build_deps") or []
                r = subprocess.run(["apt-get", "-s", "-q", "install", "--no-install-recommends"] + deps,
                                   capture_output=True, text=True) if deps else None
                ok = st in (200, "git") and (r is None or r.returncode == 0)
                say(ok, "%s source %s: %s -> %s, build deps %s" % (aid, rel["version"], url, st,
                                                                 "ok" if r is None or r.returncode == 0 else r.stdout[-150:]))
            elif m == "release":
                rel = sbstore.resolve(a, "release")
                pick = sbstore.pick_asset(rel, a["release"])
                if not pick:
                    say(False, "%s release %s: no asset" % (aid, rel["version"]))
                    continue
                st, n = head(pick[0]["url"])
                sha, how = sbstore.expected_sha(dict(rel, id=aid), pick[0], a["release"].get("verify", {}), rel["version"])
                deps = (a.get("runtime") or {}).get("apt") or []
                r = subprocess.run(["apt-get", "-s", "-q", "install", "--no-install-recommends"] + deps,
                                   capture_output=True, text=True) if deps else None
                ok = st == 200 and (r is None or r.returncode == 0)
                say(ok, "%s release %s: %s (%s, %.1f MB) %s; runtime deps %s" % (
                    aid, rel["version"], pick[0]["name"], st, (pick[0].get("size") or n) / 1048576, how,
                    "ok" if r is None or r.returncode == 0 else "FAIL " + r.stdout[-150:]))
        except sbstore.StoreError as e:
            say(False, "%s %s: %s" % (aid, m, e.detail))
os.unlink(src)
print("RESULT pass=%d fail=%d" % (res["PASS"], res["FAIL"]))
