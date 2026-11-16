#!/usr/bin/env python3
"""mk-canvas.py IN.adt OUT.adt WIDTH HEIGHT: the same device tree with another display canvas (the size the
framebuffer driver presents to WindowServer; /arm-io/disp0 canvas-width / canvas-height). The driver scales the
canvas onto the 1440x2960 panel when the boot-args give the same size (sweetfb_scale=1 sweetfb_lw=W sweetfb_lh=H).
With the display override (2x default) the desktop is WIDTH/2 x HEIGHT/2 points."""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'ref/ios-herolte/tools'))
import adt

src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
buf = adt.load(src)
root, _ = adt.parse(buf)
done = 0

def walk(node):
    global done
    names = [p[0] for p in node.props]
    if 'canvas-width' in names and 'canvas-height' in names:
        for key, val in (('canvas-width', w), ('canvas-height', h)):
            i = names.index(key)
            old = struct.unpack('<I', node.props[i][2][:4])[0]
            node.props[i] = (key, node.props[i][1], struct.pack('<I', val))
            print(f'{node.name} {key}: {old} -> {val}')
        done += 1
    for c in node.children:
        walk(c)

walk(root)
if done != 1:
    sys.exit(f'{done} nodes with a canvas size: expected exactly one')
out = adt.serialize(root)
if len(out) != len(buf):
    sys.exit(f'size changed: {len(buf)} -> {len(out)}')
open(dst, 'wb').write(out)
