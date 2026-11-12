#!/usr/bin/env python3
"""The App Store's offline mirror: what the guest installs from when it has no network (the phone until L5).

  mirror.py OUT [--max-mb N] [--total-mb N] [--source] [ID...]
For each app (default: all with a release method), resolve the latest release now, download its aarch64 asset,
verify it as the guest would (GitHub digest / sums file / sums URL), and write OUT/<id>/release.json (+ the file).
--source also mirrors the source tarball of the same tag (OUT/<id>/source.json). Files over --max-mb (default 80)
are skipped, and the whole mirror stops at --total-mb (default 1500). Re-running keeps what is already there.
The guest reads it at SBSTORE_MIRROR (default /repo/appstore/mirror).
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.path.insert(0, TOP)
sys.path.insert(0, HERE)
import sbstore  # noqa: E402
import catalog as catmod  # noqa: E402


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    out = os.path.abspath(argv[0])
    args = argv[1:]
    max_mb, total_mb, src = 80, 1500, False
    ids = []
    i = 0
    while i < len(args):
        if args[i] == "--max-mb":
            max_mb = int(args[i + 1]); i += 2; continue
        if args[i] == "--total-mb":
            total_mb = int(args[i + 1]); i += 2; continue
        if args[i] == "--source":
            src = True; i += 1; continue
        ids.append(args[i]); i += 1
    apps, errs = catmod.load_all()
    if errs:
        print("\n".join(errs))
        return 1
    os.makedirs(out, exist_ok=True)
    used = 0
    for dp, dn, fn in os.walk(out):
        used += sum(os.path.getsize(os.path.join(dp, f)) for f in fn)
    report = []
    for aid in sorted(ids or apps):
        a = dict(apps[aid])
        a.setdefault("id", aid)
        d = os.path.join(out, aid)
        if "release" in sbstore.app_methods(a):
            try:
                rel = sbstore.resolve(a, "release")
                spec = a["release"]
                pick = (sbstore._pick_assets(rel["assets"], spec["asset"], rel["version"]) if spec.get("asset")
                        else rel["assets"][:1])
                if not pick:
                    raise sbstore.StoreError("no asset")
                asset = pick[0]
                size = asset.get("size") or 0
                if size > max_mb << 20:
                    report.append((aid, "release", "skipped: %d MB > %d" % (size >> 20, max_mb)))
                elif used + size > total_mb << 20:
                    report.append((aid, "release", "skipped: the mirror is full"))
                else:
                    want, how = sbstore.expected_sha(dict(rel, id=aid), asset, spec.get("verify", {}), rel["version"])
                    dst = os.path.join(d, asset["name"])
                    old = os.path.exists(dst)
                    got = sbstore.download(asset["url"], dst, size=size, sha256=want)
                    if not old:
                        used += os.path.getsize(dst)
                    j = {"version": rel["version"], "tag": rel["tag"], "published": rel.get("published"),
                         "assets": [{"name": asset["name"], "file": asset["name"], "size": os.path.getsize(dst),
                                     "sha256": got}],
                         "verified": how, "mirrored": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())}
                    with open(os.path.join(d, "release.json"), "w") as f:
                        json.dump(j, f, indent=1)
                    # drop older mirrored files of this app
                    for fn in os.listdir(d):
                        if fn not in (asset["name"], "release.json", "source.json") and not fn.endswith("-src.tar.gz"):
                            os.unlink(os.path.join(d, fn))
                    report.append((aid, "release", "%s %s (%.1f MB, %s)" % (rel["version"], asset["name"],
                                                                           os.path.getsize(dst) / 1048576, how)))
            except sbstore.StoreError as e:
                report.append((aid, "release", "ERROR " + e.detail))
        if src and "source" in sbstore.app_methods(a):
            try:
                rel = sbstore.resolve(a, "source")
                if not rel.get("tarball"):
                    raise sbstore.StoreError("no tarball", "git-only source")
                name = "%s-%s-src.tar.gz" % (aid, sbstore.safe_ver(rel["version"]))
                dst = os.path.join(d, name)
                if used > total_mb << 20:
                    report.append((aid, "source", "skipped: the mirror is full"))
                    continue
                old = os.path.exists(dst)
                got = sbstore.download(rel["tarball"], dst)
                if not old:
                    used += os.path.getsize(dst)
                with open(os.path.join(d, "source.json"), "w") as f:
                    json.dump({"version": rel["version"], "tag": rel["tag"], "tarball_file": name,
                               "tarball_sha256": got, "assets": [],
                               "mirrored": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())}, f, indent=1)
                report.append((aid, "source", "%s (%.1f MB)" % (rel["version"], os.path.getsize(dst) / 1048576)))
            except sbstore.StoreError as e:
                report.append((aid, "source", "ERROR " + e.detail))
    for r in report:
        print("%-22s %-8s %s" % r)
    print("mirror %s: %.0f MB" % (out, used / 1048576))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
