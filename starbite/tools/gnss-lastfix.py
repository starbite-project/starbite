#!/usr/bin/env python3
# Starbite OS 1.2-alpha28 (docs/GNSS.md path iii): take Android's last GNSS fix and leave it on the microSD card for
# Starbite OS (kern/gnssfix.h: /STARBITE/GNSS/LASTFIX.TXT, one "LASTFIX v1 key=value ..." line).
#
# Run on the Mac while the phone runs LineageOS (or One UI) with adb, after a GPS fix (Maps / any GPS app, sky view):
#   python3 starbite/tools/gnss-lastfix.py            # dry run: prints the line it would write
#   python3 starbite/tools/gnss-lastfix.py --write    # writes it to the card's exFAT volume
# It reads `dumpsys location` (the gps provider's last location only; network and fused fixes are not GNSS) and
# never changes anything else on the phone. A test bridge until an on-phone helper exists.
import re
import subprocess
import sys
import time


def adb(*args):
    return subprocess.run(["adb", "shell", *args], capture_output=True, text=True, timeout=30).stdout


def last_gps_fix(dump):
    # Android 12+: "last location=Location[gps 60.169856,24.938379 hAcc=8.5 et=+1h2m3s alt=12.3 ... {...}]" under
    # the gps provider; older: "Last Known Locations:\n    gps: Location[gps 60.16,24.93 acc=8 ...]"
    best = None
    for m in re.finditer(r"Location\[gps (-?\d+\.\d+),(-?\d+\.\d+)([^\]]*)\]", dump):
        lat, lon, rest = float(m.group(1)), float(m.group(2)), m.group(3)
        f = {"lat": lat, "lon": lon}
        for key, pat in (("acc", r"\bh?[aA]cc=(\d+(?:\.\d+)?)"), ("alt", r"\balt=(-?\d+(?:\.\d+)?)"),
                         ("sats", r"satellites=(\d+)"), ("utc", r"\btime=(\d{10,13})")):
            k = re.search(pat, rest)
            if k:
                f[key] = float(k.group(1))
        if "utc" in f and f["utc"] > 1e12:
            f["utc"] /= 1000.0
        best = f if best is None else best
    return best


def card_root():
    for line in adb("ls", "/storage").split():
        if re.fullmatch(r"[0-9A-F]{4}-[0-9A-F]{4}", line):
            return "/storage/" + line
    return None


def main():
    dump = adb("dumpsys", "location")
    f = last_gps_fix(dump)
    if not f:
        print("no gps fix in dumpsys location (take a fix first: sky view, a GPS app open)")
        return 1
    utc = int(f.get("utc", time.time()))
    parts = ["LASTFIX v1", "utc=%d" % utc, "lat=%.6f" % f["lat"], "lon=%.6f" % f["lon"]]
    if "alt" in f:
        parts.append("alt=%.1f" % f["alt"])
    if "acc" in f:
        parts.append("acc=%.1f" % f["acc"])
    if "sats" in f:
        parts.append("sats=%d" % int(f["sats"]))
    parts.append("src=gps")
    line = " ".join(parts)
    print(line)
    if "--write" not in sys.argv:
        print("(dry run; --write puts it on the card)")
        return 0
    root = card_root()
    if not root:
        print("no microSD card mounted under /storage")
        return 1
    d = root + "/STARBITE/GNSS"
    adb("mkdir", "-p", d)
    adb("sh", "-c", "'printf \"%s\\n\" \"" + line + "\" > " + d + "/LASTFIX.TXT'")
    print("written:", adb("cat", d + "/LASTFIX.TXT").strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
