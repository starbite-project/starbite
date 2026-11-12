#!/usr/bin/python3 -I
"""dpkg-merge: a new base image under an old overlay (docs/STARBITE-LINUX-APPS.md, Track E).

The overlay's /var/lib/dpkg/status is a copy-up of the base it was made on (plus what the App Store installed). A
new base image brings new packages (and new versions), but the overlay's old copy hides its status file, so dpkg
would think they are missing. sbinit runs this once per base (keyed by the base's status checksum):

    dpkg-merge.py LOWER UPPER OUT

LOWER = the new base's status (read-only, under the overlay), UPPER = the visible status (the overlay's copy),
OUT = the merged status (written atomically: OUT.new, then rename). The result:
  - every package of the new base, as the base has it (its version wins);
  - plus each package only the overlay has, kept when it still has files: its /var/lib/dpkg/info/<pkg>.list (or
    <pkg>:<arch>.list) names at least one regular file that exists (an app the App Store installed); a package of the
    OLD base that the new base dropped has no files left (they were in the old lower layer) and is left out.
Prints one summary line. Exit 0, or 1 (nothing written) when an input is unreadable.
"""
import os
import sys


def stanzas(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    out = []
    for block in text.split("\n\n"):
        block = block.strip("\n")
        if not block:
            continue
        name = arch = None
        for line in block.split("\n"):
            if line.startswith("Package: "):
                name = line[9:].strip()
            elif line.startswith("Architecture: "):
                arch = line[14:].strip()
        if name:
            out.append(((name, arch), block))
    return out


def has_files(name, arch, info="/var/lib/dpkg/info"):
    for list_name in (f"{name}.list", f"{name}:{arch}.list"):
        p = os.path.join(info, list_name)
        try:
            with open(p, encoding="utf-8", errors="surrogateescape") as f:
                for line in f:
                    line = line.rstrip("\n")
                    if line and os.path.isfile(line) and not os.path.islink(line):
                        return True
        except OSError:
            continue
    return False


def main(argv):
    if len(argv) != 4:
        print("usage: dpkg-merge.py LOWER UPPER OUT", file=sys.stderr)
        return 2
    lower, upper, out = argv[1:]
    info = os.environ.get("DPKG_INFO", "/var/lib/dpkg/info")
    try:
        low = stanzas(lower)
        up = stanzas(upper)
    except OSError as e:
        print(f"dpkg-merge: {e}", file=sys.stderr)
        return 1
    have = {k for k, _ in low}
    kept, dropped = [], []
    for k, block in up:
        if k in have:
            continue
        if has_files(k[0], k[1], info):
            kept.append(block)
        else:
            dropped.append(k[0])
    blocks = [b for _, b in low] + kept
    tmp = out + ".new"
    with open(tmp, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("\n\n".join(blocks) + "\n\n")
    os.replace(tmp, out)
    print(f"dpkg-merge: {len(low)} base packages + {len(kept)} installed by the store"
          f"{(', dropped ' + ' '.join(dropped[:8]) + (' ...' if len(dropped) > 8 else '')) if dropped else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
