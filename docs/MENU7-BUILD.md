# Menu v7: build, preview, compare, boot image

How to build the loader with menu v7, its BOOT ramdisk, the comparison against the concept, and a boot image. The design is in `docs/MENU7-PORT.md`. Run all commands from the repo root on the Mac.

## 1. Assets: the SB7 container and the BOOT ramdisk

```sh
build/venv/bin/python tools/brand/menu7-assets.py          # about 5 s
build/venv/bin/python -I tools/menu7/sb7-dump.py build/brand/menu7.sb7 --check-geometry   # optional check
```

This step needs `build/brand/menu-em.sbm`, the menu v6 sprites and the EngineerMode font. If it is missing, rebuild it with `tools/brand/menu.mjs` and `tools/brand/em-font.py`.

| Output | What it is |
|---|---|
| `build/brand/menu7.sb7` | the container: fonts, icons, tiles, SAMSUNG |
| `build/brand/menu-v7.sbm` | the BOOT ramdisk: `menu-em.sbm`, padding to 4 KiB, then the container. S-Boot loads it at 0x89000000, so it must stay under 16 MB. |
| `build/brand/upparam-v7/logo.jpg` | the S-Boot splash that matches the intro's first frame. It is not packed or flashed by this step. |

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
- `H7_PROF=1`: before each `T7` line, `P7` with the frame's ms per glass phase: setup (diff, shift), base glass, base items, frozen crossfade, over glass, over items, edge, present, freeze, then the rows shifted and the rows shaded again (`build/menu7-work/r2/prof.py` averages them per class).
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

Also inside `glass*.c` only (menu7, intro7 and the headers are unchanged except `glass_int.h`). Every compare state's mean error is unchanged to 0.01/255 (`compare-all.sh`, all 21 states and every region, +0.00), and the stills differ from the round-1 loader by at most 1/255 in a few pixels.

- **Moving glass is shifted, not shaded.** When base shapes only moved (the list scrolls, the island makes room, the sections below an opening one move), `g7_frame` finds the dy by which the most glass area moved with nothing else changed, and every GLB row whose shapes all moved by it is the GLB row dy above, shifted (`glb_shift_row`), 16 pixels a NEON step. The glass refracts the wallpaper, which did not move, so the shifted row is corrected: by T x (BG8 change), T being the glass's transmission per pixel (`TPL`, written whenever GLB is), and by (1 - T) x J x (frost change), J being `shade()`'s linear part (vibrancy, tint) of the shape under the pixel, on a 4 x 4 px grid; on a top or bottom rim band row the frost change is taken where that row refracts to. Rows a shape that did not move touches, and the viewport's fade rows (170 .. 246, 2480 .. 2620) for clipped shapes, are shaded as before; rows where the fade is 0 stay as they are. A sub-pixel tail of a spring (dy rounds to 0) keeps the rows. Shifted rows are marked like half-resolution ones (`GAPX`) and drawn exactly at rest.
  Error of a moved frame against its exact render (`r2/movetest.py`, the menu's section cards moved, mean over the cards in the viewport): 176 px: 10.4/255 without the correction, 1.07 with it; -176: 10.5 -> 0.96; 60: 3.8 -> 0.85; 37.4 (sub-pixel): 2.7 -> 1.04. The rest is the rims' refraction and dispersion (p99 6-7/255).
- **Side edges by spans.** On rows where a shape's straight vertical edge is (the four normal samples all on it), its rim pixels have the normal (+-1, 0) and d depends on x only: `px_side` blends the row's A5, A and B vertically once and takes each refracted sample with one lerp. Corners are still per pixel (`px_general`).
- **Half-resolution rows.** The half grid's frost rows (`row_half`: NEON vertical blend, fixed weights) and the over layer's motion rows (`over_half_store`: the bilinear and the premultiplied store in one NEON pass) replace the per-sample code.
- **The end screens' curtain.** The full-panel rect the end screens fade in is drawn over a copy of what is below it, kept at its first frame in `G7_SHARP` (frozen, back 1: the crossfade shows SOFT only), so a fade frame is a copy and one blend pass (`compose_curtain`). SHARP is then lost; back leaving 1 takes the base layer again. Square-cornered filled rects without clip or fade blend their inside 8 pixels a NEON step.
- **The over layer while back moves.** Below 1/256 a change of back moves no pixel by a unit and is not drawn (back reaching 1 is); a change dirties only what the largest fully opaque over shape does not cover (its box inset by the corner); the over glass is then shaded at half resolution. An over shape whose box moved under half a pixel from where it was drawn (the launch card's spring tail) is not drawn again: its rows are marked `OAPX` and drawn exactly at rest.
- **Box shadows** take erf from a 4096-point table and their six vertical samples per row once.
- **The freeze.** Below the freeze threshold (back <= .002) the base glass keeps soft 0 (it changed nothing by a unit), so an overlay opening keeps GLB; the frame that freezes may shift GLB rows too; GLB rows a motion frame left approximate go into SHARP as they are and are shaded exactly afterwards, a band per `g7_idle` (`SAPX`, only while the base layer is still the frozen one). The soft version (SOFT, its content blur) is unchanged: ~19 + 7 ms VM.
- **Frame budget.** `g7_stats.full_us` no longer includes the frame's freeze, so the launch card's first frame (which freezes) no longer puts menu7 in frame-budget mode by itself: the card animates while its own frames stay under `M7_SLOW_MS`.
- **The intro** no longer copies its rows into GLB (every intro frame invalidates it).

Host timings (VM ms, mean / p90 over two runs of `scen.sh`, x12 for the A55):

| class | round 1 | round 2 | A55 estimate (round 2) |
|---|---|---|---|
| intro-wall | 22.0 / 27.1 | 21.5 / 26.9 | 258 / 323 |
| intro-glass | 32.8 / 37.2 | 31.2 / 35.0 | 375 / 420 |
| list | 24.2 / 31.2 | 10.9 / 16.7 | 130 / 200 |
| lens | 5.5 / 8.0 | 4.4 / 7.2 | 53 / 87 |
| expand | 18.7 / 21.2 | 17.1 / 20.3 | 206 / 243 |
| over-move | 22.5 / 24.6 (freeze frame 95) | 17.3 / 18.9 (freeze frame 55) | 208 / 226 (660) |
| over-rest | 3.1 / 5.6 | 2.9 / 6.1 | 35 / 74 |
| launch (incl. the error card) | 38.1 / 50.0 | 21.1 / 31.6 | 253 / 379 |
| error card (scenario D) | 30.3 / 50.4 | 18.2 / 32.3 | 219 / 388 |
| progress | 4.8 / 4.8 | 2.5 / 2.9 | 30 / 35 |
| end (handoff) | 64.0 / 64.5 | 8.5 / 42.5 (first frame 43-51) | 101 / 510 |

None of these reach 30 fps on the A55 (2.8 ms VM). What is left: the intro's wallpaper (WALL_FS 4.7 ms a frame and the upsample to the panel ~10 ms, both changing everywhere from ~1000 ms on), the opening section's own glass during expand (its height, SZ and specular change every frame: no translation), the over layer's motion rows (an overlay pane over a backdrop that fades: no translation either), the launch card while it grows, corners per pixel, SOFT (19 ms) at each freeze, and the curtain's first frame.
