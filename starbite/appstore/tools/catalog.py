#!/usr/bin/env python3
"""The App Store catalog: check the recipes (catalog/*.toml) and build catalog.json for the guest.

  catalog.py check                     validate every recipe (FORMAT.md); exit 1 on any error
  catalog.py build OUT [--resolve]     OUT/catalog.json (+ OUT/icons/ copied from build/appstore-icons if present);
                                       --resolve asks upstream for each app's latest versions now ("known")
  catalog.py resolve [ID...]           print the latest version + asset per method (live; needs the network)
Host only (Python 3.11+ for tomllib). Set SBSTORE_GITHUB_TOKEN to lift GitHub's 60 requests / hour limit.
"""
import json
import os
import re
import shutil
import sys
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
CAT = os.path.join(TOP, "catalog")
sys.path.insert(0, TOP)
import sbstore  # noqa: E402

CATS = sbstore.CATEGORIES
FORMATS = ("tar", "zip", "appimage", "deb", "binary")
SYSTEMS = tuple(sbstore.BUILD_STEPS)
PROVIDERS = ("github", "gitlab", "url")
NEEDS = tuple(sbstore.NEEDS_BITS)


def load_all():
    apps, errs = {}, []
    for fn in sorted(os.listdir(CAT)):
        if not fn.endswith(".toml"):
            continue
        p = os.path.join(CAT, fn)
        try:
            with open(p, "rb") as f:
                a = tomllib.load(f)
        except tomllib.TOMLDecodeError as e:
            errs.append("%s: TOML: %s" % (fn, e))
            continue
        apps[fn[:-5]] = a
    return apps, errs


def check_app(fid, a):
    e = []

    def need(k, typ=str):
        if not isinstance(a.get(k), typ) or (typ is str and not a.get(k)):
            e.append("%s: '%s' missing or not %s" % (fid, k, typ.__name__))

    for k in ("id", "name", "summary", "category", "licence"):
        need(k)
    if a.get("id") != fid:
        e.append("%s: id %r != the file name" % (fid, a.get("id")))
    if not re.fullmatch(r"[a-z0-9][a-z0-9.+-]*", a.get("id", "")) or len(a.get("id", "")) > 39:
        e.append("%s: bad id" % fid)
    if len(a.get("name", "").encode()) > 31:
        e.append("%s: name over 31 bytes" % fid)
    if len(a.get("summary", "").encode()) > 95:
        e.append("%s: summary over 95 bytes" % fid)
    if len(a.get("description", "").encode()) > 1000:
        e.append("%s: description over 1000 bytes" % fid)
    if a.get("category") not in CATS:
        e.append("%s: category %r not one of %s" % (fid, a.get("category"), ", ".join(CATS)))
    if not isinstance(a.get("ram_mb", 0), int):
        e.append("%s: ram_mb not an integer" % fid)
    ms = sbstore.app_methods(a)
    if not ms:
        e.append("%s: no install method" % fid)
    if a.get("default") and a["default"] not in ms:
        e.append("%s: default %r is not one of its methods %s" % (fid, a["default"], ms))
    if a.get("touch") not in (None, "good", "ok", "poor"):
        e.append("%s: touch must be good/ok/poor" % fid)
    for n in (a.get("runtime") or {}).get("needs", []):
        if n not in NEEDS:
            e.append("%s: runtime.needs %r unknown" % (fid, n))
    R = a.get("release")
    if R is not None:
        if R.get("provider", "github") not in PROVIDERS:
            e.append("%s: release.provider %r" % (fid, R.get("provider")))
        if R.get("provider", "github") in ("github", "gitlab"):
            if not re.fullmatch(r"[\w.-]+(/[\w.-]+)+", R.get("repo", "")):
                e.append("%s: release.repo" % fid)
            if not R.get("asset"):
                e.append("%s: release.asset missing" % fid)
        else:
            if not R.get("url") or not (R.get("version_url") or R.get("version")):
                e.append("%s: url provider needs url + version_url (or version)" % fid)
        if R.get("format", "tar") not in FORMATS:
            e.append("%s: release.format %r" % (fid, R.get("format")))
        if R.get("format", "tar") not in ("deb",) and not (R.get("bin") or (a.get("launcher") or {}).get("exec")):
            e.append("%s: release needs bin = [...] or a launcher exec" % fid)
    S = a.get("source")
    if S is not None:
        if S.get("system", "script") not in SYSTEMS:
            e.append("%s: source.system %r" % (fid, S.get("system")))
        if S.get("system") == "debian":
            pass
        elif not (S.get("tarball") or S.get("git")):
            e.append("%s: source needs tarball or git" % fid)
        if S.get("system") != "debian" and S.get("ref", "release") == "release" and not (R and R.get("provider") in ("github", "gitlab")) \
                and not S.get("repo"):
            e.append("%s: source.ref = release needs a github/gitlab [release] or source.repo" % fid)
        if S.get("system") == "script" and not S.get("steps"):
            e.append("%s: system = script needs steps" % fid)
        if not isinstance(S.get("build_deps", []), list):
            e.append("%s: build_deps must be a list" % fid)
    D = a.get("distro")
    if D is not None and (not isinstance(D.get("apt"), list) or not D["apt"]):
        e.append("%s: distro.apt must be a non-empty list" % fid)
    L = a.get("launcher")
    if L is not None and not isinstance(L, dict):
        e.append("%s: launcher must be a table" % fid)
    return e


def cmd_check():
    apps, errs = load_all()
    for fid, a in apps.items():
        errs += check_app(fid, a)
    ids = {}
    for fid, a in apps.items():
        ids.setdefault(a.get("name"), []).append(fid)
    for n, l in ids.items():
        if len(l) > 1:
            errs.append("duplicate name %r: %s" % (n, l))
    cats = {}
    for a in apps.values():
        cats[a.get("category")] = cats.get(a.get("category"), 0) + 1
    meth = {m: sum(1 for a in apps.values() if m in sbstore.app_methods(a)) for m in sbstore.METHODS}
    for x in errs:
        print("ERROR", x)
    print("%d recipes; methods: %s; categories: %s; %d error(s)" % (
        len(apps), ", ".join("%s %d" % kv for kv in meth.items()),
        ", ".join("%s %d" % kv for kv in sorted(cats.items(), key=lambda kv: str(kv[0]))), len(errs)))
    return 1 if errs else 0


def resolve_all(ids=None, quiet=False):
    apps, _ = load_all()
    out = {}
    for fid in sorted(ids or apps):
        a = apps[fid]
        a.setdefault("id", fid)
        r = {}
        for m in ("release", "source"):
            if m not in sbstore.app_methods(a):
                continue
            try:
                x = sbstore.resolve(a, m)
                r[m] = x["version"]
                if m == "release":
                    spec = a["release"]
                    pick = (sbstore._pick_assets(x["assets"], spec["asset"], x["version"]) if spec.get("asset")
                            else x["assets"][:1])
                    r["asset"] = pick[0]["name"] if pick else None
                    r["asset_mb"] = round((pick[0].get("size") or 0) / 1048576, 1) if pick else 0
            except sbstore.StoreError as e:
                r[m + "_error"] = e.detail
        out[fid] = r
        if not quiet:
            print("%-22s %s" % (fid, json.dumps(r)))
    return out


def cmd_build(out, do_resolve):
    rc = cmd_check()
    if rc:
        return rc
    apps, _ = load_all()
    known = {}
    kpath = os.path.join(CAT, "known.json")
    if os.path.exists(kpath):
        known = json.load(open(kpath))
    if do_resolve:
        known = {"checked": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime()), "apps": resolve_all(quiet=False)}
        with open(kpath, "w") as f:
            json.dump(known, f, indent=1, sort_keys=True)
    lst = []
    for fid in sorted(apps):
        a = dict(apps[fid])
        k = (known.get("apps") or {}).get(fid) or {}
        a["known"] = {m: k[m] for m in ("release", "source", "distro") if isinstance(k.get(m), str)}
        if k.get("asset_mb"):
            a.setdefault("release", {})["dl_mb"] = k["asset_mb"]
        lst.append(a)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "catalog.json"), "w") as f:
        json.dump({"version": 1, "built": time.strftime("%Y-%m-%d", time.gmtime()),
                   "checked": known.get("checked"), "apps": lst}, f, indent=1, sort_keys=True)
    icons = os.path.join(TOP, "..", "..", "build", "appstore-icons")
    if os.path.isdir(icons):
        shutil.copytree(icons, os.path.join(out, "icons"), dirs_exist_ok=True)
    print("wrote %s/catalog.json (%d apps)" % (out, len(lst)))
    return 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    if argv[0] == "check":
        return cmd_check()
    if argv[0] == "build":
        return cmd_build(argv[1], "--resolve" in argv)
    if argv[0] == "resolve":
        resolve_all(argv[1:] or None)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
