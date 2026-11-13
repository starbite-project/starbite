# Liquid Glass for Starbite OS: optics, behaviour, and our GPU compositor

Track C, design-ref session. v1 2026-11-12. For the GPU/OS session (starbite-os: `kern/lens.h`, `kern/gpucomp.c`,
`tools/mesa-compjob.c`) and the UI session (sbos-ui-p1: `ui/tk_glass.c`, `sbtk.h`).
Companions: [IOS-REFERENCE.md](IOS-REFERENCE.md) (where each surface uses glass) and [GAP-LIST.md](GAP-LIST.md).

Tags: **A** Apple-stated · **3P** measured by a third party from Apple's renderer (a private-filter parameter dump, or
pixel fits to iOS 27 captures) · **E** estimate. Units pt (1 pt = 3.6923 px on the S9).

**Where the numbers come from.** Apple publishes almost no optical constants. The hard numbers come from two
third-party sources:
- ShatteredGlass's dump of the private `glassBackground` CAFilter and `CASDFGlassHighlightEffect` on macOS 26 b1
  (github.com/AlexStrNik/ShatteredGlass);
- flutter_liquid_glass (liquid_glass_renderer, whynotmake-it), whose presets were fitted to iOS 27 screen captures by an
  "apple_match" harness.

The first is closest to Apple's own parameter names, the second to what iOS 27 shows. Formulas are restated in our own
words. No code is copied.

---

## 1. The material in one paragraph

Liquid Glass is **one signed-distance field (SDF) per glass group**:
- one backdrop pass does refraction, blur, face tone and shadow;
- two highlight passes sit on top.

Only a **bevel band inside the silhouette** bends light. The flat face shows the backdrop undisplaced, apart from blur
and tone. Inside the band the backdrop is sampled **inward**, by more than the band is wide, so the rim shows a
squeezed, mirrored strip of what lies further inside. That rim is the signature "liquid" edge.

The face is a **backdrop-conditioned tone**, not a tint: a wash plus the backdrop's luminance through an S-curve, plus
its chroma boosted. A thin **glint** recolours the rim on the walls facing the light (and the opposite wall). A
**0.75 pt dark contour** outside the edge gives iOS 27's "defined border". An **adaptive shadow** darkens the backdrop
more over detail (text) than over plain areas.

Nearby glass shapes in one container **merge** through a smooth union of their SDFs, and **morph** by interpolating
their rects. Glass **materialises** by ramping the lensing, never by an alpha fade. (A: WWDC25-219/284/323/356, HIG
Materials. 3P: ShatteredGlass layer tree.)

```
group = smooth-union of shapes (rounded rects / capsules, continuous corners)
pass A  backdrop under the group's bbox (+ apron) -> sharp level (~1.5 pt) + diffuse level (~5-20 pt)
pass B  per glass pixel: SDF -> bevel displacement -> mix(sharp, diffuse) -> face tone -> glint -> AA
pass C  outside the silhouette: 0.75 pt contour; the drop shadow (darken the backdrop), never under the glass
then    the content on the glass (glyphs: vibrant, light/dark by the element's flip)
```

---

## 2. Variants and the parameter set (the numbers to build)

### 2.1 Variants (A)
| Variant | Behaviour |
|---|---|
| **regular** (default; bars, buttons, CC tiles, cards, sheets) | blurs, tones by backdrop luminance, adapts: small elements flip light/dark, large ones are more opaque and don't flip; adaptive shadow |
| **clear** | permanently more transparent, **no adaptive behaviour (no flip)**. Over bright content Apple says to add a **black 35 % dimming layer** (A). Only over media (photos, camera, lock screen wallpaper), with bold content on top. Never mixed with regular in one group |
| **identity** | no glass: the switch that turns glass off |
| **tint(colour)** | a **range of tones mapped to the backdrop's brightness**, like coloured glass, not a flat fill. Primary actions only (Done, the CC active tiles, prominent buttons) |
| **interactive** | press scale + bounce, a glow from the touch point, shimmer (§5.1) |

### 2.2 The Liquid Glass slider (iOS 26.1 toggle → iOS 27 continuous slider)
- iOS 26.1 added Settings > Display & Brightness > Liquid Glass: Clear / Tinted. iOS 27 replaced it with a continuous
  slider "clear ↔ tinted" (A).
- **Starbite exposes the same slider** where iOS 27 has it: Settings > **Appearance** > Liquid Glass (A, iOS 27 User
  Guide; see specs/spec-settings.md), `s ∈ [0, 1]`. Default
  **s = 0.2** (E: between iOS 26's "Clear" default and iOS 27's higher-contrast default).
- Every material constant below is a function of s (3P, iOS 27 fits).

### 2.3 Per-element constants (regular)
`short` = min(w, h) of the element in pt.

| Quantity | Formula | Typical values |
|---|---|---|
| Bevel band h | `min(20, 0.25·short)` | 44 pt button: 11; 62 tab bar: 15.5; ≥ 80: 20 |
| Displacement amount A | `min(60, 0.5·short)` | 44: 22; 62: 31; ≥ 120: 60 |
| Profile | `t = clamp(1 + sd/h, 0, 1)` (1 at the silhouette, 0 at the band's inner edge); `d = A·(1 − sqrt(1 − t²))` (quarter circle, C1 with the flat face) | at t .5: 0.13 A; .8: 0.40 A; 1: A |
| Sample point | `q = p − d·n` (n = outward SDF gradient: **inward** sample) | |
| Corner normals | n from the SDF of the same box with the radius × 1.5 (smoother normals on the corner arc) (3P Kyant) | |
| Blur, sharp level | σ ≈ **1.5** (A-ish: the filter's `inputBlurRadius` 1.5, 3P) | |
| Blur, target σ | `σ_reg(s) = 4.5·exp(0.85·min(s,.5) + 2·max(s−.5,0)) · clamp(short/94, 0.47, 1.78)`, floor 2.1 | s 0: 4.5; .5: 6.9; 1: 18.7 (at 94 pt) |
| Blur mix | `m = clamp((σ_target − 1.5)/(σ_diffuse − 1.5), 0, 1)`: the weight of the diffuse level | |
| Saturation | in the face model's chroma gain (below), not a separate saturate | |
| Dispersion | **0** (none in iOS 26/27 regular or clear; a magnifier only, −0.06) (3P) | |

### 2.4 Face tone (3P, iOS 27 fits; replaces our `lens_shade`)
```
Y    = dot(bg, (.2126, .7152, .0722))           bg = mix(sharp, diffuse, m) at q
lumY = Y + lift·Y·(1 − Y)
face = E.rgb·E.a + T·lumY + K·(bg − Y)           (then clamp 0..1)
```

| Preset | E.rgb | E.a | T | lift | K (chroma gain) |
|---|---|---|---|---|---|
| Regular **light** | (253, 252, 253)/255 | mix(.403, .726, s) | mix(.599, .274, s) | mix(.216, .270, s) | mix(1.169, .755, s) |
| Regular **dark** | (32, 32, 32)/255 | 1 | mix(.526, .253, s) | mix(.583, .647, s) | mix(1.005, .786, s) |
| **Clear** (light = dark) | white | .126 | .954 | 0 | 1.057 |

Notes:
- **Size response for dark glass (3P):**
  - small (≤ 63 pt): T mix(.600, .298, s), lift 0;
  - large (≥ 150 pt): T mix(.4525, .2086, s), lift ~1.2.

  Interpolate by `short` between the two. Larger dark glass is more opaque.
- **Chromatic boost:** add `0.4·clamp((chroma − .15)/.65, 0, 1)` to K for already-colourful pixels (3P).
- **Clear dimming:** where the diffuse backdrop's Y > .55 under a clear element, add a black layer of **.35** over the
  element (A: 35 %). Over dark content, add none.
- **Tint:** `tone = mix(tint·0.55, mix(tint, white, .35), lumY)` (E: dark and light ends of the tint driven by the
  backdrop's brightness), then `face = mix(face, tone, .80)`. Glyphs on a tinted element are white.

### 2.5 Which preset: light, dark, flip (A rule, E numbers)
- **Small** elements (short ≤ 70 pt: bar buttons, tab bar, the search pill, the back circle, CC 1x1, banners'
  buttons) **flip** by what is behind them:
  - `Yc` = the diffuse level's mean luminance under the element (4 x 4 texels);
  - go **dark when Yc < .35**, **light when Yc > .55** (hysteresis);
  - cross-fade the preset and the glyph colour over **0.25 s**.
- **Large** elements (CC 2x2, cards, sheets, menus, the dock, the keyboard, notifications) **don't flip**: they use the
  system appearance (Starbite today: dark).
- With s ≥ 0.75 ("tinted" side), small elements stop flipping and follow the system appearance (iOS 26.4 Tinted
  behaviour, 3P).
- Glyphs on glass: light preset → #000 at 85 %; dark preset → #FFF at 95 %. **Vibrancy (E):**
  `glyph = mix(glyph, glyph·(0.75 + 0.5·chroma(bg)), 0.3)`.

### 2.6 The glint (rim highlight) (3P)
| | iOS 27 (default for Starbite) | iOS 26 / macOS 26 dump (option) |
|---|---|---|
| Light | **fixed, vertical** (from straight above); top and bottom walls glint, the bottom at **1.0x** the top | two highlight passes at **+45° and −135°** (top-left, bottom-right), each amount .5, curvature 1.7, spread 1.59 |
| Width | **1.2 pt** inward linear ramp, plus a faint bleed of .21 to 4.8 pt | |
| Angular lobe | `lobe = 1 − |dot(n, perp(L))|` = 1 − |n.x| for a vertical light | `pow(|dot(n, dir)|, 1)` |
| Colour | **recolours** toward `target = 1.6·white + 2.85·(face − lum(face))`: `face = mix(face, target, .252·.6·lobe·profile)`, then clamp. Glass over colour glints in that colour | white × .5, additive |
| Motion | none: iOS 27 dropped tilt-driven highlights (3P). Apple: lights move during lock/unlock (A) | ±15° with tilt (E) |

### 2.7 Contour, inner shadow, drop shadow
- **Contour (iOS 27's "more defined border", 3P):**
  - a band **0.75 pt wide just outside** the silhouette, darkening the backdrop;
  - peak **.43 (light), .88 (dark), .36 (clear)**;
  - directional: strongest where the normal is perpendicular to the light (the side walls), fading where the glint is:
    `alpha = peak·mix(1, 1 − lobe, dir)`, with dir .77 light / 1.0 dark.
- **Inner bevel shadow (3P, optional):** .036 of the transmitted light, 16 pt deep, 6 pt below the top wall. Barely
  visible; skip on the Mali until everything else is right.
- **Drop shadow:**
  - A: adaptive, stronger over text and as content scrolls under; deeper for larger glass.
  - 3P dump: offset (0, 8), blur radius 40, opacity .4 (HDR), **.12 SDR**. It darkens a copy of the backdrop (a
    multiply), so it is invisible over black.
  - **Starbite (SDR panel):**
    - small: offset (0, 4), σ 6, max darkening .10;
    - large (≥ 150): offset (0, 8), σ 14, .14;
    - × detail factor `clamp(0.5 + 4·var(diffuse − sharp), 0.5, 1.5)`;
    - never under the glass itself.
  - iOS 27 leans on the contour more than the shadow: keep the shadow modest.

### 2.8 Accessibility (A behaviour, E mapping)
| Setting (Starbite: Settings > Accessibility > Display) | Glass becomes |
|---|---|
| Reduce Transparency | s forced to 1, blur mix fully diffuse (σ ≥ 20), wash alpha +.15, refraction A × .5, clear behaves as regular |
| Increase Contrast | the face is solid black or white at ~.9, glyphs pure opposite, a 1 pt contrasting border, no glint |
| Reduce Motion | no press scale or stretch, bounce = 0, morphs become cross-fade + materialise, no moving light |
| Reduce Bright Effects (iOS 26.4) | the touch glow is off |

---

## 3. Shapes: continuous corners, concentricity, merging

### 3.1 Shapes (A)
- **Capsule** is the default glass shape (r = h/2).
- **Concentric:** `r = max(r_min, R_display − inset)`.
- Elements near the screen edge on iPhone are capsules with extra margin.
- On the S9, R_display ≈ 32 (unverified, `S9_CORNER_PT`). Clamp at the minimums IOS-REFERENCE gives per component
  (sheet ≥ 26, dock ≥ 24).

### 3.2 Continuous corners (E, exact exponent unknown)
- Apple's corners are squircle-like: the curvature ramps in, with no kink where the arc meets the edge. A kink shows
  as a bright notch in the glint and a jump in the refraction.
- In the SDF, replace the corner's `length(max(q, 0))` with a p-norm (n ≈ 4.5) over a radius scaled ×1.28:
  `cd = pow(pow(qx,n) + pow(qy,n), 1/n)` with `q = |p| − (b − 1.28 r)`.
- On the Mali this needs `exp2/log2` per pixel only in the corner region; cheaper: a 64-entry LUT of the corner curve.
- Use the **same corner function** in tk_raster.c (`tk_sd_rrect4`), the CPU bake and comp2, so masks and glass agree.

### 3.3 Merging (GlassEffectContainer) (A behaviour, 3P maths)
- Shapes in one group begin to merge when closer than the container's **spacing** (Apple's example: **20 pt**).
- Smooth union: `h = clamp(.5 + (b − a)/(2k), 0, 1)`, `d = mix(b, a, h) − k·h·(1 − h)`, with k = spacing.
- iOS 27 matches within 0.5 pt (3P) when k is scaled by how differently the two surfaces face:
  `k_eff = k·min(0.5·|n_a − n_b|, 1)`. This keeps the collinear edges of a row straight while the facing edges
  bridge.
- `glassEffectUnion`: a hard `min` (one shape, no blend).
- One group samples **one** backdrop. Glass never samples other glass (A): nearby glass must be one group.

---

## 4. Blur levels on the Mali (what σ means for us)
- **Sharp level:** our quarter-resolution compose (`BQ`, 4 px per texel = 1.08 pt) bilinear-sampled is already about
  σ 1-1.5 pt. **Keep it** (today it is overwritten by the next level's compose) instead of making a new pass.
- **Diffuse level:** today the dual filter goes 1/4 → 1/8 → 1/16 → back up to 1/8. That is about σ 4-6 pt (E, not
  measured): right for regular glass at s ≈ 0.2 on mid-size elements.
  - Heavy blur (s → 1 at 18.7 pt, the CC scrim at 30-40 pt, Reduce Transparency, sheets) needs one more step down to
    1/32 and up, as a second diffuse level.
  - **Measure σ** with a step-edge test in the GPU self test and log it, so these constants are checked.
- The per-element mix `m` (§2.3) blends sharp and diffuse. Per pixel that is 2 bilinear fetches. Today `bil_bd` does 4
  loads per fetch, so 8 loads per glass pixel instead of 4.

---

## 5. Behaviour

### 5.1 Press (interactive glass) (A behaviour; E numbers)
| Phase | What |
|---|---|
| Touch-down | scale to `1 + min(6/h_pt, 0.15)`: the glass **grows**, it doesn't shrink (iOS 26 system buttons grow). Spring response .25, bounce .2. Glow on: flat +8 % white over the whole shape + a radial white 15 % at the finger, radius 1.5·short, `smoothstep(r, .5r, dist)`. Glint ×1.5 |
| Hold / drag | the glow centre follows the finger (critically damped, response .15). Rubber-band follow: `offset = m·tanh(.05·drag/m)` (m = short) |
| Release | back to 1.0, response .40, bounce .35 (≈ 4 % overshoot); glow fades over .35 s |
| Haptic | light impact on press for buttons that act on release (E) |

Transient glass (A, HIG): **slider and toggle thumbs become a glass lens only while dragged**, larger than the track,
so the track shows through, magnified. This is the "magnifier" preset: band 8, amount 28, dispersion −0.06 (3P).

### 5.2 Materialise / dematerialise (A)
- Glass appears by **ramping the lensing**, not by fading: a visibility `v` 0 → 1 multiplies A, h, the blur mix, the
  tint alpha, the glint and the shadow, while the shape scales 0.8 → 1.0.
- Content alpha follows v over its second half.
- Spring response .35, no bounce. Never a bare alpha fade of a glass layer.

### 5.3 Morph (matched geometry: button → menu / popover / sheet; tab bar minimise)
- The presented panel's start rect is the source button's rect.
- One spring drives x, y, w, h, the corner radius, A, h and the face preset together.
- Content cross-fades in over the second half.
- Springs (E):

| Motion | Response | Bounce |
|---|---|---|
| merge / split | .45 | .25 |
| button → menu | .40 | .20 |
| collapse back | .35 | .10 |
| tab bar minimise | .40 | 0 |
| materialise | .35 | 0 |
| palette flip (cross-fade) | .25 s ease | |

### 5.4 Scroll edge effect (A behaviour; E numbers)
- Where a floating bar overlaps scrolling content there is no hairline. The content **dissolves**: a progressive blur
  plus a fade (iOS 26 "soft"). Over dark content it is a subtle dimming instead.
- iOS 27's default is **"hard"**: a near-opaque band with a 0.5 pt border.
- Starbite default: iOS 26 **soft**.
  - Band = the bar's height + 16 pt.
  - Blur mix ramps 0 → 1 (diffuse σ ~10) from the band's inner end to the screen edge.
  - Plus alpha toward the background, 0 → .6 (light) or a darken 0 → .25 (dark).
- On our compositor this is a lens quad with **no bevel** and a vertical ramp on m and on the tone. It reuses the same
  backdrop.

### 5.5 Layering rules (A)
- Glass is the **navigation layer** floating over content: bars, buttons, CC, sheets, menus, notifications on the lock
  screen.
- **Not in the content layer:** list rows, Settings cards and message bubbles are not glass. The exception is controls
  while they are being manipulated.
- Tint sparingly (primary actions only).
- Never glass on glass: group them.
- Don't mix regular and clear.

---

## 6. Mapping onto our compositor: what exists, what's missing

### 6.1 What exists (read 2026-11-12: starbite-os 1cc8764, sbos-ui-p1 ed8175b)
| Piece | Where | What it does now |
|---|---|---|
| Lens model | `kern/lens.h` (v9/v10) | Size class `k = smoothstep(44, 300, m)`; thickness T = mix(8, 20, k); bezel b = min(r, m/2, mix(12, 22, k)); convex-squircle profile; Snell n 1.5; peak offset normalised to **0.2·b** (≈ 2.5-4.4 pt); top-left light (−1, −1.4) with specular exp(−s/1.2) + Fresnel + depth shade; tone `.82c + mix(.04, .18, light)`; saturation about Y (v10) |
| GPU shader | `tools/mesa-compjob.c` `lens_px` → `kern/gpucomp.h` comp2, flag `GC_LENS` (256) | Per pixel: the circular-corner rounded-box SDF, lens, light, tone over a bilinear fp16 backdrop. **One shape per quad.** Quad block `C2_QB` = 160 bytes; the lens uses +100..+116 |
| Live backdrop | `kern/gpucomp.c` `gpucomp_backdrop` | The quads under the glass at 1/4 → dual filter 1/8 → 1/16 → up to 1/8; **one fixed blur**; 3 levels per frame (`GPUCOMP_LEVELS`) |
| CPU bake / fallback | `ui/tk_glass.c` | The same constants; also the **drop shadow** (y mix(2, 6, k), σ mix(6, 16, k), opacity mix(.10, .18, k)); `dark` = a denser tone under labels; lens only over bitmap backdrops |
| Toolkit glue | `sbtk.h`: `TK_GPU_LENS`, `tk_layer_glass_shape` (≤ 24 shapes per layer), `TK_GLASS_SAT` 1.2 / `_CLEAR` 1.0 | |
| Live GPU glass reaches | lock cards and gauges, home widgets, dock, Search pill, CC tiles, toast. **In-app glass is a flat dark imitation** (UI inventory, 2026-11-12) | |

### 6.2 Gaps (ranked; each row is a self-contained change)
| # | Gap | Today | Target | Change | Owner | Cost on the G72 |
|---|---|---|---|---|---|---|
| G1 | **Lens strength and profile** | peak 0.2·b ≈ 2.5-4.4 pt, Snell on a squircle bezel (12-22 pt) | band h = min(20, .25 short), amount A = min(60, .5 short), quarter-circle `d = A(1 − sqrt(1 − t²))`: the squeezed, mirrored rim | `lens_eval` / `lens_px`: replace the Snell block with the 3-line profile; `lens_params` → (h px, A px). Keep a `LENS_GAIN` constant so it can be ramped 0.33 → 1 while checking on the phone | GPU (lens.h, mesa-compjob.c) + tk_glass.c | cheaper (no refract / sqrt chain) |
| G2 | **Face tone** | `.82c + add` | the §2.4 model (E, T, lift, K) with the light / dark / clear presets and the slider s | `lens_shade` → `lens_face(rgb, preset)`; 6 more floats per quad (preset: E.a, E.grey, T, lift, K, contour peak): grow `C2_QB` to 192 or pack fp16 | GPU + UI | +10 ALU |
| G3 | **Blur: sharp + diffuse mix, per-element σ** | one fixed level | keep the 1/4 compose as "sharp"; add a 1/32 level for heavy blur; per-element mix m | `gpucomp_backdrop` keeps BQ per level (+1.07 MB each); a second fetch in `lens_px` | GPU | +4 loads per glass px |
| G4 | **Glint** | top-left light, cubic lobes + Fresnel all round (reads as an outline) | iOS 27: vertical light, both walls, 1.2 pt ramp + .21 bleed, recolour toward 1.6 white + 2.85 chroma | replace the light block; drop the Fresnel term | GPU + tk_glass.c | same |
| G5 | **Contour** | none | 0.75 pt outside band, .43 / .88 / .36, directional | in comp2: pixels with 0 < sd < 0.75 pt darken the backdrop. Needs the lens quad to extend 1 pt outside the shape | GPU | small |
| G6 | **Light / dark flip with hysteresis + glyph colour** | Lb per bake, no state; glyphs baked into layers | per element Yc from the diffuse level; state with hysteresis; cross-fade .25 s; glyphs recoloured without re-rasterising | glyph layers as A8 masks coloured at compose (`solid` + `mask`); the shell keeps `flip` per glass element | UI + GPU | none per pixel |
| G7 | **Continuous corners** | circular arcs | p-norm n ≈ 4.5, radius × 1.28, the same in tk_raster / bake / comp2 | `lens_sd` + tk_sd_rrect4 + the clip SDF in comp2 | both | small (corner region only) |
| G8 | **Merging (smooth union)** | one shape per GC_LENS quad | up to 4-8 shapes per group quad, smin with angular k | quad block carries N rects (or a group table at +O_C2), loop in `lens_px`; normals by central differences (4 extra evaluations) | GPU | ×N SDF evaluations |
| G9 | **Press / interactive** | toolkit: icons darken and scale to .88 (wrong direction) | grow + glow + glint ×1.5 (§5.1) | uniforms press (x, y, amount) per lens quad; the scale is the layer's | UI + GPU | small |
| G10 | **Materialise** | alpha fades | v ramps A, h, m, glint, shadow; scale .8 → 1 | one float `v` per lens quad | UI + GPU | none |
| G11 | **Morph** | none | rect / radius / preset interpolation on one spring | toolkit: a `tk_morph` that animates a glass shape's rect; menus / popovers / sheets start from their button | UI | none |
| G12 | **Scroll edge effect** | hard-clipped bars or flat dark bars in apps | soft progressive blur + fade under every floating bar | a lens quad flag "edge": no bevel, vertical ramps of m and alpha | GPU + UI | ≈ a lens quad |
| G13 | **Drop shadow on the GPU, adaptive** | baked into the layer, constant | darken the backdrop × detail factor; offset / σ by size | a shadow quad from the same SDF (or baked as now, but × detail from the diffuse level) | GPU | small |
| G14 | **Clear variant + 35 % dim** | `TK_GLASS_SAT_CLEAR` only | the clear preset; dim over bright backdrop | preset id per quad | GPU + UI | none |
| G15 | **Tint** | none | the §2.4 tint for prominent buttons and active CC tiles | tint rgb + alpha per quad | GPU + UI | +4 ALU |
| G16 | **In-app glass is fake** | apps draw a flat dark "glass" | real lens quads for nav circles, tab bars, toolbars, search, sheets, the keyboard panel | UI: app bars as glass shapes in their layer (`tk_layer_glass_shape`) over a live backdrop of the app's content | UI | live backdrop per app frame (already budgeted for the lock screen) |
| G17 | **Accessibility + the slider** | none | Settings > Appearance > Liquid Glass slider; Reduce Transparency / Increase Contrast / Reduce Motion | the shell passes s and flags into the preset tables | UI | none |
| G18 | **Dispersion off** | v8 had dispersion ("refraction, dispersion, rim...") | **0** for regular and clear | make sure nothing adds it | GPU | − |
| G19 | **Light during lock / unlock** | fixed | A: lights travel around the material during lock/unlock: rotate L by up to 40° with the unlock progress | one float per frame | GPU | none |

**Order (most visible per hour):** G1 + G4 + G2 (one shader change: the rim and face will finally read as iOS), then
G16 (in-app glass), G6, G3, G12, G7, then the rest.
**Phone checks per step:** the GPU self test's CPU-vs-GPU compare must still pass. Add a "glass calibration" film
(below).

### 6.3 Calibration plan (needs one attended phone session)
- This S9 also boots **iOS 27**, so we can photograph or screen-capture the same wallpaper with the same glass on both
  systems:
  - iOS 27: Control Centre over the wallpaper, a Settings sub page (back circle and title bar over a scrolled list),
    and the lock screen's notification platter.
  - Starbite: the same three scenes with the same wallpaper image.
- Compare crops at 1:1:
  - the rim band width and how far it mirrors;
  - the face Y over light and dark wallpapers;
  - the glint brightness;
  - the contour.
- Then fix `LENS_GAIN`, s's default and the σ values. Only screen captures or camera photos for study go to
  `build/design-ref/`; nothing from iOS is committed.
- Request goes through the main session (no phone access here).

### 6.4 Budget sanity (E)
- Per glass pixel after G1-G5: 2 bilinear backdrop fetches (8 loads), one SDF evaluation (or N for a group), about 60
  ALU, fp16 colour.
- Only pixels inside glass bins pay. Glass covers < 25 % of the screen in the worst views (CC, keyboard). The comp2 run
  is 572 MHz on 18 cores.
- Keep the big-move half-resolution path as it is.
- If a frame goes over budget, drop the second (diffuse) fetch for small elements first: their m is small anyway.

---

## 7. Sources
- Apple: Newsroom 2025-06-09 (new design); WWDC25-219 "Meet Liquid Glass", 356 "Get to know the new design system",
  323 "Build a SwiftUI app with the new design", 284 "Build a UIKit app with the new design"; WWDC26-269 "What's new in
  SwiftUI"; HIG Materials; docs for SwiftUI `Glass`, `GlassEffectContainer`, `GlassEffectTransition`,
  `glassEffectUnion`, `ScrollEdgeEffectStyle`, `ConcentricRectangle`, `Animation.bouncy`, and UIKit `UIGlassEffect`,
  `UIGlassContainerEffect`.
- Release coverage:
  - iOS 26.1 toggle: MacRumors 2025-10-20;
  - iOS 26.2 lock-clock slider: 9to5Mac 2025-11-04;
  - iOS 26.4 (Reduce Bright Effects, Tinted no-flip): MacRumors 2026-03-18, 9to5Mac 2026-04-10, anderegg.ca
    2026-03-29;
  - iOS 27 slider and defaults: 9to5Mac 2026-06-08, designfornative.com, mjtsai.com 2026-06-09.
- Reverse engineering:
  - ShatteredGlass (github.com/AlexStrNik/ShatteredGlass);
  - flutter_liquid_glass (github.com/whynotmake-it/flutter_liquid_glass);
  - Kyant0 AndroidLiquidGlass (Apache-2.0);
  - kube.io "Liquid Glass in the browser";
  - DnV1eX LiquidGlassKit;
  - naughtyduk liquidGL;
  - rdev / shuding liquid-glass.
- Study images (not committed): `build/design-ref/glass/` (an iOS capture of the private filter at band 20 / amount 60
  showing the mirrored rim; Apple Newsroom images).
