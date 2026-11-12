#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""sbstore: the Starbite App Store engine (runs in the Linux guest on the M3 cores; Python 3.11+, stdlib only).

Every app in the catalog (catalog.json, built from catalog/*.toml by tools/catalog.py) installs by one of up to
three methods, the user's pick per app:
  release  the upstream project's latest aarch64 release (GitHub / GitLab / a vendor URL), checksum-verified
  source   the latest release's source, compiled on the M3s (build deps from apt)
  distro   the Debian package (apt; the card's offline repo or the network)
and can be updated (side by side, one version back kept), rolled back, and removed.

Commands (also see `sbstore help`):
  sbstore list | info ID | resolve ID [METHOD] | install ID [--method M] | update ID|--all | rollback ID |
          remove ID | check | status [--json] | log ID | daemon
Machine use: `--json` makes install/update/rollback/remove print one JSON object per line
({"ev":"progress","phase":..,"pct":..,"done_kb":..,"total_kb":..} ... {"ev":"done"} | {"ev":"error","msg":..}).

`daemon` replaces sbpkg (docs/STARBITE-LINUX-APPS.md L6) when it runs: the same /run/sbwl.sock lines ("op IDX OP
SEQ" in; "store SEQ", "pkg IDX STATE PCT", "rescan" out), the same struct sbshm_store + the store icons (pool
64..159) under the store seqlock, plus the v3 table (struct sbshm_store3, starbite/appstore/sbshm_store3.h).

Environment (tests and the host use these; the guest needs none):
  SBSTORE_ROOT (/)            every path below is under it
  SBSTORE_CATALOG             catalog.json, or a directory of *.toml (default <root>/usr/share/starbite/appstore/catalog.json)
  SBSTORE_MIRROR (/srv/appstore:/repo/appstore/mirror:/usr/share/starbite/appstore/mirror)   the offline mirror
                              (tools/mirror.py; on the phone the card file appstore.erofs at /srv/appstore),
                              used when the network is down; its debs/ is an apt source (build deps offline)
  SBSTORE_REPO (/repo)        the offline Debian repo of the old store (its store/index.tsv entries join the catalog)
  SBSTORE_OFFLINE=1           never use the network
  SBSTORE_NET_PROBE (deb.debian.org:80,api.github.com:443)   up = a default route, a DNS answer and one TCP connect
  SBSTORE_PROC_ROUTE (/proc/net/route), SBSTORE_PROC_ROUTE6: where the default route is looked for (tests)
  SBSTORE_GITHUB_API (https://api.github.com), SBSTORE_GITLAB_API_<HOST> (https://<host>/api/v4)
  SBSTORE_APT (apt-get), SBSTORE_DPKG (dpkg), SBSTORE_APT_CACHE (apt-cache), SBSTORE_DPKG_QUERY (dpkg-query), SBSTORE_DPKG_DEB (dpkg-deb)
  SBSTORE_SHM (/dev/sbshm), SBSTORE_SOCK (/run/sbwl.sock), SBSTORE_RESERVE_MB (64), SBSTORE_JOBS (nproc)
  SBSTORE_BASE_LIST (/usr/share/starbite/base-packages.txt): packages a removal must never take
  SBSTORE_KEEP_BUILD_DEPS=1   keep the build dependencies after a source build
"""
import errno
import fcntl
import fnmatch
import hashlib
import json
import mmap
import os
import re
import shlex
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

VERSION = "1.1"
METHODS = ("distro", "release", "source")          # method numbers 1, 2, 3 (SBG_PKG_M_*)
M_NUM = {"distro": 1, "release": 2, "source": 3}
M_NAME = {v: k for k, v in M_NUM.items()}


# ==== configuration ============================================================================================
def env(n, d=None):
    v = os.environ.get(n)
    return v if v not in (None, "") else d


ROOT = env("SBSTORE_ROOT", "/")


def rp(p):
    """a guest path under ROOT"""
    return os.path.join(ROOT, p.lstrip("/"))


class Cfg:
    def __init__(self):
        self.catalog = env("SBSTORE_CATALOG", rp("/usr/share/starbite/appstore/catalog.json"))
        self.state_dir = rp("/var/lib/sbstore")
        self.log_dir = rp("/var/log/sbstore")
        self.opt = rp("/opt/sbstore")
        self.bin = rp("/usr/local/bin")
        self.apps = rp("/usr/local/share/applications")
        self.icons = rp("/usr/local/share/icons/hicolor")
        # the offline mirror (tools/mirror.py): the first of these that exists
        self.mirrors = [m for m in env("SBSTORE_MIRROR", rp("/srv/appstore") + ":/repo/appstore/mirror:" +
                                       rp("/usr/share/starbite/appstore/mirror")).split(":") if m]
        self.mirror = next((m for m in self.mirrors if os.path.isdir(m)), self.mirrors[0])
        self.repo = env("SBSTORE_REPO", "/repo")
        self.offline = env("SBSTORE_OFFLINE") == "1"
        self.net_probe = env("SBSTORE_NET_PROBE", "deb.debian.org:80,api.github.com:443")
        self.proc_route = env("SBSTORE_PROC_ROUTE", "/proc/net/route")
        self.proc_route6 = env("SBSTORE_PROC_ROUTE6", "/proc/net/ipv6_route")
        self.dpkg = shlex.split(env("SBSTORE_DPKG", "dpkg"))
        self.github_api = env("SBSTORE_GITHUB_API", "https://api.github.com")
        self.apt = shlex.split(env("SBSTORE_APT", "apt-get"))
        self.apt_cache = shlex.split(env("SBSTORE_APT_CACHE", "apt-cache"))
        self.dpkg_query = shlex.split(env("SBSTORE_DPKG_QUERY", "dpkg-query"))
        self.dpkg_deb = shlex.split(env("SBSTORE_DPKG_DEB", "dpkg-deb"))
        self.apt_mark = shlex.split(env("SBSTORE_APT_MARK", "apt-mark"))
        self.shm = env("SBSTORE_SHM", "/dev/sbshm")
        self.sock = env("SBSTORE_SOCK", "/run/sbwl.sock")
        self.reserve_mb = int(env("SBSTORE_RESERVE_MB", "64"))
        self.jobs = int(env("SBSTORE_JOBS", str(os.cpu_count() or 4)))
        self.base_list = env("SBSTORE_BASE_LIST", rp("/usr/share/starbite/base-packages.txt"))
        self.keep_build_deps = env("SBSTORE_KEEP_BUILD_DEPS") == "1"
        self.ua = "sbstore/%s (Starbite OS; aarch64 Linux)" % VERSION


C = Cfg()


class StoreError(Exception):
    """an error the user sees: short plain words (the store table's err field is 31 bytes)"""

    def __init__(self, short, detail=""):
        super().__init__(short)
        self.short = short
        self.detail = detail or short


class NeedsNet(StoreError):
    pass


# what the user sees after a failure: the short (the table's 31-byte err) and one sentence on what to do. Keyed by the
# StoreError short; NeedsNet's sentence depends on why the network looks down (net_why()).
HINTS = {
    "needs the network": ("Needs the internet", None),
    "package not available": ("Not on the memory card", None),
    "not in the repo": ("Not on the memory card", None),
    "Linux clock not set": ("Linux's clock is wrong",
                            "Secure downloads can't be checked while the clock is wrong. Set the date and time in "
                            "Settings, then tap GET again."),
    "not enough space": ("Not enough space",
                         "The Linux space on the memory card is full. Remove an app you don't use, then try again."),
    "download failed": ("Download stopped",
                        "The download was cut off (weak Wi-Fi, or the phone slept). Tap GET again: it carries on "
                        "where it stopped."),
    "fetch failed": ("Download stopped",
                     "Getting the source code was cut off. Check Wi-Fi, then tap GET again."),
    "download damaged": ("Download was damaged",
                         "The file arrived damaged and was thrown away. Tap GET to download it again."),
    "checksum mismatch": ("Download failed its check",
                          "The file didn't match the project's published checksum, so it was not installed. "
                          "Try again later, or choose another install method."),
    "signature check failed": ("Download failed its check",
                               "The file's signature didn't match the project's key, so it was not installed. "
                               "Choose another install method."),
    "build failed": ("Compiling failed",
                     "The build stopped with an error. Choose Latest Release or Debian instead. Details: "
                     "/var/log/sbstore/{id}.log in the Terminal."),
    "install failed": ("Install failed",
                       "Debian's installer stopped with an error. Tap GET to try again: the store first repairs "
                       "any half-finished install. Details: /var/log/sbstore/{id}.log."),
    "remove failed": ("Remove failed",
                      "Debian's installer couldn't remove it. Try again; details: /var/log/sbstore/{id}.log."),
    "would remove system parts": ("Part of Linux itself",
                                  "Removing this would take parts Linux needs, so the store left it."),
    "store busy": ("Another install is running",
                   "Wait for the other install to finish, then try again."),
    "GitHub rate limit": ("GitHub is busy",
                          "GitHub refused too many requests for now. Try again in an hour, or choose Debian."),
    "not found upstream": ("Gone from the project's site",
                           "The project no longer offers this download. Choose another install method."),
    "bad answer upstream": ("Project site answered oddly",
                            "Try again in a few minutes, or choose another install method."),
    "no such method": ("Can't install that way",
                       "This app doesn't offer that install method. Choose another one."),
    "interrupted": ("Stopped part-way",
                    "The install was interrupted (Linux stopped or the phone restarted). Tap GET again."),
}
HINT_ANY = ("Something went wrong",
            "Tap GET to try again. If it keeps failing, the details are in /var/log/sbstore/{id}.log.")


def explain(e, aid=""):
    """(short for the table, one plain sentence on what to do)"""
    short, hint = HINTS.get(getattr(e, "short", ""), HINT_ANY)
    if isinstance(e, NeedsNet) or hint is None:
        why = net_why()
        if why == "ok":
            hint = ("It isn't on the memory card, and the internet didn't answer just now. Tap GET to try again, "
                    "or choose Debian to install from the memory card.")
        else:
            hint = NET_WHY_TEXT.get(why, NET_WHY_TEXT["no answer"]) + " Or choose Debian to install from the memory card."
    return short[:31], hint.replace("{id}", aid or "sbstore")


# ==== logging ==================================================================================================
_applog = None


def log(msg, app=None):
    line = "%s sbstore: %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    try:
        os.makedirs(C.log_dir, exist_ok=True)
        with open(os.path.join(C.log_dir, "sbstore.log"), "a") as f:
            f.write(line + "\n")
        a = app or _applog
        if a:
            with open(os.path.join(C.log_dir, a + ".log"), "a") as f:
                f.write(line + "\n")
    except OSError:
        pass
    if env("SBSTORE_VERBOSE") == "1" or _mode == "daemon":
        print("sbstore: " + msg, file=sys.stderr, flush=True)


_mode = "cli"


# ==== progress =================================================================================================
class Progress:
    """phases with weights; reports overall percent. Sinks: json lines (stdout) or a human line."""

    def __init__(self, json_out=False, sink=None):
        self.json = json_out
        self.sink = sink
        self.phase = ""
        self.lo, self.hi = 0, 100
        self.last = (None, -1, 0.0)

    def stage(self, phase, lo, hi):
        self.phase, self.lo, self.hi = phase, lo, hi
        self.report(0.0)

    def report(self, frac, done_kb=0, total_kb=0, msg=""):
        frac = max(0.0, min(1.0, frac))
        pct = int(self.lo + (self.hi - self.lo) * frac)
        now = time.monotonic()
        key = (self.phase, pct)
        if key[:2] == self.last[:2] and now - self.last[2] < 1.0 and not msg:
            return
        self.last = (self.phase, pct, now)
        ev = {"ev": "progress", "phase": self.phase, "pct": pct, "done_kb": int(done_kb),
              "total_kb": int(total_kb)}
        if msg:
            ev["msg"] = msg
        if self.sink:
            self.sink(ev)
        elif self.json:
            print(json.dumps(ev), flush=True)
        elif sys.stderr.isatty():
            sys.stderr.write("\r%-12s %3d%% %s" % (self.phase, pct, msg[:50]))
            sys.stderr.flush()

    def event(self, **ev):
        if self.sink:
            self.sink(ev)
        elif self.json:
            print(json.dumps(ev), flush=True)
        else:
            if sys.stderr.isatty():
                sys.stderr.write("\n")
            m = ev.get("msg") or ev.get("ev")
            print(m, flush=True)


NOPROG = Progress(sink=lambda ev: None)


# ==== versions =================================================================================================
def vkey(v):
    """natural order of version strings: 1.10 > 1.9, 2.0 > 2.0rc1 > 1.9"""
    v = (v or "").strip().lstrip("vV")
    v = re.sub(r"^\d+:", "", v)                     # Debian epoch
    parts = []
    for tok in re.findall(r"\d+|[A-Za-z]+|~", v):
        if tok.isdigit():
            parts.append((2, int(tok), ""))
        elif tok == "~" or tok.lower() in ("rc", "alpha", "beta", "a", "b", "pre", "dev"):
            parts.append((0, 0, tok.lower()))
        else:
            parts.append((1, 0, tok.lower()))
    return parts + [(1, 0, "")]


def newer(a, b):
    """a strictly newer than b"""
    if not a:
        return False
    if not b:
        return True
    return vkey(a) > vkey(b)


def safe_ver(v):
    v = re.sub(r"[^A-Za-z0-9._+~-]", "_", v or "unknown").strip("._") or "unknown"
    return v[:60]


# ==== catalog ==================================================================================================
CATEGORIES = ("Internet", "Office", "Graphics", "Media", "Games", "Utilities", "Development", "Education",
              "Science", "System")


def load_catalog(path=None):
    """{id: app} from catalog.json or a directory of TOML recipes; plus the old offline repo's index entries"""
    path = path or C.catalog
    apps = {}
    if os.path.isdir(path):
        import tomllib                                  # host only (Python 3.11+)
        for fn in sorted(os.listdir(path)):
            if fn.endswith(".toml"):
                with open(os.path.join(path, fn), "rb") as f:
                    a = tomllib.load(f)
                apps[a["id"]] = a
    elif os.path.exists(path):
        with open(path) as f:
            for a in json.load(f)["apps"]:
                apps[a["id"]] = a
    # the old store's offline repo (sbpkg's index.tsv): entries the catalog doesn't have join as distro-only apps
    idx = os.path.join(C.repo, "store", "index.tsv")
    if os.path.exists(idx):
        with open(idx) as f:
            for line in f:
                if not line.strip() or line.startswith("#"):
                    continue
                c = line.rstrip("\n").split("\t")
                if len(c) < 9:
                    continue
                key = c[0]
                hit = apps.get(key) or next((a for a in apps.values()
                                             if a.get("distro", {}).get("apt", [None])[0] == c[8].split()[0]), None)
                if hit:
                    hit.setdefault("_repo", {"dl_kb": int(c[6] or 0), "inst_kb": int(c[7] or 0), "key": key})
                    continue
                apps[key] = {"id": key, "name": c[1], "summary": c[2], "category": c[3],
                             "ram_mb": int(c[4] or 0), "featured": c[5] == "1", "default": "distro",
                             "distro": {"apt": c[8].split()}, "licence": "", "description": c[2],
                             "_repo": {"dl_kb": int(c[6] or 0), "inst_kb": int(c[7] or 0), "key": key}}
    return apps


def app_methods(a):
    return [m for m in METHODS if isinstance(a.get(m), dict)]


def default_method(a):
    ms = app_methods(a)
    d = a.get("default")
    return d if d in ms else (ms[0] if ms else None)


# ==== state ====================================================================================================
def state_path():
    return os.path.join(C.state_dir, "state.json")


def load_state():
    try:
        with open(state_path()) as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    s.setdefault("apps", {})
    s.setdefault("apt_refs", {})        # Debian package -> [app ids that need it] (runtime / build deps we added)
    s.setdefault("latest", {})          # app id -> {method: {"version":.., "checked":.., "from": "net"|"mirror"}}
    s.setdefault("prefs", {})           # app id -> the method the user picked last
    return s


def save_state(s):
    os.makedirs(C.state_dir, exist_ok=True)
    tmp = state_path() + ".tmp"
    with open(tmp, "w") as f:
        json.dump(s, f, indent=1, sort_keys=True)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, state_path())


class Lock:
    """one operation at a time (the daemon and a CLI user)"""

    def __enter__(self):
        os.makedirs(C.state_dir, exist_ok=True)
        self.f = open(os.path.join(C.state_dir, "lock"), "w")
        t0 = time.monotonic()
        while True:
            try:
                fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.monotonic() - t0 > float(env("SBSTORE_LOCK_WAIT", "600")):
                    raise StoreError("store busy", "another sbstore operation holds the lock")
                time.sleep(0.5)

    def __exit__(self, *a):
        fcntl.flock(self.f, fcntl.LOCK_UN)
        self.f.close()


# ==== network ==================================================================================================
_net = {"t": 0.0, "up": False, "why": "not checked"}
NET_WHY_TEXT = {
    "offline mode": "The store is set to offline.",
    "no connection": "Linux has no network connection yet. Join Wi-Fi (or turn on Mobile Data) in Settings, wait "
                     "about 10 seconds, then tap GET again.",
    "no dns": "Linux is connected but can't look up names (DNS). Check that Wi-Fi really reaches the internet, then "
              "tap GET again.",
    "no answer": "Linux is connected but the internet doesn't answer. Check that Wi-Fi really reaches the internet "
                 "(open a web page in Starbite), then tap GET again.",
}


NET_WHY_CODE = {"ok": 0, "no connection": 1, "no dns": 2, "no answer": 3, "offline mode": 4}


def _default_route():
    """an IPv4 default route (sbnet-up's 'default via 10.77.0.1'), or an IPv6 one"""
    try:
        with open(C.proc_route) as f:
            for l in f.readlines()[1:]:
                p = l.split()
                if len(p) > 3 and p[1] == "00000000" and int(p[3], 16) & 1:
                    return True
    except OSError:
        return True                                    # (no /proc: don't judge by it)
    try:
        with open(C.proc_route6) as f:
            for l in f:
                p = l.split()
                if len(p) > 9 and p[0] == "0" * 32 and p[1] == "00" and p[9] != "lo":
                    return True
    except OSError:
        pass
    return False


def net_probe():
    """(up, why): a default route, then a name lookup, then a TCP connect to any probe target"""
    if C.offline:
        return False, "offline mode"
    if not _default_route():
        return False, "no connection"
    resolved = False
    for t in C.net_probe.split(","):
        host, _, port = t.strip().rpartition(":")
        try:
            addrs = socket.getaddrinfo(host, int(port), type=socket.SOCK_STREAM)
        except (OSError, ValueError):
            continue
        resolved = True
        for fam, typ, proto, _, sa in addrs[:2]:
            try:
                s = socket.socket(fam, typ, proto)
                s.settimeout(5)
                s.connect(sa)
                s.close()
                return True, "ok"
            except OSError:
                continue
    return False, "no answer" if resolved else "no dns"


def net_up(force=False):
    if C.offline:
        return False
    if not force and time.monotonic() - _net["t"] < 30 and _net["t"]:
        return _net["up"]
    up, why = net_probe()
    _net.update(t=time.monotonic(), up=up, why=why)
    return up


def net_why():
    if not _net["t"]:
        net_up()
    return _net["why"]


def _ctx():
    return ssl.create_default_context()


def clock_ok():
    return time.time() > 1735689600                     # 2025-01-01: TLS can check certificates


def http_open(url, headers=None, timeout=30):
    if url.startswith("https:") and not clock_ok():
        raise StoreError("Linux clock not set", "the guest's clock says %s: TLS cannot check certificates" %
                         time.strftime("%Y-%m-%d", time.gmtime()))
    h = {"User-Agent": C.ua}
    h.update(headers or {})
    req = urllib.request.Request(url, headers=h)
    if url.startswith("https:"):
        return urllib.request.urlopen(req, timeout=timeout, context=_ctx())
    return urllib.request.urlopen(req, timeout=timeout)


def http_bytes(url, headers=None, limit=16 << 20, tries=3):
    last = None
    for i in range(tries):
        try:
            with http_open(url, headers) as r:
                return r.read(limit)
        except urllib.error.HTTPError as e:
            if e.code in (403, 429) and "github" in url:
                raise StoreError("GitHub rate limit", "GitHub API: HTTP %d (rate limit?) for %s" % (e.code, url))
            if e.code in (404, 410):
                raise StoreError("not found upstream", "HTTP %d for %s" % (e.code, url))
            last = e
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            last = e
        time.sleep(1.5 * (i + 1))
    raise StoreError("download failed", "%s: %s" % (url, last))


def http_json(url, headers=None):
    try:
        return json.loads(http_bytes(url, headers).decode("utf-8", "replace"))
    except ValueError as e:
        raise StoreError("bad answer upstream", "%s: not JSON (%s)" % (url, e))


def free_mb(path):
    p = path
    while not os.path.exists(p):
        p = os.path.dirname(p) or "/"
    st = os.statvfs(p)
    return st.f_bavail * st.f_frsize // (1 << 20)


def need_space(mb, what, path=None):
    path = path or C.opt
    have = free_mb(path) - C.reserve_mb
    if mb > have:
        raise StoreError("not enough space", "%s needs %d MB, %d MB free (after the %d MB reserve)" %
                         (what, mb, max(have, 0), C.reserve_mb))


def download(url, dest, size=0, sha256=None, prog=NOPROG, cancel=None):
    """to dest (atomically), resuming dest.part; checks size and sha256 when given. Returns the sha256."""
    if url.startswith("/"):
        url = "file://" + url
    if os.path.exists(dest):
        h = sha_file(dest)
        if not sha256 or h == sha256:
            return h
        os.unlink(dest)
    part = dest + ".part"
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tries = 0
    while True:
        have = os.path.getsize(part) if os.path.exists(part) else 0
        if size and have > size:
            os.unlink(part)
            have = 0
        hdr = {"Range": "bytes=%d-" % have} if have and not url.startswith("file:") else {}
        try:
            if url.startswith("file:"):
                r = open(urllib.request.url2pathname(urllib.parse.urlparse(url).path), "rb")
                r.seek(have)
                total = os.fstat(r.fileno()).st_size
                mode = "ab" if have else "wb"
            else:
                r = http_open(url, hdr, timeout=60)
                code = getattr(r, "status", 200)
                if have and code == 206:
                    mode = "ab"
                    cr = r.headers.get("Content-Range", "")
                    m = re.search(r"/(\d+)$", cr)
                    total = int(m.group(1)) if m else (have + int(r.headers.get("Content-Length") or 0))
                else:
                    mode, have = "wb", 0
                    total = int(r.headers.get("Content-Length") or 0) or size
            if size and total and total != size:
                log("size from the server %d != expected %d for %s" % (total, size, url))
            total = total or size
            with r, open(part, mode) as out:
                done = have
                while True:
                    if cancel and cancel():
                        raise StoreError("cancelled")
                    b = r.read(1 << 18)
                    if not b:
                        break
                    out.write(b)
                    done += len(b)
                    prog.report(done / total if total else 0.0, done // 1024, total // 1024)
            if total and done < total:
                raise OSError("short read: %d of %d" % (done, total))
            break
        except urllib.error.HTTPError as e:
            if e.code == 416 and have:            # the .part is whole (or bad): start again once
                os.unlink(part)
                tries += 1
                if tries > 3:
                    raise StoreError("download failed", "%s: HTTP 416" % url)
                continue
            if e.code in (404, 410):
                raise StoreError("not found upstream", "HTTP %d for %s" % (e.code, url))
            tries += 1
            if tries > 5:
                raise StoreError("download failed", "%s: HTTP %d" % (url, e.code))
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            tries += 1
            log("download %s: %s (try %d, resuming)" % (url, e, tries))
            if tries > 12:                                   # ~5 min of retries: a sleep, Wi-Fi coming back
                raise StoreError("download failed", "%s: %s" % (url, e))
            for _ in range(min(5 * tries, 30)):
                if cancel and cancel():
                    raise StoreError("cancelled")
                time.sleep(1)
    h = sha_file(part)
    if size and os.path.getsize(part) != size:
        n = os.path.getsize(part)
        os.unlink(part)
        raise StoreError("download damaged", "%s: %d bytes, expected %d" % (url, n, size))
    if sha256 and h != sha256.lower():
        os.unlink(part)
        raise StoreError("checksum mismatch", "%s: sha256 %s, expected %s" % (url, h, sha256))
    os.replace(part, dest)
    return h


def sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


# ==== resolving the latest release =============================================================================
def _fmt(s, **kw):
    try:
        return s.format(**kw)
    except (KeyError, IndexError, ValueError):
        return s


def _ver_from(tag, name, how):
    how = how or "tag"
    if how == "name":
        src = name or tag
    else:
        src = tag
    if how.startswith("regex:"):
        m = re.search(how[6:], tag or "") or re.search(how[6:], name or "")
        if not m:
            return None
        src = m.group(1)
    v = (src or "").strip()
    v = re.sub(r"^(release[-_]?|version[-_]?)", "", v, flags=re.I)
    return v[1:] if len(v) > 1 and v[0] in "vV" and v[1].isdigit() else v


def gh_headers():
    h = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"}
    tok = env("SBSTORE_GITHUB_TOKEN")
    if tok:
        h["Authorization"] = "Bearer " + tok
    return h


def _pick_assets(assets, pat, version):
    pat = _fmt(pat, version=version)
    return [x for x in assets if fnmatch.fnmatchcase(x["name"], pat)]


def pick_asset(rel, spec):
    """the release's assets for us: the recipe's glob, or the single URL of a url provider"""
    if spec.get("asset"):
        return _pick_assets(rel["assets"], spec["asset"], rel["version"])
    return rel["assets"][:1]


def resolve_github(spec, want_assets=True):
    repo = spec["repo"]
    base = C.github_api.rstrip("/")
    pre = spec.get("prerelease", False)
    tagpat = spec.get("tag_pattern")
    if not pre and not tagpat:
        rels = [http_json("%s/repos/%s/releases/latest" % (base, repo), gh_headers())]
    else:
        rels = http_json("%s/repos/%s/releases?per_page=30" % (base, repo), gh_headers())
    for r in rels:
        if r.get("draft") or (r.get("prerelease") and not pre):
            continue
        if tagpat and not fnmatch.fnmatchcase(r.get("tag_name", ""), tagpat):
            continue
        ver = _ver_from(r.get("tag_name"), r.get("name"), spec.get("version_from"))
        if not ver:
            continue
        assets = [{"name": x["name"], "url": x["browser_download_url"], "size": x.get("size", 0),
                   "sha256": (x.get("digest") or "").split("sha256:", 1)[-1] if (x.get("digest") or "").startswith("sha256:") else None}
                  for x in r.get("assets", [])]
        if want_assets and spec.get("asset") and not _pick_assets(assets, spec["asset"], ver):
            continue                                    # this release has no build for us: look further back
        return {"version": ver, "tag": r.get("tag_name"), "assets": assets, "published": r.get("published_at"),
                "tarball": "https://github.com/%s/archive/refs/tags/%s.tar.gz" % (repo, urllib.parse.quote(r.get("tag_name", ""))),
                "from": "net"}
    raise StoreError("no aarch64 release", "%s: no release with an asset matching %s" % (repo, spec.get("asset")))


def resolve_gitlab(spec, want_assets=True):
    host = spec.get("host", "gitlab.com")
    api = env("SBSTORE_GITLAB_API_" + re.sub(r"\W", "_", host).upper(), "https://%s/api/v4" % host)
    proj = urllib.parse.quote(spec["repo"], safe="")
    rels = http_json("%s/projects/%s/releases?per_page=30" % (api, proj))
    tagpat = spec.get("tag_pattern")
    for r in rels:
        if r.get("upcoming_release"):
            continue
        tag = r.get("tag_name", "")
        if tagpat and not fnmatch.fnmatchcase(tag, tagpat):
            continue
        ver = _ver_from(tag, r.get("name"), spec.get("version_from"))
        links = (r.get("assets") or {}).get("links") or []
        assets = [{"name": x.get("name"), "url": x.get("direct_asset_url") or x.get("url"), "size": 0,
                   "sha256": None} for x in links]
        srcs = {s.get("format"): s.get("url") for s in (r.get("assets") or {}).get("sources") or []}
        if want_assets and spec.get("asset") and not _pick_assets(assets, spec["asset"], ver):
            continue
        return {"version": ver, "tag": tag, "assets": assets, "published": r.get("released_at"),
                "tarball": srcs.get("tar.gz") or "https://%s/%s/-/archive/%s/%s.tar.gz" % (
                    host, spec["repo"], tag, spec["repo"].split("/")[-1] + "-" + tag), "from": "net"}
    raise StoreError("no aarch64 release", "%s: no release with an asset matching %s" % (spec["repo"], spec.get("asset")))


def resolve_url(spec, want_assets=True):
    ver = spec.get("version")
    if spec.get("version_url"):
        raw = http_bytes(_fmt(spec["version_url"])).decode("utf-8", "replace")
        if spec.get("version_json"):
            obj = json.loads(raw)
            for k in spec["version_json"].split("."):
                obj = obj[k]
            ver = str(obj)
        elif spec.get("version_regex"):
            vs = re.findall(spec["version_regex"], raw)
            if not vs:
                raise StoreError("no version upstream", "no match for %s at %s" % (spec["version_regex"], spec["version_url"]))
            ver = max(vs, key=vkey)
    if not ver:
        raise StoreError("no version upstream", "recipe gives no version source")
    url = _fmt(spec["url"], version=ver, major=ver.split(".")[0])
    name = os.path.basename(urllib.parse.urlparse(url).path) or "download"
    return {"version": ver, "tag": ver, "assets": [{"name": name, "url": url, "size": 0, "sha256": None}],
            "published": None, "tarball": _fmt(spec.get("tarball", ""), version=ver, tag=ver) or None, "from": "net"}


RESOLVERS = {"github": resolve_github, "gitlab": resolve_gitlab, "url": resolve_url}


def mirror_json(app_id, what):
    p = os.path.join(C.mirror, app_id, what + ".json")
    try:
        with open(p) as f:
            j = json.load(f)
    except (OSError, ValueError):
        return None
    d = os.path.join(C.mirror, app_id)
    for a in j.get("assets", []):                    # mirrored files are local
        if a.get("file"):
            a["url"] = "file://" + os.path.join(d, a["file"])
    if j.get("tarball_file"):
        j["tarball"] = "file://" + os.path.join(d, j["tarball_file"])
    j["from"] = "mirror"
    return j


def resolve(app, method):
    """the latest version for a method: {"version", "tag", "assets", "tarball", "from": net|mirror|apt}"""
    if method == "distro" or (method == "source" and app["source"].get("system") == "debian"):
        return resolve_distro(app)
    spec = app.get("release") if method == "release" else app.get("source", {})
    if method == "source":
        ref = spec.get("ref", "release")
        if ref != "release":
            tb = _fmt(spec.get("tarball", ""), tag=ref, version=ref)
            r = {"version": ref, "tag": ref, "assets": [], "tarball": tb or None, "from": "net"}
            if not net_up():
                m = mirror_json(app["id"], "source")
                if m:
                    return m
                raise NeedsNet("needs the network", "source of %s: no network and nothing in the mirror" % app["id"])
            return r
        rspec = dict(app.get("release") or {})
        if spec.get("repo"):
            rspec = {"provider": spec.get("provider", "github"), "repo": spec["repo"],
                     "tag_pattern": spec.get("tag_pattern"), "version_from": spec.get("version_from"),
                     "prerelease": spec.get("prerelease", False)}
        spec_r = rspec
    else:
        spec_r = spec
    if not spec_r:
        raise StoreError("no such method", "%s has no %s method" % (app["id"], method))
    what = "source" if method == "source" else "release"
    err = None
    if net_up():
        try:
            r = RESOLVERS[spec_r.get("provider", "github")](spec_r, want_assets=(method == "release"))
            if method == "source" and spec.get("tarball"):
                r["tarball"] = _fmt(spec["tarball"], tag=r["tag"], version=r["version"])
            return r
        except StoreError as e:
            err = e
            log("resolve %s %s online: %s" % (app["id"], method, e.detail))
    m = mirror_json(app["id"], what)
    if m:
        return m
    if err:
        raise err
    raise NeedsNet("needs the network", "%s %s: no network and nothing in the offline mirror" % (app["id"], method))


# ==== apt (the distro method, runtime and build deps) ==========================================================
def run(argv, logto=None, cwd=None, envx=None, timeout=None, status_fd_cb=None, cancel=None, line_cb=None):
    """run argv; output to the app's log; returns (rc, the output's tail)"""
    e = dict(os.environ)
    e.update({"DEBIAN_FRONTEND": "noninteractive", "LC_ALL": "C.UTF-8"})
    if envx:
        e.update(envx)
    tail = []
    pass_fds = ()
    rfd = wfd = None
    if status_fd_cb:
        rfd, wfd = os.pipe()
        pass_fds = (wfd,)
        argv = [a.replace("@STATUSFD@", str(wfd)) for a in argv]
    lf = open(logto, "a") if logto else None
    if lf:
        lf.write("$ %s\n" % " ".join(shlex.quote(a) for a in argv))
        lf.flush()
    try:
        p = subprocess.Popen(argv, cwd=cwd, env=e, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, pass_fds=pass_fds, start_new_session=True)
    except FileNotFoundError:
        if lf:
            lf.close()
        if wfd is not None:
            os.close(wfd)
            os.close(rfd)
        raise StoreError("tool missing", "%s is not installed" % argv[0])
    if wfd is not None:
        os.close(wfd)
    import selectors
    sel = selectors.DefaultSelector()
    sel.register(p.stdout, selectors.EVENT_READ, "out")
    if rfd is not None:
        sel.register(rfd, selectors.EVENT_READ, "st")
    bufs = {"out": b"", "st": b""}
    t0 = time.monotonic()
    open_n = len(sel.get_map())
    while open_n:
        if cancel and cancel():
            os.killpg(p.pid, signal.SIGTERM)
            time.sleep(1)
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            p.wait()
            if lf:
                lf.close()
            raise StoreError("cancelled")
        if timeout and time.monotonic() - t0 > timeout:
            os.killpg(p.pid, signal.SIGKILL)
            raise StoreError("took too long", "%s: over %d s" % (argv[0], timeout))
        for key, _ in sel.select(timeout=0.5):
            fd = key.fileobj if key.data == "out" else key.fileobj
            b = os.read(fd if isinstance(fd, int) else fd.fileno(), 65536)
            if not b:
                sel.unregister(key.fileobj)
                open_n -= 1
                continue
            bufs[key.data] += b
            while b"\n" in bufs[key.data]:
                l, bufs[key.data] = bufs[key.data].split(b"\n", 1)
                s = l.decode("utf-8", "replace")
                if key.data == "st":
                    status_fd_cb(s)
                else:
                    if lf:
                        lf.write(s + "\n")
                    tail.append(s)
                    if len(tail) > 60:
                        del tail[:20]
                    if line_cb:
                        line_cb(s)
    rc = p.wait()
    if rfd is not None:
        os.close(rfd)
    if lf:
        lf.write("[exit %d]\n" % rc)
        lf.close()
    return rc, "\n".join(tail[-40:])


def dpkg_installed():
    try:
        out = subprocess.run(C.dpkg_query + ["-W", "-f=${Package} ${db:Status-Abbrev} ${Version}\n"],
                             capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    d = {}
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3 and p[1].startswith("ii"):
            d[p[0].split(":")[0]] = p[2]
    return d


def base_packages():
    try:
        with open(C.base_list) as f:
            return {l.strip() for l in f if l.strip() and not l.startswith("#")}
    except OSError:
        return set()


def resolve_distro(app):
    pk = app["distro"]["apt"][0]
    try:
        out = subprocess.run(C.apt_cache + ["policy", pk], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    m = re.search(r"Candidate:\s*(\S+)", out)
    cand = m.group(1) if m and m.group(1) != "(none)" else None
    if not cand:
        raise NeedsNet("not in the repo", "%s: apt has no candidate (offline repo without it, network down?)" % pk)
    return {"version": cand, "tag": cand, "assets": [], "tarball": None, "from": "apt"}


def apt_progress_cb(prog, lo_dl=0.0, hi_dl=0.6):
    def cb(line):
        p = line.split(":", 3)
        if len(p) < 4:
            return
        try:
            pct = float(p[2])
        except ValueError:
            return
        if p[0] == "dlstatus":
            prog.report(lo_dl + (hi_dl - lo_dl) * pct / 100, msg="downloading " + p[1])
        elif p[0] == "pmstatus":
            prog.report(hi_dl + (1 - hi_dl) * pct / 100, msg=p[3][:60])
        elif p[0] == "pmerror":
            log("apt: pmerror %s" % line)
    return cb


DEB_NET = ("deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] http://deb.debian.org/debian trixie main\n"
           "deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] http://deb.debian.org/debian-security "
           "trixie-security main\n")


def ensure_net_sources():
    """online: Debian's mirror as an apt source (the base ships offline: file:/repo only, debian.list.disabled).
    Returns True when a source was added (then apt needs an update)."""
    if not net_up():
        return False
    d = rp("/etc/apt/sources.list.d")
    try:
        for fn in os.listdir(d):
            if fn.endswith((".list", ".sources")) and "deb.debian.org" in open(os.path.join(d, fn)).read():
                return False
        if os.path.exists(rp("/etc/apt/sources.list")) and "deb.debian.org" in open(rp("/etc/apt/sources.list")).read():
            return False
        if not os.path.exists(rp("/usr/share/keyrings/debian-archive-keyring.gpg")):
            return False
        with open(os.path.join(d, "sbstore-debian.list"), "w") as f:
            f.write(DEB_NET)
        log("network up: Debian's mirror added as an apt source")
        return True
    except OSError:
        return False


def ensure_mirror_source():
    """the offline mirror's debs/ (build-essential, git ...) as a file: apt source while it is there"""
    lst = rp("/etc/apt/sources.list.d/sbstore-mirror.list")
    d = os.path.join(C.mirror, "debs")
    want = "deb [trusted=yes] file:%s ./\n" % d if os.path.exists(os.path.join(d, "Packages")) else ""
    try:
        have = open(lst).read() if os.path.exists(lst) else ""
        if want == have:
            return False
        if want:
            os.makedirs(os.path.dirname(lst), exist_ok=True)
            with open(lst, "w") as f:
                f.write(want)
        else:
            os.unlink(lst)
        log("offline mirror apt source %s" % ("added: " + d if want else "removed (the mirror is gone)"))
        return True
    except OSError:
        return False


def repair(app_id=None, at_start=False):
    """put back what an interrupted job left (Linux stopped, the phone restarted, the screen slept and the guest was
    stopped mid-dpkg): dpkg's half-configured packages, then apt's broken dependencies; at the daemon's start also the
    leftover build trees and unpacked-but-not-switched versions. Returns True when something was repaired."""
    did = False
    upd = rp("/var/lib/dpkg/updates")
    try:
        pending = [f for f in os.listdir(upd) if f.isdigit()]
    except OSError:
        pending = []
    if pending or os.path.exists(os.path.join(C.state_dir, "dpkg-running")):
        log("repairing an interrupted install: dpkg --configure -a", app_id)
        rc, out = run(C.dpkg + ["--force-confold", "--configure", "-a"], logto=_logp(app_id), timeout=1800,
                      envx={"DEBIAN_FRONTEND": "noninteractive"})
        if rc:
            log("dpkg --configure -a exited %d: %s" % (rc, out[-200:]), app_id)
        rc, out = run(C.apt + ["-y", "-q", "-f", "--no-install-recommends", "-o", "DPkg::Lock::Timeout=120",
                               "install"], logto=_logp(app_id), timeout=1800)
        if rc:
            log("apt-get -f install exited %d: %s" % (rc, out[-200:]), app_id)
        did = True
    try:
        os.unlink(os.path.join(C.state_dir, "dpkg-running"))
    except OSError:
        pass
    if at_start:
        b = os.path.join(C.state_dir, "build")
        if os.path.isdir(b) and os.listdir(b):
            log("removing unfinished build trees: %s" % " ".join(sorted(os.listdir(b))[:8]))
            shutil.rmtree(b, ignore_errors=True)
            did = True
        try:
            for aid in os.listdir(C.opt):
                d = os.path.join(C.opt, aid)
                for n in os.listdir(d) if os.path.isdir(d) else []:
                    if n.startswith(".new-") or n.startswith(".current.tmp"):
                        shutil.rmtree(os.path.join(d, n), ignore_errors=True)
                        log("removed %s's unfinished %s" % (aid, n))
                        did = True
        except OSError:
            pass
    return did


def apt_update(app_id=None):
    ensure_net_sources()
    if net_up():
        rc, _ = run(C.apt + ["-q", "update"], logto=_logp(app_id), timeout=900)
        if rc:
            log("apt-get update failed (%d)" % rc, app_id)


APT_NET_OPTS = ["-o", "Acquire::Retries=5", "-o", "Acquire::http::Timeout=30", "-o", "Acquire::https::Timeout=30"]


def _apt_get_install(missing, app_id, prog, cancel):
    """apt-get install with progress; a marker while dpkg may be mid-way, so the next start repairs an interruption"""
    mark = os.path.join(C.state_dir, "dpkg-running")
    try:
        os.makedirs(C.state_dir, exist_ok=True)
        open(mark, "w").close()
    except OSError:
        mark = None
    rc, out = run(C.apt + ["-y", "-q", "--no-install-recommends", "-o", "APT::Status-Fd=@STATUSFD@",
                           "-o", "DPkg::Lock::Timeout=120"] + APT_NET_OPTS + ["install"] + missing,
                  logto=_logp(app_id), status_fd_cb=apt_progress_cb(prog), cancel=cancel)
    if rc and re.search(r"Failed to fetch|Temporary failure resolving|Could not resolve|Connection timed out", out) \
            and not (cancel and cancel()):
        log("apt: a download failed; trying once more in 10 s (the network may have dropped)", app_id)
        time.sleep(10)
        rc, out = run(C.apt + ["-y", "-q", "--no-install-recommends", "-o", "APT::Status-Fd=@STATUSFD@",
                               "-o", "DPkg::Lock::Timeout=120"] + APT_NET_OPTS + ["install"] + missing,
                      logto=_logp(app_id), status_fd_cb=apt_progress_cb(prog), cancel=cancel)
    if mark and not rc:
        try:
            os.unlink(mark)
        except OSError:
            pass
    return rc, out


def apt_install(pkgs, app_id, prog=NOPROG, cancel=None, auto=False):
    """install Debian packages; returns those that were not installed before"""
    pkgs = [p for p in pkgs if p]
    if not pkgs:
        return []
    before = dpkg_installed()
    missing = [p for p in pkgs if p.split("=")[0] not in before]
    if not missing:
        prog.report(1.0)
        return []
    repair(app_id)
    if ensure_mirror_source():                           # the mirror's debs (build deps offline) just appeared
        run(C.apt + ["-q", "update"], logto=_logp(app_id), timeout=600, cancel=cancel)
    rc, out = _apt_get_install(missing, app_id, prog, cancel)
    if rc and ("Unable to locate package" in out or "has no installation candidate" in out) and net_up():
        ensure_net_sources()
        run(C.apt + ["-q", "update"], logto=_logp(app_id), timeout=900)
        rc, out = _apt_get_install(missing, app_id, prog, cancel)
    if rc:
        if "Unable to locate package" in out or "has no installation candidate" in out:
            raise NeedsNet("package not available", "apt: %s" % out.strip().splitlines()[-1][:200])
        if "No space left" in out:
            raise StoreError("not enough space", "apt: no space left")
        if "unmet dependencies" in out or "Unable to satisfy dependencies" in out or "held broken packages" in out:
            if net_up(force=True):
                raise StoreError("install failed", "apt: unmet dependencies: %s" % out[-300:])
            raise NeedsNet("package not available", "apt: unmet dependencies offline: %s" % out[-300:])
        if re.search(r"Temporary failure resolving|Could not resolve|Failed to fetch", out):
            raise NeedsNet("needs the network", "apt: fetch failed")
        raise StoreError("install failed", "apt-get install exited %d: %s" % (rc, out[-300:]))
    if auto:
        run(C.apt_mark + ["auto"] + missing, logto=_logp(app_id))
    after = dpkg_installed()
    return [p for p in missing if p.split("=")[0] in after]


def apt_remove(pkgs, app_id, prog=NOPROG, guard=True):
    """remove packages, never taking a base package (simulated first); returns what was removed"""
    pkgs = [p for p in pkgs if p in dpkg_installed()]
    if not pkgs:
        return []
    if guard:
        base = base_packages()
        rc, out = run(C.apt + ["-s", "-q", "remove"] + pkgs, logto=_logp(app_id))
        gone = re.findall(r"^Remv (\S+)", out, re.M)
        bad = sorted(set(gone) & base)
        if bad:
            raise StoreError("would remove system parts", "removing %s would take base packages %s" %
                             (" ".join(pkgs), " ".join(bad[:6])))
    rc, out = run(C.apt + ["-y", "-q", "-o", "APT::Status-Fd=@STATUSFD@", "remove"] + pkgs,
                  logto=_logp(app_id), status_fd_cb=apt_progress_cb(prog, 0, 0))
    if rc:
        raise StoreError("remove failed", "apt-get remove exited %d: %s" % (rc, out[-300:]))
    return pkgs


def _logp(app_id):
    if not app_id:
        return None
    os.makedirs(C.log_dir, exist_ok=True)
    return os.path.join(C.log_dir, app_id + ".log")


def refs_add(st, pkgs, app_id):
    for p in pkgs:
        r = st["apt_refs"].setdefault(p, [])
        if app_id not in r:
            r.append(app_id)


def refs_drop(st, app_id, prog=NOPROG):
    """drop app_id's references; remove the packages we added that nothing else needs"""
    free = []
    for p, r in list(st["apt_refs"].items()):
        if app_id in r:
            r.remove(app_id)
            if not r:
                free.append(p)
                del st["apt_refs"][p]
    if free:
        try:
            apt_remove(free, app_id, prog)
        except StoreError as e:
            log("kept %s: %s" % (" ".join(free), e.detail), app_id)
    return free


# ==== unpacking ================================================================================================
def _strip_member(m, strip):
    parts = [p for p in m.name.split("/") if p not in ("", ".")]
    if len(parts) <= strip:
        return None
    m.name = "/".join(parts[strip:])
    return m


def unpack_tar(path, dest, strip=0):
    cmp = None
    with open(path, "rb") as f:
        head = f.read(6)
    if head[:4] == b"\x28\xb5\x2f\xfd":
        cmp = "zstd"
    src = path
    tmp = None
    if cmp == "zstd":                                   # tarfile has no zstd before Python 3.14
        tmp = path + ".tar"
        with open(tmp, "wb") as o:
            if subprocess.run(["zstd", "-dc", path], stdout=o).returncode:
                raise StoreError("unpack failed", "zstd -dc %s" % path)
        src = tmp
    try:
        with tarfile.open(src, "r:*") as t:
            ms = []
            for m in t.getmembers():
                m = _strip_member(m, strip)
                if m is None:
                    continue
                if m.islnk():                           # hard links are named from the archive root
                    lp = [p for p in m.linkname.split("/") if p not in ("", ".")]
                    if len(lp) <= strip:
                        continue
                    m.linkname = "/".join(lp[strip:])
                ms.append(m)
            t.extractall(dest, members=ms, filter="data" if hasattr(tarfile, "data_filter") else None)
    except (tarfile.TarError, OSError, EOFError) as e:
        raise StoreError("unpack failed", "%s: %s" % (os.path.basename(path), e))
    finally:
        if tmp and os.path.exists(tmp):
            os.unlink(tmp)


def unpack_zip(path, dest, strip=0):
    try:
        with zipfile.ZipFile(path) as z:
            for i in z.infolist():
                parts = [p for p in i.filename.split("/") if p not in ("", ".")]
                if len(parts) <= strip or ".." in parts:
                    continue
                out = os.path.join(dest, *parts[strip:])
                if i.is_dir():
                    os.makedirs(out, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                mode = (i.external_attr >> 16) & 0o777
                if (i.external_attr >> 16) & 0o170000 == 0o120000:      # a symlink
                    tgt = z.read(i).decode()
                    if os.path.isabs(tgt) or ".." in tgt.split("/"):
                        continue
                    os.symlink(tgt, out)
                    continue
                with z.open(i) as s, open(out, "wb") as o:
                    shutil.copyfileobj(s, o)
                os.chmod(out, mode or 0o644)
    except (zipfile.BadZipFile, OSError) as e:
        raise StoreError("unpack failed", "%s: %s" % (os.path.basename(path), e))


def unpack_appimage(path, dest):
    """an aarch64 AppImage: --appimage-extract (no FUSE needed) -> dest (AppRun at its top)"""
    os.chmod(path, 0o755)
    work = tempfile.mkdtemp(prefix="appimage.", dir=os.path.dirname(dest))
    try:
        rc, out = run([path, "--appimage-extract"], cwd=work, timeout=900)
        sq = os.path.join(work, "squashfs-root")
        if rc or not os.path.isdir(sq):
            raise StoreError("unpack failed", "AppImage extract exited %d: %s" % (rc, out[-200:]))
        os.makedirs(dest, exist_ok=True)
        for n in os.listdir(sq):
            shutil.move(os.path.join(sq, n), os.path.join(dest, n))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def unpack(path, fmt, dest, strip=0, binpath=None):
    os.makedirs(dest, exist_ok=True)
    if fmt == "tar":
        unpack_tar(path, dest, strip)
    elif fmt == "zip":
        unpack_zip(path, dest, strip)
    elif fmt == "appimage":
        unpack_appimage(path, dest)
        if binpath and not os.path.exists(os.path.join(dest, binpath)):
            # bin[0] runs the AppImage's AppRun (APPDIR set, as the AppImage runtime would)
            w = os.path.join(dest, binpath)
            os.makedirs(os.path.dirname(w), exist_ok=True)
            with open(w, "w") as f:
                f.write('#!/bin/sh\nAPPDIR="$(dirname "$(readlink -f "$0")")"\n'
                        'while [ ! -e "$APPDIR/AppRun" ] && [ "$APPDIR" != / ]; do APPDIR=$(dirname "$APPDIR"); done\n'
                        'export APPDIR APPIMAGE="$APPDIR"\nexec "$APPDIR/AppRun" "$@"\n')
            os.chmod(w, 0o755)
    elif fmt == "binary":
        out = os.path.join(dest, binpath or os.path.join("bin", os.path.basename(path)))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copyfile(path, out)
        os.chmod(out, 0o755)
    else:
        raise StoreError("bad recipe", "unknown format %s" % fmt)


# ==== verification =============================================================================================
def sums_lookup(text, name):
    """the sha256 for name from a sums file ("<hex>  name", "<hex> *name", BSD "SHA256 (name) = hex", or bare hex)"""
    text = text.strip()
    if re.fullmatch(r"[0-9a-fA-F]{64}", text.split()[0] if text else "") and len(text.split()) == 1:
        return text.split()[0].lower()
    for l in text.splitlines():
        m = re.match(r"^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$", l)
        if m and (m.group(2) == name or m.group(2).endswith("/" + name) or os.path.basename(m.group(2)) == name):
            return m.group(1).lower()
        m = re.match(r"^SHA256 \((.+)\) = ([0-9a-fA-F]{64})$", l)
        if m and (m.group(1) == name or os.path.basename(m.group(1)) == name):
            return m.group(2).lower()
    return None


def catalog_dir():
    """where keys/ and icons/ are: beside catalog.json, or (a TOML directory, the source tree) beside catalog/"""
    return os.path.dirname(C.catalog.rstrip("/"))


def _dearmor(text):
    """an ASCII-armoured OpenPGP block -> its binary packets (for gpgv's --keyring)"""
    import base64
    body, on = [], False
    for l in text.splitlines():
        if l.startswith("-----BEGIN PGP"):
            on, body = True, []
            continue
        if l.startswith("-----END PGP"):
            break
        if on and l.strip() and ":" not in l and not l.startswith("="):
            body.append(l.strip())
    return base64.b64decode("".join(body))


def verify_signature(sig, data, ver_spec):
    """an OpenPGP detached signature over data with the recipe's pinned key (sqv, else gpgv); returns the signer"""
    key = os.path.join(catalog_dir(), ver_spec["key"])
    if not os.path.exists(key):
        raise StoreError("signing key missing", "%s is not in the catalog" % ver_spec["key"])
    want = [f.upper().replace(" ", "") for f in ([ver_spec["fingerprint"]] if isinstance(ver_spec.get("fingerprint"), str)
                                                else ver_spec.get("fingerprint") or [])]
    if shutil.which("sqv"):
        r = subprocess.run(["sqv", "--keyring", key, sig, data], capture_output=True, text=True)
        got = r.stdout.upper()
        ok = r.returncode == 0
        err = r.stderr.strip()[-200:]
    elif shutil.which("gpgv"):
        kr = sig + ".keyring"
        with open(key) as f, open(kr, "wb") as o:
            o.write(_dearmor(f.read()))
        r = subprocess.run(["gpgv", "--status-fd", "1", "--keyring", kr, sig, data], capture_output=True, text=True)
        os.unlink(kr)
        got = " ".join(l for l in r.stdout.splitlines() if "VALIDSIG" in l).upper()
        ok = r.returncode == 0 and bool(got)
        err = r.stderr.strip()[-200:]
    else:
        raise StoreError("no signature tool", "neither sqv nor gpgv is installed")
    if not ok:
        raise StoreError("bad signature", "%s: %s" % (os.path.basename(data), err))
    if want and not any(f in got for f in want):
        raise StoreError("unexpected signer", "%s signed by %s, expected %s" % (os.path.basename(data), got.strip()[:80],
                                                                               " or ".join(want)))
    return (want[0] if want else got.split()[0] if got.split() else "?")[-16:]


def _sig_url(rel, ver_spec_sig, asset, version):
    """a signature named by an asset glob ("{asset}.sig") or a URL template"""
    pat = _fmt(ver_spec_sig, asset=asset["name"], version=version, url=asset.get("url", ""))
    if "://" in pat:
        return pat
    hits = [a for a in rel["assets"] if fnmatch.fnmatchcase(a["name"], pat)]
    if not hits:
        raise StoreError("no signature upstream", "signature %s missing" % pat)
    return hits[0]["url"]


def check_file_signature(rel, asset, ver_spec, version, path):
    """[release.verify] signature + key: the downloaded file's detached signature. "" when the recipe has none
    (or the install comes from the offline mirror: the mirror checked it when it was made)"""
    if not ver_spec.get("signature") or not ver_spec.get("key") or rel.get("from") == "mirror":
        return ""
    sig = path + ".sig.tmp"
    with open(sig, "wb") as f:
        f.write(http_bytes(_sig_url(rel, ver_spec["signature"], asset, version), limit=1 << 20))
    try:
        return " + OpenPGP signature (%s)" % verify_signature(sig, path, ver_spec)
    finally:
        os.unlink(sig)


def expected_sha(rel, asset, ver_spec, version):
    """(sha256 or None, how): from the API's digest, a checksum asset, or a sums URL"""
    if asset.get("sha256") and ver_spec.get("github_digest", True):
        return asset["sha256"].lower(), "sha256 (GitHub digest)"
    if ver_spec.get("sha256_asset"):
        pat = _fmt(ver_spec["sha256_asset"], asset=asset["name"], version=version)
        hits = [a for a in rel["assets"] if fnmatch.fnmatchcase(a["name"], pat)]
        if not hits:
            raise StoreError("no checksum upstream", "checksum asset %s missing" % pat)
        txt = http_bytes(hits[0]["url"], limit=4 << 20).decode("utf-8", "replace")
        h = sums_lookup(txt, asset["name"])
        if not h:
            raise StoreError("no checksum upstream", "%s has no line for %s" % (hits[0]["name"], asset["name"]))
        return h, "sha256 (" + hits[0]["name"] + ")"
    if ver_spec.get("sha256sums_url"):
        url = _fmt(ver_spec["sha256sums_url"], version=version)
        if rel.get("from") == "mirror" and rel.get("sums_file"):
            url = "file://" + os.path.join(C.mirror, rel["id"], rel["sums_file"])
        raw = http_bytes(url, limit=8 << 20)
        signed = ""
        if ver_spec.get("sums_signature") and ver_spec.get("key") and rel.get("from") != "mirror":
            d = tempfile.mkdtemp(prefix="sums.", dir=os.path.join(C.state_dir) if os.path.isdir(C.state_dir) else None)
            try:
                with open(os.path.join(d, "sums"), "wb") as f:
                    f.write(raw)
                with open(os.path.join(d, "sums.asc"), "wb") as f:
                    f.write(http_bytes(_fmt(ver_spec["sums_signature"], version=version), limit=1 << 20))
                signed = ", signed by %s" % verify_signature(os.path.join(d, "sums.asc"), os.path.join(d, "sums"), ver_spec)
            finally:
                shutil.rmtree(d, ignore_errors=True)
        txt = raw.decode("utf-8", "replace")
        line = _fmt(ver_spec.get("sha256_line", asset["name"]), version=version, asset=asset["name"])
        h = sums_lookup(txt, line)
        if not h:
            raise StoreError("no checksum upstream", "%s has no line for %s" % (url, line))
        return h, "sha256 (" + os.path.basename(urllib.parse.urlparse(url).path) + signed + ")"
    if rel.get("from") == "mirror" and asset.get("sha256"):
        return asset["sha256"], "sha256 (mirror)"
    return None, "unverified (upstream gives no checksum; TLS only)"


# ==== activation: links, launcher, icon ========================================================================
def app_dir(app_id):
    return os.path.join(C.opt, app_id)


def _rel_link(target, link):
    if os.path.lexists(link):
        os.unlink(link)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    os.symlink(target, link)


def set_current(app_id, ver):
    d = app_dir(app_id)
    tmp = os.path.join(d, ".current.tmp")
    if os.path.lexists(tmp):
        os.unlink(tmp)
    os.symlink(ver, tmp)
    os.replace(tmp, os.path.join(d, "current"))     # atomic flip


def _find_icon(root, name):
    best = None
    for dp, dn, fn in os.walk(root):
        for f in fn:
            stem, ext = os.path.splitext(f)
            if stem == name and ext in (".svg", ".png"):
                p = os.path.join(dp, f)
                m = re.search(r"/(\d+)x\d+/", p)
                score = 10000 if ext == ".svg" else (int(m.group(1)) if m else 1)
                if best is None or score > best[0]:
                    best = (score, p)
    return best[1] if best else None


def guest_path(p):
    """a path under ROOT as the guest will see it: only when preparing a root from outside
    (SBSTORE_GUEST_PATHS=1, e.g. installing into an image's tree); else the real path (the guest: ROOT = /)"""
    r = os.path.abspath(ROOT)
    p = os.path.abspath(p)
    if r == "/" or env("SBSTORE_GUEST_PATHS") != "1":
        return p
    return "/" + os.path.relpath(p, r)


def activate(app, ver, bins, method):
    """links into /usr/local/bin, the launcher .desktop and its icon, for /opt/sbstore/<id>/current"""
    aid = app["id"]
    cur = os.path.join(app_dir(aid), "current")
    links = []
    for b in bins:
        tgt = os.path.join(cur, b)
        if not os.path.exists(os.path.join(app_dir(aid), ver, b)):
            raise StoreError("install incomplete", "%s: %s missing after install" % (aid, b))
        ln = os.path.join(C.bin, os.path.basename(b))
        if os.path.lexists(ln) and not os.path.islink(ln):
            log("not replacing %s (not ours)" % ln, aid)
            continue
        _rel_link(guest_path(tgt), ln)
        links.append(ln)
    write_launcher(app, os.path.join(app_dir(aid), ver), method)
    return links


def desktop_escape(s):
    return s.replace("\n", " ")


def write_launcher(app, vdir, method):
    """sbstore-<id>.desktop: from the app's own .desktop when the recipe names one, else from the recipe"""
    aid = app["id"]
    L = app.get("launcher") or {}
    if not L and (method == "distro" or (app.get(method) or {}).get("format") == "deb"
                  or (app.get(method) or {}).get("system") == "debian"):
        return None                                     # the Debian package(s) bring their own .desktop
    exec_ = (app.get(method) or {}).get("exec") or L.get("exec") or ((app.get(method) or {}).get("bin") or [aid])[0]
    exec_w = exec_.split()
    if exec_w and not exec_w[0].startswith("/") and "/" in exec_w[0]:
        exec_w[0] = guest_path(os.path.join(app_dir(aid), "current", exec_w[0]))
    exec_ = " ".join(exec_w)
    envs = L.get("env") or {}
    if envs:
        exec_ = "env " + " ".join("%s=%s" % (k, shlex.quote(v)) for k, v in envs.items()) + " " + exec_
    icon_name = app.get("icon") or aid
    icon_file = None
    if vdir and os.path.isdir(vdir):
        icon_file = _find_icon(vdir, icon_name)
    if not icon_file:
        cat_icon = os.path.join(catalog_dir(), "icons", aid + ".png")
        if os.path.exists(cat_icon):
            icon_file = cat_icon
    icon_field = icon_name
    if icon_file:
        ext = os.path.splitext(icon_file)[1]
        sub = "scalable" if ext == ".svg" else "256x256"
        dst = os.path.join(C.icons, sub, "apps", "sbstore-%s%s" % (aid, ext))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(icon_file, dst)
        icon_field = "sbstore-" + aid
    lines = ["[Desktop Entry]", "Type=Application", "Name=" + desktop_escape(app["name"]),
             "Comment=" + desktop_escape(app.get("summary", "")), "Exec=" + exec_, "Icon=" + icon_field,
             "Terminal=" + ("true" if L.get("terminal") else "false"),
             "Categories=%s;" % app.get("category", "Utility"),
             "X-Starbite-Pkg=%s" % aid, "X-Starbite-Store=%s" % aid, "X-Starbite-Method=%s" % method]
    if L.get("keyboard"):
        lines.append("X-Starbite-Keyboard=true")
    os.makedirs(C.apps, exist_ok=True)
    p = os.path.join(C.apps, "sbstore-%s.desktop" % aid)
    with open(p + ".tmp", "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(p + ".tmp", p)
    return p


def deactivate(app_id, st_app):
    for ln in st_app.get("links", []):
        if os.path.islink(ln):
            os.unlink(ln)
    for p in [os.path.join(C.apps, "sbstore-%s.desktop" % app_id)]:
        if os.path.exists(p):
            os.unlink(p)
    for sub in ("scalable", "256x256"):
        for ext in (".svg", ".png"):
            p = os.path.join(C.icons, sub, "apps", "sbstore-%s%s" % (app_id, ext))
            if os.path.exists(p):
                os.unlink(p)


# ==== source builds ============================================================================================
BUILD_STEPS = {
    "meson": ["meson setup _sbbuild --prefix=\"$PREFIX\" --buildtype=release --libdir=lib $OPTIONS",
              "ninja -C _sbbuild -j $JOBS", "ninja -C _sbbuild install"],
    "cmake": ["cmake -S . -B _sbbuild -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\"$PREFIX\" "
              "-DCMAKE_INSTALL_LIBDIR=lib $OPTIONS", "cmake --build _sbbuild -j $JOBS", "cmake --install _sbbuild"],
    "autotools": ["[ -x ./configure ] || autoreconf -fi", "./configure --prefix=\"$PREFIX\" $OPTIONS",
                  "make -j $JOBS", "make install"],
    "make": ["make -j $JOBS PREFIX=\"$PREFIX\" $OPTIONS", "make install PREFIX=\"$PREFIX\" $OPTIONS"],
    "cargo": ["cargo install --path . --root \"$PREFIX\" --locked -j $JOBS $OPTIONS || "
              "cargo install --path . --root \"$PREFIX\" -j $JOBS $OPTIONS"],
    "go": ["mkdir -p \"$PREFIX/bin\" && go build -trimpath -o \"$PREFIX/bin/\" $OPTIONS ./..."],
    "python": ["python3 -m venv \"$PREFIX\"", "\"$PREFIX/bin/pip\" install --no-cache-dir $OPTIONS ."],
    "script": [],
    "debian": [],                                   # Debian's own source: _install_debian_source
}


def build_progress(prog, build_min):
    """ninja "[n/m]", cmake "[ 42%]", else elapsed time against the recipe's estimate"""
    t0 = time.monotonic()
    est = max(1.0, float(build_min or 10)) * 60

    def cb(line):
        m = re.match(r"^\[(\d+)/(\d+)\]", line)
        if m and int(m.group(2)):
            prog.report(int(m.group(1)) / int(m.group(2)), msg=line[:60])
            return
        m = re.match(r"^\[\s*(\d+)%\]", line)
        if m:
            prog.report(int(m.group(1)) / 100.0, msg=line[:60])
            return
        el = time.monotonic() - t0
        prog.report(min(0.95, el / est), msg=line[:60])
    return cb


def build_source(app, rel, prefix, prog, cancel=None):
    S = app["source"]
    aid = app["id"]
    bdir = os.path.join(C.state_dir, "build", "%s-%s" % (aid, safe_ver(rel["version"])))
    shutil.rmtree(bdir, ignore_errors=True)
    os.makedirs(bdir)
    srcd = os.path.join(bdir, "src")
    # fetch
    prog.stage("downloading", 0, 15)
    if rel.get("tarball"):
        tb = os.path.join(C.state_dir, "dl", "%s-%s-src.tar" % (aid, safe_ver(rel["version"])))
        h = download(rel["tarball"], tb, sha256=rel.get("tarball_sha256"), prog=prog, cancel=cancel)
        unpack_tar(tb, srcd, strip=1)
        os.unlink(tb)
        src_sha = h
    elif S.get("git"):
        rc, out = run(["git", "clone", "--depth", "1", "--recurse-submodules", "--shallow-submodules", "-b",
                       rel["tag"], S["git"], srcd], logto=_logp(aid), cancel=cancel, timeout=3600)
        if rc:
            raise StoreError("fetch failed", "git clone: %s" % out[-200:])
        src_sha = None
    else:
        raise StoreError("bad recipe", "%s: [source] has no tarball or git" % aid)
    # build deps
    prog.stage("build deps", 15, 30)
    deps = list(S.get("build_deps") or [])
    added = apt_install(deps, aid, prog, cancel, auto=True)
    # build
    prog.stage("building", 30, 95)
    system = S.get("system", "script")
    steps = list(BUILD_STEPS.get(system, [])) + list(S.get("steps") or [])
    if not steps:
        raise StoreError("bad recipe", "%s: nothing to build (system %s)" % (aid, system))
    envx = {"PREFIX": prefix, "JOBS": str(C.jobs), "OPTIONS": " ".join(shlex.quote(o) for o in S.get("options", [])),
            "VERSION": rel["version"], "CARGO_HOME": os.path.join(C.state_dir, "cargo"),
            "GOPATH": os.path.join(C.state_dir, "go"), "GOFLAGS": "-modcacherw",
            "PATH": os.environ.get("PATH", "/usr/bin:/bin") + ":" + os.path.join(C.state_dir, "cargo", "bin")}
    envx.update(S.get("env") or {})
    script = "set -e\n" + "\n".join(steps) + "\n"
    rc, out = run(["sh", "-c", script], logto=_logp(aid), cwd=srcd, envx=envx, cancel=cancel,
                  line_cb=build_progress(prog, S.get("build_min")))
    shutil.rmtree(bdir, ignore_errors=True)
    if rc:
        raise StoreError("build failed", "build exited %d (log: /var/log/sbstore/%s.log): %s" % (rc, aid, out[-300:]))
    return added, src_sha


# ==== the operations ===========================================================================================
def _app(cat, aid):
    a = cat.get(aid)
    if not a:
        raise StoreError("no such app", "no app %r in the catalog" % aid)
    return a


def installed_version(st, aid):
    a = st["apps"].get(aid)
    return a and a.get("version")


def op_install(cat, aid, method=None, prog=NOPROG, cancel=None, update=False):
    global _applog
    app = _app(cat, aid)
    method = method or (load_state()["apps"].get(aid, {}).get("method") if update else None) or default_method(app)
    if method not in app_methods(app):
        raise StoreError("no such method", "%s can't be installed by %s (has: %s)" %
                         (aid, method, ", ".join(app_methods(app))))
    _applog = aid
    with Lock():
        st = load_state()
        cur = st["apps"].get(aid)
        parked = None
        opt_kind = lambda m, rec=None: m in ("release", "source") and not (rec or {}).get("debian_source") and \
            (app.get(m) or {}).get("system") != "debian" and (app.get(m) or {}).get("format") != "deb"  # noqa: E731
        if cur and cur.get("method") != method:
            if opt_kind(cur["method"], cur) and opt_kind(method) and os.path.isdir(app_dir(aid)):
                # /opt to /opt (release <-> source): park the working install; it comes back if the new one fails
                log("switching %s from %s to %s (the %s install is kept until the new one works)" %
                    (aid, cur["method"], method, cur["method"]))
                deactivate(aid, cur)
                parked = (app_dir(aid) + ".switch", cur)
                shutil.rmtree(parked[0], ignore_errors=True)
                os.replace(app_dir(aid), parked[0])
                del st["apps"][aid]
                save_state(st)
            else:
                log("switching %s from %s to %s: removing the %s install first" % (aid, cur["method"], method,
                                                                                   cur["method"]))
                _remove_locked(cat, st, aid, prog, keep_refs=True)
            cur = None
        try:
            return _install_locked(app, st, aid, method, cur, prog, cancel, update)
        except BaseException:
            if parked:                                   # give the old install back
                shutil.rmtree(app_dir(aid), ignore_errors=True)
                os.replace(parked[0], app_dir(aid))
                st = load_state()
                st["apps"][aid] = parked[1]
                pspec = app.get(parked[1]["method"]) or {}
                try:
                    parked[1]["links"] = activate(app, parked[1]["dir"], pspec.get("bin") or [], parked[1]["method"])
                except StoreError as e:
                    log("restoring %s: %s" % (aid, e.detail), aid)
                save_state(st)
                log("the switch failed: %s %s by %s is back" % (aid, parked[1]["version"], parked[1]["method"]), aid)
            raise
        finally:
            if parked and os.path.isdir(parked[0]):
                shutil.rmtree(parked[0], ignore_errors=True)


def _install_locked(app, st, aid, method, cur, prog, cancel, update):
    """op_install's body, under the lock"""
    if True:
        log("%s %s by %s" % ("update" if update else "install", aid, method))
        if method == "distro":
            return _install_distro(app, st, prog, cancel, update)
        if method == "source" and app["source"].get("system") == "debian":
            return _install_debian_source(app, st, prog, cancel)
        prog.stage("resolving", 0, 2)
        rel = resolve(app, method)
        rel["id"] = aid
        ver = safe_ver(rel["version"])
        st["latest"].setdefault(aid, {})[method] = {"version": rel["version"], "checked": int(time.time()),
                                                    "from": rel.get("from")}
        if cur and not newer(rel["version"], cur["version"]) and os.path.isdir(os.path.join(app_dir(aid), ver)):
            save_state(st)
            prog.event(ev="done", msg="%s %s is already the newest (%s)" % (aid, cur["version"], method))
            return st["apps"][aid]
        spec = app.get(method)
        vdir = os.path.join(app_dir(aid), ver)
        tmpd = os.path.join(app_dir(aid), ".new-" + ver)
        shutil.rmtree(tmpd, ignore_errors=True)
        refs_before = {p: list(r) for p, r in st["apt_refs"].items()}
        added = []
        try:
            if method == "release":
                pick = pick_asset(rel, spec)
                if not pick:
                    raise StoreError("no aarch64 release", "no asset matches %s" % spec["asset"])
                asset = pick[0]
                dl_mb = (asset.get("size") or 0) >> 20
                need_space(dl_mb + int(spec.get("size_mb", max(dl_mb * 3, 20)) * 1.2), "%s %s" % (aid, ver))
                sha, how = expected_sha(rel, asset, spec.get("verify", {}), rel["version"])
                prog.stage("runtime", 2, 10)
                added += apt_install((app.get("runtime") or {}).get("apt", []), aid, prog, cancel, auto=True)
                added += _recommends(app, st, prog, cancel)
                refs_add(st, added, aid)
                prog.stage("downloading", 10, 80)
                dl = os.path.join(C.state_dir, "dl", aid, asset["name"])
                got = download(asset["url"], dl, size=asset.get("size") or 0, sha256=sha, prog=prog, cancel=cancel)
                prog.stage("verifying", 80, 83)
                how += check_file_signature(rel, asset, spec.get("verify", {}), rel["version"], dl)
                prog.report(1.0, msg=how)
                prog.stage("installing", 83, 98)
                fmt = spec.get("format", "tar")
                if fmt == "deb":
                    _install_debs(app, dl, st, prog, cancel)
                    os.makedirs(tmpd, exist_ok=True)
                else:
                    unpack(dl, fmt, tmpd, strip=spec.get("strip", 0), binpath=(spec.get("bin") or [None])[0])
                with open(os.path.join(tmpd, ".sbstore.json"), "w") as f:
                    json.dump({"id": aid, "version": rel["version"], "method": method, "asset": asset["name"],
                               "sha256": got, "verified": how}, f)
                if os.path.isdir(vdir):
                    shutil.rmtree(vdir)
                os.replace(tmpd, vdir)
                bins = spec.get("bin") or []
                verified = how
                os.unlink(dl)
                try:
                    os.rmdir(os.path.dirname(dl))
                except OSError:
                    pass
            else:   # source
                S = spec
                need_space(int(S.get("build_mb", 600)) + int(S.get("size_mb", 100)), "building %s" % aid)
                prog.stage("runtime", 0, 2)
                added += apt_install((app.get("runtime") or {}).get("apt", []), aid, prog, cancel, auto=True)
                added += _recommends(app, st, prog, cancel)
                refs_add(st, added, aid)
                if os.path.isdir(vdir):
                    shutil.rmtree(vdir)
                os.makedirs(vdir)
                badded, src_sha = build_source(app, rel, guest_path(vdir), prog, cancel)
                if C.keep_build_deps:
                    refs_add(st, badded, aid)
                else:
                    try:
                        apt_remove([p for p in badded if p not in st["apt_refs"]], aid, guard=True)
                    except StoreError as e:
                        log("build deps kept: %s" % e.detail, aid)
                with open(os.path.join(vdir, ".sbstore.json"), "w") as f:
                    json.dump({"id": aid, "version": rel["version"], "method": method, "source_sha256": src_sha,
                               "built": int(time.time())}, f)
                bins = S.get("bin") or []
                verified = "built from source" + (" (tarball sha256 %s…)" % src_sha[:12] if src_sha else "")
            prog.stage("installing", 98, 100)
            prev = cur["version"] if cur else None
            prev_dir = safe_ver(prev) if prev else None
            set_current(aid, ver)
            links = activate(app, ver, bins, method)
        except BaseException as e:
            shutil.rmtree(tmpd, ignore_errors=True)
            if not cur:
                shutil.rmtree(vdir, ignore_errors=True)
                # give back what this attempt added
                newly = [p for p in st["apt_refs"] if p not in refs_before]
                st["apt_refs"] = refs_before
                if newly:
                    try:
                        apt_remove(newly, aid)
                    except StoreError:
                        pass
                try:
                    os.rmdir(app_dir(aid))
                except OSError:
                    pass
            save_state(st)
            if isinstance(e, StoreError):
                log("FAILED %s: %s" % (aid, e.detail))
            raise
        # one version back is kept for rollback; older ones go
        keep = {ver}
        if prev_dir and prev_dir != ver:
            keep.add(prev_dir)
        for d in os.listdir(app_dir(aid)):
            if d not in keep and d not in ("current",) and not d.startswith("."):
                shutil.rmtree(os.path.join(app_dir(aid), d), ignore_errors=True)
        debn = st.pop("_deb_names", {}).get(aid)
        rec = {"method": method, "version": rel["version"], "dir": ver, "links": links,
               "installed_at": int(time.time()), "verified": verified, "from": rel.get("from"),
               "prev": prev if prev_dir and prev_dir != ver else None, "prev_dir": prev_dir if prev_dir != ver else None}
        if debn:
            rec["packages"] = debn
        miss = st.pop("_missing", {}).get(aid)
        if miss:
            rec["missing"] = miss
        st["apps"][aid] = rec
        st["prefs"][aid] = method
        save_state(st)
        log("%s %s %s by %s: done (%s)" % ("updated" if cur else "installed", aid, rel["version"], method, verified))
        prog.event(ev="done", msg="%s %s installed (%s; %s)" % (app["name"], rel["version"], method, verified))
        return rec


def _recommends(app, st, prog, cancel):
    """runtime.recommends: best effort (an offline phone may not have them); what is missing is noted"""
    rec = (app.get("runtime") or {}).get("recommends") or []
    if not rec:
        return []
    try:
        return apt_install(rec, app["id"], prog, cancel, auto=True)
    except StoreError as e:
        if isinstance(e, NeedsNet) or "not available" in e.short:
            st.setdefault("_missing", {})[app["id"]] = rec
            log("recommended %s not installed (%s): the app works without them" % (" ".join(rec), e.detail), app["id"])
            return []
        raise


def _install_debs(app, path, st, prog, cancel):
    """format = "deb": one .deb, or a tarball of .debs (LibreOffice): apt installs them (dependencies resolved)"""
    aid = app["id"]
    debs = []
    if path.endswith(".deb"):
        debs = [path]
    else:
        x = path + ".x"
        shutil.rmtree(x, ignore_errors=True)
        unpack_tar(path, x)
        for dp, dn, fn in os.walk(x):
            debs += [os.path.join(dp, f) for f in fn if f.endswith(".deb")]
    if not debs:
        raise StoreError("unpack failed", "no .deb inside %s" % os.path.basename(path))
    names = []
    for d in debs:
        out = subprocess.run(C.dpkg_deb + ["-f", d, "Package"], capture_output=True, text=True).stdout.strip()
        if out:
            names.append(out)
    rc, out = run(C.apt + ["-y", "-q", "--allow-downgrades", "-o", "APT::Status-Fd=@STATUSFD@", "install"] +
                  [os.path.abspath(d) for d in debs], logto=_logp(aid), status_fd_cb=apt_progress_cb(prog), cancel=cancel)
    if rc:
        raise StoreError("install failed", "apt-get install (debs) exited %d: %s" % (rc, out[-300:]))
    old = [p for p in (st["apps"].get(aid) or {}).get("packages", []) if p not in names]
    refs_add(st, names, aid)
    if old:                                             # the previous version's packages (other names)
        try:
            apt_remove(old, aid, prog)
            for p in old:
                if st["apt_refs"].get(p) == [aid]:
                    del st["apt_refs"][p]
        except StoreError as e:
            log("previous packages kept: %s" % e.detail, aid)
    st.setdefault("_deb_names", {})[aid] = names
    if not path.endswith(".deb"):
        shutil.rmtree(path + ".x", ignore_errors=True)


def _install_distro(app, st, prog, cancel, update):
    aid = app["id"]
    pkgs = app["distro"]["apt"]
    prog.stage("resolving", 0, 2)
    if net_up():
        apt_update(aid)
    try:
        rel = resolve_distro(app)
    except StoreError:
        rel = {"version": "?", "from": "apt"}
    prog.stage("installing", 2, 100)
    inst = dpkg_installed()
    if update:
        targets = [p for p in pkgs if p in inst]
        rc, out = run(C.apt + ["-y", "-q", "--only-upgrade", "-o", "APT::Status-Fd=@STATUSFD@", "install"] + targets,
                      logto=_logp(aid), status_fd_cb=apt_progress_cb(prog), cancel=cancel)
        if rc:
            raise StoreError("update failed", "apt-get upgrade exited %d: %s" % (rc, out[-300:]))
        added = []
    else:
        added = apt_install(pkgs, aid, prog, cancel)
        refs_add(st, added, aid)
    ver = dpkg_installed().get(pkgs[0], rel.get("version"))
    links = []
    if app.get("launcher"):
        write_launcher(app, None, "distro")
    rec = {"method": "distro", "version": ver, "packages": pkgs, "links": links, "installed_at": int(time.time()),
           "verified": "Debian signatures (apt)", "from": rel.get("from")}
    if st["apps"].get(aid):
        rec["installed_at"] = st["apps"][aid].get("installed_at", rec["installed_at"])
    st["apps"][aid] = rec
    st["prefs"][aid] = "distro"
    st["latest"].setdefault(aid, {})["distro"] = {"version": rel.get("version"), "checked": int(time.time()), "from": "apt"}
    save_state(st)
    log("installed %s %s by distro" % (aid, ver))
    prog.event(ev="done", msg="%s %s installed (Debian package)" % (app["name"], ver))
    return rec


DEB_SRC = """Types: deb-src
URIs: http://deb.debian.org/debian
Suites: trixie trixie-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
"""


def _install_debian_source(app, st, prog, cancel):
    """source.system = "debian": Debian's own source of the package, built on the M3s with dpkg-buildpackage
    (always matches trixie's libraries), then installed with apt; the build dependencies go again afterwards"""
    aid = app["id"]
    S = app["source"]
    pkgs = (app.get("distro") or {}).get("apt") or [S.get("package")]
    srcpkg = S.get("package") or pkgs[0]
    if not net_up():
        raise NeedsNet("needs the network", "Debian source builds fetch the source and build deps from Debian")
    need_space(int(S.get("build_mb", 1500)), "building %s" % aid)
    prog.stage("resolving", 0, 3)
    srcl = rp("/etc/apt/sources.list.d/sbstore-src.sources")
    if not os.path.exists(srcl):
        os.makedirs(os.path.dirname(srcl), exist_ok=True)
        with open(srcl, "w") as f:
            f.write(DEB_SRC)
    rc, out = run(C.apt + ["-q", "update"], logto=_logp(aid), timeout=900, cancel=cancel)
    if rc:
        raise NeedsNet("needs the network", "apt-get update: %s" % out[-200:])
    cur = st["apps"].get(aid)
    if cur and cur.get("debian_source"):
        cand = resolve_distro(app)["version"]
        if not newer(cand, cur["version"]):
            prog.event(ev="done", msg="%s %s is already the newest (Debian source)" % (aid, cur["version"]))
            return cur
    before = set(dpkg_installed())
    prog.stage("build deps", 3, 25)
    rc, out = run(C.apt + ["-y", "-q", "--no-install-recommends", "-o", "APT::Status-Fd=@STATUSFD@", "install",
                           "build-essential", "dpkg-dev", "fakeroot"], logto=_logp(aid),
                  status_fd_cb=apt_progress_cb(prog), cancel=cancel)
    if not rc:
        rc, out = run(C.apt + ["-y", "-q", "--no-install-recommends", "-o", "APT::Status-Fd=@STATUSFD@",
                               "build-dep", srcpkg], logto=_logp(aid), status_fd_cb=apt_progress_cb(prog), cancel=cancel)
    if rc:
        raise StoreError("build deps failed", "apt-get build-dep %s: %s" % (srcpkg, out[-300:]))
    bdir = os.path.join(C.state_dir, "build", aid + "-deb")
    shutil.rmtree(bdir, ignore_errors=True)
    os.makedirs(bdir)
    try:
        prog.stage("downloading", 25, 30)
        rc, out = run(C.apt + ["-q", "source", srcpkg], logto=_logp(aid), cwd=bdir, cancel=cancel, timeout=3600)
        if rc:
            raise StoreError("fetch failed", "apt-get source %s: %s" % (srcpkg, out[-300:]))
        tree = [d for d in os.listdir(bdir) if os.path.isdir(os.path.join(bdir, d))]
        if not tree:
            raise StoreError("fetch failed", "apt-get source left no tree")
        prog.stage("building", 30, 92)
        rc, out = run(["dpkg-buildpackage", "-b", "-uc", "-us", "-j%d" % C.jobs], logto=_logp(aid),
                      cwd=os.path.join(bdir, tree[0]), cancel=cancel,
                      envx={"DEB_BUILD_OPTIONS": "nocheck nodoc parallel=%d" % C.jobs},
                      line_cb=build_progress(prog, S.get("build_min")))
        if rc:
            raise StoreError("build failed", "dpkg-buildpackage exited %d (log: /var/log/sbstore/%s.log): %s" %
                             (rc, aid, out[-300:]))
        debs = {}
        for f in os.listdir(bdir):
            if f.endswith(".deb"):
                debs[f.split("_")[0]] = os.path.join(bdir, f)
        want = [debs[p] for p in pkgs if p in debs]
        if not want:
            raise StoreError("build failed", "the build made none of %s" % " ".join(pkgs))
        prog.stage("installing", 92, 99)
        rc, out = run(C.apt + ["-y", "-q", "--allow-downgrades", "--reinstall", "-o", "APT::Status-Fd=@STATUSFD@",
                               "install"] + want, logto=_logp(aid), status_fd_cb=apt_progress_cb(prog), cancel=cancel)
        if rc:
            raise StoreError("install failed", "apt-get install (built debs): %s" % out[-300:])
        ver = subprocess.run(C.dpkg_deb + ["-f", want[0], "Version"], capture_output=True, text=True).stdout.strip()
    finally:
        shutil.rmtree(bdir, ignore_errors=True)
        if not C.keep_build_deps:                       # what the build brought, minus what the app needs
            after = set(dpkg_installed())
            extra = sorted(after - before - set(pkgs) - set(st["apt_refs"]))
            needed = set()
            if extra:
                rc, out = run(C.apt + ["-s", "-q", "remove"] + extra, logto=_logp(aid))
                needed = set(re.findall(r"^Remv (\S+)", out, re.M)) - set(extra)
            if extra and not (needed & set(pkgs)):
                try:
                    apt_remove(extra, aid)
                except StoreError as e:
                    log("build deps kept: %s" % e.detail, aid)
    refs_add(st, [p for p in pkgs if p not in before], aid)
    rec = {"method": "source", "version": ver or "?", "packages": pkgs, "links": [], "debian_source": srcpkg,
           "installed_at": int(time.time()), "verified": "built from Debian source (signed .dsc)", "from": "net"}
    st["apps"][aid] = rec
    st["prefs"][aid] = "source"
    save_state(st)
    if app.get("launcher"):
        write_launcher(app, None, "source")
    log("built and installed %s %s from Debian source" % (aid, ver))
    prog.event(ev="done", msg="%s %s built from Debian source" % (app["name"], ver))
    return rec


def op_update(cat, aid, prog=NOPROG, cancel=None):
    st = load_state()
    cur = st["apps"].get(aid)
    if not cur:
        raise StoreError("not installed", "%s is not installed" % aid)
    return op_install(cat, aid, cur["method"], prog, cancel, update=True)


def op_rollback(cat, aid, prog=NOPROG):
    app = _app(cat, aid)
    with Lock():
        st = load_state()
        cur = st["apps"].get(aid)
        if not cur:
            raise StoreError("not installed", "%s is not installed" % aid)
        if cur["method"] == "distro" or cur.get("debian_source") or (app.get(cur["method"]) or {}).get("format") == "deb":
            raise StoreError("no rollback", "package-based installs roll back with apt, not the store")
        pd = cur.get("prev_dir")
        if not pd or not os.path.isdir(os.path.join(app_dir(aid), pd)):
            raise StoreError("no older version", "%s: no previous version is kept" % aid)
        prog.stage("installing", 0, 100)
        set_current(aid, pd)
        spec = app.get(cur["method"]) or {}
        links = activate(app, pd, spec.get("bin") or [], cur["method"])
        cur.update(version=cur["prev"], dir=pd, prev=cur["version"], prev_dir=cur["dir"], links=links,
                   rolled_back_at=int(time.time()))
        save_state(st)
        log("rolled %s back to %s" % (aid, cur["version"]), aid)
        prog.event(ev="done", msg="%s rolled back to %s" % (app["name"], cur["version"]))
        return cur


def _remove_locked(cat, st, aid, prog, keep_refs=False):
    cur = st["apps"].get(aid)
    if not cur:
        raise StoreError("not installed", "%s is not installed" % aid)
    prog.stage("removing", 0, 100)
    deactivate(aid, cur)
    if cur["method"] == "distro" or cur.get("debian_source"):
        app = cat.get(aid) or {"distro": {"apt": cur.get("packages", [])}}
        app = app if app.get("distro") else dict(app, distro={"apt": cur.get("packages", [])})
        apt_remove(app["distro"]["apt"][:1] + [p for p in app["distro"]["apt"][1:]
                                               if st["apt_refs"].get(p) == [aid]], aid, prog)
        for p in app["distro"]["apt"]:
            if st["apt_refs"].get(p) == [aid]:
                del st["apt_refs"][p]
    shutil.rmtree(app_dir(aid), ignore_errors=True)
    shutil.rmtree(os.path.join(C.state_dir, "dl", aid), ignore_errors=True)
    if not keep_refs:
        refs_drop(st, aid, prog)
    del st["apps"][aid]
    save_state(st)


def op_remove(cat, aid, prog=NOPROG):
    global _applog
    _applog = aid
    with Lock():
        st = load_state()
        name = (cat.get(aid) or {}).get("name", aid)
        _remove_locked(cat, st, aid, prog)
        log("removed %s" % aid)
        prog.event(ev="done", msg="%s removed" % name)


def op_check(cat, prog=NOPROG, ids=None):
    """the newest version of every installed app (its method) and of each method of every app (cheap ones)"""
    st = load_state()
    ups = []
    online = net_up(force=True)
    if online and any(a.get("method") == "distro" for a in st["apps"].values()):
        apt_update()
    todo = ids or sorted(a for a in cat if a in st["apps"])
    for i, aid in enumerate(todo):
        app = cat[aid]
        prog.report(i / max(1, len(todo)), msg=aid)
        for m in app_methods(app):
            inst = st["apps"].get(aid)
            if not ids and not (inst and inst["method"] == m):
                continue                                    # all: the installed apps' methods (spares the API)
            try:
                r = resolve(app, m)
                st["latest"].setdefault(aid, {})[m] = {"version": r["version"], "checked": int(time.time()),
                                                       "from": r.get("from")}
            except StoreError as e:
                log("check %s %s: %s" % (aid, m, e.detail))
        inst = st["apps"].get(aid)
        if inst:
            lv = st["latest"].get(aid, {}).get(inst["method"], {}).get("version")
            if lv and newer(lv, inst["version"]):
                ups.append((aid, inst["version"], lv))
    st["checked_at"] = int(time.time())
    st["checked_online"] = online
    save_state(st)
    return ups


# ==== the shared tables (daemon) ===============================================================================
SBSHM_STORE_OFF = 0x1020000
SBSHM_STORE_TEXT_OFF = 0x102A000                   # sbshm_store3.h: the descriptions (24 KiB)
SBSHM_STORE_TEXT_SIZE = 0x6000
SBSHM_STORE3_OFF = 0x1030000
SBSHM_ICON_OFF = 0x1040000
SBSHM_BLK_OFF = 0x2000000
SBSHM_ICON_PX = 160
SBSHM_ICON_BYTES = SBSHM_ICON_PX * SBSHM_ICON_PX * 4
SBSHM_ICONS_CAT = 64
SBSHM_ICONS_MAX = 160
SBSHM_PKGS_MAX = 160
SBSHM_NONE = 0xFFFFFFFF
# sbshm_pkg.state
PKG_IDLE, PKG_QUEUED, PKG_INSTALLING, PKG_REMOVING, PKG_FAILED = 0, 1, 2, 3, 4
PKG_DOWNLOADING, PKG_BUILDING, PKG_VERIFYING = 5, 6, 7         # v3 (sbshm_pkg3.phase; sbshm_pkg.state stays <= 4)
OP_INSTALL, OP_REMOVE, OP_CANCEL, OP_UPDATE, OP_ROLLBACK, OP_CHECK = 1, 2, 3, 4, 5, 6
F_INSTALLED, F_SYSTEM, F_TOO_BIG, F_NO_SPACE, F_FEATURED = 1, 2, 4, 8, 16
F_UPDATE, F_ROLLBACK, F_NEEDS_NET = 32, 64, 128
PKG_FMT = "<40s32s96s16s32s8I"                      # struct sbshm_pkg, 248 bytes
STORE_HDR_FMT = "<16I"                              # seq n overlay_total_kb overlay_free_kb repo_ok pad[11]
PKG3_FMT = "<16I24s24s24s24s24s24s48s"              # struct sbshm_pkg3, 256 bytes
STORE3_HDR_FMT = "<16I"                             # magic version n net_ok mirror_ok checked_at text_used pad[9]
STORE3_MAGIC = 0x33525453                           # "STR3"
assert struct.calcsize(PKG_FMT) == 248 and struct.calcsize(PKG3_FMT) == 256
NEEDS_BITS = {"network": 1, "audio": 2, "gpu": 4, "camera": 8, "files": 16, "keyboard": 32}
TOUCH = {"poor": 1, "ok": 2, "good": 3}
PHASE_STATE = {"downloading": PKG_DOWNLOADING, "verifying": PKG_VERIFYING, "building": PKG_BUILDING,
               "build deps": PKG_INSTALLING, "runtime": PKG_INSTALLING, "installing": PKG_INSTALLING,
               "resolving": PKG_INSTALLING, "removing": PKG_REMOVING}


def _b(s, n):
    b = (s or "").encode("utf-8")[:n - 1]
    while b and (b[-1] & 0xC0) == 0x80:               # never cut a UTF-8 character in half
        b = b[:-1]
    if b and b[-1] >= 0xC0:
        b = b[:-1]
    return b


class Shm:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        size = os.fstat(self.fd).st_size
        if size and size < SBSHM_BLK_OFF:              # a test file: make it big enough
            os.ftruncate(self.fd, SBSHM_BLK_OFF)
        # the store region only (an offset map); a /dev/sbshm that refuses offsets (sbguest.ko before Track E's
        # offset support: EINVAL) is mapped from 0 to the block rings instead, as sbpkg does
        try:
            if env("SBSTORE_SHM_NO_OFFSET") == "1":
                raise OSError(errno.EINVAL, "offset maps disabled (test)")
            self.m = mmap.mmap(self.fd, SBSHM_BLK_OFF - SBSHM_STORE_OFF, offset=SBSHM_STORE_OFF)
            self.base = SBSHM_STORE_OFF
        except OSError as e:
            log("offset map of %s refused (%s): mapping it from 0" % (path, e))
            self.m = mmap.mmap(self.fd, SBSHM_BLK_OFF, offset=0)
            self.base = 0
        self.seq_off = SBSHM_STORE_OFF - self.base
        self.seq = struct.unpack_from("<I", self.m, self.seq_off)[0] & ~1

    def _w(self, off, data):
        """8-byte word stores into the write-combined region (like sbpkg's wcopy)"""
        o = off - self.base
        n = len(data) & ~7
        if n:
            mv = memoryview(self.m)[o:o + n].cast("Q")
            mv[:] = memoryview(data)[:n].cast("Q")
            mv.release()
        if len(data) > n:
            self.m[o + n:o + len(data)] = data[n:]

    def publish(self, store_hdr, pkgs, hdr3, pkgs3, text, icons):
        """the store seqlock: odd, the tables (and icons), even. Returns the new seq."""
        self.seq |= 1
        struct.pack_into("<I", self.m, self.seq_off, self.seq)
        for i, px in icons:
            self._w(SBSHM_ICON_OFF + (SBSHM_ICONS_CAT + i) * SBSHM_ICON_BYTES, px)
        body = struct.pack(STORE_HDR_FMT, 0, *store_hdr)[4:] + b"".join(pkgs)
        self._w(SBSHM_STORE_OFF + 4, body + b"\0" * ((8 - len(body) % 8) % 8))
        self._w(SBSHM_STORE_TEXT_OFF, text[:SBSHM_STORE_TEXT_SIZE].ljust(SBSHM_STORE_TEXT_SIZE, b"\0")[:SBSHM_STORE_TEXT_SIZE])
        self._w(SBSHM_STORE3_OFF, struct.pack(STORE3_HDR_FMT, *hdr3) + b"".join(pkgs3))
        self.seq += 1
        struct.pack_into("<I", self.m, self.seq_off, self.seq)
        return self.seq


def load_icon_argb(app, catdir):
    """160x160 premultiplied ARGB (what the icon pool holds): <catalog dir>/icons/<id>.argb, or the old repo's"""
    for p in (os.path.join(catdir, "icons", app["id"] + ".argb"),
              os.path.join(C.repo, "store", "icons", (app.get("_repo") or {}).get("key", app["id"]) + ".argb")):
        try:
            with open(p, "rb") as f:
                b = f.read()
            if len(b) == SBSHM_ICON_BYTES:
                return b
        except OSError:
            pass
    return None


class Daemon:
    """sbpkg's role, with three methods: the table, the socket client, a queue, one job at a time (a child)"""

    def __init__(self):
        self.cat = load_catalog()
        self.order = self._order()
        self.idx = {aid: i for i, aid in enumerate(self.order)}
        self.live = {}           # idx -> {"state", "pct", "phase", "err", "done_kb", "total_kb", "eta"}
        self.queue = []          # (idx, op, method)
        self.job = None          # {"idx", "op", "p": Popen, "buf", "t0"}
        self.shm = None
        self.sock = None
        self.rbuf = b""
        self.next_conn = 0.0
        self.icons_done = False
        self.layout_seq = 0
        self.dirty = True
        self.last_check = 0.0
        self.catdir = catalog_dir()
        self.net = {"up": False, "why": "not checked"}
        self.net_changed = False
        self.net_kick = threading.Event()
        self.aptup = None

    # ---- the network, watched in a thread (a DNS lookup can block for many seconds: never in the loop)
    def _net_loop(self):
        while True:
            up, why = net_probe()
            _net.update(t=time.monotonic(), up=up, why=why)
            if up != self.net["up"] or why != self.net["why"]:
                self.net = {"up": up, "why": why}
                self.net_changed = True
            self.net_kick.wait(60 if up else 10)
            self.net_kick.clear()

    def on_net_change(self):
        self.net_changed = False
        up, why = self.net["up"], self.net["why"]
        log("network %s" % ("up: the store can download releases and source" if up else
                            "down (%s): releases and source builds use the memory card's copy" % why))
        if up and ensure_net_sources():
            self.apt_update_bg()
        self.dirty = True

    def apt_update_bg(self):
        if self.aptup is not None and self.aptup.poll() is None:
            return
        try:                                            # jobs wait for it, the table does not
            self.aptup = subprocess.Popen(C.apt + ["-q"] + APT_NET_OPTS + ["update"], stdin=subprocess.DEVNULL,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                          start_new_session=True)
            self.aptup_t0 = time.monotonic()
        except OSError as e:
            log("apt-get update: %s" % e)

    def _order(self):
        feat = sorted((a for a in self.cat.values() if a.get("featured")), key=lambda a: a["name"].lower())
        rest = sorted((a for a in self.cat.values() if not a.get("featured")),
                      key=lambda a: (a.get("category", ""), a["name"].lower()))
        return [a["id"] for a in feat + rest][:SBSHM_PKGS_MAX]

    # ---- the tables
    def tables(self):
        st = load_state()
        inst_deb = dpkg_installed()
        base = base_packages()
        try:
            sv = os.statvfs(ROOT)
            ov_total = sv.f_blocks * sv.f_frsize // 1024
            ov_free = sv.f_bavail * sv.f_frsize // 1024
        except OSError:
            ov_total = ov_free = 0
        try:
            with open("/proc/meminfo") as f:
                mem_mb = int(re.search(r"MemTotal:\s+(\d+)", f.read()).group(1)) // 1024
        except (OSError, AttributeError):
            mem_mb = 1500
        mem_mb = int(env("SBSTORE_MEM_MB", mem_mb))
        ram_limit = mem_mb - 600 if mem_mb > 900 else 300
        net = self.net["up"] if hasattr(self, "net") else net_up()
        pkgs, pkgs3 = [], []
        text = b""
        hints = {}
        for i, lv in self.live.items():                     # the failures' what-to-do sentences first (few, short)
            h = _b(lv.get("hint", ""), 400)
            if h and len(text) + len(h) + 1 <= SBSHM_STORE_TEXT_SIZE:
                hints[i] = (len(text), len(h))
                text += h + b"\0"
        for i, aid in enumerate(self.order):
            a = self.cat[aid]
            ms = app_methods(a)
            ia = st["apps"].get(aid)
            lv = self.live.get(i, {})
            flags = F_FEATURED if a.get("featured") else 0
            dpk = (a.get("distro") or {}).get("apt") or []
            if ia:
                flags |= F_INSTALLED
            elif dpk and all(p in inst_deb for p in dpk):
                flags |= F_INSTALLED
                if all(p in base for p in dpk):
                    flags |= F_SYSTEM
            if (a.get("ram_mb") or 0) > ram_limit:
                flags |= F_TOO_BIG
            sel = (ia or {}).get("method") or st["prefs"].get(aid) or default_method(a)
            rel_spec = a.get("release") or {}
            src_spec = a.get("source") or {}
            repo = a.get("_repo") or {}
            if sel == "release":
                inst_kb = int(rel_spec.get("size_mb", 50)) * 1024
                dl_kb = int(rel_spec.get("size_mb", 50)) * 1024 // 3
            elif sel == "source":
                inst_kb = int(src_spec.get("size_mb", 100)) * 1024 + int(src_spec.get("build_mb", 600)) * 1024
                dl_kb = 0
            else:
                inst_kb, dl_kb = repo.get("inst_kb", 0), repo.get("dl_kb", 0)
            if not ia and inst_kb + C.reserve_mb * 1024 > ov_free:
                flags |= F_NO_SPACE
            if not net and sel in ("release", "source") and not mirror_json(aid, "release" if sel == "release" else "source"):
                flags |= F_NEEDS_NET
            lat = dict((a.get("known") or {}))
            lat = {m: {"version": v} for m, v in lat.items() if isinstance(v, str)}
            lat.update(st["latest"].get(aid, {}))
            if ia:
                lvn = lat.get(ia["method"], {}).get("version")
                if lvn and newer(lvn, ia["version"]):
                    flags |= F_UPDATE
                if ia.get("prev_dir") and os.path.isdir(os.path.join(app_dir(aid), ia["prev_dir"])):
                    flags |= F_ROLLBACK
            state = lv.get("state", PKG_IDLE)
            icon = SBSHM_ICONS_CAT + i if i < SBSHM_ICONS_MAX - SBSHM_ICONS_CAT and self._has_icon(i) else SBSHM_NONE
            pkgs.append(struct.pack(PKG_FMT, _b(aid, 40), _b(a["name"], 32), _b(a.get("summary", ""), 96),
                                    _b(a.get("category", ""), 16), _b(lv.get("err", ""), 32), dl_kb, inst_kb,
                                    int(a.get("ram_mb") or 0), flags, state if state <= PKG_FAILED else PKG_INSTALLING,
                                    lv.get("pct", 0),
                                    icon, 0))
            desc = _b(a.get("description") or a.get("summary", ""), 1000) + b"\0"
            if len(text) + len(desc) <= SBSHM_STORE_TEXT_SIZE:
                doff, dlen = len(text), len(desc) - 1
                text += desc
            else:
                doff, dlen = 0, 0
            mbits = sum(1 << M_NUM[m] for m in ms)
            needs = sum(NEEDS_BITS.get(n, 0) for n in (a.get("runtime") or {}).get("needs", []))
            if (a.get("launcher") or {}).get("keyboard"):
                needs |= NEEDS_BITS["keyboard"]
            f3 = 0
            if ia and ia.get("verified", "").startswith("sha256"):
                f3 |= 1                                     # VERIFIED
            if ia and ia.get("from") == "mirror":
                f3 |= 4                                     # from the offline mirror
            if (a.get("launcher") or {}).get("terminal"):
                f3 |= 8                                     # opens in the terminal
            hoff, hlen = hints.get(i, (0, 0))
            pkgs3.append(struct.pack(
                PKG3_FMT, mbits, M_NUM.get((ia or {}).get("method"), 0), M_NUM.get(sel, 0),
                state if state >= PKG_DOWNLOADING else (state if state else 0), lv.get("done_kb", 0),
                lv.get("total_kb", 0), lv.get("eta", 0), int(src_spec.get("build_min", 0)),
                TOUCH.get(a.get("touch"), 0), needs, f3, doff, dlen, hoff, int(rel_spec.get("size_mb", 0)), hlen,
                _b((ia or {}).get("version", ""), 24),
                _b(lat.get((ia or {}).get("method") or sel, {}).get("version", ""), 24),
                _b(lat.get("release", {}).get("version", ""), 24), _b(lat.get("distro", {}).get("version", ""), 24),
                _b(lat.get("source", {}).get("version", ""), 24), _b(a.get("licence", ""), 24),
                _b(_source_label(a), 48)))
        hdr = (len(pkgs), ov_total, ov_free, 1 if os.path.exists(os.path.join(C.repo, "Packages")) else 0) + (0,) * 11
        hdr3 = (STORE3_MAGIC, 1, len(pkgs3), 1 if net else 0, 1 if os.path.isdir(C.mirror) else 0,
                st.get("checked_at", 0), len(text),
                0 if net else NET_WHY_CODE.get(getattr(self, "net", _net)["why"], 5),
                1 if (self.job or self.queue) else 0) + (0,) * 7
        return hdr, pkgs, hdr3, pkgs3, text

    def write_apps_map(self):
        """/run/sbpkg/apps.tsv as sbpkg wrote it: store index, key, desktop id of each installed app's launchers"""
        st = load_state()
        rows = []
        for i, aid in enumerate(self.order):
            ia = st["apps"].get(aid)
            pk = (ia or {}).get("packages") or ((self.cat[aid].get("distro") or {}).get("apt") if not ia else None) or []
            if ia and os.path.exists(os.path.join(C.apps, "sbstore-%s.desktop" % aid)):
                rows.append("%d\t%s\tsbstore-%s" % (i, aid, aid))
            for p in pk:
                for suf in ("", ":arm64", ":all"):
                    lp = rp("/var/lib/dpkg/info/%s%s.list" % (p, suf))
                    if os.path.exists(lp):
                        for l in open(lp, errors="replace"):
                            m = re.match(r"^/usr/share/applications/([^/]+)\.desktop$", l.strip())
                            if m:
                                rows.append("%d\t%s\t%s" % (i, aid, m.group(1)))
                        break
        d = env("SBSTORE_RUN", "/run/sbpkg")
        try:
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "apps.tsv.tmp"), "w") as f:
                f.write("".join(r + "\n" for r in rows))
            os.replace(os.path.join(d, "apps.tsv.tmp"), os.path.join(d, "apps.tsv"))
        except OSError:
            pass

    def _has_icon(self, i):
        return self._icons.get(i) is not None if hasattr(self, "_icons") else False

    def publish(self):
        if not self.shm:
            return
        icons = []
        if not self.icons_done:
            self._icons = {}
            for i, aid in enumerate(self.order[:SBSHM_ICONS_MAX - SBSHM_ICONS_CAT]):
                px = load_icon_argb(self.cat[aid], self.catdir)
                self._icons[i] = px
                if px:
                    icons.append((i, px))
        t = self.tables()
        blob = repr(t).encode()
        if not icons and blob == getattr(self, "_last", None):
            self.dirty = False
            return
        self._last = blob
        seq = self.shm.publish(t[0], t[1], t[2], t[3], t[4], icons)
        if not self.icons_done:
            self.icons_done = True
            self.layout_seq = seq
        self.send("store %d" % seq)
        self.dirty = False

    # ---- the socket (sbwl owns the rings)
    def send(self, line):
        if not self.sock:
            return
        try:
            self.sock.sendall((line + "\n").encode())
        except OSError as e:
            log("sbwl socket: %s" % e)
            self.sock.close()
            self.sock = None

    def connect(self):
        if self.sock or time.monotonic() < self.next_conn:
            return
        self.next_conn = time.monotonic() + 2
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(C.sock)
            s.setblocking(False)
            self.sock = s
            log("connected to %s" % C.sock)
            self.dirty = True
            self._last = None                       # tell the new sbwl the seq, changed or not
        except OSError:
            pass

    def read_sock(self):
        if not self.sock:
            return
        try:
            b = self.sock.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            b = b""
        if not b:
            log("sbwl went away")
            self.sock.close()
            self.sock = None
            return
        self.rbuf += b
        while b"\n" in self.rbuf:
            l, self.rbuf = self.rbuf.split(b"\n", 1)
            p = l.decode(errors="replace").split()
            if len(p) == 4 and p[0] == "op":
                try:
                    self.handle_op(int(p[1]), int(p[2]), int(p[3]))
                except ValueError:
                    pass

    def handle_op(self, idx, opword, opseq):
        op, method = opword & 0xFF, (opword >> 8) & 0xFF
        if op == OP_CHECK:                          # a = SBSHM_NONE: every installed app; else that app's methods
            if idx != SBSHM_NONE and (opseq < self.layout_seq or idx >= len(self.order)):
                return
            if not any(q[1] == OP_CHECK and q[0] == idx for q in self.queue):
                self.queue.append((idx, OP_CHECK, 0))
            return
        if opseq < self.layout_seq or idx >= len(self.order):
            log("op %d for %d ignored (seq %d < %d, or no such entry)" % (op, idx, opseq, self.layout_seq))
            return
        if op == OP_CANCEL:
            self.queue = [q for q in self.queue if q[0] != idx]
            if self.job and self.job["idx"] == idx:
                self.job["cancelled"] = True
                try:
                    os.killpg(self.job["p"].pid, signal.SIGTERM)
                except OSError:
                    pass
            self.live[idx] = {"state": PKG_IDLE, "pct": 0}
            self.dirty = True
            return
        if any(q[0] == idx for q in self.queue) or (self.job and self.job["idx"] == idx):
            return
        self.queue.append((idx, op, method))
        if not self.net["up"]:
            self.net_kick.set()
        self.live[idx] = {"state": PKG_QUEUED, "pct": 0}
        self.send("pkg %d %d 0" % (idx, PKG_QUEUED))
        self.dirty = True

    # ---- the job (a child: `sbstore <op> ID --json`)
    def start_next(self):
        if self.job or not self.queue:
            return
        if getattr(self, "aptup", None) is not None:
            rc = self.aptup.poll()
            if rc is None and time.monotonic() - self.aptup_t0 < 120:
                return                                  # apt's lists first (a slow network: not for ever)
            log("apt-get update: %s" % ("ok" if rc == 0 else "still running, jobs go ahead" if rc is None else
                                        "rc %d (offline sources still read)" % rc))
            self.aptup = None                           # (a slow one carries on detached; apt's lock serialises)
        idx, op, method = self.queue.pop(0)
        me = [sys.executable, os.path.abspath(__file__)]
        if op == OP_CHECK:
            argv = me + ["check", "--json"] + ([self.order[idx]] if idx != SBSHM_NONE else [])
            idx = SBSHM_NONE                            # no progress on the entry
        else:
            aid = self.order[idx]
            verb = {OP_INSTALL: "install", OP_REMOVE: "remove", OP_UPDATE: "update", OP_ROLLBACK: "rollback"}.get(op)
            if not verb:
                return
            argv = me + [verb, aid, "--json"]
            if op == OP_INSTALL and method in M_NAME:
                argv += ["--method", M_NAME[method]]
        try:
            os.makedirs(C.log_dir, exist_ok=True)
            errf = open(os.path.join(C.log_dir, "jobs.log"), "ab")     # a crash's traceback lands here, not nowhere
        except OSError:
            errf = subprocess.DEVNULL
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=errf, stdin=subprocess.DEVNULL,
                             start_new_session=True)
        if errf is not subprocess.DEVNULL:
            errf.close()
        os.set_blocking(p.stdout.fileno(), False)
        self.job = {"idx": idx, "op": op, "p": p, "buf": b"", "t0": time.monotonic(), "err": ""}
        if idx != SBSHM_NONE:
            self.live[idx] = {"state": PKG_REMOVING if op == OP_REMOVE else PKG_INSTALLING, "pct": 0}
            self.dirty = True

    def poll_job(self):
        j = self.job
        if not j:
            return
        try:
            b = j["p"].stdout.read()
        except (BlockingIOError, TypeError):
            b = None
        if b:
            j["buf"] += b
            while b"\n" in j["buf"]:
                l, j["buf"] = j["buf"].split(b"\n", 1)
                try:
                    ev = json.loads(l)
                except ValueError:
                    continue
                self.on_event(j, ev)
        rc = j["p"].poll()
        if rc is None:
            return
        idx = j["idx"]
        if idx != SBSHM_NONE:
            if rc == 0:
                self.live[idx] = {"state": PKG_IDLE, "pct": 100}
                self.write_apps_map()
                self.send("rescan")
            elif j.get("cancelled"):
                self.live[idx] = {"state": PKG_IDLE, "pct": 0}       # cancelled: back to GET, no error
            else:
                if not j["err"]:                            # died without an error event (killed, a crash)
                    short, hint = explain(StoreError("interrupted" if rc < 0 else "internal error"), self.order[idx])
                    j["err"], j["hint"] = short, hint
                self.live[idx] = {"state": PKG_FAILED, "pct": 0, "err": j["err"], "hint": j.get("hint", "")}
                log("FAILED %s (exit %d): %s -- %s%s" % (self.order[idx], rc, j["err"], j.get("hint", ""),
                                                         " [%s]" % j["detail"][:300] if j.get("detail") else ""))
            self.send("pkg %d %d %d" % (idx, self.live[idx]["state"], self.live[idx]["pct"]))
        self.job = None
        self.dirty = True

    def on_event(self, j, ev):
        idx = j["idx"]
        if ev.get("ev") == "error":
            j["err"] = ev.get("short") or ev.get("msg", "failed")
            j["hint"] = ev.get("hint", "")
            j["detail"] = ev.get("msg", "")
        if idx == SBSHM_NONE or ev.get("ev") != "progress":
            return
        st = PHASE_STATE.get(ev.get("phase"), PKG_INSTALLING)
        if j["op"] == OP_REMOVE:
            st = PKG_REMOVING
        el = time.monotonic() - j["t0"]
        pct = ev.get("pct", 0)
        eta = int(el * (100 - pct) / pct) if pct > 3 else 0
        prev = self.live.get(idx, {})
        self.live[idx] = {"state": st, "pct": pct, "done_kb": ev.get("done_kb", 0),
                          "total_kb": ev.get("total_kb", 0), "eta": eta}
        if prev.get("pct") != pct or prev.get("state") != st:
            self.send("pkg %d %d %d" % (idx, min(st, PKG_FAILED) if st < PKG_DOWNLOADING else PKG_INSTALLING, pct))
            self.dirty = True

    def run(self):
        global _mode
        _mode = "daemon"
        try:
            self.shm = Shm(C.shm)
        except OSError as e:
            log("no shared region %s (%s): the table is not published" % (C.shm, e))
        up, why = net_probe()
        _net.update(t=time.monotonic(), up=up, why=why)
        self.net = {"up": up, "why": why}
        log("daemon: %d apps (%s), mirror %s, network %s" % (len(self.order), C.catalog, C.mirror,
                                                            "up" if up else "down (%s)" % why))
        repair(at_start=True)
        ensure_mirror_source()
        ensure_net_sources()
        if env("SBSTORE_NO_APT_UPDATE") != "1":         # in the background: file: sources (repo, mirror debs)
            self.apt_update_bg()                        # work offline
        threading.Thread(target=self._net_loop, daemon=True).start()
        signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
        self.write_apps_map()
        last_pub = 0.0
        while True:
            self.connect()
            self.read_sock()
            self.poll_job()
            self.start_next()
            now = time.monotonic()
            if self.net_changed:
                self.on_net_change()
            if self.net["up"] and now - self.last_check > 6 * 3600 and not self.job and not self.queue:
                self.last_check = now
                self.queue.append((SBSHM_NONE, OP_CHECK, 0))
            if self.dirty and now - last_pub > 0.25 or now - last_pub > 5:
                self.publish()
                last_pub = now
            time.sleep(0.1)


def _source_label(a):
    for m in ("release", "source"):
        s = a.get(m) or {}
        if s.get("repo"):
            host = {"github": "github.com", "gitlab": s.get("host", "gitlab.com")}.get(s.get("provider", "github"), "")
            return "%s/%s" % (host, s["repo"]) if host else s["repo"]
        if s.get("url"):
            return urllib.parse.urlparse(s["url"]).netloc
    if a.get("distro"):
        return "Debian (%s)" % a["distro"]["apt"][0]
    return ""


# ==== CLI ======================================================================================================
def _status(cat):
    st = load_state()
    rows = []
    for aid in sorted(cat):
        a = cat[aid]
        ia = st["apps"].get(aid)
        lat = st["latest"].get(aid, {})
        rows.append({"id": aid, "name": a["name"], "category": a.get("category"), "methods": app_methods(a),
                     "default": default_method(a), "installed": ia and ia["version"], "method": ia and ia["method"],
                     "latest": {m: v.get("version") for m, v in lat.items()},
                     "update": bool(ia and newer(lat.get(ia["method"], {}).get("version"), ia["version"])),
                     "rollback": bool(ia and ia.get("prev_dir"))})
    return rows


def card_engine():
    """a newer engine + catalog on the card (appstore.erofs: engine/sbstore.py, engine/catalog.json, made by
    tools/mirror-engine.sh) runs instead of the base's copy: store fixes ship with the card file, no new base.
    Only ever newer (VERSION), only once (SBSTORE_FROM_CARD), never in tests (SBSTORE_NO_CARD_ENGINE=1)."""
    if env("SBSTORE_FROM_CARD") == "1" or env("SBSTORE_NO_CARD_ENGINE") == "1":
        return
    for m in C.mirrors:
        eng = os.path.join(m, "engine", "sbstore.py")
        try:
            with open(eng) as f:
                v = re.search(r'^VERSION = "([^"]+)"', f.read(), re.M)
        except OSError:
            continue
        if not v or not newer(v.group(1), VERSION):
            log("the card's engine %s is not newer than %s: using the base's" % (v and v.group(1), VERSION))
            return
        e = dict(os.environ, SBSTORE_FROM_CARD="1")
        cat = os.path.join(m, "engine", "catalog.json")
        if not env("SBSTORE_CATALOG") and os.path.exists(cat):
            e["SBSTORE_CATALOG"] = cat
        log("running the card's newer engine %s (base has %s): %s" % (v.group(1), VERSION, eng))
        try:
            os.execve(sys.executable, [sys.executable, "-I", eng, "daemon"], e)
        except OSError as ex:
            log("the card's engine didn't start (%s): using the base's" % ex)
        return


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="sbstore", description="the Starbite App Store engine")
    ap.add_argument("cmd", choices=["list", "info", "resolve", "install", "update", "rollback", "remove", "check",
                                    "status", "log", "daemon", "help", "version"])
    ap.add_argument("id", nargs="?")
    ap.add_argument("method_pos", nargs="?")
    ap.add_argument("--method", "-m", choices=METHODS)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "help":
        print(__doc__)
        return 0
    if a.cmd == "version":
        print(VERSION)
        return 0
    if a.cmd == "daemon":
        card_engine()
        Daemon().run()
        return 0
    cat = load_catalog()
    prog = Progress(json_out=a.json)
    stop = {"now": False}
    signal.signal(signal.SIGTERM, lambda *x: stop.update(now=True))     # the daemon's cancel: stop apt / make too
    cancel = lambda: stop["now"]  # noqa: E731
    try:
        if a.cmd == "list":
            for r in _status(cat):
                print("%-22s %-12s %-24s %s" % (r["id"], r["category"], "/".join(r["methods"]),
                                               ("installed %s (%s)" % (r["installed"], r["method"])) if r["installed"] else ""))
        elif a.cmd == "status":
            rows = _status(cat)
            if a.json:
                print(json.dumps(rows, indent=1))
            else:
                for r in rows:
                    if r["installed"]:
                        print("%-22s %-10s %-8s%s%s" % (r["id"], r["installed"], r["method"],
                                                        "  update: %s" % r["latest"].get(r["method"]) if r["update"] else "",
                                                        "  (rollback ok)" if r["rollback"] else ""))
        elif a.cmd == "info":
            app = _app(cat, a.id)
            st = load_state()
            print(json.dumps({"app": app, "installed": st["apps"].get(a.id), "latest": st["latest"].get(a.id)},
                             indent=1, default=str))
        elif a.cmd == "resolve":
            app = _app(cat, a.id)
            ms = [a.method or a.method_pos] if (a.method or a.method_pos) else app_methods(app)
            out = {}
            for m in ms:
                try:
                    r = resolve(app, m)
                    if m == "release":
                        pick = pick_asset(r, app["release"])
                        r["asset"] = pick[0] if pick else None
                        if pick:
                            try:
                                r["sha256"], r["verify"] = expected_sha(dict(r, id=a.id), pick[0],
                                                                        app["release"].get("verify", {}), r["version"])
                            except StoreError as e:
                                r["verify"] = "ERROR " + e.detail
                        r.pop("assets", None)
                    out[m] = r
                except StoreError as e:
                    out[m] = {"error": e.detail}
            print(json.dumps(out, indent=1))
        elif a.cmd == "install":
            op_install(cat, a.id, a.method or a.method_pos, prog, cancel)
        elif a.cmd == "update":
            if a.all or not a.id:
                ups = op_check(cat, NOPROG, ids=None)
                for aid, old, new in ups:
                    op_update(cat, aid, prog, cancel)
                if not ups:
                    prog.event(ev="done", msg="everything is up to date")
            else:
                op_update(cat, a.id, prog, cancel)
        elif a.cmd == "rollback":
            op_rollback(cat, a.id, prog)
        elif a.cmd == "remove":
            op_remove(cat, a.id, prog)
        elif a.cmd == "check":
            ups = op_check(cat, prog, ids=[a.id] if a.id else None)
            for aid, old, new in ups:
                prog.event(ev="update", id=aid, old=old, new=new, msg="%s: %s -> %s" % (aid, old, new))
            prog.event(ev="done", msg="%d update(s)" % len(ups))
        elif a.cmd == "log":
            p = os.path.join(C.log_dir, (a.id or "sbstore") + ".log")
            with open(p) as f:
                sys.stdout.write("".join(f.readlines()[-200:]))
        return 0
    except KeyboardInterrupt:
        return 130
    except Exception as e:                                  # noqa: BLE001: every failure reaches the user in words
        if not isinstance(e, StoreError):
            import traceback
            log("error: internal: %s" % "".join(traceback.format_exception(e)).rstrip(), _applog)
            e = StoreError("internal error", "%s: %s" % (type(e).__name__, e))
        short, hint = explain(e, a.id or "")
        log("error: %s (%s) -- told the user: %s" % (e.short, e.detail, hint))
        if a.json:
            print(json.dumps({"ev": "error", "code": e.short, "short": short, "hint": hint, "msg": e.detail}),
                  flush=True)
        else:
            print("sbstore: %s: %s\n  (%s)" % (short, hint, e.detail), file=sys.stderr)
        return 3 if isinstance(e, NeedsNet) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
