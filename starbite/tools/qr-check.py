#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Decodes every symbol starbite/tools/qr-test.c wrote with a real reader (zbarimg from zbar-tools, binary mode:
# the payload bytes as they are, no charset guessing, no newline) and checks it against manifest.txt exactly.
#   python3 starbite/tools/qr-check.py starbite/out/qr          (in the Lima VM; apt-get install zbar-tools)
import os
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor


def decode(path):
    r = subprocess.run(["zbarimg", "--raw", "-q", "-Sdisable", "-Sqrcode.enable", "-Sbinary", path],
                       capture_output=True, timeout=60)
    return r.returncode, r.stdout


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: qr-check.py <directory with manifest.txt>")
    d = sys.argv[1]
    rows = []
    with open(os.path.join(d, "manifest.txt")) as f:
        for line in f:
            file, hexdata, ver, ecc, mask, name = line.split(" ", 5)
            data = b"" if hexdata == "-" else bytes.fromhex(hexdata)
            rows.append((file, data, int(ver), "LMQH"[int(ecc)], int(mask), name.strip()))

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(lambda r: decode(os.path.join(d, r[0])), rows))

    ok, bad = 0, []
    seen = Counter()
    for (file, data, ver, ecc, mask, name), (rc, out) in zip(rows, results):
        if rc == 0 and out == data:
            ok += 1
            seen[(ver, ecc)] += 1
            seen[mask] += 1
        else:
            bad.append((file, name, rc, len(data), out[:40]))
    for b in bad:
        print("FAIL %s (%s): zbarimg exit %d, %d bytes expected, got %r" % b)
    vers = sorted({v for v, _ in (k for k in seen if isinstance(k, tuple))})
    print("decoded %d of %d symbols; versions %s, ecc %s, masks %s" % (
        ok, len(rows), ",".join(map(str, vers)), "".join(sorted({e for k in seen if isinstance(k, tuple)
                                                                    for e in k[1]}, key="LMQH".index)),
        ",".join(str(m) for m in range(8) if seen[m])))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
