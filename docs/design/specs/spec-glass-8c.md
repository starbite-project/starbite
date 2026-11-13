# Liquid Glass to iOS 27, batch 8c: the remaining gaps, ranked (design-ref, 2026-11-13)

For Track A (GPU lens) and B (toolkit). Baseline: trunk 1c166b85 `kern/lens.h` ("glass V3"). Numbers and formulas are
in [../LIQUID-GLASS.md](../LIQUID-GLASS.md); G numbers refer to its §6.2.

## Already in the trunk (don't redo)
- **G1** band / amount (20 / 60, quarter circle, `LENS_GAIN`).
- **G2** face model with the light / dark / clear presets and `LENS_S` = .2.
- **G4** iOS 27 vertical glint, recolouring.
- **G5** the 0.75 pt contour.
- **G14** clear + 35 % dim.
- **G15** tint.
- The shader's half of press (flat glow .08, glint ×1.5).
- **G18** no dispersion.

## Ranked: what's left, by how visible it is on the phone
| Rank | Gap | Why it's visible now | Change | Where | Size |
|---|---|---|---|---|---|
| 1 | **Calibrate G1 on the phone** before anything else | A = 60 pt is iOS 27's value, but nobody has seen ours on the S9's panel. Too strong reads as a fisheye; too weak and nothing changed | Three camera photos (lock card, dock, a CC tile) at `LENS_GAIN` .33 / .66 / 1.0, plus the same views on iOS 27 on this S9 if the capture session happens (CAPTURE-LIST §G) | lens.h `LENS_GAIN` | S |
| 2 | **G7 corners: continuous shape + smooth normals** | With a 60 pt inward sample, the circular corner's normal jumps where the arc meets the straight edge. Each corner then shows a visible kink, a seam in the mirrored rim. It was invisible at 3 pt and is obvious at 60 | (a) Normals from the same box with the radius × **1.5** (smooth field; the silhouette stays at r). (b) The silhouette as a p-norm corner, n ≈ 4.5 over r × 1.28. Use one shared function in lens.h `lens_sd`, tk_raster `tk_sd_rrect4` and the clip SDF in comp2 | lens.h, mesa-compjob.c, tk_raster.c | M |
| 3 | **G3: sharp + diffuse backdrop, per-element blur** | The mirrored rim needs detail to read as liquid. Today it samples only the 1/8 blur, so the rim shows mush. Also one blur for everything: CC tiles, dock and sheets all get the same frost | Keep the 1/4 compose (`BQ`) as the **sharp** level (≈ σ 1.5 pt; today it is overwritten) and fetch it in the band. The face mixes sharp / diffuse by `m(σ_target)` (LIQUID-GLASS §2.3). Add a 1/32 step for heavy blur (CC scrim, sheets, Reduce Transparency) | gpucomp.c `gpucomp_backdrop` (+1.07 MB per level), lens_px (+1 bilinear) | M |
| 4 | **G16: the remaining flat "glass" in apps** | 18 `d_lg_flat` call sites still draw dark discs and pills with no lens: in-app buttons, fields, confirm card. Next to real glass they look dead | Make each one a lens shape over the app's live content; the toolkit side | UI (shell_*.inc) | M |
| 5 | **G6: flip hysteresis + glyph recolour** | The preset blends between yc .35 and .55 with no state. Over scrolling content, bars and buttons shimmer between light and dark, and their glyphs don't follow | Per-element state (dark below .35, light above .55); cross-fade .25 s; glyphs as A8 masks coloured at compose | UI (state) + lens quad (a preset mix float) | M |
| 6 | **G12: scroll edge as a lens quad** | iOS 26 soft edge (iOS 27 default: hard + 0.5 pt border, see the note) under every floating bar. Today it is per-app fades | A lens quad with no bevel (A = h = 0); vertical ramps of the blur mix and the tone | lens.h flag + comp2 | S-M |
| 7 | **G9: press, the rest** | Glass should grow (1 + min(6/h, .15)) and light from the finger. The shader only has a flat glow | Radial glow at (x, y): 15 % white, r 1.5·short, `smoothstep(r, .5r, d)`. Scale on the layer, spring response .25 / bounce .2 in, .40 / .35 out | lens quad + toolkit | S-M |
| 8 | **G10: materialise** | Glass appears with an alpha fade today; iOS ramps the lensing | One `v` per lens quad multiplies A, h, the glint, the contour and the shadow; scale .8 → 1; spring .35, no bounce | lens.h + toolkit | S |
| 9 | **G13: adaptive shadow on the GPU** | Shadows are baked and constant. iOS darkens more over text. iOS 27 leans on the contour, so this is low | Darken the backdrop by .10-.14 × a detail factor from the variance of diffuse − sharp (needs G3) | comp2 | S |
| 10 | **G8: merging (smooth union)** | Needed for the tab bar's selection and search circle, CC's expand and the keyboard's balloon (shell_kb.inc already does a CPU smin) | N ≤ 4 rects per lens quad; `smin` with angular k (§3.3); normals by central differences | lens quad layout + comp2 | L |
| 11 | **G11: morph** | Button → menu / popover / sheet | Toolkit: one spring drives rect, radius, A, h and the preset | UI | M |
| 12 | **G17: the slider + accessibility** | Settings > Display & Brightness > Liquid Glass; Reduce Transparency / Increase Contrast / Reduce Motion / Reduce Bright Effects | `LENS_S` becomes a runtime value; flags into lens_make | UI + lens.h | S |
| 13 | **G19: light during lock / unlock** | iOS 27 dropped tilt-driven highlights. Keep the fixed light | — | — | skip |

**Note: iOS 27 scroll edge.** iOS 27 changed the default scroll edge to "hard" (a near-opaque band + 0.5 pt border), and
bottom edges are no longer visible by default (3P). With the user's bar now at iOS 27, **use hard at the top, nothing
at the bottom**, unless the captures show otherwise.

**Self-test guard for each step:** the GPU stays within 3 levels of `cpu_px2`. Add a lens calibration still (the three
views above) to the self test so each photo session is reproducible.
