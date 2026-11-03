"""Multi-frame motion check for menu v7's moving frames (docs/MENU7-BUILD.md 6.1).

  motion.py lists menu.dl N STEP A.dl B.dl   A: the menu's section cards (base shapes with flag 1)
                                             at 0, then N frames each STEP px further (cumulative,
                                             no rest between); B: the last position alone (exact)
  motion.py err A.ppm B.ppm D                the error of A's last frame against B: the cards' boxes
                                             (moved by D), the icon tiles' boxes, the whole panel
"""
import sys


def lists(src, n, step, oa, ob):
    head, shapes = [], []
    for line in open(src):
        p = line.split()
        if not p or p[0] in ('#', 'frame'):
            continue
        if p[0] in ('dim', 'back', 'layer', 'clip'):
            head.append(line.rstrip())
        elif p[0] == 'shape' and not int(p[2], 0) & 2:
            shapes.append(p)

    def frame(d):
        out = ['frame'] + [h for h in head if not h.startswith('clip') and not h.startswith('layer over')]
        for p in shapes:
            q = list(p)
            if int(q[2], 0) & 1:
                q[4] = '%.3f' % (float(q[4]) + d)
            out.append(' '.join(q))
        return out

    a = []
    for i in range(n + 1):
        a += frame(i * step)
    open(oa, 'w').write('\n'.join(a) + '\n')
    open(ob, 'w').write('\n'.join(frame(n * step)) + '\n')


def err(pa, pb, D):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(pa)).astype(int)
    b = np.asarray(Image.open(pb)).astype(int)
    cards = [(629, 296), (957, 296), (1285, 296), (1613, 1260)]   # menu.dl's section cards (x 72 .. 1368)
    tiles = [691, 1019, 1347, 1675]                                # their icon tiles (x 122 .. 294)
    d = np.abs(a - b).max(axis=2)
    m = np.zeros(d.shape, bool)
    t = np.zeros(d.shape, bool)
    for y, h in cards:
        y0, y1 = int(max(246, y + D)), int(min(2480, y + D + h))
        if y1 > y0:
            m[y0:y1, 72:1368] = True
    for y in tiles:
        y0, y1 = int(max(246, y + D - 8)), int(min(2480, y + D + 180))
        if y1 > y0:
            t[y0:y1, 114:302] = True
    tm = d[t] if t.any() else np.zeros(1, int)
    print(f"cards mean {d[m].mean():.2f} p99 {np.percentile(d[m], 99):.0f} | tiles mean {tm.mean():.2f} max {tm.max()} | "
          f"panel mean {d.mean():.3f} p99.9 {np.percentile(d, 99.9):.0f} max {d.max()} n64 {(d > 64).sum()}")


if __name__ == '__main__':
    if len(sys.argv) == 7 and sys.argv[1] == 'lists':
        lists(sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), sys.argv[5], sys.argv[6])
    elif len(sys.argv) == 5 and sys.argv[1] == 'err':
        err(sys.argv[2], sys.argv[3], float(sys.argv[4]))
    else:
        sys.exit(__doc__)
