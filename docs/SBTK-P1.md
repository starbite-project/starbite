# sbtk P1: the Starbite OS UI toolkit and lock screen, on the host

Phase P1 of the UI port plan (`starbite-os-ui-port-plan.md` in the shared notes): a new UI toolkit for Starbite OS
in freestanding C, and the lock screen of the approved concept (`docs/concepts/starbite-os-shell/index.html`,
sha256 `5a87bbb4…`, copied into this branch as the spec it was measured against) built on it, running on the
Mac. Nothing here is built for or staged on the phone.

## What it is

`starbite/sbos/ui/`:

| File | What it does |
|---|---|
| `sbtk.h` | the toolkit's whole interface |
| `tk_comp.c` | layers (cached premultiplied bitmaps; translate, scale about an origin, opacity, clip with mask-image fades, 3-slice stretch), the compositor: damage rects, half-resolution motion frames, a NEON 2x present (1/4 3/4 bilinear), flattening a group into one layer (CALayer's shouldRasterize), idle-time half copies |
| `tk_glass.c` | Liquid Glass: the wallpaper (the concept's CSS gradients and star), its backdrop (blur 14 pt, saturate 1.9, brightness 1.06, once), the per-element bake (shadow, tint gradient, inset rims and glow, specular radials, 1 pt gradient border), glass over local content |
| `tk_raster.c` | bake-time drawing: CSS paints (premultiplied interpolation), rounded rects with per-corner radii, arcs, gaussian blurs, CSS box-shadow, CSS saturate / brightness |
| `tk_text.c` | stb_truetype (v1.26, public domain, `third_party/`) on static Inter Tight 400-800 / Outfit 300 instances; GPOS kerning, tabular figures, Inter's raised colon, Blink's line-box rules |
| `tk_motion.c` | the concept's `SPR` springs and `tracker()` line for line |
| `tk_math.c`, `tk_pack.c` | math without libm; the asset pack reader |
| `shell_lock.c`, `shell.h` | the lock screen: status bar, wordmark, date, clock, the three widgets, the five vendor cards with ghost stacks that spring open into member rows (RUNNING, LAST USED, BETA), list scrolling, the focus sheet with slide to start and the boot overlay, torch and Emergency holds with the ring, the swipe-up unlock to a placeholder home (the concept's first home page, drawn but without behaviour) |
| `host/` | the host harness (`sbtk-host`: stills, films, frame benchmarks, uibench's kernels) and the host platform layer |
| `tools/` | `run.sh` (assets, build, compare, film), fonts, icons, pack, headless-Chrome reference frames (`cdp.mjs`, `concept-ref.mjs`, `concept-geom.mjs`), `compare.py`, `frametimes.py`, the film encoder (AVFoundation) and contact sheets |

The toolkit and shell files include only `sbtk.h` / `shell.h`, `<stdint.h>`, `<stddef.h>` and `<arm_neon.h>` and are
compiled `-ffreestanding -fno-builtin`; the kernel provides `tk_alloc`, `tk_us`, `tk_log` (P2).

## Run it

```sh
sh starbite/sbos/ui/tools/run.sh assets     # fonts (fontTools + HarfBuzz), 102 icons (Chrome), build/sbtk/sbtk.pak
sh starbite/sbos/ui/tools/run.sh build      # build/sbtk/sbtk-host (clang, arm64)
sh starbite/sbos/ui/tools/run.sh compare    # tools/states.json: Chrome and sbtk -> build/sbtk/compare/
sh starbite/sbos/ui/tools/run.sh film unlock   # unlock | group | focus | holds -> build/sbtk/films/*.mp4
UIBENCH=uibench-lines.txt sh starbite/sbos/ui/tools/run.sh film unlock   # + the A55 estimate
build/sbtk/sbtk-host build/sbtk/sbtk.pak frames unlock=:0.3   # one state's motion and rest frame cost
```

## How it matches the concept

Each state is rendered by headless Chrome at 1440 x 2960 (the repo's own font files, the clock fixed at Tue 10 Nov
21:05, no transitions, sRGB, `sbSet` driving the state) and by `sbtk-host`, then diffed. Mean |error| per channel
(0..255) over the panel, and the share of pixels off by more than 64:

| state | mean | > 64 | | state | mean | > 64 |
|---|---|---|---|---|---|---|
| lock | 2.02 | 0.75 % | | group-apple | 1.92 | 0.80 % |
| unlock 25 % | 2.28 | 0.54 % | | group-microsoft | 2.00 | 0.70 % |
| unlock 50 % | 2.48 | 0.91 % | | group-starbite (scrolled 304) | 1.65 | 0.55 % |
| unlock 75 % | 1.43 | 0.39 % | | list scrolled 60 | 2.08 | 0.78 % |
| home | 1.17 | 0.26 % | | focus win11 (at rest) | 2.31 | 0.03 % |
| hint shown | 2.02 | 0.75 % | | focus ios26 at 50 % (moving) | 4.04 | 0.58 % |

Budget used: at rest <= 2.5 mean and < 1 % over 64; mid-animation <= 5. What is left is anti-aliasing (text edges
sit within about 1 px of Chrome's; stb_truetype and CoreText rasterise slightly differently) and, in the moving
focus state, the card's backdrop (Chrome filters the live scene under the card every frame; sbtk takes it once and
moves it with the card).

What Chrome does that the concept's CSS doesn't say, and sbtk now does the same:
- **Backdrop roots.** The list (`.nscroll`, a mask-image) is a backdrop root, so the vendor cards' glass filters
  only the list's own content (their ghost stacks), never the wallpaper; `#home` (`will-change: opacity`) is one
  too, so home's glass shows no wallpaper blur. The lock widgets and hold buttons do blur the wallpaper.
- **Letter-spacing turns contextual alternates off** (Blink), so the big clock uses the tabular colon, while
  "07:00 · Vibrate" gets Inter's raised colon.
- **Line boxes** use ascent and descent rounded to whole CSS px and a floored half-leading.
- **Grid min-content:** the Apple and Starbite card text columns grow to the nowrap line and push the chevron
  (Starbite's ends up clipped by the card).

Two slips in the concept's CSS, fixed in the reference render as the concept means them (`cdp.mjs`
`CONCEPT_FIXES`; `SBTK_CONCEPT_ASIS=1` renders it as is): `.dock .app span{display:none}` hides the dock's icons
and badges, not just their labels (the dock shows empty); the app pages' `.row` rule also lands on the Battery
widget's rows (16 px, padding, a border). Worth fixing in the concept itself.

## Frame times (host: one M4 core; the films at 60 fps)

The films drive the shell with synthetic touches at 120 Hz through the same gesture code, springs and compositor
the phone will run; motion frames compose at 720 x 1480 and present 2x, the first frame at rest recomposes
what moved at full resolution.

| film | motion frames | host mean / p90 / max (ms) | first frame at rest (ms) |
|---|---|---|---|
| unlock (swipe, spring, home scaling in) | 34 | 1.32 / 2.67 / 3.54 | 2.0 |
| group (Apple open and closed, scroll with rubber band, Starbite opens and scrolls into view) | 124 | 0.91 / 1.02 / 4.82 | 3.4 |
| focus (Microsoft open, Windows 11 row, sheet springs in, slide to start, boot) | 96 | 1.87 / 3.63 / 6.60 | 5.7-8.6 |
| holds (torch 420 ms, Emergency 700 ms, rings) | 102 | 0.43 / 0.57 / 2.11 | 3.7 |

Per state with everything damaged (`sbtk-host … frames`): a motion frame of the swiping lock over the scaling
home is 1.7 ms compose + 0.95 ms present; at rest 6.7 ms (full resolution, a scaled layer); the lock at rest 3.7 ms.

**A55: not measured yet.** uibench (P0) gives each kernel's MB/s on the phone; `frametimes.py` divides the host's
uibench kernels (`sbtk-host … bench`: blend 37 GB/s, 2x into RAM 29 GB/s on one M4 core) by the phone's and scales
each frame's compose and present time by them, per uibench set (1 or 4 A55, 1053 or 1794 MHz). Until those numbers
arrive, menu7's uncalibrated x12 to x33 (one A55 at ~1050 MHz) puts the swipe's motion frames at a mean of 16 to 44 ms
on one core (p90 32 to 88), and at roughly a third of that with the rows split over four cores: 60 fps is within
reach only on four cores at the good end of that range.

Costs outside the frames (bake, all float C, host): wallpaper + glass backdrop ~100 ms, home ~100 ms, lock ~40 ms,
the five vendor cards ~550 ms (glass bakes and local backdrops), opening the focus sheet ~220 ms (the scrim
snapshot blur and the card). At x12-x33 that is 10-30 s at boot and 3-7 s per sheet on one A55: the P2 work list
below starts there.

## P2 (the phone), in the order it matters
1. Bake paths in NEON and at reduced resolution where blurred (shadow, glow, local backdrop, scrim), the bakes
   cached to storage across boots, and the cards baked lazily; the focus sheet's scrim at quarter resolution end to
   end.
2. The row split over the four A55 (menu7's glass_smp / uibench's worker pattern) in `tk_comp_frame`.
3. Partial panel updates from the damage rects (DSI command mode windows).
4. The asset pack compressed (14.9 MB of raw icons now) and merged into `sbos-extra.sb7`.
5. The catalog from the store instead of the table in `shell_lock.c`; the home, Control Centre, Spotlight,
   Emergency mode (the hold only toggles the button's lit state here) and the apps, per the plan.
6. Glass over the home during the unlock as a snapshot backdrop (today the lock's glass over the fading-in home
   samples the wallpaper only; Chrome's lock widgets see home behind them).

Licences: Inter / Inter Tight and Outfit are OFL (`tools/brand/fonts`); stb_truetype is public domain / MIT.
