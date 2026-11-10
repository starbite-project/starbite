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

**A55, from the phone's uibench (P0, 2026-11-10 22:10, `build/starbite-os/uibench-1/pmsg.txt`).** `frametimes.py`
scales each frame's compose by host / A55 throughput of the half-resolution blend and its present by the host's 2x
into RAM against the A55's 2x into the framebuffer. One A55 is x44 (compose) / x60 (present) one M4 core; four A55
at 1053 MHz x11 / x15; four A55 at 1794 MHz x6.4 / x8.8. Motion frames (mean / p90 ms, share within 16.7 ms):

| film | 1 A55, 1053 MHz | 4 A55, 1053 MHz | 4 A55, 1794 MHz |
|---|---|---|---|
| unlock | 63 / 124, 47 % | 16 / 31, 50 % | 9.3 / 18, 59 % |
| group | 46 / 53, 1 % | 11.5 / 13.4, 96 % | 6.8 / 7.8, 98 % |
| focus | 91 / 176, 27 % | 23 / 44, 55 % | 13.3 / 26, 55 % |
| holds | 21 / 27, 41 % | 5.2 / 6.8, 96 % | 3.0 / 4.0, 100 % |

The first frame at rest (full resolution, once per gesture): 13-44 ms on four A55 at 1794 MHz.

So, on four A55 boosted to 1794 MHz while a finger is down: the Systems list and the holds hold 60 fps; the
unlock and the focus sheet average under 16.7 ms but their heaviest frames (the whole panel moving: the wallpaper
copy, the scaled home, the flattened lock with its glass; the scrim and the scaled card) reach 18-26 ms. What
brings them in: skipping fully transparent spans and copying fully opaque ones instead of blending (a cached
layer is mostly one or the other; the flattened lock is ~70 % transparent), and the scaled layers' bilinear only
while they scale. The rest frame drawn in bands over the next idle frames, as menu7 does.

Costs outside the frames (bake, all float C, host): wallpaper + glass backdrop ~100 ms, home ~100 ms, lock ~40 ms,
the five vendor cards ~550 ms (glass bakes and local backdrops), opening the focus sheet ~220 ms (the scrim
snapshot blur and the card). Float bake code scales worse than the NEON kernels uibench measures (assume x40-x60 on
one A55): 30-50 s at boot and ~10 s per sheet as it is. The P2 work list below starts there.

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

## P2 (the phone): Starbite OS Next

`starbite-next.img` / `starbite-next-selftest.img`, built by `starbite/sbos/build.sh` when the phone's pack exists
(`sbtk-pack.py build/sbtk --phone`, 8.4 MB: fonts and only the icons the shell draws). Catalog key `starbite-next`
(method bare, as starbite-os). The regular `starbite-os.img` is byte for byte unchanged by any of this.

- **Kernel glue** (`ui/kern_next.c`; `main.c` built with `SBOS_NEXT` maps the UI entry points to it): sbtk's heap on
  one arena block (`ui/tk_heap.c`, 200 000 random operations under ASan / UBSan in `ui/tools/heaptest.c`), the bake
  in steps from the loop (watchdog fed, the screen filling in), 60 Hz frames only while something moves, composed
  into the old UI's back buffer and presented into the scanout buffer (motion frames 2x straight into it), the A55 at
  1794 MHz while a finger is down or anything moves (back to 1053 MHz 400 ms later), the rows of each frame in 8
  bands over cores 1-3 (workers from the first touch until 5 s without motion), slide to start through the loader's
  one-shot next boot, the torch hold on the flashlight LED, the live clock, date, battery and temperature.
- **Memory**: layers sized to content, the wallpaper / glass backdrop / unlock cache / focus scrim at half
  resolution, an opened card's face and rows only while it is open, half copies only for what moves (the half canvas
  follows each rest frame), solid-colour layers. Host figures: 56 MB of layers at rest, about 85-125 MB peaks in the
  films (the self test logs the phone's).
- **Self test**: the bake's steps, then the four films with synthetic touches through the real gesture code, each
  film's frame count, mean, p90, max, frames over 16.7 ms, the A55 clock, the bands per core and the heap; recovery.
- **Stages staged** (`build/store/`): P2a one core; P2b the four-core split; P2c plus exact span skipping in the
  blend, the LED and the modem's status slot. Request: `sbos-ui-p2-request.md` in the shared notes.
- **Host estimate** for P2c (uibench-1 ratios, four A55 at 1794 MHz, motion frames mean / p90): unlock 6.9 / 14.4 ms,
  the list 4.1 / 4.6, the focus sheet 7.5 / 14.7, holds 1.8 / 2.7.
- **Not yet**: the apps from home, the Control Centre, Spotlight, Emergency mode, the power menu on a long press.

### P2 on the phone, 2026-11-11 (self tests by the main session)

| image | bake | unlock mean / max | list | focus open; frames mean / max | holds |
|---|---|---|---|---|---|
| P2d | 5.5 s | 8.6 / 27 ms | 6.7 / 41 | 1.2 s; 15.4 / 59 (34 of 59 over 16.7) | 1.8 / 6 |
| P2e | 5.4 s | 7.7 / 27 | 6.4 / 25 | 0.87 s; 9.6 / 19 (18 of 88 over) | 1.6 / 6 |
| P2f | 2.7 s | 7.7 / 27 | 6.4 / 25 | 0.41 s; 8.4 / 34 (3 of 101 over) | 1.6 / 6 |

What moved them: cached glass faces, the card's backdrop from the half-resolution scrim, culling under opaque
layers and nearest sampling in motion frames (P2e); the paired-row present, the frozen screen under the sheet,
the bakes on four cores (P2f); the exact-2x path for half-resolution layers at rest (P2g, against the 25-34 ms
first frames at rest). P2h fixed a clock that stayed on its first minute (a re-made layer restarted its content
generation, so the compositor kept the old pixels: `sbtk-host clocktest` and the self test now check it) and added
the Control Centre (1.54 against the concept) and home's second page (0.93); P2i the app zoom with the generic app
screen (0.31-0.38; 0.90 half-way).
