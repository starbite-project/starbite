#!/usr/bin/env python3
"""The App Store's screenshots for the card (Track F's carousel): shots.sh's PNGs (720 x 1280) -> baseline JPEG,
quality 85, sRGB (Starbite's mj decoder refuses progressive), plus index.json:
    {"<catalog id>": [{"file": "<id>-<n>.jpg", "w": 720, "h": 1280, "caption": "..."}]}
Their place on the card: p1 (exFAT) /starbite/linux/shots/. Runs on the Mac (sips does the JPEG):
    python3 shots-pack.py PNG_DIR OUT_DIR
A shot is left out when its run failed (<id>.log without RESULT PASS) or it is nearly one colour (an app that
showed nothing: its PNG under 8 KB)."""
import json, os, pathlib, subprocess, sys

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
dst.mkdir(parents=True, exist_ok=True)
index, left = {}, []
for cap in sorted(src.glob('*.captions')):
    aid = cap.stem
    log = src / (aid + '.log')
    if not log.exists() or 'RESULT PASS' not in log.read_text(errors='replace'):
        left.append(aid + ' (the run failed)')
        continue
    shots, seen = [], set()
    for line in cap.read_text().splitlines():
        n, _, caption = line.partition('\t')
        png = src / f'{aid}-{n}.png'
        if not png.exists() or png.stat().st_size < 8192:
            left.append(f'{aid}-{n} (missing or blank)')
            continue
        if png.read_bytes() in seen:
            left.append(f'{aid}-{n} (the same as an earlier shot)')
            continue
        seen.add(png.read_bytes())
        jpg = dst / f'{aid}-{n}.jpg'
        subprocess.run(['sips', '-s', 'format', 'jpeg', '-s', 'formatOptions', '85', str(png), '--out', str(jpg)],
                       check=True, capture_output=True)
        out = subprocess.run(['sips', '-g', 'pixelWidth', '-g', 'pixelHeight', str(jpg)], capture_output=True,
                             text=True).stdout.split()
        w, h = int(out[out.index('pixelWidth:') + 1]), int(out[out.index('pixelHeight:') + 1])
        kind = subprocess.run(['file', '-b', str(jpg)], capture_output=True, text=True).stdout
        if (w, h) != (720, 1280) or 'progressive' in kind or 'baseline' not in kind:
            left.append(f'{aid}-{n} ({w}x{h}, {kind.strip()[:40]})')
            jpg.unlink()
            continue
        shots.append({'file': jpg.name, 'w': w, 'h': h, 'caption': caption})
    if shots:
        index[aid] = shots
(dst / 'index.json').write_text(json.dumps(index, indent=1, ensure_ascii=False) + '\n')
size = sum(p.stat().st_size for p in dst.iterdir())
print(f'shots-pack: {sum(len(v) for v in index.values())} shots of {len(index)} apps, {size // 1024} KB in {dst}')
for x in left:
    print('  left out:', x)
