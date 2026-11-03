# Menu v7: porting the approved boot concept to the S9

The user approved `docs/concepts/ios27-boot.html` (the whole boot: S-Boot splash, Liquid Glass
intro, "Choose a System" with glass provider sections, focus lens, island, dock, options menu,
Details sheet, alerts, launch card, error card) and asked for it to be ported **exactly**. This
document is the architecture for that port: menu v7 of the loader, the default when its assets
are in the BOOT ramdisk, with menu v6 and v5 kept as fallbacks.

Four builders work in parallel against frozen headers. Sources of truth, in order:

1. The concept's Spec panel and its `<script>` (behaviour, formulas, springs, timings). The
   script is the reference implementation: where this document and the script disagree on
   behaviour, the script wins, except for the deliberate differences in section 6.
2. The concept's CSS, measured: `docs/concepts/boot-geometry/<state>.json` (every visible
   element's box, opacity, transform, glass attributes, and for text the baseline, pen x, advance
   and computed font), made by `tools/menu7/concept-geometry.sh` from headless Chrome. Layout
   numbers come from there; `menu7.h` restates the main ones.
3. The headers `shim/starlte/glass.h`, `menu7.h`, `intro7.h`, `sb7.h`: the contracts. They are
   frozen. A builder who needs a change says so in their report; the integrator changes the
   header for everyone.

Out of scope here: the Arch spinner loop and Kali's missing GUI (separate work).

---

## 1. Module split and ownership

| Builder | Owns (creates, edits, commits) | Delivers |
|---|---|---|
| **(a) glass** | `shim/starlte/glass.c`, any `shim/starlte/glass_*.c`; `tools/menu7/preview/h7.c`, `tools/menu7/preview/run.sh`, `tools/menu7/preview/geom2dl.py`, `tools/menu7/preview/diff.py` | everything in `glass.h`: wallpaper and its blur planes, squircle SDF/normal tables, glass compositing of up to 20 shapes per layer, the lenses, the overlay backdrop and freeze, the SAMSUNG glass wordmark, all content drawing (text, images, tiles, rects, shadows, arcs, strokes), the edge shade, dirty rects, present; the host preview harness |
| **(b) assets** | `tools/brand/menu7-assets.mjs` (or `.py`) and any `tools/brand/menu7-*.{mjs,py}`; `tools/brand/fonts/InterTight*`, `Inter[opsz,wght]*`, `JetBrainsMono*` with their `OFL.txt`; `tools/brand/fonts/SOURCES-menu7.md`; `tools/brand/splash7.mjs`; `shim/starlte/sb7.c`; `tools/menu7/sb7-dump.py` | `build/brand/menu7.sb7` (the SB7ASSET container, `sb7.h`), `build/brand/menu-v7.sbm` (the BOOT ramdisk: `menu-em.sbm` + padding to 4 KiB + `menu7.sb7`), `build/brand/upparam-v7/logo.jpg` (S-Boot's splash matching the concept's, not flashed), the C reader `sb7.c`, a verifier |
| **(c) menu7** | `shim/starlte/menu7.c`, any `shim/starlte/menu7_*.c`; integration: `shim/starlte/build.sh`, `xnu_boot.S`, `xnu_boot.c`, `menu.c`, `menu.h`, `fbtext.c`, `fbtext.h`; `tools/menu7/preview/loader_stubs.c`, `tools/menu7/preview/concept-catalog.toml` | everything in `menu7.h` except what intro7 implements; the loader integration (section 4.2.4); the harness's loader side (catalog, settings, readiness, keys/touch script, fake clock, `em_main` stub) |
| **(d) intro7** | `shim/starlte/intro7.c`; `tools/menu7/preview/intro-film.sh` | everything in `intro7.h`: the 2350 ms intro as a pure function of t, run at 30 fps with skip and Lab |

Architect-owned (frozen): this file, `shim/starlte/{glass,menu7,intro7,sb7}.h`,
`tools/menu7/concept-geometry.sh`, `tools/menu7/concept-dump.js.html`,
`docs/concepts/boot-geometry/*.json`. Nobody else edits `docs/concepts/ios27-boot.html`.

**Dependencies.** Everything draws text, so (b) delivers a first `menu7.sb7` with **fonts only**
as early as possible (the format is fixed; icons, provider glyphs, tiles and the height field
follow). Until then (a) tests with any TTF rasterised by a throwaway script into the same format;
(c) and (d) only need `g7_text_w` numbers, which the geometry JSON already gives (`tw`).
(a) can validate the whole renderer without menu7: `geom2dl.py` turns a geometry JSON into a
display list (section 5). (c) and (d) compile against the headers and the harness from day one.

**Rules for everyone.**
- Build in a private copy: `build/menu7-work/<builder>/` holding a copy of `shim/` (and of
  `tools/menu7/preview/`); build with
  `limactl shell starbite -- bash -lc 'cd <copy>/shim/starlte && sh build.sh'`. Never build in
  the main checkout's `shim/starlte/out` (other people's builds).
- Do not flash or touch the phone; no adb. Mind the Mac disk (`df -h /`, ~14 GB free): no large
  files, no frame dumps beyond a few PNGs per state; nothing big inside the VM's own disk.
- Commit only files you own, with messages ending
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Loader code rules stay: `-ffreestanding`, no libc, no absolute relocations (no static tables of
  pointers, no static initialisers holding addresses), `-Werror`. The stack is 8 KiB
  (`xnu_boot.S`): no local arrays above 512 bytes. `.bss` is stored in the Image as zeros: keep
  each v7 file's `.bss` under 128 KiB; big buffers live in the memory map (section 3).

---

## 2. FP/SIMD: enabled for the v7 renderer

**Decision: turn FP/SIMD on in the loader and compile the four v7 C files (`glass*.c`,
`menu7*.c`, `intro7.c`, `sb7.c`) with it; every other file stays `-mgeneral-regs-only`.**

### 2.1 Cycle budget

The loader runs on one Cortex-A55 at S-Boot's clock. Measured (NOTES.md, `starlte_smp.c` clock
survey): **S-Boot leaves the A55 cluster at about 1050 MHz** (ACPM can take it to 1690 MHz; see
2.4). At 30 fps a frame has 33.3 ms = **35 M cycles** at 1050 MHz. The DECON framebuffer is
write-combined (`mmu.c`), the canvas write-back; presenting the full panel (17 MB read + 17 MB
written) takes about 6 ms.

Per-pixel costs assumed below (the glass builder measures and reports the real ones):

| Path | Integer only (scalar, 8.8 fixed point, LUTs) | FP/SIMD (NEON 8 x u16 or 4 x f32 streams; scalar f32 with fsqrt/fdiv) |
|---|---|---|
| pane interior (frost from upsampled A5/B rows, vibrancy, tint, specular, coverage) | ~25 cycles/px | ~5 |
| straight rim band (1-D bend/rim tables; the refracted sample moves along one axis) | ~30 | ~10 |
| corner rim (2-D SDF and normal from per-radius tables, 2-D bilinear frost and dispersion) | ~120 | ~30 |
| content (A8 glyph or image blend) | ~10 per covered px | ~6 |
| crossfade (overlay backdrop SHARP/SOFT) | ~6 | ~1.5 |

The expensive states (region = what is recomposed per frame):

| State | Region | Integer only | FP/SIMD | ms at 1050 MHz (FP/SIMD) |
|---|---|---|---|---|
| menu idle, no motion | nothing (signatures equal) | 0 | 0 | 0 |
| countdown ring ticking | island 940 x 136 | 6 M | 2.5 M | 2.4 |
| focus lens moving between rows | ~1296 x 400: 330 k interior, 150 k straight rim, 40 k corner, 100 k content | 18 M | 6 M | 6 (+0.8 present) |
| **section expand** (0.5 s; everything under the header moves) | ~1296 x 2450: 2.2 M interior, 750 k straight rim, 255 k corner (5 cards, cached tables), 800 k content | 108 M | 31 M + 4.7 M present | **34** |
| **launch card growing to full screen** (0.6 s) | 4.26 M: card interior (base hidden under cov = 1), 563 k straight rim, 77 k corner, glyph and text | 130 M | 30 M + 6.3 M present | **35** |
| overlay opening (crossfade + frosted menu) | full screen crossfade + 940 x 1100 frost | 50 M | 22 M | 21 |
| overlay freeze (once, at open) | SHARP copy + SOFT at half res | - | ~45 M | one 45 ms frame |
| intro, wallpaper changing (0-1600 ms) | 720 x 1480 wall from radial terms + upsample + present | 110 M | 25 M | 24 |

Integer-only would put section expand and the launch card at 8-10 fps; with FP/SIMD they sit at
the 30 fps target on S-Boot's clock (just), and everything else well under it. The spec's own
"Software path" note (NEON over dirty rects, interiors baked per tint, refraction only in the rim
band, corner tables instead of pow) is the plan; the numbers above assume it.

Animations are time based (springs integrate real dt, the intro is a function of real t), so a
slow frame drops frames, never slows the motion.

### 2.2 How

- `xnu_boot.S` entry (after the log page is set up, before `bl star_prepare_xnu`):
  `mrs x22, cpacr_el1`, store it in log slot 119 (`0x91fff000 + 952`), then
  `orr x14, x22, #(3 << 20)` (FPEN = 0b11), `msr cpacr_el1, x14`, `isb`. x22 is callee-saved,
  so it survives the C code.
- Before `br x16` (after `star_dcache_all`): `msr cpacr_el1, x22`, `isb`. Every OS is entered
  with the CPACR S-Boot left; each OS initialises its own FP state (Linux `__cpu_setup`, XNU,
  edk2) and none relies on the V registers' contents.
- `build.sh` compiles the v7 files with `CFLAGS_V7`: `CFLAGS` without `-mgeneral-regs-only` and
  `-mcpu=cortex-a53`, plus `-mcpu=cortex-a55 -fno-math-errno -ffp-contract=fast` (A55: FP16,
  dot product, CRC32; `-fno-math-errno` lets `__builtin_sqrtf` be `fsqrt`). No `-ffast-math`.
  NEON intrinsics come from GCC's `<arm_neon.h>` (freestanding-safe). There is no libm: `exp`,
  `pow`, `sin`, `cos` are the builders' own (polynomials or tables); `sqrtf` is `__builtin_sqrtf`.
- Nothing else changes: exception vectors (`em_vec.S`) and every other file never touch V
  registers, so nothing needs saving; the v7 entry points (`menu7_*` in `menu.h`) take and return
  no floats, so general-regs-only callers can call them.
- GCC may call `memcpy`/`memset` for struct copies in FP code: `sb7.c` defines `memcpy`, `memset`
  and `memmove` (plain loops). `store.c`'s "no struct copies" rule stays for the other files.

### 2.3 Stats

Per frame class (`M7_PH_*`: intro, menu, lens, list, expand, overlays, launch, progress, end), menu7
also writes slots 128 + class: frames << 48 | worst ms << 32 | total ms (MENU7-BUILD.md section 6).

menu7 writes frame statistics to log slots 120..127 (`g7_get_stats` plus per-state worst frame:
120 intro frames | worst ms, 121 menu frames | worst ms, 122 expand worst, 123 launch worst,
124 freeze ms, 125 bake ms, 126 CNTFRQ-based boot-to-menu ms, 127 flags). Lab can show them as
it shows `star_anim_stats`. (Slot 119 is the saved CPACR.)

### 2.4 CPU clock (reserve, not in v7.0)

If the measured worst frames of section expand or the launch card exceed 40 ms, menu7 may raise
the A55 cluster to 1690 MHz through ACPM DVFS (domain cpucl0 = 2, the request XNU's
`starlte_dvfs` sends; ACPM sets the voltage) behind `M7_CPU_BOOST`, owner (c). Every OS then
starts at 1690 MHz instead of 1050; Linux and XNU set their own clocks anyway, Windows would run
faster. Only after on-device numbers and the user's go-ahead. The secondary cores are not an
option: PSCI CPU_OFF does not return a core cleanly here (em.c "Parked core").

---

## 3. Memory map

The loader's identity map (`mmu.c`): write-back 0x80000000..0x8bffffff, non-cacheable above.
Taken: the BOOT Image with its appended iOS payload (< 0x83800000), the ramdisk 0x89000000..
0x8a000000 (S-Boot, read in place by sb7.c), S-Boot's FDT at 0x8a000000 (and whatever follows it
up to 0x8c000000: not used), the OS staging 0x8c000000..0x91000000, the log page 0x91fff000, the
XNU window 0x92000000..0xaf800000, UFS DMA 0xaf000000, the framebuffer 0xcc000000.

v7 uses **0x84000000..0x88800000** (72 MiB, write-back) and, for the intro only, **0x83800000..
0x84000000** (`I7_CAPT`, 8 MiB, write-back), nothing else:

| Address | Size | Name | Owner | Holds |
|---|---|---|---|---|
| 0x83800000 | 8 MiB | `I7_CAPT` | intro7 | S-Boot's splash as the intro found it on the panel: its non-black row bands (at most `I7_CAPT_BANDS` 8, `I7_CAPT_MAX_PX` 393216 px = 1.5 MiB), each an `sb7_image` (ARGB premultiplied, RAW, 64-byte aligned) put in with `sb7_put` as `SB7_S_CAPT0 + i`. Used only when `intro7_capture` finds at run time that the BOOT Image (`_start` + image_size, which with a payload appended is also `boot_rd_end`) ends at or below 0x83800000, that the appended payload (`star_info` rd_start .. rd_end, from `boot_rd_*`, read through `m7_rd_range`) does not overlap it, and that the menu ramdisk's SB7 tail (`star_font_blob`) does not either (BOOT is 0x3700000 bytes, so an Image at 0x80080000 ends by 0x83780000). `intro7_release` unregisters the images when `intro7_run` returns; their decoded copies stay in `G7_CACHE` (unreachable) until glass resets it. Static asserts in intro7.c: ends at `G7_CANVAS`, holds `I7_CAPT_MAX_PX`, at most half of `G7_CACHE` |
| 0x84000000 | 17 MiB | `G7_CANVAS` | glass | the composed panel (ARGB, stride 1440); dirty rects go from here to the FB |
| 0x85100000 | 17 MiB | `G7_SHARP` | glass | the base layer frozen under an overlay; while nothing is frozen, GLB (the base glass composed over BG8, without the lens); on an end screen (frozen, back 1) CURT, the panel below the curtain |
| 0x86200000 | 17 MiB | `G7_BASE` | glass | BG8: wallpaper (720 x 1480, bilinear to the panel) x dim .8, dithered: the opaque base, static after the intro |
| 0x87300000 | 18 MiB | `G7_POOL` | glass | WALLTEX 720 x 1480 (4.26 MB), FIELD 720 x 1480 (intro's static field, 4.26 MB), blur A 360 x 740 (1.07 MB), frost A5 360 x 740 (1.07 MB), blur B 180 x 370 (0.27 MB), SOFT 720 x 1480 (4.26 MB; while nothing is frozen TPL, the base glass's transmission, 1440 x 2960 x 8 bits), tables and row buffers (1 MiB, below), decode scratch (1 MiB); glass lays it out (`glass_int.h`, static asserts) |
| 0x88500000 | 3 MiB | `G7_CACHE` | glass | decoded icons and provider glyphs, rendered OS tiles (124 for every row, 92, 220, 380 on demand), LRU |
| 0x88800000 | 8 MiB | (ui.c) | - | `UI_SCRATCH` of ui.c (EngineerMode, v6): untouched |

The pool's table area (`P_TAB`, 1 MiB): 0 .. 0x2B200 the wallpaper's radial and column tables
(`glass_wall.c`), 0x2C000 the row buffers (`glass.c`, 10 x 1440 floats), 0x40000 the sdSq g(t)
tables, 0x50000 the specular column tables (5 sets x 20 shapes x 1440 floats, .. 0xDCA00), 0xDD000
the half-resolution rows (.. 0xEF690), 0xF0000 the moving frames' frost-correction rows (18 x 360
floats and 3 x 1440 int16, .. 0xF8700). Each region has a `_Static_assert` against the next. No v7
file's `.bss` exceeds 128 KiB (glass.c 101 KiB, glass_shade.c 101 KiB, menu7.c 90 KiB).

All are overlaid only by code that does not run with v7: anim.c's buffers (0x84000000..
0x85c00000, the v6 intro), menu.c's `MENU_SAVE` (0x86000000, v5), menu6.c's `CANVAS` and
`DD_SAVE` (0x87100000.., v6). If v7 fails to start (`menu7_main` returns -1 before drawing),
v6 runs and owns them. EngineerMode (Lab) draws on the framebuffer through ui.c and uses only
`UI_SCRATCH` and the NC RAM test window, so `G7_CANVAS` survives Lab: menu7 calls
`g7_present_all()` when Lab returns.

**Fail-back.** No OS load touches 0x84000000..0x88800000 (all OS buffers are in the NC areas or
bank2), so after a failed start every v7 buffer is still valid: the menu comes back on the error
card without baking again.

**Handoff.** `xnu_boot.S` cleans and invalidates the whole data cache by set/way before the MMU
goes off, so the dirty v7 lines reach RAM and nothing stale stays behind for the OS. Nothing new.

**More RAM, if ever needed** (not used by v7.0):
- Bank2 (0x880000000.., 2 GiB, NC, free at menu time; XNU payloads are copied there only after
  the menu returns): `mmu.c` can map its first 64 MiB write-back while the menu runs (an l2
  table for l1[34] instead of the 1 GiB NC block). Before `menu7_main` returns a pick: clean +
  invalidate exactly that range by VA (`dc civac`, 64 B lines), restore the NC block, `tlbi
  vmalle1`, `dsb`, `isb`. After a failed start, everything that lived there must be rebuilt (an
  XNU copy may have overwritten it).
- 0x83800000..0x84000000 (8 MiB): taken by `I7_CAPT` during the intro (checked at run time,
  above); free again after it (`intro7_release`: `sb7_get` no longer finds SB7_S_CAPT0 ..).

---

## 4. Interfaces

### 4.1 glass (`glass.h`)

**Layers.** A frame (`struct g7_frame`) has two layers, as the concept's DOM has:

```
base:  BASE wallpaper (mix to blur B by back) x dim                     cvBase, uOpaque = 1
       base shapes in DOM order: section i, its tile, ..., dock capsule, Power, island, lens
       the SAMSUNG glass wordmark (intro only)                          uTxP / uCap
       dither                                                            fixed hash
       base items in DOM order (#menu: status, wordmark, title, ...)    the menu DOM
over:  backdrop (0, .004, .014) at alpha back x .45                     cvOver, uOpaque = 0
       over shapes in DOM order: preview, options menu, sheet, sheet close, sheet Start,
       alert, alert buttons, notice, launch card, Try Again, Back; then the overlay lens
       dither; composited premultiplied: out = acc x al + below x (1 - al)
       over items (#over, then #splash, then #end)
edge:  x 0..35 and 1405..1440, black 60 % -> 0 (linear)                 .edge
```

Shapes are shaded with GLASS_FS **exactly** (K = 1, so `aa = 1.25 + 30 soft` and the rim
falloff is `max(2, 1.1) = 2` px): `sdSq` (superellipse n 2.6 -> 2 and radius x 1.18 -> 1 by
smoothstep(.72, .98, r / min half size)), the shadow (offset 24, e^(-d/48), 32 %, cull at
110 / 150 px), coverage, bezel `clamp(.8 r, 16, 64)`, the normal from central differences of the
SDF at 1.5 px (where d > -bez - 4), `frost()` (A5 then toward B by max(.8 soft, .75 SZ, .92 FR)),
dispersion from A, vibrancy, tint, milk, edge light with `L = normalize(-.45 + .9 lx, -.9 + .6 ly)`,
inner band, far-side darkening, specular, the lens rule, `G7_S_CLIP`'s viewport fade, the dither
`(hash(gl_FragCoord) - .5) / 255` with gl_FragCoord = (x + .5, 2960 - y - .5). The wallpaper is
WALL_FS at 720 x 1480 (pixel centres mapped to panel px), and blur A / B are the exact four
BLUR_FS passes each of `renderWall` (bilinear, clamp to edge), sampled bilinearly as WebGL does.
Every colour operation works on sRGB values (no linearisation). The over layer is drawn with
soft = 0 and ta = 0 (as `o.draw(shO, {...U, soft: 0, ta: 0})`).

Tables and planes are implementation choices of glass (per-radius corner tables of d and n,
1-D edge tables per bezel, A5 = the 5 frost taps pre-summed on the 360 x 740 grid (exact: the
±12 px taps are ±3 texels of A), per-row upsampling of A5 and B, separable specular). They must
reproduce GLASS_FS to within 1/255 per channel except where noted in section 6.

**Content items.** One kind per thing the concept's DOM draws:

| Kind | Concept elements |
|---|---|
| `G7_TEXT` | every text node; `G7_I_GRAD` for `.pn` (the provider names' background-clip:text sheen); `G7_I_ELLIP` for `.dd` (row detail) and `.rn` (row name); `scale` for transformed text (press .965, preview 1.04, options .25 -> 1, alert 1.12 -> 1, notice, launch content .94 -> 1 / 1.06 -> 1, intro section scale .96 -> 1) |
| `G7_IMAGE` | line icons, provider tile glyphs, the wordmark stars, the battery, the warning icon, SAMSUNG |
| `G7_TILE` | OS tiles (rows 124, island 92, sheet 220, launch 380); `sat` .3 and alpha .46 for systems that cannot start |
| `G7_RECT` | tags, `.sd`/`.dot` status dots (filled; `ring` 3.5 for missing, `ring` + `G7_I_DASH` for coming), separators, `.msep`, segmented control capsules and the `.kf` focus rings (two rects: 12 px blue, 6 px #0b0d12, under the segment), the sheet's `.grp`, grab bar, switch track and knob, launch bar track and fill, error reason chip, island message disc, alert icon disc, `#erIc` disc and its 2 px inset ring, the end screen's black |
| `G7_SHADOW` | box-shadows and drop-shadows: switch knob, `.sd.ready` glow (0 0 12 px green 50 %), sheet tile (0 18 30 .35), launch tile (0 40 70 .45) (a squircle's drop shadow is drawn as its rounded rect: r = 0.22 w) |
| `G7_ARC` | the countdown ring (track r 39 width 7 white 16 %, progress #6fb3ff with round caps) |
| `G7_STROKE` | the section chevron (i-chev, stroke 2.6 at 60 px, rotated 0..99 degrees by menu7), the end spinner's 8 bars |

Clip 0 is the panel; menu7 gives `.rows` (overflow hidden), `#vp` (y 170..2620) and the launch
card (rounded, its spring radius) as clips; `G7_I_FADE` applies `#vp`'s mask
(`linear-gradient(transparent 0, #000 76px, #000 calc(100% - 140px), transparent 100%)`).

**Text.** Pen positions in 1/64 px, glyph origins rounded to whole px at scale 1 (bilinear from
the atlas when scaled), kerning from the font section, letter-spacing `track x px` after every
character (CSS adds it after the last one too; `tw` in the geometry JSON includes it). Glyph
coverage blends the colour over what is below (straight alpha, sRGB). `g7_text_w` must match the
geometry JSON's `tw` to 0.5 px for every text in every state (the font acceptance test).

**Tiles.** `G7_TILE` draws from `G7_CACHE`: glass renders a tile once per (tile, size) from its
`sb7_tile` record: squircle n = 5 (the concept's 72-segment polygon, anti-aliased), the linear
gradient c1 -> c2 from (0, 0) to (.75, 1) of the box, the glyph layer, the sheen (white .28 at
0, 0 at .45, black .14 at 1, vertical), the 1.1-unit white rim at 24 % centred on the outline.

**Dirty tracking.** Keys are stable per element (menu7 derives them from the element: e.g.
`'r' << 24 | entry << 8 | part`). A shape's dirty box is its box grown by 110 px each side and
150 px top and bottom; an item's is its drawn bounds (text: pen box with the glyph bounds; shadow:
with its blur). Merged into at most 8 rects (horizontal bands); a frame whose dirty area exceeds
60 % of the panel recomposes everything. A frame-wide change (`dim`, `back` while not frozen,
`lx`/`ly`, `wall` other than final, `word`) dirties the panel.

**Freeze (overlays).** See `glass.h`. SOFT is the base layer at soft = 1 (base glass with
`uSoft` = 1 at half resolution) under its content rendered at half size, scaled .965 about
(720, 1332) (the `#menu` transform-origin 50 % 45 %), Gaussian-blurred σ = 13 half-res px
(CSS blur(26px)), x .75 (brightness). Frames while frozen: `mix(SHARP, SOFT, back)`; SHARP drawn
scaled `1 - .035 back` about (720, 1332) when the frame budget allows (D4).

### 4.2 menu7 (`menu7.h`)

#### 4.2.1 The concept's script, function by function

menu7.c ports the script's behaviour line by line. Map (concept -> menu7):

| Concept | menu7 |
|---|---|
| `Spring`, `spA`, `ease`, `eOut`, `clamp` | `m7_spring_*`, `m7_spa`, `m7_ease`, `m7_eout`, `m7_clamp` |
| `P` (providers and entries), `PB`, `EB` | sections from the catalog (4.2.2) |
| `S` (mode, ov, open, focus, ofocus, def, last, auto, cd, run, ctx, alert, sheet) | one static state struct |
| `buildMenu`, `tagHTML`, `subHTML`, `rowHTML`, `provSub`, `subtitleHTML`, `refreshRows` | layout of sections, rows, tags, subtitles into base items |
| `meta`, `collect`, `rectP`, `lensTarget`, `lensStep`, `lensShape`, `mkLens` | shape emission and the two lenses (LB base, LO over) with the same insets and radii |
| `measureScroll`, `ensureVisible`, `scroll`, `room` | scroll spring, room spring (the island pushes the list by 176) |
| `menuKeys`, `setFocus`, `moveFocus`, `openProv`, `closeProv`, `toggleProv`, `menuActivate`, `menuBack`, `startEntry`, `shakeRow` | the same |
| `pillCountdown`, `pillMsg`, `startCountdown`, `hidePill`, `cancelCountdown` | the island |
| `openCtx`, `ctxKeys`, `ctxActivate`, `closeCtx`, `setDefault` | options menu |
| `ALERTS`, `openAlert`, `alertKeys`, `alertActivate`, `closeAlert` | alerts (power, recovery, download) |
| `openSheet`, `sheetKeys`, `sheetActivate`, `closeSheet` | Details sheet |
| `toast` | notice |
| `launch`, `launchError`, `launchRetry`, `launchBack`, `launchStep` | launch and error cards (4.2.5) |
| `endScreen`, `endKey` | end screens: handoff, off, restart, recovery, download |
| `closeOverlays`, `hardReset`, `toMenu` | the same |
| `prepMenuForBoot`, `finishIntro` | `m7_prepare_boot`, `m7_finish_intro` (intro7 calls them) |
| `key`, `keyDown`, `keyUp`, `hold`, pointer handlers, `tap`, wheel | `m7_input` events -> the same dispatch (no wheel) |
| `update` | per-frame state -> geometry (springs to boxes) |
| `render`, `sig` | `m7_build` + `g7_frame` (glass does the signature) |
| `frame` | the main loop (4.2.3) |
| `go(st)` | `m7_preview_state` (harness only) |

Every UI string is the concept's, verbatim, with its typography (’ “ ” · …): "Choose a System",
"<name> starts by default", "Volume to move · Power to choose · Hold Power for options" /
"Bixby goes back · Hold Bixby for Lab", "Starting in N s · Tap to cancel", "Auto-start Off",
"Couldn’t Start …", and so on.

#### 4.2.2 The catalog as provider sections

- Sections in this order, one per category: Apple (`CAT_APPLE`), Microsoft (`CAT_MICROSOFT`),
  Google (`CAT_ANDROID`), Linux (`CAT_LINUX`), Starbite (`CAT_STARBITE`), Other (`CAT_OTHER`,
  only when it has a visible entry), Lab (`CAT_LAB`). Glyph `SB7_P_*`, tint `M7_TINT_*`.
- Rows: the category's catalog entries without `CF_HIDDEN`, in catalog order. The Lab section's
  first row is **Starbite Lab** (not a catalog entry: name "Starbite Lab", detail "Display ·
  touch · keys · UART · memory · clocks", tag Lab, ready, its own tile), which opens Lab
  (`em_main`); visible lab-category entries follow it.
- Row state: `star_entry_ready(i, &why)` true: **ready** (green dot, the version line, tags).
  False: **missing** (grey ring, "Not Installed", the detail is `why` in sentence case, see
  4.2.5). An entry whose `file[0]` is empty is **coming** (dashed orange ring, "Coming Soon",
  detail = version): a catalog author's placeholder.
- Tags: `TAG_BETA` "Beta" (.tag), `TAG_EXPERIMENTAL` "Experimental" (.tag.exp), `TAG_STABLE`
  "Stable" and `TAG_DAILY` "Daily" (.tag.stable), `TAG_LAB` "Lab" (.tag); "Default" (.tag.def,
  star) first when the key is the default. Tags only on ready rows (as `tagHTML`).
- Provider subtitle as `provSub`: the ready names joined ", " then "· N Systems" in tertiary;
  Lab: "N Tools Ready" (Starbite Lab counts); a section without ready entries: "N Coming Soon",
  or "Coming Soon" when it has no entries at all. An empty section does not open: Power gives a
  press pulse and a notice ("Nothing Here Yet", "<Provider> systems are coming soon").
- Tiles: `sb7_tile_for(key, category)`.
- "Last Used" on the row whose key is `settings.last_key`.

#### 4.2.3 Main loop, settings, boot-next, fail-back

`menu7_main` on its first call: `star_menu_begin()`, sections from the catalog and readiness
(UFS/SD reads happen here, while S-Boot's splash is still on the panel), `g7_init`, `g7_bake`,
`hw_tp_start()`. Then:

1. `star_menu_next()` >= 0 (one-shot next): no intro; the launch card appears full screen over the
   wallpaper (LA and content springs from 0) and the call returns that entry. (Deliberate: a
   reboot-to must not wait for an intro.)
2. Otherwise `intro7_run()`; `I7_LAB` opens Lab after it.
3. The loop: `m7_input` until empty -> dispatch; dt = real time since the last step (clamped to
   50 ms); `m7_step(dt)`; `m7_build`; `g7_frame`; then wait (polling input every 2 ms) until 33.3 ms
   after the frame's start. When `g7_frame` returned 0 and no spring moves, keep polling.

Settings (`star_menu_settings`, `star_menu_settings_save`):
- `S.def` = `default_key`, `S.last` = `last_key`, `S.auto` = `timeout_s` (0, 3, 5, 10).
- "Set as Default" and the sheet's switch: `default_key` = key, save. Choosing 3/5/10 s also
  makes the entry the default (as `ctxActivate`); Off sets `timeout_s` = 0.
- No valid record yet: no default, no countdown, and the first "Set as Default" writes
  `timeout_s` = 5 (the concept's first-run auto value).
- No default: the subtitle reads "Hold Power on a system for options" (i-info, secondary).
- Launch: `star_menu_remember(idx)` (last used) before returning the pick.
- Countdown after the intro when `timeout_s` > 0 and the default is ready.

Fail-back: when `star_menu_failure(&name, &why)` returns 1, `menu7_main` resumes in launch mode
with the error card for the entry it returned last (its row, its springs as they were), as
`launchError()` does (shake, red tint, content crossfade). Try Again returns the same pick again;
Back shrinks the card into its row (`launchBack`).

Recovery, Download, Power: the concept's alerts and end screens, then `hw_reboot(INFORM_RECOVERY)`,
`hw_reboot(INFORM_DOWNLOAD)` (as v6), `hw_reboot(INFORM_NORMAL)` (Restart), `hw_power_off()`; if
it returns (a charger keeps the phone on) the end screen says "Couldn’t Power Off" / "A charger
keeps Galaxy S9 on. Restarting…" and reboots after 1.5 s.

Lab (dock Lab, Hold Bixby anywhere, the Starbite Lab row): `em_main(in, cmdline, len)`; it
returns an entry + 1 to launch (from the Lab row's rect) or 0: readiness is computed again (Lab
may have mounted the SD store), then `g7_present_all()` and a full frame.

#### 4.2.4 Loader integration (owner c)

- `menu.h`: declare `int menu7_ready(void); int menu7_main(const struct star_info *, char *,
  unsigned); void menu7_progress(unsigned, unsigned, const char *, const char *); void
  menu7_handoff(const char *);` and `void star_menu_progress(unsigned, unsigned, const char *,
  const char *); void star_menu_handoff(const char *);` (menu.c: forward to menu7 when v7 is
  active, else nothing).
- `xnu_boot.c` `star_splash_draw`, "SBMENU01" branch: if `menu7_ready()` do not call
  `star_intro_splash()` (S-Boot's splash stays); `menu.c` `star_menu`: if `menu7_ready()`,
  `r = menu7_main(...)`; `r >= 0` returns it; `r < 0`: `star_intro_splash()`,
  `star_fb_row_base(36)`, then v6 as today.
- `fbtext.c`/`fbtext.h`: `void star_fb_mute(int on)`: while on, `star_fb_text*`, `star_fb_hex`
  and `star_fb_fill_rows` draw nothing. menu7 mutes before returning a pick; `fatal()` unmutes
  (no menu comes back after it). The loader's status lines would otherwise scribble over the
  launch card.
- `xnu_boot.c`: `star_menu_progress()` calls at the real stages of each method, with the concept's
  wording and real details, e.g. Linux: "Reading kernel" (`linux.image · 31.4 MB`), "Verifying
  kernel CRC32" (`0x9A3F1C07`, the store entry's CRC), "Loading device tree", "Loading initramfs",
  "Handing off to kernel" (`EL1 · 0x92080000`); XNU: "Reading kernelcache", "Verifying image",
  "Building device tree", "Loading ramdisk", "Handing off to XNU"; UEFI: "Reading firmware",
  "Verifying CRC32", "Starting firmware"; Android: "Reading kernel", "Loading ramdisk", "Loading
  device tree", "Handing off to kernel". `star_menu_handoff("EL1 · 0x…")` just before
  `star_prepare_xnu` returns the entry.
- Optional (phase 2): a chunk callback in `store_load` so the bar moves inside a long read.
- `xnu_boot.S`: CPACR save/enable/restore (2.2). `build.sh`: the v7 files with `CFLAGS_V7`,
  linked into `xnu_boot`.

#### 4.2.5 Launch, progress and errors on the device

- Start: the launch card grows from the row (`{x + 16, y + 6, w - 32, h - 12}`, r 62), the
  island (r 68) or a button, `.52/.88` on x, y, w, h, r; content fades in at 180 ms; the base
  freezes behind it. "Bixby to cancel" works until the springs settle; then menu7 mutes fbtext
  and returns the pick (the hint fades). Stage 0's text is already on the card.
- `menu7_progress(k, n, title, detail)`: the bar eases from its value to (k + 1) / n over
  <= 120 ms (the concept's shaped stages), stage and detail text swap; synchronous frames.
- `menu7_handoff(detail)`: the end screen "handoff": black, the static 8-bar spinner,
  "Starting <name>", the mono detail, no "Press any key" sentence.
- Errors: the error card's title is "Couldn’t Start <X>" with X = "Linux", "Android", "Windows"
  for those categories and the entry's name otherwise (as `launchError`); the chip is `why` in
  sentence case with known acronyms kept (CRC, CRC32, UFS, SD, DTB, FDT, XNU, UEFI, OS, RAM, ID:
  "PAYLOAD CRC MISMATCH" -> "Payload CRC mismatch"); the message by class: CRC -> "The <kind>
  for <name> didn’t pass verification. Reflash it with USB Toolkit, or try again."; missing file ->
  "<name> needs <file>. Add it with USB Toolkit, or try again."; other -> "Starbite couldn’t start
  <name>. Try again, or choose another system."; the mono line is the loader's raw reason.

#### 4.2.6 Input

`m7_input` reads `hw_keys()` (K_UP, K_DOWN, K_POWER, K_BIXBY; debounced 20 ms) and
`hw_tp_event()` (finger 0, panel px). Power: `M7_EV_POWER_DOWN` at once (press visual), then
`M7_EV_POWER_HOLD` at 520 ms or `M7_EV_POWER` on release; Bixby: `M7_EV_BIXBY_HOLD` at 700 ms or
`M7_EV_BIXBY` on release; Volume: on press, repeating after 450 ms every 110 ms (the browser's
key repeat in the concept). Touch down / move / up go to the concept's pointer logic: long-press
480 ms on a row, drag after 32 px in the list (1:1, rubber band x .5, release projects 0.32 s of
velocity), tap. Any input cancels the countdown; Power during the countdown starts now.

### 4.3 intro7 (`intro7.h`)

- The loader starts while S-Boot's logo.jpg is on the panel: Samsung's own splash (UP_PARAM;
  `tools/brand/splash-noknox.py` makes the stock one without "Secured by Knox", losslessly).
  `intro7_capture` (first thing in `intro7_run`, before anything is drawn) reads the panel once
  (uncached, 64-bit loads), finds its picture rows (a pixel counts when its largest channel is at least 8: JPEG leaves
  1..7 values in background blocks), groups them into bands (black gaps up to
  `I7_CAPT_GAP` 16 rows join, two pads, so two padded boxes never split one JPEG block's faint
  values; more than 8 bands and the gap doubles), grows each band's box by
  `I7_CAPT_PAD` 8 px (never past the middle of a gap) and copies it unthresholded into `I7_CAPT` as a premultiplied ARGB image with alpha = its largest channel, so over the
  intro's black t = 0 base it is exactly what S-Boot drew. The intro's first frame is then the
  splash pixel for pixel (`intro-film.sh` with `I7_SPLASH`: 0 px differ for the stock, the
  Knox-free, the "Powered by starbite", the concept logo.jpg and Starbite's "One mark"; with the
  old 8-row gap "Powered by starbite" lost 37 faint px between its last two bands). From the captured picture: the
  bands above `I7_NAT_SPLIT` (the logo block) fade over 260 ms rising -36 eo(t / 420) (#gs9), the
  others fade over 200 ms (#pow), there is no glass wordmark: the wordmark band (a line of text:
  at most 160 rows, at least 3 times as wide as tall) is Samsung's across panel row 1232, else
  Starbite's across row 1380 (`I7_NAT_WORD_ROW`, the capsule's centre). It stays until 260 ms,
  eases into the concept wordmark's box centred on (720, 1380) by 700 ms (Samsung's grows to
  900 wide; Starbite's keeps its size, a 14 px settle), then shrinks with the capsule and fades
  over the melt as the island comes out of it, so the only wordmark on screen is the real one.
  The splash's star (`I7_STAR_*`: a band 24..128 px, nearly square, blue: 2 sum B >= 3 sum R and
  sum B >= sum G, so green, cyan and teal icons are not stars; star-shaped: alpha fills at most
  half the box, One mark's 0.23 against a solid tile's 1.0; centroid above `I7_NAT_SPLIT`; R
  clamped to 8..54; only the One mark splash has one) fades in place over 120 ms, no rise, while the wallpaper's star lights
  at its alpha centroid with R = sqrt(alpha area / `I7_STAR_AREA`), the area of WALL_FS's core at
  R 1 (One mark: (961.75, 1259.02), R 27.05, against the splash's 961.74, 1259.0, 27), and flies
  home through (1350, 1180) (`I7_FLY_STAR_C`). Without one the star starts at `M7_STAR0` and flies
  through `I7_FLY_C`, as before. All of it is chosen once at the capture: every frame is still a
  pure function of t. The wallpaper, the island and the menu are the concept's.
  `docs/concepts/splash/HANDOFF.md`'s optional glass drain (item 6) and reveal centre (item 7) are
  not done: both need glass*.c, and the captured word stays white until the melt. A black panel
  (`I7_CAPT_BLACK`): the concept's splash items as before (byte-identical frames). A failed
  capture (`I7_CAPT` not free, more than 1.5 MiB of picture, the SB7 table full): S-Boot drew
  something that was not copied, and with One mark flashed the concept's SAMSUNG splash (or its
  glass SAMSUNG wordmark) would be a hard cut from the approved one. So there are no splash
  items and no glass wordmark: the first frame clears the panel to black and the wallpaper star
  lights at One mark's star (`I7_ONE_STAR`, 961.74, 1259.0, R 27) and flies through
  `I7_FLY_STAR_C`; the capsule is born empty (h7: `H7_CAPT_BUSY=1`).
  The concept's "black 250 ms, splash until 1600 ms" phase is S-Boot's own time and is not
  replayed.
- The capture's result is in log slots 116..117 (`intro7.h`): slot 116 = px << 32 | 0xca97 << 16
  | why << 8 | bands (why: 0 `I7_CAPT` not free, 1 black panel, 2 too much picture, 3 SB7 table
  full, 0xff the native path ran), slot 117 its duration in us. The duration is unmeasured on the
  device (both passes read the uncached panel; estimated ~50 ms); if it is large, pass 1 can scan
  only S-Boot's logo rows and a strip at the top.
- `intro7_frame(f, t)` is the concept's `introAt(t)` with reduced motion off, using the
  constants in `intro7.h`, star0 = `M7_STAR0_*` (the measured `#spStar`), or the captured star. It calls
  `m7_build(f, &xf)` then sets `f->wall`, `f->word`, `f->dim` and appends the splash items to
  `f->over` (SAMSUNG `SB7_S_SAMSUNG` at (70, 1270) opacity `1 - t / 220`; "Galaxy S9"
  `SB7_F_IT500_60` +.005 em centred, baseline 1580, rising; "Powered by" `SB7_F_IN400_31` +.02 em
  #8d8d92 baseline 2518; "starbite" `SB7_F_IT300_66` -.01 em pen 614 baseline 2598 with
  `SB7_I_GSTAR_66` at (828, 2542.3)).
- The wallpaper moves only in the intro: glass rebuilds it from `f->wall` (radial terms over the
  static field) while `f->wall` differs from the final wall; the blur planes stay final (D6).
- `intro7_run()`: `m7_prepare_boot()`, frames at 30 fps from real time until 2350 ms, then
  `m7_finish_intro(0)`; a key or tap: `m7_finish_intro(1)`, `I7_SKIPPED`; Bixby held 700 ms:
  `m7_finish_intro(1)`, `I7_LAB`. Keys already down when the intro starts are ignored until
  released (as anim.c).

### 4.4 Assets (`sb7.h`)

**Fonts.** From the google/fonts repository (what fonts.googleapis.com serves the concept):

- `https://github.com/google/fonts/raw/main/ofl/intertight/InterTight%5Bwght%5D.ttf`
- `https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf`
- `https://github.com/google/fonts/raw/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf`
- each directory's `OFL.txt`

over HTTPS, into `tools/brand/fonts/`, with the URL, the google/fonts commit and the sha256 of
every file in `tools/brand/fonts/SOURCES-menu7.md` (and in the container's INFO section). Set the
variable axes (wght; opsz = min(px, 32) for Inter), apply `cv11` and `ss01` (and `tnum` for the
countdown digit), kerning on, FreeType grayscale, no hinting. The 29 fonts and their charsets are
in `sb7.h`. Acceptance: every `tw` in `docs/concepts/boot-geometry/*.json` reproduced to 0.5 px by
summing the container's advances, kerning and letter-spacing (`sb7-dump.py --check-geometry`).
Estimated size ~3 MB.

**Icons.** The concept's `<symbol>` SVGs at the sizes and stroke widths in `sb7.h`, rendered
(resvg or Chrome) to A8 coverage (ARGB for `SB7_I_WARN_150` and `SB7_I_BAT_84`), RLE. The nominal
box is the SVG's 1 em box (24-unit viewBox), with a margin for anti-aliasing.

**Provider glyphs.** `g-star`, `g-flask` with their masks, 96 px, white,
drop-shadow(0 4px 10px rgba(0,0,0,.25)) baked, ARGB RLE; `SB7_P_OTHER` new. Apple, Microsoft,
Google and Linux are the real marks instead of the concept's drawn `g-apple`, `g-win`, `g-andr`,
`g-tux` (the user's request): the Apple logo (white), the Microsoft four squares, the Google "G" and
Tux, in their own colours, with the same shadow (`tools/brand/menu7_logos.py`, sources and
trademark notes in `tools/brand/logos/SOURCES.md`).

**OS tiles.** One `sb7_tile` per tile id with glyph layers at 92, 124, 220, 380: for the real
operating systems the official logo (`menu7_logos.py`, rendered by rsvg-convert at each size, in
its own colours, `LOGO_G` in `menu7-assets.py`); for the rest the concept's `TG()` artwork (text
glyphs in Inter Tight 700 / JetBrains Mono 700 at the concept's sizes, `star`, `usb`, `spark`,
`bars`, `touch`, `uart`, `mem`, `chrome`). The squircle, gradient, sheen and rim stay the
concept's. The tile map (`SB7_T_TILEMAP`) for today's catalog:

| Catalog key | Tile (c1 -> c2, glyph) |
|---|---|
| ios26 | #0b2560 -> #3d8cff, Apple logo (white) |
| win11 | #ffffff -> #dbe4f0, Windows 11 logo (#0078d4) |
| android | #05202c -> #0b4a5e (Android navy), Android robot head (#3ddc84) |
| linux-ubuntu, linux | #c13a0d -> #f47b3c, Ubuntu Circle of Friends (2022, white) |
| linux-ubuntu-gnome | #5e1b58 -> #d2491f, Circle of Friends |
| linux-ubuntu-lomiri | #77216f -> #e95420, Circle of Friends |
| linux-arch | #0c1015 -> #2a333e, Arch Linux logo (#1793d1) |
| linux-pmos | #ffffff -> #dfeadf, postmarketOS logo (#009900) |
| linux-kali | #0a1b3c -> #367bf0, Kali dragon (white) |
| linux-debian | #ffffff -> #eee1e6, Debian swirl (#d70751) |
| linux-fedora | #0b1a30 -> #294172, Fedora logo |
| linux-shell, linux-olddtb, linux-bringup | #0c0c0e -> #2c2c31, ">_" mono (concept UEFI Shell look) |
| lab-* (prefix) | #121215 -> #2c2c31, bars (concept Display Test) |
| Starbite Lab row (key "@lab") | #3a145e -> #ad61f2, flask (new) |
| category defaults | Apple: Apple logo on the provider grey; Microsoft: as win11; Android: as android; Linux: Tux on #15161a -> #3a3d45; Starbite, other, Lab: their glyphs |

The concept catalog's demo ids for the same systems (a17, phosh, gnome, arch, pmos, kali) map to
these tiles too; only tiles the map reaches are written. `menu7-assets.py --concept-art` builds a
container with the concept's own artwork for those ids and the provider glyphs instead
(`build/brand/concept/`, for `compare-all.sh` with `CMP_RD`; docs/MENU7-BUILD.md section 4).

**Splash.** `SB7_S_SAMSUNG`: the concept's `drawSam` (paths `WORD`, stroke 17, miter, butt) at
k = 1 into 1300 x 220, A8. `SB7_T_HEIGHT`: `buildHeight()`'s 650 x 110 field. `splash7.mjs`
renders the concept's splash (t = 0: SAMSUNG, "Galaxy S9", "Powered by starbite") into the
1079 x 1747 logo.jpg region at (180, 1144), quality 95, for UP_PARAM (`upparam-pack.py`), not
flashed.

**Budget.** The container <= 8 MiB; the whole ramdisk (`menu-v7.sbm`) < 15 MiB (S-Boot puts the
FDT at 0x8a000000, 16 MiB above the ramdisk); BOOT has ~30 MB free. CRCs are checked once at boot
with the CRC32 instructions (~2 ms).

---

## 5. Preview harness

One host program, `tools/menu7/preview/h7` (owner a), built in the Lima VM (aarch64 Linux, gcc
13, `-O2 -mcpu=cortex-a55 -DM7_PREVIEW`) from the real `glass*.c`, `menu7*.c`, `intro7.c`,
`sb7.c` and `loader_stubs.c` (owner c), like `tools/anim-preview`: the memory map and the
framebuffer are `mmap(MAP_FIXED_NOREPLACE)` at their addresses (or moved by `G7_HOSTOFF` on the
Mac). `loader_stubs.c` provides `hw_*` (fake clock: each `hw_ticks` call advances by a frame or
by `--step-us`; scripted keys and touches), `cat_*` and `store_*` over `build/menu6-preview/
catalog.sbc` (or a catalog made from `tools/store/catalog.toml` by `sbstore2.py`),
`star_entry_ready` from `--ready key,key,...`, settings from `--settings default=K,last=K,
timeout=5`, `star_font_blob` over the ramdisk file, `em_main` returning 0.

```
h7 --ramdisk build/brand/menu-v7.sbm --state <state> [--p 0.4] [--t ms] [--keys up,down,power...]
   [--ready ...] [--settings ...] --out out.png [--scale 0.4625] [--stats]
h7 --ramdisk ... --list scene.dl --out out.png        # a display list from geom2dl.py (glass only)
```

`--state`: `menu countdown collapsed apple ctx submenu sheet alert recovery launch error handoff
missing lab` (via `m7_preview_state`, the concept's `go()` then `snapAll()`), and `intro` with
`--t` (via `intro7_frame`). `--stats` prints g7 timings (host times; x ~12 for the A55, as
`AP_A55X` in anim-preview).

`geom2dl.py` turns `docs/concepts/boot-geometry/<state>.json` into a display list (shapes from
`glass` entries, text from `text`/`base`/`tx`/`font`/`color`, rects from `bg`/`br`, icons from
`svg`): glass renders the concept's exact scene with no menu7, for the renderer's own diff.

`diff.py`: renders the concept (`build/menu7/concept-shots/<state>.png`, made by
`concept-geometry.sh` at 1000 x 1500, solo) and h7's output scaled by 0.4625 (the concept's device
scale in that window) with an area filter, crops the screen rect, and writes side by side + an
absolute-difference heat map + the mean error per region (status, title, each section, dock,
island, overlay). The concept's own canvas runs at 0.4625 of the panel (K = 2.16), so compare at
that scale; at native resolution v7 is sharper by design (D3).

Compare: all 14 states above, with the concept's data replaced by a catalog that matches it
(`tools/menu7/preview/concept-catalog.toml`, owner c: the concept's entries, readiness and
default/last), and the intro at t = 0, 200, 400, 600, 800, 1000, 1100, 1250, 1400, 1550, 1700,
1850, 2000, 2350. Acceptance: per region mean error under 3/255 and no structural difference a
reviewer can see, except the listed deliberate differences.

---

## 6. Deliberate differences on the hardware

| | Concept | Device | Why |
|---|---|---|---|
| D1 | requestAnimationFrame (60-120 Hz) | 30 fps, frames dropped under load (section expand and the launch card at ~30 fps at 1050 MHz) | software rendering on one A55 (section 2); motion is time based |
| D2 | the light follows the pointer (`light` springs) | fixed light, lx = ly = 0 | no pointer; the accelerometer later (no driver yet) |
| D3 | glass canvases at 0.32-0.75 of the panel (K 1.3-3) | K = 1: aa 1.25 px, rim falloff 2 px | native resolution; edges are crisper, the formulas are the same |
| D4 | the menu behind an overlay blurs 0 -> 26 px, darkens, zooms to .965 progressively | crossfade between the frozen sharp base and a soft version rendered once (sharp side zoomed when the budget allows) | a full-screen blur per frame does not fit |
| D5 | "black 250 ms, splash until 1600 ms", then the concept's SAMSUNG / Galaxy S9 / Powered by starbite fade | S-Boot's own splash (Samsung's logo.jpg), as long as S-Boot and the bake take; the intro starts from the captured panel and fades it out; Samsung's own wordmark takes the glass wordmark's place | it is S-Boot's screen; the user asked for the native splash |
| D6 | the blur planes follow the blooming star in the intro | baked once at the final wallpaper; glass that appears from 1050 ms refracts the final state | per-frame blurs do not fit; by 1050 ms the reveal is past 3250 px |
| D7 | concept demo data (iOS 14, iPadOS 18, tvOS, Darwin, iOS 7, WinPE, UEFI Shell, GSI, ChromeOS, Starbite OS, USB Toolkit, Demos, the four Lab tools) | the catalog; Lab = Starbite Lab (EngineerMode) + visible lab entries | catalog-driven |
| D8 | battery "87%" | the battery glyph without a percentage | no fuel gauge driver in the loader |
| D9 | launch progress is a 3.6 s timer in 5 stages; Bixby cancels any time | real loader stages via `menu7_progress`; Bixby cancels until the card settles | the load is synchronous |
| D10 | Lab opens a Lab section with a notice | dock Lab / Hold Bixby open EngineerMode directly | the tools live in EngineerMode |
| D11 | Reduced motion, CSS fallback, "Fail the next start" options | not present | no such settings on the device; failures are real |
| D12 | end screens "Prototype ends here. Press any key to return." | the real action (reboot, power off, handoff) | |
| D13 | screen corner radius 118 (the mockup) | not drawn | the panel's own corners |
| D14 | text by Skia (subpixel positions, its own AA) | FreeType A8 at fixed sizes, whole-pixel glyph origins, bilinear when scaled | atlases; differences under one pixel |
| D15 | one-shot boot-next goes through the menu | launch card directly, no intro | a reboot-to must not wait |

Tearing: the DECON scans out the framebuffer we write; v7 presents dirty rects right after
composing them (no vsync source in the loader). Watch for it on the first boot.

---

## 7. Integration and first boot

1. Merge order: sb7.c + container (b) -> glass (a) with `geom2dl` diffs -> menu7 (c) + integration
   -> intro7 (d). Each step builds the real `xnu_boot.Image` (`sh build.sh`) with v7 linked but
   inactive until the ramdisk has SB7.
2. The integrator diffs all states with `diff.py`, checks `readelf -r` (no ABS64), the Image size,
   and the ramdisk size, then hands the user `menu-v7.sbm` + the loader for one flash.
3. First boot reads: log slots 116..117 (the splash capture), 119 (CPACR), 120..127 (frame stats), the user's photo of the menu;
   if v7 fails before drawing, v6 appears (fallback), and the log says why.
