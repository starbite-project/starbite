# Starbite boot splash: the handoff into the v7 intro

The final splash is `build/splash-concepts/final/logo.jpg`. It is 1079 x 1747 px, baseline JPEG, q95 4:4:4, the same format as Samsung's stock file. S-Boot draws it 1:1 at (180, 1144). `tools/brand/splash-final.mjs` renders it and `tools/brand/splash-final-check.py` cleans, encodes and checks it. The approval page is `docs/concepts/splash/starbite-splash.html`. Nothing has been packed or flashed.

## What is on the panel

Two marks on pure black. 99.2 % of the panel stays off.

| band | panel rows | panel cols | intro7 box (padded by 8) | becomes |
|---|---|---|---|---|
| 1 star | 1234..1284 (50) | 938..986 | y 1226..1292, x 930..994 (66 x 64) | the wallpaper star |
| 2 word | 1327..1435 (108) | 478..933 | y 1319..1443, x 470..941 (124 x 471) | the glass capsule, then the island |

- **Word:** "starbite" in Inter Tight 300, 144 px, -0.01 em, #FFFFFF. Its rows are centred on the capsule centre y = 1380.
- **Star:** the wallpaper shader's own core (`|x|^.62 + |y|^.62 <= 1`), R 27, #6FB3FF, with a flat #CFE6FF inner core at 0.3 R. No halo.
  - Its centre in panel pixels is (961.74, 1259.0).
- **Block alignment:** the box starts at panel row 1144, so box row 0 is a JPEG block edge.
  - The star uses box block rows 80..144.
  - The word uses block rows 176..304.
  - Box rows 144..176 are two whole empty 16-row blocks.
- **Black:** every pixel outside the content's blocks decodes to exactly 0,0,0.

### Decode check (intro7's rule: largest channel >= 8, gaps <= 8 rows join)

| encode | decoder | bands | max value outside content blocks | one block further out |
|---|---|---|---|---|
| logo.jpg, PIL q95 4:4:4 | PIL, djpeg | 2, rows as the PNG | 0 | 0 |
| cjpeg q95 4:4:4 | PIL, djpeg | 2, rows as the PNG | 0 | 0 |
| PIL / cjpeg q92 4:2:0 | PIL, djpeg | 2, rows within 3 px | 9 (chroma upsampling bleed) | 0 |
| cjpeg q85 4:2:0 | PIL, djpeg | 2, rows within 7 px | 7 | 0 |

Under every decoder the gap is 42 to 43 black rows, far above the 8-row join. So the splash always gives exactly two bands. The 4:2:0 bleed stays inside the adjacent block and is captured raw with its band, so it never reaches the wallpaper.

## Which band becomes the wordmark

**Band 2, the word.** It is the band that becomes `capt_sam`. Today `capt_sam` is chosen as the band that crosses `I7_NAT_SAM_ROW`. With this splash, that row has to be the capsule centre, 1380.

The word is already where the glass capsule is born. It does not glide or grow. It turns to glass in place, melts into the capsule (700..1050 ms) and rises into the island from 1050 ms.

The star band (band 1) never moves. The wallpaper's star lights on top of it at exactly the same centre and radius, while the captured star fades out over the same 120 ms. Then the star flies home to (1290, 232).

## intro7 / menu7 changes (one commit, together with the flash)

The splash and these constants must land in one commit and one flash. A splash flashed early should never meet an old loader, and the reverse too. The fallbacks are described below.

1. `intro7.h`: `I7_NAT_SAM_ROW 1232` becomes **1380**. `I7_NAT_SAM_MAX_H 160` stays, because the word box is 124 rows tall.
2. `intro7.h`: `I7_NAT_SAM_W 900.0f` becomes **471.0f**, the padded band width.
   - This makes `k = 1`, so the word keeps its size.
   - The glide `p` still eases the band centre (705.5, 1381) to (720, 1380). That is a 14 px settle to the right while "Galaxy S9" would have faded. It is barely visible and centres the word in the capsule.
   - If a strict zero-move is wanted, centre the capsule on the band centre instead.
3. `menu7.h`: `M7_STAR0_X/Y/R` change from (841.85, 2556.15, 13.85) to **(961.74f, 1259.0f, 27.0f)**. These are the splash star's exact centre and R, so `w->sr` starts at the drawn size.
4. `intro7.h`: `I7_FLY_CX/CY` change from (1390, 1520) to **(1350.0f, 1180.0f)**. This gives a short arc up and to the right, from (962, 1259) to (1290, 232).
5. `intro7.c` `capt_items`: the star band crossfades with no rise.
   - Set `capt_star` = the band whose box contains (`M7_STAR0_X`, `M7_STAR0_Y`).
   - Draw it at `dy = 0` with alpha `1 - c01(t / I7_STAR_FADE)`. `I7_STAR_FADE` is 120, the same as `I7_SI_RAMP`, the wallpaper star's light-up.
   - The result is one star that hands over in place, with no pop, no double, and none of the old 36 px rise showing under the new star.

These five changes are a small patch. Verify it with before and after frames from the page (`#t=3560`, `#t=3900`, `#t=4400`) against `tools/menu7/preview` captures.

### Optional, only if cheap on the device

6. **Glass drain.**
   - Today `word.ta` is 0 whenever bands were captured, so the captured word stays white until the melt, then shrinks and fades. That is already clean.
   - The concept page shows the v7 drain instead: white to glass over 220..740 ms.
   - To match it on the device:
     - Give the glass wordmark the splash word's box: 144 px Inter Tight 300, ink box x 479..932, rows 1327..1433.
     - Ramp `word.ta` in as the captured band's alpha goes `1 - ease((t - 220) / 520)`.
   - Skip this if the glass wordmark cannot hit that box exactly.
7. **Reveal from where the star woke.**
   - The page centres WALL_FS's reveal mask on the star's starting point, with one uniform, `uRevC = M7_STAR0`, instead of on the moving star.
   - In `glass_wall.c` this is one extra distance per pixel, only on non-final intro frames.
   - Keep it only if the NEON path does not need a second radial table. Otherwise the reveal around the moving star is fine.

No glow is added to the star. If a glow is ever added, use a +/-1 level dither and set anything below 1 level to 0. Keep the halo under 40 px so the star band stays short.

## Fallbacks (why both must land together)

- **New splash, today's loader.**
  - Neither band crosses row 1232, so there is no `capt_sam`. Both bands are above `I7_NAT_SPLIT` (1973), so both fade over 260 ms and rise 36 px, like the old "Galaxy S9" line.
  - Meanwhile the wallpaper star lights at the old (842, 2556) and flies home, and the capsule is born empty at (720, 1380).
  - It is calm and nothing breaks, but the star seems to jump. Acceptable as a fallback, not as the design.
- **Old splash, new loader.**
  - The "Powered by starbite" splash has no band across 1380 that is at most 160 rows tall, so there is no `capt_sam` and its bands fade out as above.
  - The wallpaper star starts at (962, 1259), between SAMSUNG and the old star. Also calm, not designed.

## Flash path (not done here)

1. Pack: `tools/brand/upparam-pack.py`, with the new `logo.jpg` in place of the current one.
2. Flash: `tools/brand/upparam-flash.sh`. It checks the known current image and reads the write back.
3. Flash the loader with the five constants in the same session.
