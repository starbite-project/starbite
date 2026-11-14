#!/usr/bin/env python3
"""sbstore host tests: a local HTTP server (Range, a flaky mode), a fake GitHub / GitLab API, a fake apt and dpkg,
a fake sbwl socket for the daemon. Run: python3 starbite/appstore/tests/test_sbstore.py [-v]"""
import hashlib
import http.server
import io
import json
import os
import shutil
import socket
import socketserver
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SBSTORE = os.path.join(HERE, "..", "sbstore.py")
FAKEAPT = os.path.join(HERE, "fakeapt.py")
sys.path.insert(0, os.path.join(HERE, ".."))
import sbstore  # noqa: E402  (unit-level helpers only; operations run as subprocesses with their own env)


# ---- the server -------------------------------------------------------------------------------------------------
class Files:
    def __init__(self):
        self.d = {}            # path -> bytes or callable
        self.flaky = {}        # path -> cut after N bytes, once
        self.slow = set()      # paths sent slowly (cancel tests)
        self.hits = []         # (path, range header)


FILES = Files()


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        FILES.hits.append((self.path, self.headers.get("Range")))
        body = FILES.d.get(path)
        if callable(body):
            body = body()
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        start = 0
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            start = int(rng[6:].split("-")[0])
            if start >= len(body):
                self.send_response(416)
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(body) - 1, len(body)))
        else:
            self.send_response(200)
        part = body[start:]
        self.send_header("Content-Length", str(len(part)))
        self.end_headers()
        if path in FILES.slow:                       # trickle: 64 KiB every 0.2 s
            for k in range(0, len(part), 65536):
                try:
                    self.wfile.write(part[k:k + 65536])
                    self.wfile.flush()
                except OSError:
                    return
                time.sleep(0.2)
            return
        cut = FILES.flaky.pop(path, None)
        if cut is not None:
            self.wfile.write(part[:cut])
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        self.wfile.write(part)


class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


SRV = TS(("127.0.0.1", 0), H)
PORT = SRV.server_address[1]
BASE = "http://127.0.0.1:%d" % PORT
threading.Thread(target=SRV.serve_forever, daemon=True).start()


# ---- building test payloads ---------------------------------------------------------------------------------------
def tgz(files, top="pkg-1.0"):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w:gz") as t:
        for name, (data, mode) in files.items():
            ti = tarfile.TarInfo("%s/%s" % (top, name) if top else name)
            ti.size = len(data)
            ti.mode = mode
            t.addfile(ti, io.BytesIO(data))
    return b.getvalue()


def script(text):
    return ("#!/bin/sh\n" + text + "\n").encode(), 0o755


def sha(b):
    return hashlib.sha256(b).hexdigest()


def gh_release(tag, assets, prerelease=False, digest=True):
    return {"tag_name": tag, "name": tag, "draft": False, "prerelease": prerelease, "published_at": "2026-11-01",
            "assets": [{"name": n, "browser_download_url": BASE + "/dl/" + n, "size": len(b),
                        "digest": ("sha256:" + sha(b)) if digest else None} for n, b in assets.items()]}


TOML_HELLO = '''
id = "hello"
name = "Hello"
summary = "says hello"
description = "A test app."
category = "Utilities"
licence = "MIT"
icon = "hello"
ram_mb = 10
default = "release"
[launcher]
exec = "hello --gui"
[runtime]
apt = ["libhello1"]
needs = ["network"]
[release]
provider = "github"
repo = "o/hello"
asset = "hello-{version}-linux-aarch64.tar.gz"
format = "tar"
strip = 1
bin = ["bin/hello"]
size_mb = 1
[source]
ref = "release"
tarball = "%s/src/hello-{tag}.tar.gz"
system = "make"
build_deps = ["make-dep"]
bin = ["bin/hello"]
build_min = 1
build_mb = 5
size_mb = 1
[distro]
apt = ["hello-deb"]
''' % BASE


class Env:
    """a fresh guest root, catalog, fake apt db"""

    def __init__(self, tomls=None, available=None, installed=None):
        self.d = tempfile.mkdtemp(prefix="sbstore-test.")
        self.root = os.path.join(self.d, "root")
        self.cat = os.path.join(self.d, "catalog")
        os.makedirs(self.root)
        os.makedirs(self.cat)
        for i, t in enumerate(tomls or [TOML_HELLO]):
            import tomllib
            aid = tomllib.loads(t)["id"]
            with open(os.path.join(self.cat, aid + ".toml"), "w") as f:
                f.write(t)
        self.db = os.path.join(self.d, "apt.json")
        av = {"libhello1": {"version": "1.0"}, "make-dep": {"version": "4.4"},
              "hello-deb": {"version": "2.10-3", "depends": ["libhello1"]}, "basepkg": {"version": "1"},
              "needs-base": {"version": "1", "depends": []}}
        av.update(available or {})
        json.dump({"available": av, "installed": dict(installed or {"basepkg": "1"})}, open(self.db, "w"))
        os.makedirs(os.path.join(self.root, "usr/share/starbite"), exist_ok=True)
        with open(os.path.join(self.root, "usr/share/starbite/base-packages.txt"), "w") as f:
            f.write("basepkg\n")
        self.mirror = os.path.join(self.d, "mirror")
        self.env = dict(os.environ)
        py = sys.executable
        self.env.update({
            "SBSTORE_ROOT": self.root, "SBSTORE_CATALOG": self.cat, "SBSTORE_MIRROR": self.mirror,
            "SBSTORE_REPO": os.path.join(self.d, "norepo"), "SBSTORE_GITHUB_API": BASE + "/gh",
            "SBSTORE_NET_PROBE": "127.0.0.1:%d" % PORT, "FAKEAPT_DB": self.db,
            "SBSTORE_APT": "%s %s apt-get" % (py, FAKEAPT), "SBSTORE_APT_CACHE": "%s %s apt-cache" % (py, FAKEAPT),
            "SBSTORE_APT_MARK": "%s %s apt-mark" % (py, FAKEAPT),
            "SBSTORE_DPKG_QUERY": "%s %s dpkg-query" % (py, FAKEAPT),
            "SBSTORE_DPKG_DEB": "%s %s dpkg-deb" % (py, FAKEAPT), "SBSTORE_RESERVE_MB": "1",
            "SBSTORE_DPKG": "%s %s dpkg" % (py, FAKEAPT),
            "FAKEAPT_DPKG_UPDATES": os.path.join(self.root, "var/lib/dpkg/updates"),
            "SBSTORE_LOCK_WAIT": "5", "SBSTORE_OFFLINE": "", "SBSTORE_JOBS": "2"})

    def run(self, *args, expect=0, **envx):
        e = dict(self.env)
        e.update(envx)
        p = subprocess.run([sys.executable, SBSTORE] + list(args) + ["--json"], env=e, capture_output=True,
                           text=True, timeout=120)
        evs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
        if expect is not None and p.returncode != expect:
            raise AssertionError("sbstore %s: rc %d (want %d)\nstdout: %s\nstderr: %s" %
                                 (" ".join(args), p.returncode, expect, p.stdout[-2000:], p.stderr[-2000:]))
        return evs

    def state(self):
        with open(os.path.join(self.root, "var/lib/sbstore/state.json")) as f:
            return json.load(f)

    def apt(self):
        return json.load(open(self.db))

    def path(self, p):
        return os.path.join(self.root, p.lstrip("/"))

    def run_bin(self, name, *args):
        return subprocess.run([self.path("/usr/local/bin/" + name)] + list(args), capture_output=True,
                              text=True).stdout.strip()

    def close(self):
        shutil.rmtree(self.d, ignore_errors=True)


def publish_hello(ver, payload_text=None, digest=True, prerelease_newer=None, sums=False):
    name = "hello-%s-linux-aarch64.tar.gz" % ver
    body = tgz({"bin/hello": script("echo hello %s \"$@\"" % (payload_text or ver)),
                "share/icons/hicolor/256x256/apps/hello.png": (b"\x89PNG fake", 0o644)}, top="hello-%s" % ver)
    FILES.d["/dl/" + name] = body
    assets = {name: body}
    if sums:
        s = ("%s  %s\n" % (sha(body), name)).encode()
        FILES.d["/dl/SHA256SUMS"] = s
        assets["SHA256SUMS"] = s
    rel = gh_release("v" + ver, assets, digest=digest)
    FILES.d["/gh/repos/o/hello/releases/latest"] = rel
    rels = [rel]
    if prerelease_newer:
        pn = "hello-%s-linux-aarch64.tar.gz" % prerelease_newer
        FILES.d["/dl/" + pn] = body
        rels.insert(0, gh_release("v" + prerelease_newer, {pn: body}, prerelease=True))
    FILES.d["/gh/repos/o/hello/releases"] = rels
    # the source of the same tag
    mk = ("all:\n\techo built > built.txt\ninstall:\n\tmkdir -p $(PREFIX)/bin\n"
          "\tprintf '#!/bin/sh\\necho hello-from-source %s \"$$@\"\\n' > $(PREFIX)/bin/hello\n"
          "\tchmod 755 $(PREFIX)/bin/hello\n") % ver
    src = tgz({"Makefile": (mk.encode(), 0o644)}, top="hello-v" + ver)
    FILES.d["/src/hello-v%s.tar.gz" % ver] = src
    return body


# ---- tests --------------------------------------------------------------------------------------------------------
class Unit(unittest.TestCase):
    def test_versions(self):
        n = sbstore.newer
        self.assertTrue(n("1.10", "1.9"))
        self.assertTrue(n("v2.0", "2.0rc1"))
        self.assertTrue(n("2.0rc1", "1.9.9"))
        self.assertFalse(n("1.0", "1.0"))
        self.assertTrue(n("1:2.0", "1.9"))
        self.assertTrue(n("0.40.0", "0.39.0"))

    def test_sums(self):
        h = "a" * 64
        self.assertEqual(sbstore.sums_lookup("%s  foo.tar.gz\n%s  bar\n" % ("b" * 64, h), "bar"), h)
        self.assertEqual(sbstore.sums_lookup("%s *dir/bar\n" % h, "bar"), h)
        self.assertEqual(sbstore.sums_lookup("SHA256 (bar) = %s\n" % h, "bar"), h)
        self.assertEqual(sbstore.sums_lookup(h + "\n", "anything"), h)
        self.assertIsNone(sbstore.sums_lookup("%s  other\n" % h, "bar"))

    def test_tar_safety(self):
        d = tempfile.mkdtemp()
        try:
            b = io.BytesIO()
            with tarfile.open(fileobj=b, mode="w:gz") as t:
                for n in ("top/ok.txt", "top/../../evil.txt"):
                    ti = tarfile.TarInfo(n)
                    ti.size = 2
                    t.addfile(ti, io.BytesIO(b"hi"))
            p = os.path.join(d, "x.tar.gz")
            open(p, "wb").write(b.getvalue())
            out = os.path.join(d, "out")
            with self.assertRaises(sbstore.StoreError):
                sbstore.unpack_tar(p, out, strip=1)
            self.assertFalse(os.path.exists(os.path.join(d, "evil.txt")))
        finally:
            shutil.rmtree(d)

    def test_struct_sizes(self):
        self.assertEqual(struct.calcsize(sbstore.PKG_FMT), 248)
        self.assertEqual(struct.calcsize(sbstore.PKG3_FMT), 256)
        self.assertLess(sbstore.SBSHM_STORE_OFF + 64 + 160 * 248, sbstore.SBSHM_STORE_TEXT_OFF)
        self.assertLessEqual(sbstore.SBSHM_STORE3_OFF + 64 + 160 * 256, sbstore.SBSHM_ICON_OFF)

    def test_seed_catalog_valid(self):
        cat = os.path.join(HERE, "..", "catalog")
        tool = os.path.join(HERE, "..", "tools", "catalog.py")
        if not os.path.exists(tool) or not any(f.endswith(".toml") for f in os.listdir(cat)):
            self.skipTest("no seed catalog yet")
        p = subprocess.run([sys.executable, tool, "check"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)


class Release(unittest.TestCase):
    def setUp(self):
        FILES.d.clear()
        FILES.hits.clear()
        self.e = Env()

    def tearDown(self):
        self.e.close()

    def test_install_update_rollback_remove(self):
        publish_hello("1.0")
        evs = self.e.run("install", "hello", "--method", "release")
        self.assertEqual(evs[-1]["ev"], "done")
        self.assertTrue(any(ev.get("phase") == "downloading" for ev in evs))
        self.assertEqual(self.e.run_bin("hello", "x"), "hello 1.0 x")
        st = self.e.state()
        self.assertEqual(st["apps"]["hello"]["version"], "1.0")
        self.assertTrue(st["apps"]["hello"]["verified"].startswith("sha256"))
        self.assertEqual(st["apt_refs"], {"libhello1": ["hello"]})
        desk = open(self.e.path("/usr/local/share/applications/sbstore-hello.desktop")).read()
        self.assertIn("Exec=hello --gui", desk)
        self.assertIn("Icon=sbstore-hello", desk)
        self.assertTrue(os.path.exists(self.e.path("/usr/local/share/icons/hicolor/256x256/apps/sbstore-hello.png")))
        # nothing newer: a no-op
        evs = self.e.run("update", "hello")
        self.assertIn("already the newest", evs[-1]["msg"])
        # 1.1 appears
        publish_hello("1.1")
        evs = self.e.run("check")
        self.assertTrue(any(ev.get("ev") == "update" and ev["new"] == "1.1" for ev in evs))
        self.e.run("update", "hello")
        self.assertEqual(self.e.run_bin("hello"), "hello 1.1")
        self.assertEqual(sorted(os.listdir(self.e.path("/opt/sbstore/hello"))), ["1.0", "1.1", "current"])
        # roll back, and forward again
        self.e.run("rollback", "hello")
        self.assertEqual(self.e.run_bin("hello"), "hello 1.0")
        self.assertEqual(self.e.state()["apps"]["hello"]["version"], "1.0")
        self.e.run("rollback", "hello")
        self.assertEqual(self.e.run_bin("hello"), "hello 1.1")
        # 1.2: 1.0 is pruned, 1.1 kept
        publish_hello("1.2")
        self.e.run("update", "--all")
        self.assertEqual(sorted(os.listdir(self.e.path("/opt/sbstore/hello"))), ["1.1", "1.2", "current"])
        # remove: everything goes, the runtime dep too, the base package stays
        self.e.run("remove", "hello")
        self.assertFalse(os.path.exists(self.e.path("/opt/sbstore/hello")))
        self.assertFalse(os.path.lexists(self.e.path("/usr/local/bin/hello")))
        self.assertFalse(os.path.exists(self.e.path("/usr/local/share/applications/sbstore-hello.desktop")))
        a = self.e.apt()
        self.assertNotIn("libhello1", a["installed"])
        self.assertIn("basepkg", a["installed"])
        self.assertEqual(self.e.state()["apps"], {})

    def test_resume(self):
        body = publish_hello("1.0")
        FILES.flaky["/dl/hello-1.0-linux-aarch64.tar.gz"] = len(body) // 2
        self.e.run("install", "hello")
        ranges = [r for p, r in FILES.hits if p.startswith("/dl/hello") and r]
        self.assertTrue(ranges and ranges[0].startswith("bytes=%d-" % (len(body) // 2)), FILES.hits)
        self.assertEqual(self.e.run_bin("hello"), "hello 1.0")

    def test_checksum_mismatch(self):
        publish_hello("1.0")
        rel = FILES.d["/gh/repos/o/hello/releases/latest"]
        rel["assets"][0]["digest"] = "sha256:" + "0" * 64
        evs = self.e.run("install", "hello", expect=1)
        self.assertEqual(evs[-1]["code"], "checksum mismatch")
        self.assertFalse(os.path.exists(self.e.path("/opt/sbstore/hello")))
        self.assertNotIn("libhello1", self.e.apt()["installed"])       # the runtime dep given back
        self.assertEqual(self.e.state()["apps"], {})

    def test_sums_asset(self):
        publish_hello("1.0", digest=False, sums=True)
        t = TOML_HELLO.replace('size_mb = 1\n[source]', 'size_mb = 1\n[release.verify]\nsha256_asset = "SHA256SUMS"\n[source]')
        self.e.close()
        self.e = Env([t])
        self.e.run("install", "hello")
        self.assertIn("SHA256SUMS", self.e.state()["apps"]["hello"]["verified"])

    def test_unverified_is_said(self):
        publish_hello("1.0", digest=False)
        self.e.run("install", "hello")
        self.assertTrue(self.e.state()["apps"]["hello"]["verified"].startswith("unverified"))

    def test_space(self):
        publish_hello("1.0")
        evs = self.e.run("install", "hello", expect=1, SBSTORE_RESERVE_MB=str(10 ** 9))
        self.assertEqual(evs[-1]["code"], "not enough space")

    def test_prerelease_skipped_and_tag_pattern(self):
        publish_hello("1.0", prerelease_newer="2.0")
        t = TOML_HELLO.replace('repo = "o/hello"\nasset', 'repo = "o/hello"\ntag_pattern = "v*"\nasset')
        self.e.close()
        self.e = Env([t])
        self.e.run("install", "hello")
        self.assertEqual(self.e.state()["apps"]["hello"]["version"], "1.0")

    def test_offline_mirror_and_needs_net(self):
        body = publish_hello("1.0")
        evs = self.e.run("install", "hello", expect=3, SBSTORE_OFFLINE="1")
        self.assertEqual(evs[-1]["code"], "needs the network")
        m = os.path.join(self.e.mirror, "hello")
        os.makedirs(m)
        name = "hello-1.0-linux-aarch64.tar.gz"
        open(os.path.join(m, name), "wb").write(body)
        json.dump({"version": "1.0", "tag": "v1.0", "assets": [{"name": name, "file": name, "size": len(body),
                                                                 "sha256": sha(body)}]},
                  open(os.path.join(m, "release.json"), "w"))
        self.e.run("install", "hello", SBSTORE_OFFLINE="1")
        st = self.e.state()["apps"]["hello"]
        self.assertEqual((st["version"], st["from"]), ("1.0", "mirror"))
        self.assertTrue(st["verified"].startswith("sha256"))


class Formats(unittest.TestCase):
    def setUp(self):
        FILES.d.clear()
        self.e = None

    def tearDown(self):
        if self.e:
            self.e.close()

    def recipe(self, fmt, asset, extra=""):
        return '''
id = "f"
name = "F"
summary = "s"
category = "Utilities"
licence = "MIT"
[release]
provider = "github"
repo = "o/f"
asset = "%s"
format = "%s"
%s
''' % (asset, fmt, extra)

    def test_zip(self):
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w") as z:
            zi = zipfile.ZipInfo("f-1/bin/f")
            zi.external_attr = 0o755 << 16
            z.writestr(zi, "#!/bin/sh\necho zip-ok\n")
        FILES.d["/dl/f.zip"] = b.getvalue()
        FILES.d["/gh/repos/o/f/releases/latest"] = gh_release("v3", {"f.zip": b.getvalue()})
        self.e = Env([self.recipe("zip", "f.zip", 'strip = 1\nbin = ["bin/f"]')])
        self.e.run("install", "f")
        self.assertEqual(self.e.run_bin("f"), "zip-ok")

    def test_binary(self):
        body = b"#!/bin/sh\necho bin-ok\n"
        FILES.d["/dl/f-aarch64"] = body
        FILES.d["/gh/repos/o/f/releases/latest"] = gh_release("2.5.1", {"f-aarch64": body})
        self.e = Env([self.recipe("binary", "f-aarch64", 'bin = ["f"]')])
        self.e.run("install", "f")
        self.assertEqual(self.e.run_bin("f"), "bin-ok")
        self.assertEqual(self.e.state()["apps"]["f"]["version"], "2.5.1")

    def test_appimage(self):
        body = (b"#!/bin/sh\n[ \"$1\" = --appimage-extract ] || exit 1\nmkdir -p squashfs-root/usr/bin\n"
                b"printf '#!/bin/sh\\necho appimage-ok\\n' > squashfs-root/AppRun\nchmod 755 squashfs-root/AppRun\n")
        FILES.d["/dl/F-aarch64.AppImage"] = body
        FILES.d["/gh/repos/o/f/releases/latest"] = gh_release("v1.2.3", {"F-aarch64.AppImage": body})
        self.e = Env([self.recipe("appimage", "F-aarch64.AppImage", 'bin = ["f"]')])
        self.e.run("install", "f")
        self.assertEqual(self.e.run_bin("f"), "appimage-ok")          # the wrapper runs AppRun

    def test_deb_tarball(self):
        debs = tgz({"DEBS/a.deb": (json.dumps({"Package": "lo-a"}).encode(), 0o644),
                    "DEBS/b.deb": (json.dumps({"Package": "lo-b"}).encode(), 0o644)}, top="LO_1_aarch64_deb")
        FILES.d["/dl/LO_1_aarch64_deb.tar.gz"] = debs
        FILES.d["/gh/repos/o/f/releases/latest"] = gh_release("1", {"LO_1_aarch64_deb.tar.gz": debs})
        self.e = Env([self.recipe("deb", "LO_*_aarch64_deb.tar.gz")])
        self.e.run("install", "f")
        a = self.e.apt()
        self.assertIn("lo-a", a["installed"])
        self.assertIn("lo-b", a["installed"])
        self.e.run("rollback", "f", expect=1)
        self.e.run("remove", "f")
        self.assertNotIn("lo-a", self.e.apt()["installed"])

    def test_url_provider(self):
        body = tgz({"bin/f": script("echo url-ok")}, top="f")
        FILES.d["/v.json"] = {"LATEST": "128.0.1"}
        FILES.d["/rel/128.0.1/f-128.0.1.tar.gz"] = body
        FILES.d["/rel/128.0.1/SHA256SUMS"] = ("%s  linux-aarch64/f-128.0.1.tar.gz\n" % sha(body)).encode()
        r = '''
id = "f"
name = "F"
summary = "s"
category = "Internet"
licence = "MPL-2.0"
[release]
provider = "url"
version_url = "%s/v.json"
version_json = "LATEST"
url = "%s/rel/{version}/f-{version}.tar.gz"
format = "tar"
strip = 1
bin = ["bin/f"]
[release.verify]
sha256sums_url = "%s/rel/{version}/SHA256SUMS"
sha256_line = "linux-aarch64/f-{version}.tar.gz"
''' % (BASE, BASE, BASE)
        self.e = Env([r])
        self.e.run("install", "f")
        self.assertEqual(self.e.run_bin("f"), "url-ok")
        self.assertIn("SHA256SUMS", self.e.state()["apps"]["f"]["verified"])


class Source(unittest.TestCase):
    def setUp(self):
        FILES.d.clear()
        self.e = Env()

    def tearDown(self):
        self.e.close()

    def test_build(self):
        publish_hello("1.0")
        evs = self.e.run("install", "hello", "--method", "source")
        self.assertTrue(any(ev.get("phase") == "building" for ev in evs))
        self.assertEqual(self.e.run_bin("hello", "a"), "hello-from-source 1.0 a")
        st = self.e.state()
        self.assertEqual(st["apps"]["hello"]["method"], "source")
        self.assertNotIn("make-dep", self.e.apt()["installed"])       # build deps removed after the build
        self.assertFalse(os.listdir(self.e.path("/var/lib/sbstore/build")))
        # switching to the release method replaces the source install
        self.e.run("install", "hello", "--method", "release")
        self.assertEqual(self.e.run_bin("hello"), "hello 1.0")
        self.assertEqual(self.e.state()["apps"]["hello"]["method"], "release")

    def test_build_deps_and_what_they_pulled_in_are_removed(self):
        """cp7 VM: removing only build-essential left gcc & co. (580 MB) on the 2 GiB overlay"""
        e = Env(available={"make-dep": {"version": "4.4", "depends": ["libmake1"]}, "libmake1": {"version": "1"}})
        try:
            publish_hello("1.0")
            e.run("install", "hello", "--method", "source")
            inst = e.apt()["installed"]
            self.assertNotIn("make-dep", inst)
            self.assertNotIn("libmake1", inst)
            self.assertIn("basepkg", inst)
            self.assertIn(["apt-get", "-q", "clean"], [c[:3] for c in e.apt()["calls"]])
        finally:
            e.close()

    def test_switch_failure_keeps_the_old_install(self):
        publish_hello("1.0")
        self.e.run("install", "hello", "--method", "release")
        FILES.d["/src/hello-v1.0.tar.gz"] = tgz({"Makefile": (b"all:\n\tfalse\n", 0o644)}, top="hello-v1.0")
        evs = self.e.run("install", "hello", "--method", "source", expect=1)
        self.assertEqual(evs[-1]["code"], "build failed")
        self.assertEqual(self.e.run_bin("hello"), "hello 1.0")              # the release install is back
        st = self.e.state()["apps"]["hello"]
        self.assertEqual((st["method"], st["version"]), ("release", "1.0"))
        self.assertTrue(os.path.exists(self.e.path("/usr/local/share/applications/sbstore-hello.desktop")))
        self.assertFalse(os.path.exists(self.e.path("/opt/sbstore/hello.switch")))

    def test_build_failure_cleans_up(self):
        publish_hello("1.0")
        FILES.d["/src/hello-v1.0.tar.gz"] = tgz({"Makefile": (b"all:\n\tfalse\n", 0o644)}, top="hello-v1.0")
        evs = self.e.run("install", "hello", "--method", "source", expect=1)
        self.assertEqual(evs[-1]["code"], "build failed")
        self.assertFalse(os.path.exists(self.e.path("/opt/sbstore/hello")))
        self.assertEqual(self.e.state()["apps"], {})
        log = open(self.e.path("/var/log/sbstore/hello.log")).read()
        self.assertIn("make", log)


class Distro(unittest.TestCase):
    def setUp(self):
        self.e = Env()

    def tearDown(self):
        self.e.close()

    def test_install_remove(self):
        self.e.run("install", "hello", "--method", "distro")
        a = self.e.apt()
        self.assertEqual(a["installed"].get("hello-deb"), "2.10-3")
        st = self.e.state()["apps"]["hello"]
        self.assertEqual((st["method"], st["version"]), ("distro", "2.10-3"))
        self.e.run("remove", "hello")
        self.assertNotIn("hello-deb", self.e.apt()["installed"])

    def test_missing_package_says_network(self):
        t = TOML_HELLO.replace('apt = ["hello-deb"]', 'apt = ["nope"]')
        self.e.close()
        self.e = Env([t])
        evs = self.e.run("install", "hello", "--method", "distro", expect=3)
        self.assertIn(evs[-1]["code"], ("package not available", "not in the repo"))

    def test_base_guard(self):
        t = TOML_HELLO.replace('apt = ["hello-deb"]', 'apt = ["needs-base"]')
        self.e.close()
        self.e = Env([t], available={"basepkg": {"version": "1", "depends": ["needs-base"]}},
                     installed={"basepkg": "1", "needs-base": "1"})
        st = os.path.join(self.e.root, "var/lib/sbstore")
        os.makedirs(st)
        json.dump({"apps": {"hello": {"method": "distro", "version": "1", "packages": ["needs-base"]}}},
                  open(os.path.join(st, "state.json"), "w"))
        evs = self.e.run("remove", "hello", expect=1)
        self.assertEqual(evs[-1]["code"], "would remove system parts")
        self.assertIn("basepkg", self.e.apt()["installed"])


class DaemonNoOffset(unittest.TestCase):
    """a /dev/sbshm that refuses offset maps (the old sbguest.ko): the table still lands at the store's offset"""

    def test_table_without_offset_maps(self):
        FILES.d.clear()
        publish_hello("1.0")
        e = Env()
        try:
            shm = os.path.join(e.d, "shm")
            with open(shm, "wb") as f:
                f.truncate(sbstore.SBSHM_BLK_OFF)
            sockp = os.path.join(e.d, "s.sock")
            srv = socket.socket(socket.AF_UNIX)
            srv.bind(sockp)
            srv.listen(1)
            p = subprocess.Popen([sys.executable, SBSTORE, "daemon"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                 env=dict(e.env, SBSTORE_SHM=shm, SBSTORE_SOCK=sockp, SBSTORE_SHM_NO_OFFSET="1",
                                          SBSTORE_RUN=os.path.join(e.d, "run")))
            srv.settimeout(10)
            c, _ = srv.accept()
            c.settimeout(20)
            buf = b""
            while b"store " not in buf:
                buf += c.recv(4096)
            p.terminate()
            err = p.communicate(timeout=5)[1].decode()
            c.close()
            srv.close()
            self.assertIn("mapping it from 0", err)
            with open(shm, "rb") as f:
                f.seek(sbstore.SBSHM_STORE_OFF)
                hdr = struct.unpack("<16I", f.read(64))
                name = f.read(40).rstrip(b"\0")
                f.seek(sbstore.SBSHM_STORE3_OFF)
                magic = struct.unpack("<I", f.read(4))[0]
                f.seek(0)
                zero = f.read(4096)
            self.assertEqual((hdr[1], name, magic), (1, b"hello", sbstore.STORE3_MAGIC))
            self.assertEqual(hdr[0] % 2, 0)
            self.assertEqual(zero, b"\0" * 4096)          # nothing written outside the store's regions
        finally:
            e.close()


class DaemonTest(unittest.TestCase):
    """the daemon against a fake sbwl (a unix socket server) and a plain file as the shared region"""

    def extra_env(self, e):
        return {}

    def setUp(self):
        FILES.d.clear()
        self.e = Env()
        self.shm = os.path.join(self.e.d, "shm")
        with open(self.shm, "wb") as f:
            f.truncate(sbstore.SBSHM_BLK_OFF)
        self.sockp = os.path.join(self.e.d, "sbwl.sock")
        self.srv = socket.socket(socket.AF_UNIX)
        self.srv.bind(self.sockp)
        self.srv.listen(1)
        e = dict(self.e.env, SBSTORE_SHM=self.shm, SBSTORE_SOCK=self.sockp, SBSTORE_MEM_MB="1500",
                 SBSTORE_RUN=os.path.join(self.e.d, "run"))
        e.update(self.extra_env(self.e))
        self.p = subprocess.Popen([sys.executable, SBSTORE, "daemon"], env=e, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE)
        self.srv.settimeout(10)
        self.c, _ = self.srv.accept()
        self.c.settimeout(30)
        self.buf = b""

    def tearDown(self):
        self.p.terminate()
        self.p.wait(5)
        self.c.close()
        self.srv.close()
        self.e.close()

    def line(self, pred, t=30):
        end = time.time() + t
        while time.time() < end:
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                l = l.decode()
                if pred(l):
                    return l
            self.buf += self.c.recv(4096)
        raise AssertionError("no matching line")

    def table(self):
        with open(self.shm, "rb") as f:
            f.seek(sbstore.SBSHM_STORE_OFF)
            hdr = struct.unpack("<16I", f.read(64))
            pk = [struct.unpack(sbstore.PKG_FMT, f.read(248)) for _ in range(hdr[1])]
            f.seek(sbstore.SBSHM_STORE3_OFF)
            h3 = struct.unpack("<16I", f.read(64))
            p3 = [struct.unpack(sbstore.PKG3_FMT, f.read(256)) for _ in range(h3[2])]
            f.seek(sbstore.SBSHM_STORE_TEXT_OFF)
            text = f.read(h3[6])
        return hdr, pk, h3, p3, text

    def test_table_and_install(self):
        publish_hello("1.0")
        seq = int(self.line(lambda l: l.startswith("store ")).split()[1])
        hdr, pk, h3, p3, text = self.table()
        self.assertEqual(hdr[0], seq)
        self.assertEqual(hdr[1], 1)
        self.assertEqual(pk[0][0].rstrip(b"\0"), b"hello")
        self.assertEqual(h3[0], sbstore.STORE3_MAGIC)
        self.assertEqual(p3[0][0], (1 << 1) | (1 << 2) | (1 << 3))      # all three methods
        self.assertEqual(p3[0][2], 2)                                    # default: release
        self.assertEqual(text[p3[0][11]:p3[0][11] + p3[0][12]], b"A test app.")
        self.assertEqual(p3[0][-2].rstrip(b"\0"), b"MIT")
        self.assertEqual(p3[0][-1].rstrip(b"\0"), b"github.com/o/hello")
        # install by source through the op line: b = op | method << 8
        self.c.sendall(b"op 0 %d %d\n" % (1 | (3 << 8), seq))
        self.line(lambda l: l.startswith("pkg 0 1"))                       # queued
        self.line(lambda l: l == "rescan", t=60)
        self.line(lambda l: l.startswith("pkg 0 0 100"))
        time.sleep(0.6)
        hdr, pk, h3, p3, text = self.table()
        self.assertTrue(pk[0][8] & sbstore.F_INSTALLED)
        self.assertEqual(p3[0][1], 3)                                    # installed by source
        self.assertEqual(p3[0][16].rstrip(b"\0"), b"1.0")
        self.assertEqual(self.e.run_bin("hello"), "hello-from-source 1.0")
        self.assertEqual(open(os.path.join(self.e.d, "run", "apps.tsv")).read(), "0\thello\tsbstore-hello\n")
        # a stale seq is ignored; a remove with the right one works
        self.c.sendall(b"op 0 2 %d\n" % (seq - 2))
        time.sleep(1)
        self.assertTrue(os.path.exists(self.e.path("/usr/local/bin/hello")))
        self.c.sendall(b"op 0 2 %d\n" % seq)
        self.line(lambda l: l == "rescan", t=30)
        self.assertFalse(os.path.lexists(self.e.path("/usr/local/bin/hello")))

    def test_cancel(self):
        body = publish_hello("1.0")
        big = body + os.urandom(3 << 20)                  # a slow 3 MB download (the tar reader ignores the tail)
        name = "/dl/hello-1.0-linux-aarch64.tar.gz"
        FILES.d[name] = big
        FILES.slow.add(name)
        rel = FILES.d["/gh/repos/o/hello/releases/latest"]
        rel["assets"][0].update(size=len(big), digest="sha256:" + sha(big))
        seq = int(self.line(lambda l: l.startswith("store ")).split()[1])
        self.c.sendall(b"op 0 %d %d\n" % (1 | (2 << 8), seq))
        self.line(lambda l: l.startswith("pkg 0 2"), t=30)                # downloading
        time.sleep(1)
        self.c.sendall(b"op 0 3 %d\n" % seq)
        self.line(lambda l: l.startswith("pkg 0 0 0"), t=15)
        time.sleep(1)
        hdr, pk, h3, p3, text = self.table()
        self.assertEqual(pk[0][9], sbstore.PKG_IDLE)
        FILES.slow.discard(name)
        self.assertFalse(os.path.exists(self.e.path("/opt/sbstore/hello")))
        self.assertFalse(os.path.lexists(self.e.path("/usr/local/bin/hello")))
        part = [f for f in os.listdir(self.e.path("/var/lib/sbstore/dl/hello")) if f.endswith(".part")]
        self.assertTrue(part, "the partial download is kept for a resume")

    def test_quiesce(self):
        """Track E's snapshot: quiet when idle, then no job starts during the hold"""
        publish_hello("1.0")
        seq = int(self.line(lambda l: l.startswith("store ")).split()[1])
        time.sleep(1)                                                     # (the start's apt update done)
        self.c.sendall(b"op %d 7 %d\n" % (sbstore.SBSHM_NONE, seq))
        self.assertEqual(self.line(lambda l: l.startswith("quiet ")), "quiet %d 1" % seq)
        self.c.sendall(b"op 0 %d %d\n" % (1 | (2 << 8), seq))
        self.line(lambda l: l.startswith("pkg 0 1"))                      # queued, not started
        time.sleep(2)
        self.c.sendall(b"op %d 7 %d\n" % (sbstore.SBSHM_NONE, seq + 1))
        self.assertEqual(self.line(lambda l: l.startswith("quiet ")), "quiet %d 0" % (seq + 1))

    def test_failure_reported(self):
        publish_hello("1.0")
        FILES.d["/gh/repos/o/hello/releases/latest"]["assets"][0]["digest"] = "sha256:" + "1" * 64
        seq = int(self.line(lambda l: l.startswith("store ")).split()[1])
        self.c.sendall(b"op 0 %d %d\n" % (1 | (2 << 8), seq))
        self.line(lambda l: l.startswith("pkg 0 4"), t=30)
        time.sleep(0.6)
        hdr, pk, h3, p3, text = self.table()
        self.assertEqual(pk[0][9], sbstore.PKG_FAILED)
        self.assertEqual(pk[0][4].rstrip(b"\0"), b"Download failed its check")
        hint = text[p3[0][13]:p3[0][13] + p3[0][15]].decode()             # hint_off, hint_len
        self.assertIn("checksum", hint)
        self.assertEqual(h3[7], 0)                                        # net_why: ok
        self.assertEqual(h3[8], 0)                                        # busy: no job left


class DaemonNoNet(DaemonTest):
    """the network down (a closed port): the reason in the table, and an install says what to do in words"""

    def extra_env(self, e):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()                                    # nothing listens there now
        return {"SBSTORE_NET_PROBE": "127.0.0.1:%d" % port}

    def test_table_and_install(self):
        pass

    def test_cancel(self):
        pass

    def test_failure_reported(self):
        publish_hello("1.0")
        seq = int(self.line(lambda l: l.startswith("store ")).split()[1])
        hdr, pk, h3, p3, text = self.table()
        self.assertEqual(h3[3], 0)                                        # net_ok
        self.assertEqual(h3[7], 3)                                        # net_why: no answer
        self.assertTrue(pk[0][8] & sbstore.F_NEEDS_NET)
        self.c.sendall(b"op 0 %d %d\n" % (1 | (2 << 8), seq))
        self.line(lambda l: l.startswith("pkg 0 4"), t=60)
        time.sleep(0.6)
        hdr, pk, h3, p3, text = self.table()
        self.assertEqual(pk[0][4].rstrip(b"\0"), b"Needs the internet")
        hint = text[p3[0][13]:p3[0][13] + p3[0][15]].decode()
        self.assertIn("internet doesn't answer", hint)
        self.assertIn("Debian", hint)


class Network(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.old = (sbstore.C.proc_route, sbstore.C.proc_route6, sbstore.C.net_probe, sbstore.C.offline)

    def tearDown(self):
        sbstore.C.proc_route, sbstore.C.proc_route6, sbstore.C.net_probe, sbstore.C.offline = self.old
        shutil.rmtree(self.d, ignore_errors=True)

    def route(self, default):
        p = os.path.join(self.d, "route")
        with open(p, "w") as f:
            f.write("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n")
            f.write("sbnet0\t0000000A\t00000000\t0001\t0\t0\t0\tFCFFFFFF\n")
            if default:
                f.write("sbnet0\t00000000\t01004D0A\t0003\t0\t0\t0\t00000000\n")
        sbstore.C.proc_route = p
        sbstore.C.proc_route6 = os.path.join(self.d, "none")

    def test_no_route(self):
        self.route(False)
        sbstore.C.net_probe = "127.0.0.1:%d" % PORT
        self.assertEqual(sbstore.net_probe(), (False, "no connection"))

    def test_no_dns(self):
        self.route(True)
        sbstore.C.net_probe = "no-such-host.invalid:80"
        self.assertEqual(sbstore.net_probe(), (False, "no dns"))

    def test_up_with_any_target(self):
        self.route(True)
        sbstore.C.net_probe = "no-such-host.invalid:80,127.0.0.1:%d" % PORT
        self.assertEqual(sbstore.net_probe(), (True, "ok"))

    def test_words(self):
        self.route(False)
        sbstore.C.net_probe = "127.0.0.1:%d" % PORT
        sbstore._net.update(t=0)
        short, hint = sbstore.explain(sbstore.NeedsNet("needs the network", "x"), "firefox")
        self.assertEqual(short, "Needs the internet")
        self.assertIn("Join Wi-Fi", hint)
        short, hint = sbstore.explain(sbstore.StoreError("build failed", "x"), "btop")
        self.assertIn("/var/log/sbstore/btop.log", hint)
        for code, (sh, h) in sbstore.HINTS.items():
            self.assertLessEqual(len(sh.encode()), 31, code)
        import re                                                   # every error the engine raises has words
        src = open(SBSTORE).read()
        codes = set(re.findall(r'(?:StoreError|NeedsNet)\("([^"]+)"', src))
        self.assertEqual(sorted(c for c in codes if c not in sbstore.HINTS), [])


class CardEngine(unittest.TestCase):
    def test_newer_engine_on_the_card_runs(self):
        e = Env()
        try:
            eng = os.path.join(e.mirror, "engine")
            os.makedirs(eng)
            mark = os.path.join(e.d, "mark")
            with open(os.path.join(eng, "sbstore.py"), "w") as f:
                f.write('VERSION = "9.9"\nimport os, sys\nopen(os.environ["MARK"], "w").write('
                        '" ".join(sys.argv[1:]) + " " + os.environ["SBSTORE_FROM_CARD"])\n')
            subprocess.run([sys.executable, SBSTORE, "daemon"], env=dict(e.env, MARK=mark), timeout=30)
            self.assertEqual(open(mark).read(), "daemon 1")
            with open(os.path.join(eng, "sbstore.py"), "w") as f:      # an older one: the base's engine runs
                f.write('VERSION = "0.9"\n')
            p = subprocess.Popen([sys.executable, SBSTORE, "daemon"], env=dict(e.env, SBSTORE_SOCK="/nonexistent",
                                 SBSTORE_SHM="/nonexistent"), stderr=subprocess.PIPE)
            time.sleep(3)
            p.terminate()
            p.communicate(timeout=10)
            self.assertIn("not newer", open(e.path("/var/log/sbstore/sbstore.log")).read())
        finally:
            e.close()


class QtPlugin(unittest.TestCase):
    def test_qt_apps_get_their_wayland_plugin(self):
        """Track E: Calibre / KeePassXC / Stellarium abort without Qt's Wayland plugin (only a Recommends)"""
        e = Env(available={"hello-deb": {"version": "2.10-3", "depends": ["libhello1", "libqt6gui6"]},
                           "libqt6gui6": {"version": "6.8"}, "qt6-wayland": {"version": "6.8"}})
        try:
            e.run("install", "hello", "--method", "distro")
            inst = e.apt()["installed"]
            self.assertIn("qt6-wayland", inst)
            e.run("remove", "hello")
            self.assertNotIn("qt6-wayland", e.apt()["installed"])       # (it goes with the app)
        finally:
            e.close()


class Repair(unittest.TestCase):
    def test_dpkg_stale_file_handle_says_so(self):
        """8i: every dpkg run answered ESTALE on triggers/Lock (the overlay after a base change): plain words, no
        "Install failed", and no network retry loop"""
        e = Env()
        try:
            evs = e.run("install", "hello", "--method", "distro", expect=1, FAKEAPT_STALE="1")
            self.assertEqual(evs[-1]["code"], "Linux storage error")
            short, hint = sbstore.explain(sbstore.StoreError("Linux storage error"), "hello")
            self.assertEqual(short, "Linux can't save installs")
            self.assertIn("Latest Release", hint)
            calls = [c for c in e.apt()["calls"] if c[0] == "apt-get" and "install" in c and "-s" not in c]
            self.assertEqual(len(calls), 1, calls)                   # (the old `"…" or` test retried online)
        finally:
            e.close()

    def test_lists_apt_cant_read_are_started_afresh(self):
        e = Env()
        try:
            lists = e.path("/var/lib/apt/lists")
            os.makedirs(lists)
            for n in ("_repo_._Release", "_repo_._Packages"):          # they pass the checks; apt still fails
                with open(os.path.join(lists, n), "w") as f:
                    f.write("Origin: x\n")
            e.run("install", "hello", "--method", "distro",
                  FAKEAPT_LISTS_BROKEN_ONCE=os.path.join(lists, "_repo_._Release"))
            self.assertEqual(os.listdir(lists), ["partial"])
            self.assertIn("2.10-3", e.state()["apps"]["hello"]["version"])
            self.assertIn("starting them afresh", open(e.path("/var/log/sbstore/hello.log")).read())
        finally:
            e.close()

    def test_damaged_apt_lists_and_status_after_an_unclean_stop(self):
        """cp3: the overlay's ext4 recovered after a hard stop; lists/_repo_._Release was damaged and every apt
        command failed ('Splitting of clearsigned file ... failed')"""
        e = Env()
        try:
            lists = e.path("/var/lib/apt/lists")
            os.makedirs(lists)
            with open(os.path.join(lists, "_repo_._Release"), "wb") as f:
                f.write(b"\0" * 2048)
            with open(os.path.join(lists, "_repo_._InRelease"), "wb") as f:
                f.write(b"-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\nOrigin: x\n")   # cut short
            with open(os.path.join(lists, "_repo_._Packages"), "wb") as f:
                f.write(b"Package: hello-deb\nVersion: 2.10-3\n\n")
            dk = e.path("/var/lib/dpkg")
            os.makedirs(dk)
            with open(os.path.join(dk, "status"), "wb") as f:
                f.write(b"Package: basepkg\n" + b"\0" * 512)
            with open(os.path.join(dk, "status-old"), "wb") as f:
                f.write(b"Package: basepkg\nStatus: install ok installed\n\n")
            e.run("install", "hello", "--method", "distro")
            self.assertEqual(sorted(os.listdir(lists)), ["_repo_._Packages"])
            self.assertEqual(open(os.path.join(dk, "status"), "rb").read(),
                             b"Package: basepkg\nStatus: install ok installed\n\n")
            self.assertTrue(os.path.exists(os.path.join(dk, "status.broken")))
            lg = open(e.path("/var/log/sbstore/hello.log")).read()
            self.assertIn("removed 2 damaged apt list file(s)", lg)
        finally:
            e.close()

    def test_interrupted_dpkg_is_repaired_before_the_next_install(self):
        e = Env()
        try:
            upd = e.path("/var/lib/dpkg/updates")
            os.makedirs(upd)
            open(os.path.join(upd, "0001"), "w").close()                  # dpkg stopped mid-way
            e.run("install", "hello", "--method", "distro")
            calls = [c for c in e.apt()["calls"] if c[0] == "dpkg"]
            self.assertTrue(any("--configure" in c and "-a" in c for c in calls), calls)
            self.assertEqual(os.listdir(upd), [])
            self.assertIn("2.10-3", e.state()["apps"]["hello"]["version"])
        finally:
            e.close()


if __name__ == "__main__":
    unittest.main()
