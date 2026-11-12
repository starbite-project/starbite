#!/usr/bin/env python3
"""A fake apt-get / apt-cache / apt-mark / dpkg-query / dpkg-deb for sbstore's host tests.
State in $FAKEAPT_DB (JSON): {"available": {pkg: {"version": v, "depends": [..]}}, "installed": {pkg: v},
"auto": [..], "calls": [[tool, args...], ...]}"""
import json, os, sys

db_path = os.environ["FAKEAPT_DB"]
db = json.load(open(db_path))
db.setdefault("installed", {}); db.setdefault("auto", []); db.setdefault("calls", [])
tool, args = sys.argv[1], sys.argv[2:]
db["calls"].append([tool] + args)
status_fd = None
opts = []
i = 0
rest = []
while i < len(args):
    if args[i] == "-o":
        opts.append(args[i + 1]); i += 2; continue
    rest.append(args[i]); i += 1
for o in opts:
    if o.startswith("APT::Status-Fd="):
        status_fd = int(o.split("=", 1)[1])
flags = [a for a in rest if a.startswith("-")]
words = [a for a in rest if not a.startswith("-")]

def save():
    json.dump(db, open(db_path, "w"))

def st(line):
    if status_fd is not None:
        os.write(status_fd, (line + "\n").encode())

def closure(pkgs):
    out, todo = [], list(pkgs)
    while todo:
        p = todo.pop(0)
        if p in out or p in db["installed"]:
            continue
        out.append(p)
        todo += db["available"].get(p, {}).get("depends", [])
    return out

once = os.environ.get("FAKEAPT_LISTS_BROKEN_ONCE")   # cp3: apt can't read its saved index until the lists are redone
if once and tool == "apt-get" and "install" in args and os.path.exists(once):
    lists = os.path.dirname(once)
    if os.path.exists(os.path.join(lists, "_repo_._Packages")):   # (the lists still the old ones: still broken)
        print("E: Splitting of clearsigned file /var/lib/apt/lists/_repo_._Release failed as it doesn't contain all expected parts")
        print("E: The package lists or status file could not be parsed or opened.")
        save(); sys.exit(100)
if tool == "apt-get":
    verb, pk = (words[0] if words else ""), words[1:]
    if verb == "update":
        print("Hit: fake"); save(); sys.exit(0)
    if verb == "install":
        names = []
        for p in pk:
            if p.endswith(".deb"):
                names.append(json.load(open(p))["Package"]) if p.endswith(".deb") else None
                db["available"].setdefault(names[-1], {"version": "9"})
            else:
                names.append(p)
        if "--only-upgrade" in flags:
            for p in names:
                db["installed"][p] = db["available"][p]["version"]
            save(); sys.exit(0)
        bad = [p for p in names if p not in db["available"]]
        if bad:
            print("E: Unable to locate package %s" % bad[0]); save(); sys.exit(100)
        todo = closure(names)
        for n, p in enumerate(todo):
            st("dlstatus:%s:%d:Retrieving %s" % (p, 100 * (n + 1) // len(todo), p))
        for n, p in enumerate(todo):
            st("pmstatus:%s:%d:Installing %s" % (p, 100 * (n + 1) // len(todo), p))
            db["installed"][p] = db["available"][p]["version"]
            print("Setting up %s" % p)
        save(); sys.exit(0)
    if verb == "remove":
        gone = [p for p in pk if p in db["installed"]]
        # reverse deps go too
        changed = True
        while changed:
            changed = False
            for q, v in list(db["installed"].items()):
                if q not in gone and any(d in gone for d in db["available"].get(q, {}).get("depends", [])):
                    gone.append(q); changed = True
        if "-s" in flags:
            for p in gone:
                print("Remv %s [%s]" % (p, db["installed"][p]))
            save(); sys.exit(0)
        for p in gone:
            st("pmstatus:%s:50:Removing %s" % (p, p))
            del db["installed"][p]
        save(); sys.exit(0)
    print("E: fake apt-get: %s" % verb); save(); sys.exit(100)
if tool == "apt-cache":
    p = words[1]
    a = db["available"].get(p)
    print("%s:\n  Installed: %s\n  Candidate: %s" % (p, db["installed"].get(p, "(none)"), a["version"] if a else "(none)"))
    save(); sys.exit(0)
if tool == "apt-mark":
    db["auto"] += words[1:]; save(); sys.exit(0)
if tool == "dpkg-query":
    for p, v in sorted(db["installed"].items()):
        print("%s ii  %s" % (p, v))
    save(); sys.exit(0)
if tool == "dpkg":                       # --configure -a (the store's repair): the pending updates are applied
    upd = os.environ.get("FAKEAPT_DPKG_UPDATES")
    if upd and os.path.isdir(upd):
        for f in os.listdir(upd):
            os.unlink(os.path.join(upd, f))
    save(); sys.exit(0)
if tool == "dpkg-deb":
    print(json.load(open(words[-2] if len(words) > 1 else words[0]))[words[-1]]); save(); sys.exit(0)
sys.exit(2)
