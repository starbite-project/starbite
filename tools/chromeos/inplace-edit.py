#!/usr/bin/env python3
"""Same-length text edit of a small file inside chromeos.img, written straight into its data block.

The image is 6.8 GB and the card copy takes minutes to re-send, so edits to ROOT-A files are made
without mounting: the file's one data block is found with debugfs, the replacement keeps the file's
size (no ext2 metadata changes), and the changed 4 KiB block can be dd'd into the card copy too.

  inplace-edit.py IMG OFFSET SIZE OLD NEW [--out BLOCK.bin]
    OFFSET  byte offset of the file's data block in IMG (p3 start * 512 + block * 4096)
    SIZE    file size (bytes after it in the block are left alone)
    OLD/NEW Python string literals (\\n allowed); len(NEW) must equal len(OLD), OLD must occur once
  Prints the block's SHA-256 before and after; --out saves the new block for the card copy.
"""
import argparse, ast, hashlib, sys

ap = argparse.ArgumentParser()
ap.add_argument("img"); ap.add_argument("offset", type=lambda s: int(s, 0)); ap.add_argument("size", type=int)
ap.add_argument("old"); ap.add_argument("new"); ap.add_argument("--out")
a = ap.parse_args()
old = ast.literal_eval('"' + a.old.replace('"', '\\"') + '"').encode()
new = ast.literal_eval('"' + a.new.replace('"', '\\"') + '"').encode()
if len(old) != len(new):
    sys.exit(f"length differs: old {len(old)} new {len(new)}")
if a.size > 4096:
    sys.exit("only single-block files")
with open(a.img, "r+b") as f:
    f.seek(a.offset); blk = f.read(4096)
    data = blk[:a.size]
    print("before", hashlib.sha256(blk).hexdigest())
    if data.count(new) and not data.count(old):
        print("already applied"); newblk = blk
    else:
        if data.count(old) != 1:
            sys.exit(f"OLD occurs {data.count(old)} times: refusing")
        newblk = data.replace(old, new) + blk[a.size:]
        f.seek(a.offset); f.write(newblk)
    print("after ", hashlib.sha256(newblk).hexdigest())
if a.out:
    open(a.out, "wb").write(newblk)
