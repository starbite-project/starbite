#!/usr/bin/python3
"""Honest RAM for the store (measure-ram.sh runs this as root on the VM, aarch64 like the guest).

    measure-ram.py ROOT LIST OUT.tsv [KEY...]

ROOT is a chroot of base.erofs + every store entry installed, with cage (wlroots 0.18, headless, pixman, 720x1440:
the phone's window at L1's 2x) on wayland-0 for uid 1000. Per entry, the app is started as the user in a
session of its own with a real document; every 0.5 s the PSS (smaps_rollup) of all processes of that session is
summed. Reported: the peak, the mean of the last 5 s, the peak RSS sum. ram_mb = the peak PSS, rounded up to 10 MB.
cage, the D-Bus daemon and D-Bus-activated services (dconf) are not counted (they are the platform's)."""
import glob
import os
import signal
import subprocess
import sys
import time

ROOT, LIST, OUT = sys.argv[1:4]
ONLY = set(sys.argv[4:])
D = "/home/user/Documents"
P = "/home/user/Pictures"
ENV = {
    "HOME": "/home/user", "USER": "user", "LOGNAME": "user", "XDG_RUNTIME_DIR": "/run/user/1000",
    "WAYLAND_DISPLAY": "wayland-0", "XDG_SESSION_TYPE": "wayland", "GDK_BACKEND": "wayland",
    "GSK_RENDERER": "cairo", "NO_AT_BRIDGE": "1", "GTK_A11Y": "none", "MOZ_ENABLE_WAYLAND": "1",
    "LIBGL_ALWAYS_SOFTWARE": "1", "GTK_USE_PORTAL": "0", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
    "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/games", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
    "SAL_USE_VCLPLUGIN": "gtk3",
}
# workload -> (argv, seconds, what it does)
WORK = {
    "firefox": (["firefox-esr", "--no-remote", "--profile", "/tmp/ffprof", f"file://{D}/page.html"], 45,
                "Firefox ESR on a local 250 KB article (30 sections, tables, SVG, a canvas animation)"),
    "lo-writer": (["libreoffice", "--writer", "--norestore", "--nologo", f"{D}/doc.odt"], 40,
                  "LibreOffice Writer (gtk3 VCL) with a 25-page document open"),
    "lo-calc": (["libreoffice", "--calc", "--norestore", "--nologo", f"{D}/sheet.ods"], 40,
                "LibreOffice Calc with a 20000 x 8 sheet open"),
    "lo-impress": (["libreoffice", "--impress", "--norestore", "--nologo", f"{D}/slides.fodp"], 40,
                   "LibreOffice Impress with a 10-slide presentation open"),
    "gimp": (["gimp", "--no-splash", f"{P}/photo.png"], 50,
             "GIMP with a 4000x3000 photo open"),
    "inkscape": (["inkscape", f"{P}/drawing.svg"], 45, "Inkscape with a 2000-shape SVG open"),
    "drawing": (["drawing", f"{P}/photo-small.png"], 30, "Drawing with a 1600x1200 picture open"),
    "apostrophe": (["apostrophe", f"{D}/notes.md"], 35, "Apostrophe with a 4 KB Markdown file and its preview"),
    "xournalpp": (["xournalpp"], 30, "Xournal++ with a new notebook"),
    "foot": (["foot"], 12, "foot with a shell"),
    "text-editor": (["gnome-text-editor", "--standalone", f"{D}/notes.md"], 20, "Text Editor with a Markdown file"),
    "files": (["pcmanfm", "/home/user"], 20, "PCManFM on the home folder"),
    "nautilus": (["nautilus", "--new-window", "/home/user"], 25, "Nautilus on the home folder"),
    "image-viewer": (["gpicview", f"{P}/photo.png"], 20, "gpicview on a 4000x3000 photo"),
    "pdf-viewer": (["papers", f"{D}/doc.pdf"], 20, "Papers on a 25-page PDF"),
}


def desktop_exec(pkg):
    """the Exec= of the package's main .desktop file (generic GTK apps), field codes dropped"""
    lst = f"{ROOT}/var/lib/dpkg/info/{pkg}.list"
    if not os.path.exists(lst):
        lst = f"{ROOT}/var/lib/dpkg/info/{pkg}:arm64.list"
    for f in open(lst).read().split():
        if f.startswith("/usr/share/applications/") and f.endswith(".desktop"):
            for line in open(ROOT + f, encoding="utf-8", errors="replace"):
                if line.startswith("Exec="):
                    return [a for a in line[5:].split() if not a.startswith("%")]
    return None


def session_pids(sid):
    pids = []
    for st in glob.glob("/proc/[0-9]*/stat"):
        try:
            s = open(st).read()
        except OSError:
            continue
        rest = s[s.rfind(")") + 2:].split()
        if int(rest[3]) == sid:  # field 6: session
            pids.append(int(st.split("/")[2]))
    return pids


def mem(pids):
    pss = rss = 0
    for p in pids:
        try:
            for line in open(f"/proc/{p}/smaps_rollup"):
                if line.startswith("Pss:"):
                    pss += int(line.split()[1])
                elif line.startswith("Rss:"):
                    rss += int(line.split()[1])
        except OSError:
            pass
    return pss, rss


def shot(name):
    """grim's picture of the output (proof that the app drew), in ROOT/tmp/shots"""
    cmd = ["chroot", ROOT, "setpriv", "--reuid=1000", "--regid=1000", "--init-groups", "env", "-i"]
    cmd += [f"{k}={v}" for k, v in ENV.items()] + ["grim", f"/tmp/shots/{name}.png"]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)


def measure(argv, secs, name):
    cmd = ["chroot", ROOT, "setpriv", "--reuid=1000", "--regid=1000", "--init-groups", "env", "-i"]
    cmd += [f"{k}={v}" for k, v in ENV.items()] + argv
    log = open(f"/tmp/measure-{os.path.basename(argv[0])}.log", "w")
    pr = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True,
                          cwd="/")
    sid = pr.pid
    samples, peak_rss, nproc = [], 0, 0
    t0 = time.time()
    while time.time() - t0 < secs:
        time.sleep(0.5)
        pids = session_pids(sid)
        pss, rss = mem(pids)
        samples.append((time.time() - t0, pss))
        peak_rss = max(peak_rss, rss)
        nproc = max(nproc, len(pids))
    shot(name)
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(sid, sig)
        except ProcessLookupError:
            break
        for _ in range(10):
            if not session_pids(sid):
                break
            time.sleep(0.3)
    pr.wait()
    peak = max(p for _, p in samples)
    tail = [p for t, p in samples if t >= secs - 5] or [samples[-1][1]]
    return peak, sum(tail) / len(tail), peak_rss, nproc


def prepare():
    """a Firefox profile owned by the user, without first-run tabs (only our page loads)"""
    prof = f"{ROOT}/tmp/ffprof"
    os.makedirs(prof, exist_ok=True)
    with open(f"{prof}/user.js", "w") as f:
        for k, v in (("browser.startup.homepage_override.mstone", '"ignore"'), ("startup.homepage_welcome_url", '""'),
                     ("browser.aboutwelcome.enabled", "false"), ("datareporting.policy.firstRunURL", '""'),
                     ("browser.shell.checkDefaultBrowser", "false"), ("app.update.enabled", "false")):
            f.write(f"user_pref(\"{k}\", {v});\n")
    for d, _, fs in os.walk(prof):
        os.chown(d, 1000, 1000)
        for x in fs:
            os.chown(os.path.join(d, x), 1000, 1000)


def main():
    prepare()
    rows = []
    for line in open(LIST, encoding="utf-8"):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        key, apt, work = f[0], f[5].split(), f[6]
        if ONLY and key not in ONLY:
            continue
        if work == "gtk-app":
            argv = desktop_exec(apt[0])
            secs, what = 20, "launched from its .desktop Exec, idle main window"
        else:
            argv, secs, what = WORK[work]
        if not argv:
            print(f"measure-ram: {key}: no command")
            continue
        peak, steady, rss, nproc = measure(argv, secs, key)
        ram = -(-peak // 10240) * 10
        rows.append((key, ram, f"PSS peak {peak // 1024} MB / last-5s {int(steady) // 1024} MB / RSS {rss // 1024} MB, "
                               f"{nproc} procs; cage headless pixman 720x1440, {secs} s: {what}"))
        print(f"measure-ram: {key:22s} peak PSS {peak / 1024:7.1f} MB  steady {steady / 1024:7.1f} MB  "
              f"RSS {rss / 1024:7.1f} MB  procs {nproc}  -> ram_mb {ram}", flush=True)
    with open(OUT, "w") as o:
        o.write("# key\tram_mb\tmethod (measure-ram.sh)\n")
        for r in rows:
            o.write("\t".join(map(str, r)) + "\n")


main()
