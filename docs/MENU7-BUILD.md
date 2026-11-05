# Menu v7: build, preview, compare, boot image

How to build the loader with menu v7, its BOOT ramdisk, the comparison against the concept, and a boot image. The design is in `docs/MENU7-PORT.md`. Run all commands from the repo root on the Mac.

## 1. Assets: the SB7 container and the BOOT ramdisk

```sh
build/venv/bin/python tools/brand/menu7-assets.py          # about 5 s
build/venv/bin/python -I tools/menu7/sb7-dump.py build/brand/menu7.sb7 --check-geometry   # optional check
```

The OS logos' source SVGs are committed under `tools/brand/logos/src/` and pinned by sha256 (sources, licences and trademark notes in `tools/brand/logos/SOURCES.md`; the container's INFO section carries a short credit line per logo). `build/venv/bin/python tools/brand/menu7_logos.py --fetch` downloads them again and exits 1, keeping nothing, if any upstream file changed. `build/venv/bin/python -I tools/menu7/sb7-premul-check.py build/brand/menu7.sb7` checks that every ARGB image is premultiplied (only Tux's own black body shows as opaque black; this is an asset check only, the black squares seen on the device are not explained by it), and `tools/menu7/preview/logos-dl.py small|large` makes display lists of every catalog tile and provider glyph for `run.sh list`.

This step needs `build/brand/menu-em.sbm`, the menu v6 sprites and the EngineerMode font. If it is missing, rebuild it with `tools/brand/menu.mjs` and `tools/brand/em-font.py`.

| Output | What it is |
|---|---|
| `build/brand/menu7.sb7` | the container: fonts, icons, tiles, SAMSUNG |
| `build/brand/menu-v7.sbm` | the BOOT ramdisk: `menu-em.sbm`, padding to 4 KiB, then the container. S-Boot loads it at 0x89000000, so it must stay under 16 MB. |
| `build/brand/upparam-v7/logo.jpg` | the concept's splash as a logo.jpg (no longer needed: the intro starts from whatever S-Boot drew). It is not packed or flashed by this step. |

Starbite's own splash, "One mark" (`build/splash-concepts/final/logo.jpg`,
`docs/concepts/splash/HANDOFF.md`), not flashed:

```sh
mkdir -p build/brand/upparam-onemark && cp build/splash-concepts/final/logo.jpg build/brand/upparam-onemark/
build/venv/bin/python -I tools/brand/upparam-pack.py build/brand/upparam-starbite/up_param.img \
    build/brand/upparam-onemark/up_param.img logo.jpg=build/brand/upparam-onemark/logo.jpg
```

Every other member is the "Powered by starbite" image's, byte for byte; sha256 5bb50d61257b303a...
(in `upparam-flash.sh`'s known list). Flash it only together with a loader whose intro7 has the
One mark handoff (the star and the 1380 word band); older loaders fall back to a plain fade.

The pair to flash in one session (not flashed yet). The loader is master after `137ab5f` (the
glass speed round 3 part 1: half-res glass, cached gs_prepare) plus the One mark review fixes
(the commit that adds this table); the earlier `cdbe2c8`-only Image and its test image are gone
from `out/` and must not be flashed:

| file | sha256 |
|---|---|
| `build/brand/upparam-onemark/up_param.img` | 5bb50d61257b303abaa1e746fe00b68bbc3d240c5a6469bf16f6b4af98269b52 |
| `shim/starlte/out/xnu_boot.Image` | b47e5c6f9eea2429787ca77af667f4fd6aaa9539f0b3f7cb65ebd6b49e113d9f |
| `build/brand/menu-v7.sbm` (BOOT ramdisk) | d3fcca0b57f370b6da0fb5f13bdfe87fc0e84840f4cb46dc115aebf85754ce58 |
| `out/xnu-boot-ios26-v7test.img` | 4d36a1bc5b6f5be691738a437c5ba9a3fd5219b24cc717cb8e37be4ed200cdf5 |

The test image is section 3's command with `ios26-v7test build/ios26-v7test-cmdline.txt` (it is
reproducible: the same inputs give the same sha256). On the first boot with One mark, read log slot
116 (`I7_LOG_CAPT`, 0x91fff000 + 116 * 8) before trusting the handoff: bits 8..15 (why) must be 0xff
(`I7_CAPT_OK`) and bits 0..7 (bands) 2, with bits 16..31 the tag 0xca97. Any other why means the
capture failed and the intro faded up from black with the star at One mark's star instead.

The native splash (Samsung's own, without "Secured by Knox"), not flashed:

```sh
build/venv/bin/python -I tools/brand/splash-noknox.py backup/20261102-185306/UP_PARAM.img build/brand/upparam-native
```

It takes logo.jpg from the stock UP_PARAM (checks its sha256), puts a block of the image's own
background over the Knox mark in the DCT domain (jpegtran -crop / -drop: no re-encoding, 0 pixels
change outside x 536..999, y 376..439), and packs `up_param.img` with `upparam-pack.py`.

## 2. The loader

```sh
limactl shell starbite -- bash -lc 'cd /Users/dev/project-starbite/shim/starlte && sh build.sh'
```

`build.sh` links v7 when `glass.c`, `sb7.c`, `intro7.c` and `menu7.c` are all present, and compiles those files with FP/SIMD (`CFLAGS_V7`). The build fails on any ABS64 relocation, and every file compiles with `-Werror`. The output is `shim/starlte/out/xnu_boot.Image`.

At run time, `star_menu` tries v7 first. v7 runs when the ramdisk carries a valid SB7ASSET container and all 29 fonts load. Otherwise v6 runs, and v5 if v6 is unavailable.

## 3. Boot image (iOS 26 daily, v7)

```sh
LOADER=shim/starlte/out/xnu_boot.Image RAMDISK=build/brand/menu-v7.sbm ADT=out/starlte-iphone11-1792.adt \
    tools/mkimage-ios26.sh ios26-v7 build/ios26-daily-cmdline.txt        # -> out/xnu-boot-ios26-v7.img
```

## 4. Preview and comparison with the concept

```sh
sh tools/menu7/preview/run.sh build                               # h7 in the VM (real glass, menu7, intro7)
sh tools/menu7/preview/compare-all.sh [state ...]                 # -> build/menu7-compare/
```

`compare-all.sh` renders each concept state twice: once in headless Chrome, and once from the loader's own code through h7. For each state it writes:

- `<state>.png`: concept, loader and |diff| ×4, side by side
- `all.png`: every pair on one contact sheet

It also prints the mean error per region.

The real-logo ramdisk (`build/brand/menu-v7.sbm`) no longer matches the concept's tile artwork: the concept ids ios26, win11, a17, phosh, gnome, arch, pmos and kali (and the provider glyphs Apple, Microsoft, Google, Linux) show the real logos, so the sections holding them differ by design. Measured on `menu` (`collapsed`): sections 0..3 mean 2.53 / 4.20 / 3.47 / 3.58 (2.53 / 4.20 / 3.47 / 4.22, sections 4, 5 at 2.47, 2.20), against 2.34 / 2.37 / 2.22 / 2.23 (2.34 / 2.37 / 2.22 / 2.78, 2.47, 2.20) with the concept's artwork; status, title and dock are unchanged (2.32, 1.88, 2.47). To measure concept fidelity, build the concept-artwork ramdisk and compare against it:

```sh
build/venv/bin/python tools/brand/menu7-assets.py --concept-art                 # -> build/brand/concept/
CMP_RD=build/brand/concept/menu-v7.sbm sh tools/menu7/preview/compare-all.sh
```

The concept-artwork ramdisk is for comparison only; the device ramdisk is `build/brand/menu-v7.sbm`.

The intro from a real splash: `h7 --splash logo.ppm [--splash-at X,Y]` puts S-Boot's panel on the fake framebuffer (logo.jpg's size at (180, 1144), a 1440 x 2960 picture as the whole panel) and the intro captures it (`intro7_capture`, also in `--run`). `I7_SPLASH=logo.jpg sh tools/menu7/preview/intro-film.sh 0 100 ...` converts a JPG / PNG, films it and checks the first frame: `first frame: 0 px differ from the splash`.

The states are: menu, countdown, collapsed, apple, ctx, submenu, sheet, alert, recovery, launch (at 40 %), error, missing, lab, handoff, and the intro at 0, 300, 700, 1050, 1400, 1800 and 2350 ms.

Chrome needs these adjustments to give a fair picture of the concept:

- **Few animation frames under virtual time.** Headless Chrome runs only 2 to 4 animation frames under virtual time, so springs barely move. The menu states are unaffected, because `#snap` snaps every spring.
- **Intro, missing and lab.** These are shot in real time over DevTools (`tools/menu7/preview/cdp-shot.mjs`). The intro runs from the start and stops itself at exactly T ms (`#stopat=T`, injected into a copy of the page). h7 plays the intro the way `intro7_run` does, at 30 fps up to T.
- **The handoff end screen.** Its CSS fade-in never runs under virtual time, so the copy of the page skips the fade.

Frame timing: `h7 --run [loader_stubs options]` runs `menu7_main` on the fake clock and prints one line per `g7_frame` call:

```
T7 <fake ms> <host ms> <pixels> <class> [full]
```

`<class>` is what the frame shows (`M7_PH_*` in `menu7.h`): intro-wall, intro-glass, idle, lens, list, expand, over-move, over-rest, launch, progress, end. `--again` with `--fail WHY` runs three progress stages, skips the handoff (a failed start never reaches it) and calls `menu7_main` a second time, which draws the error card as the device does; `H7_NOHANDOFF=1` skips the handoff without `--again`. h7 maps the memory map with `MAP_POPULATE`, so page faults stay out of the timings.

Environment switches of `h7 --run`:

- `H7_FORKCHECK=1`: at the first frame after motion (intro, list, expand, over-move, launch) that is not motion, and at the first frame with nothing to draw (both only with `back` at 0 or 1), a forked child finishes the rest bands (`g7_idle`), redraws the panel with `G7_F_FULL` and prints `FORKCHECK <ms>: N px differ from a full redraw`. N must be 0.
- `H7_MOTIONCHECK=1`: every drawn frame, motion frames included and with no `g7_idle` first, against a `G7_F_FULL` redraw of the same frame: `MC <ms> mean <m> n16 <a> n64 <b> worst <w>` (per pixel the largest channel error; n16 / n64 count the pixels over 16 / 64 of 255). Motion frames are approximate, so the figure to watch is n64 per motion frame (`build/menu7-work/fix3/mcsum.py` sums it per class; 6.1 has the reference values). `H7_MCDUMP=<ms>` also writes that frame as drawn and as redrawn (`mc-frame.ppm`, `mc-full.ppm`).
- `H7_PROF=1`: before each `T7` line, `P7` with the frame's ms per glass phase: setup (diff, shift), base glass, base items, frozen crossfade, over glass, over items, edge, present, freeze, then the rows shifted and the rows shaded again (`build/menu7-work/r2/prof.py` averages them per class).
- `H7_POISON=1` (or `--poison`, any mode): before anything runs, fill the v7 RAM (0x84000000..0x88800000: CANVAS, SHARP, BASE, the pool with its planes, tables and row buffers, CACHE) and the framebuffer with a per-byte hash of the address; `H7_POISON=a5` fills 0xA5 bytes. The phone's DRAM holds garbage at boot, while the host's maps are zero, and zero premultiplied pixels and transmissions blend to nothing, so a read of memory v7 never wrote shows only with poison. The files' `.bss` needs nothing: `image.ld` stores it as zeros in the image. Every run, still and list must match its unpoisoned output exactly. `H7_POISON_ONLY=<bits>` narrows it (1 CANVAS, 2 SHARP, 4 BASE, 8 pool, 16 CACHE, 32 framebuffer, 64 the pool bytes `H7_POISON_POOL=<offset>,<length>`) to find the buffer.
- `H7_FBHASH=1`: `FH <ms> frame|idle <hash>` (FNV-1a of the panel) after every `g7_frame` and every `g7_idle` from menu7, to compare whole runs frame by frame (poison against none); `H7_FHDUMP=<ms>` writes that frame's panel (`fh-<ms>-frame.ppm` / `-idle.ppm`). `--run` writes `--out` when `--tmax` ends the run as well.
- `H7_DBG=1`: the shift's and the freeze's diagnostics on stderr; `H7_DBG=5` / `6` turn the moving-frame correction off / to the BG8 term only (to measure it).

For example, `--keys 'wait:3000,down,wait:600,power,wait:3000' --tmax 12000`.

## 5. Running on a slow phone (review fixes)

- **Real-time springs.** Every loop (menu, intro, progress) advances the springs by the frame's real dt (`m7_advance`: capped at 0.5 s, in 50 ms steps), so animations end on time however few frames are drawn.
- **Frame-budget mode.** When a full-panel frame takes over `M7_SLOW_MS` (300 ms; `g7_stats.full_us`, scaled from the last frame that drew at least 1/8 of the panel), the launch card's springs snap and the pick is returned at once, each progress stage draws one frame with the bar at its target, and the handoff end screen is skipped. Log slot 127 bit 32 records it.
- **Speed check.** Before anything is drawn, `menu7_main` reads `g7_bake`'s time. Above `M7_BAKE_MAX_MS` (400 ms; VM 60–75 ms) v7 does not run and v6 does (slot 127 bit 16, slot 125 the bake in ms). Boot-args `starmenu7=0` turns v7 off and `starmenu7=2` skips the check.
- **Input during long frames.** `g7_set_poll` makes the renderer sample the keys every 2 ms between rows. Holds are timed from the moment the key went down, and fire only while it is still down. Both Volume keys held 700 ms power off at once (then reboot if a charger keeps the PMIC up), with no drawing.
- **Testing it on the host.** `h7 --run --tick-us N` makes every clock read cost N µs. Since the renderer reads the clock between rows, a full frame then costs about rows × N. `--cmd 'starmenu7=0'` passes boot-args, `hold-vol` holds both Volume keys, and after a pick `--run` plays five progress stages and the handoff, printing their times.

## 6. Frame cost (performance work)

What makes the frames cheaper, all inside `glass*.c` (the look is unchanged: the compare sheets' mean errors moved by at most +0.03/255):

- **BG8.** `G7_BASE` holds the final wallpaper × .8 already dithered (built at its first use, not in the bake). A pixel without glass is a copy.
- **GLB.** While nothing is frozen, `G7_SHARP` holds the base glass composed over BG8 (every base shape before the lens). A frame whose glass did not change where it draws (the lens moved, the countdown ring or a text changed) copies GLB and shades only the lens on top. Per-row valid ranges (`GV0`/`GV1`) follow the diff. A freeze builds SHARP from GLB in place.
- **The shader by spans.** Each shape's row is cut, by binary search on the SDF, into shadow-only, rim and interior spans. The interior and the straight top and bottom rim bands (one d, normal and refraction offset per row, so the refracted samples are rows) run 4 pixels per NEON step. Shadows beside straight edges are geometric sequences, above and below them a constant. Only corners and the vertical straight edges stay per pixel.
- **Motion frames at half resolution.** When the glass changes over more than 1/8 of the panel (scroll, room, sections, intro, overlay panes, the launch card), that frame shades it on the half-resolution grid (premultiplied C and transmission M), bilinear to the panel. Those rows are marked and drawn again exactly once the glass rests: one 128-row band per resting frame, or per `g7_idle()` call, which `menu7`'s loop makes when it has nothing to draw. A rest frame is pixel-identical to a full redraw (`h7 --list … --check-full`).
- **NEON** for the frost rows, the dithered stores, the wallpaper rows, the crossfade under overlays (integer 9/3/3/1 weights), the freeze's box blurs and the bake's blur passes. The static field is evaluated on a 4-texel grid and interpolated.

On-device timing: log slots 128 + class (`M7_PH_*`), each `frames << 48 | worst ms << 32 | total ms`. Slots 120..127 are unchanged.

Host timings (VM ms, `h7 --run`, mean / p90; ×12 for the A55):

| class | before | after |
|---|---|---|
| bake | 58 | 19 |
| intro-wall | 45 / 57 | 22 / 27 |
| intro-glass | 99 / 102 | 32 / 36 |
| list | 81 / 104 | 24 / 30 |
| lens | 14 / 19 | 5 / 7 |
| expand | 57 / 70 | 18 / 21 |
| over-move | 65 / 69 (freeze 202) | 23 / 24 (freeze 95) |
| over-rest | 6 / 8 | 3 / 5 |
| launch | 101 / 106 | 43 / 48 |
| end (handoff) | 114 | 65 / 65 |
| error card (real fail-back) | 87 / 136 | 30 / 50 |

Measured with `build/menu7-work/scen.sh` (four key scripts) and summarised per class. The end and error-card rows come from the review's corrected runs: `--again` used to start the fail-back after the handoff, an order the device never takes, so it only redrew the handoff screen.

### 6.1 Round 2: moving frames, the curtain, the freeze

Also inside `glass*.c` only (menu7, intro7 and the headers are unchanged except `glass_int.h`). Every compare state's mean error is unchanged to 0.01/255 (`compare-all.sh`, all 21 states and every region, +0.00), and the stills differ from the round-1 loader by at most 1/255 in a few pixels (the review fixes leave all 18 `--state` stills byte-identical to the first round-2 commit).

- **Moving glass is shifted, not shaded (review fixes below).** When base shapes only moved (the list scrolls, the island makes room, the sections below an opening one move), `g7_frame` finds the dy by which the most glass area moved with nothing else changed, and every GLB row whose shapes all moved by it is the GLB row dy above, shifted (`glb_shift_row`), 16 pixels a NEON step. The glass refracts the wallpaper, which did not move, so the shifted row is corrected: by T x (BG8 change), T being the glass's transmission per pixel (`TPL`, written whenever GLB is), and by (1 - T) x J x (frost change), J being `shade()`'s linear part (vibrancy, tint) of the shape under the pixel, on a 4 x 4 px grid; on a top or bottom rim band row the frost change is taken where that row refracts to. Rows a shape that did not move touches, and the viewport's fade rows (170 .. 246, 2480 .. 2620) for clipped shapes, are shaded as before; rows where the fade is 0 stay as they are. A sub-pixel tail of a spring (dy rounds to 0) keeps the rows. Shifted rows are marked like half-resolution ones (`GAPX`) and drawn exactly at rest.
  Review fixes to the shift:
  - **No drift.** GLB holds the glass at the integer offset it was last drawn at, not at the frame's exact y, so taking dy from the last frame's y let each frame's rounding add up (40 frames of -4.4 px left the cards and tiles 16 px from their place; sub-pixel steps never moved them). Each GLB row now keeps where its glass sits relative to where it is (`GOF`) and the shifts since it was shaded (`GSN`); dy is taken from where the largest moving shape's rows sit, a row is shifted only while its offset stays within 1 px, and at most `SHIFT_MAX` (4) + 0..3 (by 128-row band, so the rows come due in turns) times in a row, after which it is shaded again. A row shaded in part over shifted content is never a shift's source.
  - **Horizontal edges are shaded.** The four rows around each moving shape's top and bottom edge are shaded again on every shifting frame (an offset there moves the whole edge, and the rim highlight depends on where the shape is), and every row a shifting frame shades again is shaded at half resolution, as the other motion frames (`GAPX`, exact at rest).
  - **Nested shapes.** Where a shape lies on another (an icon tile on its card), the frost correction is weighted per pixel by the upper shape's coverage (`gs_cover_row`: edge AA, alpha, the viewport fade) between its J and the J of the shape under it, so the tile box's corners take the card's correction.
  - **A stale `mover` flag** carried into the next frame's diff (`layer_diff_save` copied it into the previous frame's entries) skipped `glb_inval` for a shape that had moved: one removed, faded out or resized the frame after a shift kept its old glass in GLB for good (`review2/t1.dl`: 840288 px off a full redraw at rest; now 0).

  Error of the last frame of N consecutive moving frames (no rest between) against an exact render of the end position (`tools/menu7/preview/motion-check.sh`: the menu's section cards; cards: mean over the cards' boxes; n64: panel pixels over 64/255):

  | N x step (px) | round 1 (half resolution) cards / panel mean / n64 | round 2 as first committed | round 2 with the review fixes |
  |---|---|---|---|
  | 1 x 176 | 0.43 / 0.37 / 421 | 1.07 / 0.61 / 176 | 1.25 / 0.78 / 408 |
  | 8 x 22 | 0.43 / 0.37 / 421 | 1.59 / 0.89 / 168 | 1.02 / 0.68 / 420 |
  | 22 x 8 | 0.43 / 0.37 / 421 | 2.98 / 1.60 / 148 | 1.08 / 0.70 / 421 |
  | 44 x 4 | 0.43 / 0.37 / 421 | 4.44 / 2.32 / 150 | 0.99 / 0.65 / 421 |
  | 40 x -4.4 | 0.37 / 0.39 / 1062 | 8.62 / 5.86 / 87529 | 1.02 / 0.77 / 1155 |
  | 60 x 0.3 | 0.40 / 0.38 / 821 | 4.82 / 4.23 / 92483 | 1.04 / 0.75 / 932 |

  So a shifted motion frame is still about 2.5x less accurate in mean than round 1's half-resolution frame (0.4 -> 1.0-1.25/255 over the cards: the correction's 4 px grid, the rims' refraction and dispersion, up to 1 px of offset at corner arcs), with about the same count of pixels over 64/255 (the half-resolution edge rows). The first commit's "10.4 -> 1.07" compared shifting without and with the correction from an exact frame, which hid both the drift and the comparison with round 1.
  Real menu motion (`scen.sh`'s four scripts with `H7_MOTIONCHECK=1`, per list frame): round 1 mean 0.25/255, n64 276 on average, worst frame 2193; round 2 as first committed 0.83, 2826, 12131 (max error 241); with the fixes 0.49, 277, 2386 (max 107). Lens frames n64 27 / 386 / 49, expand 17 / 622 / 21. A list that ends mid-motion (`build/menu7-work/listbench.dl`, `--check-full` compares a motion frame, so it is not 0 by design): round 1 216,929 px differ (mean 0.21, n64 32), round 2 as first committed 2,146,334 (0.86, 130), with the fixes 1,884,530 (0.64, 30): far more pixels differ by a unit or two (the shifted interiors), about as many by much.
- **Side edges by spans.** On rows where a shape's straight vertical edge is (the four normal samples all on it), its rim pixels have the normal (+-1, 0) and d depends on x only: `px_side` blends the row's A5, A and B vertically once and takes each refracted sample with one lerp. Corners are still per pixel (`px_general`).
- **Half-resolution rows.** The half grid's frost rows (`row_half`: NEON vertical blend, fixed weights) and the over layer's motion rows (`over_half_store`: the bilinear and the premultiplied store in one NEON pass) replace the per-sample code.
- **The end screens' curtain.** The full-panel rect the end screens fade in is drawn over a copy of what is below it, kept at its first frame in `G7_SHARP` (frozen, back 1: the crossfade shows SOFT only), so a fade frame is a copy and one blend pass (`compose_curtain`); the fade's last frame (the curtain at alpha 1) too, which used to leave this path and shade the whole over glass under the opaque curtain again (42 ms VM, as the first frame). SHARP is then lost; back leaving 1 takes the base layer again. Square-cornered filled rects without clip or fade blend their inside 8 pixels a NEON step.
- **The over layer while back moves.** Below 1/256 a change of back moves no pixel by a unit and is not drawn (back reaching 1 is); a change dirties only what the largest fully opaque over shape does not cover (its box inset by the corner); the over glass is then shaded at half resolution. An over shape whose box moved under half a pixel from where it was drawn (the launch card's spring tail) is not drawn again: its rows are marked `OAPX` and drawn exactly at rest.
- **Box shadows** take erf from a 4096-point table and their six vertical samples per row once.
- **The freeze.** Below the freeze threshold (back <= .002) the base glass keeps soft 0 (it changed nothing by a unit), so an overlay opening keeps GLB; the frame that freezes may shift GLB rows too; GLB rows a motion frame left approximate go into SHARP as they are and are shaded exactly afterwards, a band per `g7_idle` (`SAPX`, only while the base layer and the wordmark are still the frozen ones: a band of a changed base or word would tear the backdrop). If the base changes first (menu7's list spring still moving under the options menu), the remaining SAPX rows stay approximate until the overlay closes: freezing again would be exact but costs a full shade (~95 ms VM) in the middle of the overlay's motion, and the freeze-time items cannot be redrawn (their strings live only during `g7_frame`). Those rows are now within 1 px and 4-7 shifts of exact, like the half-resolution rows. The soft version (SOFT, its content blur) is unchanged: ~19 + 7 ms VM.
- **Frame budget.** `g7_stats.full_us` no longer includes the frame's freeze, so the launch card's first frame (which freezes) no longer puts menu7 in frame-budget mode by itself: the card animates while its own frames stay under `M7_SLOW_MS`.
- **The intro** no longer copies its rows into GLB (every intro frame invalidates it).

Host timings (VM ms, mean / p90 over two runs of `scen.sh`). Round 1 is from the earlier session; the two round-2 columns were measured in one session, alternating runs (the code the fixes did not touch, intro-wall and intro-glass, came out 3-4 % slower in the second column: VM noise of that size applies to every row):

| class | round 1 | round 2 as first committed | round 2 with the review fixes | A55 estimate (fixes; uncalibrated) |
|---|---|---|---|---|
| intro-wall | 22.0 / 27.1 | 22.9 / 28.1 | 23.8 / 29.2 | 285 / 350 |
| intro-glass | 32.8 / 37.2 | 33.6 / 37.6 | 34.5 / 39.1 | 414 / 470 |
| list | 24.2 / 31.2 | 11.2 / 17.6 | 13.2 / 20.8 | 159-238 / 250-374 |
| lens | 5.5 / 8.0 | 4.6 / 7.8 | 4.8 / 8.0 | 57 / 96 |
| expand | 18.7 / 21.2 | 18.1 / 21.7 | 18.9 / 22.6 | 227 / 271 |
| over-move | 22.5 / 24.6 (freeze frame 95) | 18.5 / 21.1 (freeze frame 59) | 19.3 / 21.5 (freeze frame 72) | 231-347 / 258-387 (860) |
| over-rest | 3.1 / 5.6 | 3.0 / 5.9 | 3.1 / 6.3 | 37 / 76 |
| launch (incl. the error card) | 38.1 / 50.0 | 22.9 / 33.7 | 23.5 / 35.1 | 282 / 421 |
| error card (scenario D) | 30.3 / 50.4 | 21.8 / 34.5 | 22.5 / 36.0 | 270 / 432 |
| progress | 4.8 / 4.8 | 2.7 / 3.2 | 2.7 / 3.3 | 33 / 39 |
| end (handoff) | 64.0 / 64.5 | 9.1 / 46.6 (first and last frame 43-50) | 6.2 / 3.6 (first frame 50) | 74-111 / 43-65 (first frame 600-900) |

The fixes cost the list about 2 ms VM a frame (the edge rows and the rows due after `SHIFT_MAX`, shaded at half resolution) and the freeze frame after a scroll about 13 ms (more rows left to shade exactly). The end screen's last frame is now a blend (2.7 ms instead of 42).

**The A55 column is not calibrated.** x12 (the factor used so far) has not been measured on the device. Round 2 moved work into NEON-heavy paths (`vld4q` / `vst4q` and `vqrdmulhq` chains in `glb_shift_row`, `row_half`, `over_half_store`, the 8-pixel rect blend); the in-order A55 issues about one 128-bit op a cycle at ~1.95 GHz against the M4's four at ~4.4 GHz, so those classes may run at x12-x18 (the ranges above: list, over-move, end). The memory-bound copies (the curtain, into a write-combined framebuffer, `mmu.c`) are probably near x12. On the first device boot of this image, read log slots 128-138 (`frames << 48 | worst ms << 32 | total ms` per class, `M7_PH_*`) after running the same key script as one of `scen.sh`'s scenarios, divide by the host's figures for that scenario, and replace this column with the per-class factors.

Test procedure for changes to the moving frames: `--check-full` on every list in `build/menu7-work` (0 for every list that ends at rest), the same with `H7_POISON=1` and the run scenarios' `H7_FBHASH=1` sequences with and without it (identical: `build/menu7-work/poison/ptest.sh` and `fh.sh`), `H7_FORKCHECK=1` over `scen.sh` (every FORKCHECK 0), `H7_MOTIONCHECK=1` over `scen.sh` (n64 per list frame near round 1's ~280 on average, worst frame under ~2500), and `tools/menu7/preview/motion-check.sh` (the table above: no growth with N).

None of these reach 30 fps on the A55 (2.8 ms VM). What is left: the intro's wallpaper (WALL_FS 4.7 ms a frame and the upsample to the panel ~10 ms, both changing everywhere from ~1000 ms on), the opening section's own glass during expand (its height, SZ and specular change every frame: no translation), the over layer's motion rows (an overlay pane over a backdrop that fades: no translation either), the launch card while it grows, corners per pixel, SOFT (19 ms) at each freeze, and the curtain's first frame.

### 6.2 Round 3 part 2: MV4 route 2 (half-resolution canvas, 2x present)

All in `glass*.c` (the interface `glass.h` is unchanged; coordinates stay panel px), plus the log slots in `menu7.c`'s `m7_frame_timed`.

- **The canvas is 720 x 1480.** Canvas pixel (i, j) is the panel point (2i + 1, 2j + 1): the grid the glass has been shaded on since round 3 part 1, and the wallpaper texture's own texels. BG8, GLB, TPL, SHARP and SOFT are canvas-sized; the glass is stored 1:1 (C + M x below) instead of bilinear to the panel (that upsample, `base_glass_half`, was 33 % of all VM time). The shift works in canvas rows (frost-correction blocks 2 x 2 canvas px, `GOF` in canvas px).
- **The present scales 2x and draws the content.** Each dirty rect is scaled 2x (bilinear, the 1/4-3/4 weights, as byte averages: `vrhadd` then `vhadd`, within one unit) into a 16-row band buffer in write-back RAM, the content items (text, images, tiles, rects, rings, arcs, strokes, shadows) are drawn there at panel resolution, then the edge shade, then the band goes to the framebuffer; a band without items goes straight to the framebuffer. Text, icons and tiles are exactly as sharp as before (1:1 crops: `build/cmp-final/*-crop*.png` in the worktree that made them). A change of an item alone is presented again without composing the canvas (`PR` rects).
- **Over glass while nothing is frozen** (toasts, the Lab card): kept in `OVP` (canvas grid, premultiplied) and composited by the present after the base items, so base text under or near it stays sharp. Frozen (overlays), the base items are in SHARP and the over glass goes into the canvas as before.
- **The curtain** (end screens): frozen, the curtain and the over items below it are drawn on the canvas, the canvas below it kept at its first frame (`CURT`, canvas-sized), so a fade frame is a canvas copy, a quarter-panel blend and the present.
- **The freeze's soft version** (uSoft = 1 glass, content blurred 26 px) is shaded and blurred on the quarter grid (360 x 740) and scaled 2x into SOFT: VM soft glass 16.9 -> 6.0 ms, content 7.1 -> 2.7 ms.
- **Motion frames on the quarter grid** (`G7_QMOTION`, default 1): a frame whose base glass changes over more than a quarter of the panel without a shift (the intro's glass arriving, a section opening) or whose frozen over glass moves shades those rows on the quarter grid (K = 4 uniforms) and marks them (`GAPX` / `OAPX`); `g7_idle` draws them exactly at rest, 64 canvas rows a call. Rims are softer while moving (`build/cmp5/exp-motion-crop.png`: motion frame | exact).
- **Frost planes on the canvas grid**: A5 and B sampled at the canvas grid, built row by row at first use (not in the bake), 10:10:10 in the upper half of G7_SHARP; a frost row is a row read.
- **NEON**: 1:1 glyph and bitmap blends (integer), the intro's wallpaper (`gw_wall`, 4 texels a step), the field interpolation, the blur passes' stores and A5, the 2x upscalers. The bake is 19.5 -> 16.7 ms VM.

Memory: G7_CANVAS holds the canvas (4.26 MB), the band buffer (+0x420000), CURT (+0x500000), the freeze's quarter planes (+0x920000) and the quarter motion rows (+0xC60000); G7_SHARP the GLB / frozen base (4.26 MB) and the frost planes (+0x420000, +0x840000); G7_BASE BG8 and OVP (+0x420000).

Checks (VM): every `--state` still `--check-full` 0; every display list in `build/menu7-work` that ends at rest `--check-full` 0; `H7_FORKCHECK=1` over the four `scen.sh` scenarios: 28 checks, all 0; `H7_POISON` per RAM region (`H7_POISON_ONLY` 1..16) leaves the run hash sequences unchanged (poisoning the framebuffer changes them, as before this round: the intro captures S-Boot's splash from it); against the full-resolution renderer every still is within 0.05-0.42/255 mean (`build/cmp-final.txt`). Motion frames' error against an exact frame (`H7_MOTIONCHECK=1`, mean / pixels over 64 per frame): list .54 / 48 (round 3 part 1: .35 / 0), expand .66 / 12, intro-glass 1.18 / 33, launch .29 / 85.

Host timings (VM ms, mean / max, `scen.sh` with a pinned ramdisk; the baseline is commit e0c20ac on the same ramdisk). A55 at S-Boot's ~1050 MHz: the plan's x22-x33 for CPU-bound work, **not measured** (MV1); the present's framebuffer writes are memory-bound and may scale differently.

| class | before | after | A55 at x22-x33 [est] |
|---|---|---|---|
| intro-wall | 21.3 / 30.4 | 7.7 / 11.3 | 169-254 ms |
| intro-glass | 31.3 / 38.8 | 11.1 / 12.6 | 244-366 ms |
| list | 11.8 / 23.1 | 6.8 / 11.4 | 150-225 ms |
| lens | 2.5 / 20.4 | 1.6 / 7.1 | 35-53 ms |
| expand | 17.2 / 21.6 | 6.2 / 7.9 | 136-205 ms |
| over-move | 17.0 / 58.2 (freeze) | 6.5 / 21.1 (freeze) | 143-215 ms (freeze frame 460-700) |
| over-rest | 1.6 / 18.0 | 0.9 / 6.9 | 20-30 ms |
| launch | 19.7 / 46.5 | 9.0 / 19.6 | 198-297 ms |
| progress | 1.7 / 9.5 | 1.0 / 6.1 | 22-33 ms |
| end | 4.5 / 29.2 | 3.0 / 12.0 | 66-99 ms |
| idle (rest bands) | 0.12 | 0.74 | 16-24 ms |
| bake | 19.5 | 16.7 | 367-551 ms (v6 above 400) |

Still not 30 fps on the A55 for the moving classes (that needs about 1-1.5 ms VM). What is left, by VM cost: the present of large areas (2-5 ms: the 2x and the content items at panel resolution), corner and rim pixels of the shader (`px_general`), the shifted rows' upkeep in list frames (`glb_shift_row`, the edge rows), the intro's wallpaper (~3 ms) and full glass shading while the intro's cards arrive.

**The bake and the v6 fallback.** `M7_BAKE_MAX_MS` (400 ms) is checked against the A55's bake time; at x22 or more even this bake (16.7 ms VM) is near or above it, and the round-3-part-1 bake (19.5 ms VM) was above it from x21. If MV1's boot shows v6 (slot 127 bit 16, slot 125 the bake in ms), boot with `starmenu7=2` (skips the check) to measure v7, then decide on the threshold.

**One phone boot for MV1.** Run one `scen.sh` key script, then read: slots 128..138 (per class `frames << 48 | worst ms << 32 | total ms`, the total now summed in us), slot 139 (`0x4d563401` = this renderer `<< 32 |` panel pixels presented / 1024), 125 (bake ms), 127 (flags), 140..144 (bench: alu, fma, WB copy, FB copy us; SCTLR), 145..156 (glass phases in ms: 0 setup and the shift, 1 base glass, 2 base items (freeze only), 3 frozen crossfade, 4 over glass, 7 present (2x, items, edge), 8 freeze, 9 the intro's wallpaper; 10, 11 rows shifted / shaded again). Per class, A55 ms / VM ms of the same scenario is the factor.
