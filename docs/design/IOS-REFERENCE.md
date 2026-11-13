# iOS 26 / 27 design reference for Starbite OS (sbtk)

Track C, design-ref session. v1.1 2026-11-12: §2 app switcher, §5 notifications, §9 lock / home / widgets and §11
island sizes are now measured (Apple iPhone User Guide iOS 26 + 27 art, Newsroom WWDC25 full-resolution renders). Owner of `docs/design/`. The UI session (sbos-ui-p1) builds from this file.
Companion files: [LIQUID-GLASS.md](LIQUID-GLASS.md) (the material, mapped onto our GPU compositor) and
[GAP-LIST.md](GAP-LIST.md) (what to change in Starbite OS Next, ranked).

## 0. How to read this file

### Tags on every number
- **A**: Apple-stated (HIG text or table, API documentation, WWDC).
- **M**: measured by us from an image of known resolution. Most come from Apple's own iOS 26 HIG artwork and the Apple
  iPhone User Guide's drawings; some come from MacStories review captures. The method is noted where it matters.
- **3P**: a third party's published value (named).
- **E**: an estimate (iOS 17/18 conventions plus the visible iOS 26 change). Verify before you rely on it.

### Units: the S9 frame
- The S9 panel is 1440 x 2960 px and Starbite's logical canvas is **390 x 802 pt**. **1 pt = 3.6923 px** (`TK_PT`,
  `PT()`). Every value here is in pt. Where px are given they are `pt x 3.6923`, rounded.
- The reference phone is the 390/393-pt-wide iPhone class (iPhone 14/15/16, iOS 26). Measurements taken on a 393 or 402
  pt phone are converted as follows:
  - **Fixed chrome is NOT scaled.** Alert width, button and bar heights, radii, glyph sizes, toggles and font sizes
    stay in pt.
  - **Width-filling layouts scale by 390/393.** Sheets, the tab-bar capsule, keyboard keys and grids lose the
    difference in their side margin or gap.
- **The S9 is 50 pt shorter than an iPhone 16 (802 vs 852).** Spend the difference at the top. The S9 has no island,
  so the status band can be 44 pt instead of 54. Take the rest out of free space (list heights, the lock screen's
  middle), never out of chrome.

| Frame constant (S9) | Value | iOS 26 equivalent | Note |
|---|---|---|---|
| Canvas | 390 x 802 pt | 393 x 852 (iPhone 16) | |
| `SAFE_TOP` (status band) | 44 | 54 (island phones), 47 (notch) | Already in shell_lock.c. Keep. |
| Status text centre | y 30 (`SB_MID`) | y ~31 (island side columns) | |
| Bottom safe area / home indicator band | **30** (E) | 34 (A) | The home indicator bar is 134 x 5 pt, r 2.5, centred 8 pt above the bottom (E, long-stable). |
| Content margin | 20 (`MARGIN`) | 16 (lists), 20 (large title, cards) | iOS uses 16 inside lists and 20 for large titles. Keep 20 for page content. Lists: see §6. |
| Display corner radius | **32** (`S9_CORNER_PT`, unverified) | ~55 (iPhone 16) | Concentric radii are computed from **our** corner: `r_child = 32 - inset`. iOS's look relies on big radii, so where a rule gives less than the iOS value, clamp to the minimum given per component (`CONCENTRIC()` clamps at 24). |

### Multi-touch (2026-11-13)
The trunk delivers every finger:
- finger 0 through `sh_touch`, as before;
- fingers 1.. through `sh_touch_finger(finger, ph, x, y, t_ms)` (shell.h).

So every iOS pinch, two-finger rotate and two-finger tap in these specs is a **requirement**, not a later item:
Photos grid and viewer, Camera zoom, Maps, Files previews.

### Dark and light
iOS 26 is light by default, with a system Dark appearance. Starbite OS Next is dark-first today (wallpaper and glass).
Every colour table gives both. Build dark first, then light. Glass flips per element by what is behind it (LIQUID-GLASS §3),
**independently** of the system appearance.

---

## 1. Control Centre  (priority 1 from sbos-ui-p1)

Source: Apple iPhone User Guide drawings of the iOS 26 Control Centre on an iPhone 16 (393 pt), measured with PIL at
1.1345 px/pt, precision ±0.7 pt (M). Module sizes are fixed in pt (E): on 390 pt keep the module size and lose 1.5 pt
of side margin.

### Grid (closed modules)
| Item | iOS 26 @393 (M) | **Starbite (390 x 802)** | px |
|---|---|---|---|
| 1x1 module (circle) | 68.7 dia | **U = 68** | 251 |
| Gap (h and v) | 14.7 | **G = 14.5** | 54 |
| Pitch | 83.4 | **82.5** | 305 |
| 4-col grid width | 318.7 | 4U + 3G = **315.5** | 1165 |
| Side margin | 37.0 | **37.25** (x0) | 138 |
| 2x2 module | 152 x 152, r **~30** (continuous) | **150.5 x 150.5, r 30** | 556, r 111 |
| 2x1 wide (Focus) | 152 x 68.7 capsule | **150.5 x 68, r 34** | |
| 1x2 tall slider | 68.7 x 152.5 capsule | **68 x 150.5, r 34** | |
| 1x1 radius | full circle | **34** | |
| "+" (top-left) and Power (top-right) | iOS 27: ~28 pt glass circles, centre y 36, outer edges on the grid margins (iOS 26: bare glyphs at y ~40) | **28 dia, centre y 30**, outer edges x 37.25 / 352.75 | |
| In-panel status row | centre y ~100 (Wi-Fi left, "100%" + battery right) | **centre y 92**: no island, so the band moves up 8 | |
| First module row top | 127.4 | **119** | |
| Rows | 127.4, 211, 294, 377.7, 461.4, 544.3 ... | **119 + 82.5 n** (119, 201.5, 284, 366.5, 449, 531.5, 614) | 7 rows fit above 700 |
| Page jump icons (favourites / music / connectivity) | ~14 pt glyphs, x ~375, ~53 pt apart, centred on the screen middle | **14 pt glyphs at x 376, y 348 / 401 / 454** | |

Default page 1 (M, iOS 26):
- row 1: connectivity 2x2 | Now Playing 2x2;
- row 2: Orientation Lock, Silent, then the tops of two tall sliders (Brightness, Volume);
- row 3: Focus 2x1 | the slider bottoms;
- row 4: Flashlight, Timer, Calculator, Camera;
- row 5: Screen Mirroring, Code Scanner (for us: Survival, Wi-Fi Calling, etc.).

### Connectivity 2x2 (closed) (M)
- Four slots: Airplane (top-left), AirDrop (top-right; ours: **Hotspot** or **Survival**), Wi-Fi (bottom-left), and a
  2x2 cluster of mini circles in the bottom-right slot (Cellular, Bluetooth, VPN, Satellite; ours: Cellular,
  Bluetooth, Torch, Rotation lock).
- Big circles **55.6** dia, inner padding 13.5-14, gap between big circles **14**. Mini circles **24.7** dia, gaps
  5.5 h / 6.2 v.
- Glyphs: big ~23 x 13 pt (Airplane), about SF Symbol 20-22 pt; mini 10-12 pt.
- On colours: Wi-Fi, Bluetooth and AirDrop use **systemBlue** (#0088FF light / #0091FF dark); Cellular **systemGreen**;
  Airplane **systemOrange**. Off = plain glass (white ~25-30 % over the blur).

### Expanded connectivity (long-press the 2x2) (E)
- The 2x2 **morphs** (glass grows from the module's frame) into a centred panel ~318 wide.
- Three rows of 62-68 pt circles: Airplane, AirDrop, Cellular / Wi-Fi, Bluetooth, Hotspot / VPN, Satellite. Each has a
  13 pt / 600 label and a 12 pt secondary state line ("Off", the network name) underneath.
- The rest of the grid blurs and dims further: scrim +15 %, extra blur.
- Close: tap outside, then it morphs back into the 2x2 frame.
- Long-press haptic: medium impact at commit.
- Spring: response 0.40, damping 0.80 for the morph; contents cross-fade over the last 60 %.
- Our version: build it at the CC grid's open (not in the tap), so the long-press shows it at once.

### Now Playing 2x2 (M)
- Artwork 52 x 52 top-left at inset 13.5; AirPlay 38 pt circle top-right.
- Title 15 pt / 600, white, x inset 16.8; second line 15 pt / 400 at white 60 %.
- Transport row centre at module bottom − 25: glyphs ~20-23 pt wide, centres 42 apart.

### Sliders (tall 1x2) (M)
- The capsule's empty part is glass. The fill rises from the bottom in **solid white** (#FFFFFF, no glass) and is
  clipped to the capsule.
- Glyph (sun / speaker) ~25 pt wide, centre 22 pt above the slider bottom, drawn in the fill's colour contrast: dark
  grey on white fill, white on glass.
- Dragging: the whole module scales to **1.04-1.06** (E) on a gel spring. Selection haptic at every 1/16 step (E) and a
  rigid tick at 0 and 100 %.
- Long-press expands into a full-height single slider (E): 1x2 → ~150 x 400 centred, same morph spring.

### Material and motion
- Behind the panel the whole screen is blurred with a heavy gaussian, sigma ~30-40 pt (M estimate), and darkened 25-35 %
  (dark appearance; E).
- Modules are **regular Liquid Glass**: lensing rim, top-left specular, no stroke.
- Active state: the glass is **tinted** with the function colour (orientation lock green, flashlight / calculator red-
  pink, camera blue, Focus indigo glyph).
- Open: swipe down from the top-right (x > 2/3 of the width). The blur ramps with the finger.
- Modules fly in **staggered by row**, top rows first, ~20 ms apart (E), each from scale 0.9 and 30 pt higher on a
  spring with response 0.40, damping 0.80 (E).
- Close: swipe up, or tap empty space; the same in reverse, faster (response 0.32).
- Haptics: light impact when the panel commits open.
- Edit mode (long-press empty space): grid top 79, unit ~75.3, gap 8 (M). The "-" badge is a 16 pt circle; the resize
  handle is a curved arc at bottom-right; empty cells show as faint glass circles; 4 x 8 cells per page.

---

## 2. App switcher  (priority 2)

Source: Apple iPhone User Guide "Switch between open apps", iOS 26 and iOS 27 editions (same geometry within 3 pt), M.
Scales are ratios, so they transfer to the S9 directly. x values are @390.

| Item | iOS 26 (M) | **Starbite (390 x 802)** |
|---|---|---|
| Focused card scale | **0.690** | 0.69: **269 x 553** |
| Card behind (left) scale | 0.648 | 0.65 |
| Card in front (right) scale | 0.700 | 0.70 |
| Card vertical centre | 0.488 x screen height (rides ~11 pt high) | **y 391** |
| Focused card left edge | 88.4 | **x 60.5** (centred: (390 − 269)/2) |
| Behind card peek (to the left of the focused card) | **70** | 70 |
| Next card (right) left edge | focused left + **209** (= 0.78 card width): it covers the focused card's right ~60 pt | focused left + 209 |
| Stacking | cards further right sit **on top** (newest on the right, older under to the left) | |
| Card radius | **~40** = display radius (~57) x scale | the rule gives 32 x .69 = 22 on the S9; use **30** (E) so it reads as iOS |
| Shadow | none on light backdrops; on dark, ~20 blur, black 15-20 % (E) | |
| Title row | app icon **25.5**, left = card left + 24, bottom 7 above the card top. Only the **focused** card shows its name: 17 / 600 white, 8 right of the icon | same |
| Backdrop | home + wallpaper heavily blurred, dimmed ~30 % | blur + dim .30 |

- **Enter:** in an app, swipe up from the home indicator and **pause** mid-screen (~1/3 height). The app shrinks with
  the finger into a card and the neighbours slide in from the left. Light haptic when the switcher commits (E). Our
  threshold today: < 0.3 pt/ms for 180 ms; keep it.
- **Browse:** a horizontal pan with rubber band at the ends. Release decelerates and snaps a card into focus (spring
  response ~0.40). Cards change scale continuously with x: interpolate .648 / .690 / .700 by position.
- **Close:** fling a card up; it leaves with the release velocity and fades. The others close the gap (spring 0.35 /
  0.85, E).
- **Open:** tap a card; it scales .69 → 1 and its radius 30 → the display's (spring 0.40 / 0.90, E).
- **Quick switch:** a horizontal swipe along the home indicator slides straight to the previous app. Full-screen cards
  with a ~10-12 pt gap (E).

---

## 3. Alerts, action sheets, menus, sheets  (priority 3)

Source: Apple HIG iOS 26 artwork (536 x 1162 px renders = 402-pt phone, 0.75 pt/px), M. These components are
**fixed-size**: use the pt values as they are on 390.

### 3.1 Alert (M)
| Item | Value | px |
|---|---|---|
| Width | **300** (iOS ≤ 18: 270) | 1108 |
| Corner radius | **36** (continuous; circle fit 34-36) | 133 |
| Text inset (left / right) | **24** | 89 |
| Title | **left-aligned** (new in 26), 17 / 600 (Headline), label colour | |
| Message | 15 / 400, secondaryLabel; title→message baseline pitch ~32 incl. the paragraph gap | |
| Message → buttons | 28 | |
| Buttons | **capsules, 48 tall**, 2 actions side by side at **132 wide, 8 gap**; 3+ actions stacked full width (300 − 28 = 272) with 8 gaps | 177 h |
| Button inset from the alert edge | **14** (left, right, bottom) | 52 |
| Secondary button fill | light **#DADADC** on body #EBEBEC; dark **#363639** on body #1A1A1A-#232323 | |
| Primary (default) | filled systemBlue (#0088FF / #0091FF), label 17 / 600 white | |
| Destructive | label systemRed (#FF383C / #FF4245); filled red if it is the prominent action | |
| Body | Liquid Glass, nearly opaque: light #EBEBEC → #F6F6F7 with a 1 px white specular rim; dark #1A1A1A → #232323 with a grey rim | |
| Backdrop dim | light: screen to ~#CDCDD1 (≈ black 20 %); dark: to ~#0A0A0A (≈ black 50-60 %) | |
| Shadow | sigma ~20-30, y +8, black 18 % (E) | |
| Appear | from scale **1.10 → 1.0** + fade 0 → 1; spring response 0.35, damping 0.85 (E). Disappear: fade only, 0.2 s ease-out. Haptic: none (warning notification for errors). | |

### 3.2 Action sheet / confirmation dialog (M)
- **With a source button** (iOS 26): a glass **popover anchored to the button**, morphing out of it.
  - Panel ~**246 wide**, at least 10 from the screen edge, radius **26-28**; body glass #EFEFF0 (light) with a white rim.
  - Rows are **capsule buttons inside the panel**: ~40 tall with 8 gaps (pitch 48), inset 10-11 from the panel edge,
    fill #E3E3E4 (light) / #2C2C2E (dark, E).
  - Labels centred 17 / 400; destructive red. No Cancel row (tap outside cancels).
- **Without a source**: bottom sheet geometry (3.4), inset 6 pt, concentric bottom corners, the same capsule rows.

### 3.3 Menus (context / pull-down) (E, the same component family as 3.2)
- Glass panel ~250 wide, radius 26-34; rows 44-48 tall with a 17 / 400 label and a trailing 20 pt SF-Symbol-style
  glyph; section gaps 8 (a thicker 8 pt band in a darker fill, not a hairline).
- **Morph:** the source button's glass shape grows into the menu (LIQUID-GLASS §5.3) and shrinks back on dismiss.
- Context menu (long-press): the content lifts (scale 1.03, shadow up), the rest blurs, the menu grows from the touch
  side. Haptic: **medium impact** at the lift.

### 3.4 Sheets with detents (M)
| Item | Medium detent | Large detent |
|---|---|---|
| Side inset | **6** | 0 |
| Bottom inset | **6** (floating) | 0 |
| Top edge | ~52 % of the screen height: on S9 **y = 802 − 6 − 417 = 379** (height 417) | **y 61** on iPhone; on S9 **y = SAFE_TOP + 8 = 52** |
| Top corner radius | **33** (continuous) | 33-38 |
| Bottom corners | concentric with the display: S9 32 − 6 = 26 → clamp **≥ 26** | screen corner |
| Grabber | **36 x 5** capsule, top 5 below the sheet edge, light #C0C0C0 / dark #5A5A5E (tertiaryLabel) | same |
| Close button | **44 glass circle**, "xmark" 17 pt / 600, 16 from the top and leading edges; Done/confirm trailing in the same row (prominent = blue glass) | same |
| Material | **Liquid Glass** (translucent, inset) | becomes **opaque**: systemBackground / grouped background |
| Behind | barely dimmed (≈ black 10 %) | the presenting view scales to ~0.92, its top corners round, dims ≈ black 25 % (E) |

- Present: slide up on a spring with response 0.45, damping 1.0 (E).
- Detent change follows the finger; on release, a spring with response 0.40, damping 0.85 (slight overshoot when flung).
- **Soft** haptic when the sheet lands on a detent (E).

---

## 4. Keyboard  (priority 4)

Source: MacStories iOS 18 vs 26 capture, iPhone 16 Pro Max (440 pt), 1.611 px/pt, ±1 pt (M). Values scaled to 390
where they fill the width.

| Item | iOS 26 @440 (M) | **Starbite @390** | px |
|---|---|---|---|
| Panel | full width, glass, **top corners ~30**; light #E1E2E5, dark ~#2B2B2E (E) | same; side inset 0 (the panel reaches the curved edges; keys are inset) | |
| Side margin to the first key | 8 | **4** | 15 |
| Letter key | **36 x 45** | **31.6 x 44** (10 keys + 9 gaps = 382) | 117 x 162 |
| Horizontal gap | 6.8 | **6** | 22 |
| Row pitch | 56.5 (vertical gap ~12) | **54** (gap 10): the S9 is shorter | 199 |
| Key radius | **~8** (iOS 18: ~5) | **8** | 30 |
| Key fill | **all keys white #FEFFFF**: shift, delete, 123 and return are the same colour (new in 26) | light #FFFFFF; dark #6B6B6F (E), on panel #2B2B2E | |
| Key shadow | none visible in 26 (iOS 18 had a 1 pt bottom shadow) | none | |
| Letters | ~22-23 / 400 caps, black / white | **22 / 400** | |
| Row 2 (A-L) | indented by half a key + half a gap | x0 = 4 + 18.8 | |
| Row 3 | shift and delete ~44-50 wide, 7 letters centred | shift / delete **44**, letters as row 1, centred | |
| Bottom row | **123 ~101, space ~209, return ~101**, gaps 6.8; return shows the **⏎ glyph**, no word | **123 = 88, space = 194, return = 88** (gaps 6) | |
| Below the keys | emoji (left) and dictation mic (right) glyphs on the panel's bottom strip, ~40 above the home indicator | emoji / globe 24 pt glyph at x 24, mic at x 366, centred 22 above the bottom safe band | |
| Predictive bar | ~44 strip above the keys, 3 suggestions split by 1 pt hairlines at 30 % | 40 | |
| Total height | ≈ 345-350 incl. the home-indicator band | **≈ 306**: 40 predictive + 4 x 54 rows + 50 bottom strip | 1130 |
| Pop-up balloon (E) | ~1.4x key width, rises one key height, same radius, white, letter 32 / 400 | 44 x 96 combined shape (key + balloon), letter 32 | |
| Feedback | key click + **light** haptic on key down (Settings > Sounds & Haptics > Keyboard Feedback) | light haptic on key down (already in P3h) | |
| Motion | show/hide: 0.25 s with curve 7 (≈ spring response 0.35-0.40, damping 1.0) | spring 0.38 / 1.0 | |

---

## 5. Notification Centre and banners  (priority 5)

Source: Apple Newsroom WWDC25 iOS 26 hero (a straight-on iPhone 16 Pro lock screen, 1.972 px/pt, ±0.5 pt), M.
Insets are fixed pt; width = screen − 2 x inset.

### Platter
| Item | iOS 26 (M) | **Starbite** |
|---|---|---|
| Side inset | **14.5** | 14.5: x 14.5, **w 361** |
| Corner radius | **~23** (circle fit 22-24; continuous) | 23 |
| Gap between platters | **8** | 8 |
| Height | **24.6 + 18 x text lines** (first cap top 15 below the platter top) | same |
| App icon / avatar | **38 x 38**, inset 13.7 from the left, **vertically centred**. A message's sending app is a ~17 pt badge at the avatar's bottom-right | same |
| Text column | starts **65** from the platter edge | x 79.5 |
| Title / sender | **15 / 600**, white | |
| Body | **15 / 400**, white 90-95 %; line height **18**; lock screen truncates after **4** lines | |
| Timestamp | 13-15 / 400 at white 60-70 %, right inset **18.3**, on the title's baseline | |
| Material | regular glass; over a mid-grey sky it reads as a slight darkening (≈ black .12-.20 over the blurred wallpaper) with a 1 pt rim brighter at the top | lens glass (G16) |

### Stacks, list, Notification Centre
- **Display as:** Count ("N Notifications" pill), **Stack** (default: grouped per app, newest on top) or List (A).
- Notifications collect at the **bottom** of the lock screen. The stack's bottom is **20.5** above the flashlight /
  camera row.
- **Collapsed group (E, not re-measured for 26):**
  - the 2nd card is scale .95, visible 8-9 below the front, opacity .7;
  - the 3rd is .90, a further 7-8, opacity .5.
  - Matches PEEK1 / PEEK2.
- **Expanded group:** a header row with the app name 20 / 700 white on the left; "Show Less" and "x" glass buttons on
  the right; cards 8 apart.
- **Clear:** an "x" glass circle (~31) next to the "Notification Centre" header; press-and-hold gives "Clear All". A
  left swipe on a platter shows **Options / Clear** glass buttons (56 wide, scale in from .8); a long swipe clears.
- **Notification Centre** = the lock-screen surface:
  - Pull down from the top-left or centre of any screen (top-right is CC).
  - The whole sheet follows the finger. The blur and dim ramp to full over the first ~25 % of travel; release past
    ~1/3 commits (spring ~0.40, E).
  - Clock, date and widgets at the top. Older history appears when scrolling up past the latest.

### Banners (E unless stated)
- The same platter, max 2 body lines.
- On island iPhones it drops out of the island and sits **11** from the screen top, covering the status bar. On notch
  phones it sits ~8 below the status bar.
- **Starbite:** top **y 10** (it covers the status band; with the live pill of §11 it drops out of the pill).
- In: from behind the island / pill, slide plus scale down, spring ~0.50 with slight overshoot.
- Auto-dismiss after ~5-6 s, sliding back up. Swipe up dismisses; pull down expands; tap opens.
- Haptic: the notification's own pattern only (no extra UI haptic). Persistent banners stay until dismissed.

---

## 6. Settings  (priority 6)

Source: HIG iOS 26 list artwork (@2x) and MacStories iOS 26 Settings captures (M); HIG text for search (A).

| Item | iOS 26 | Starbite (390) |
|---|---|---|
| Large title | "Settings" 34 / 700, tracking +0.40, left at the content margin, baseline SAFE_TOP + 44 + 36 (`LARGE_BASE`) (A) | as is |
| Search | at the **bottom**, in a toolbar: Settings is Apple's own example where search is the only item (A) | a 48 glass capsule, x 20..370, bottom 30 + 8 above the screen bottom; tap: it rises above the keyboard (spring 0.38 / 1.0) |
| Background (grouped) | light **#F2F2F7**; dark **#000000** | |
| Section card | light #FFFFFF; dark **#1C1C1E** | |
| Card radius | **25-26** (iOS 18: 10) (M) | **26** |
| Card side inset | 16-20 (E) | **16** (x 16..374), so 358 wide |
| Row height | **52** single line (iOS 18: 44) (M) | 52 |
| Row with subtitle | 64-68 (E) | 66 |
| Icon square | **30**, continuous radius **7-8**, white 18-20 pt glyph, at x card + 16 (E) | 30, r 7.5 |
| Icon → text | 12-14 (E) | text at card + 16 + 30 + 13 = card + 59 |
| Row text inset (no icon) | 16 (M) | 16 |
| Separator | 0.5 pt, **#C6C6C8** light / rgba(84,84,88,.60) dark; starts at the text's x and runs to card right − 0 (E: iOS 26 runs to the edge) | 0.5 pt (2 px) |
| Label / value / chevron | 17 / 400 label; 17 / 400 secondaryLabel value; chevron 13 / 600 tertiaryLabel (~8 x 13 glyph), trailing inset 16 (A/E) | |
| Section header | **sentence case, 15 / 600** (iOS 26; ≤ 18 was 13 caps) (E), 8 above the card, x at the card's text inset | |
| Section footer | 13 / 400 secondaryLabel, 8 below the card | |
| Section gap | 35 between cards (header included) (E) | |
| Account row (top) | 64-68 tall, 56-60 circular avatar (E) | "Starbite" row: 66 tall with the device glyph |
| Pushed page | inline title 17 / 600 centred on `NAV_MID`; back = a **44 glass circle with the chevron only** (no text) at x 20 | `BACK_X 20, BACK_D 44` (already in code) |
| Toggle in a row | see §12.4 | |

---

## 7. Messages  (priority 7)

> E: no raw iOS 26 Messages capture was measured yet. The bubble geometry is unchanged since iOS 17 apart from the
> glass chrome.

| Item | Value |
|---|---|
| Bubble text | 17 / 400; white on blue (sent), label on grey (received) |
| Bubble padding | 12 h, 7-8 v; a one-line bubble ≈ 36 tall |
| Radius | **18** (a one-line bubble is a near-capsule) |
| Max width | ~75 % of the width: **292** on 390 |
| Side margin | bubble edge **16** from the screen edge; the tail reaches 5 pt further out |
| Tail | only on the **last** bubble of a run: a curved hook at the bottom-outer corner, ~10 x 16 |
| Spacing | same sender **2**, sender change **10**, timestamp header 11 / 600 + 11 / 400 secondaryLabel centred with 16 above and below |
| Colours | iMessage = systemBlue **#0088FF / #0091FF**; SMS = systemGreen **#34C759 / #30D158** (we are SMS: **green**); received = systemGray5 **#E9E9EB / #262628** |
| "Delivered" | 11 / 600 secondaryLabel, right-aligned under the last sent bubble |
| Input bar (iOS 26) | a "+" **glass circle 36** at x 16; a glass capsule field 36 tall from x 60 to 374, placeholder "Text Message" 17 / 400 at tertiaryLabel. It floats above the content with the scroll-edge blur and sits 8 above the keyboard or the bottom safe band. Send = a 30 blue circle with a white up-arrow inside the field's right end, shown only when there is text |
| Nav | glass back circle 44 (x 20); avatar 40 + name 11 / 400 centred in a glass capsule; video circle at the right |
| Motion | a sent bubble rises from the field: scale 0.85 → 1, spring response 0.35, damping 0.75 |
| Haptic | none on send (E) |

---

## 8. Photos  (priority 8)

- Tabs (iOS 26): **Library** and **Collections** in a floating glass capsule, plus a separate search circle (A, 3P
  MacStories). Library view has a "Years / Months / All" segmented glass capsule floating above the tab bar (M visual).
- Grid: 3 columns by default (pinch 1-5 / 13), **square tiles, 1-2 pt gutters** (E; iOS 18 was 0.67 pt). On 390 with
  1.5 gutters: tile **129**. Newest at the bottom; the grid starts scrolled to the end.
  - No large title in Library: a 22 / 700 "Library" or the date range at top-left over the scroll-edge blur, plus a
    "Select" glass capsule at top-right.
- Viewer: full-bleed photo on black (dark) / white (light); tap toggles the chrome.
  - Top: back glass circle 44, date/time centred 15 / 600 + 13 / 400.
  - Bottom: share, heart, info and delete as **44 glass circles**, or one grouped capsule.
  - Open: the tile **zooms** into the viewer (matched geometry, spring response 0.40, damping 0.90).
  - Swipe down shrinks the photo back into its tile (follows the finger; background alpha by distance).
- Haptic: none.

---

## 9. Lock screen, home screen, dock, widgets

### 9.1 Lock screen
Source: Newsroom WWDC25 hero (iPhone 16 Pro, 402 x 874, 1.972 px/pt), M; MacStories edit-mode captures (±3); HIG
widget sizes (A). For the S9 the vertical positions move up ~10 pt (no island: a 44 status band instead of 54-62).

| Element | iOS 26 @402 (M unless noted) | **Starbite (390 x 802)** |
|---|---|---|
| Status bar on the lock | no time (the big clock replaces it); right cluster centre y 31 | as today (y 30) |
| CC grabber under the right cluster | ~47 x 2.5 bar at y 48, white 60 % | 47 x 2.5 at x 300..347, y 40 |
| Date | **22 / 600** white, centred, centre y 92 (e.g. "Tue Apr 1") | 22 / 600 centred, **centre y 82** (today 19 / 600 at baseline 96) |
| Clock, default | digit height **~83** (≈ a 117 pt font at SF's cap ratio .71), centred, digits top ~y 120 | Display 700 → **600**, size **116**, digits top **y 108** |
| Clock, stretched | user-resizable up to ~345 pt digits; hero mid-stretch: 188 pt digits | later (GAP #30) |
| Clock material | iOS 26 "Glass" style: clear Liquid Glass digits, refraction and specular edges; the iOS 26.2 slider sets their glass intensity (A behaviour) | later: lens text mask (GAP #30); solid white tonight |
| Widgets row | under the clock; with a tall clock they move above the buttons. Circular **72 x 72**, rectangular **160 x 72**, inline 234 x 26 (A, 390/393 class) | row top **y 214** |
| Notifications | bottom-anchored; their bottom **20.5** above the button row; §5 platters | bottom at **y 677** |
| Flashlight / camera | **50.2** glass circles at **71** from each side, centre **75 above the bottom** in the WWDC hero (Apple's later art shows ~58: unresolved) | 50 circles, centres **x 71 / 319, y 727** (today HOLD 47 / 293 + 25 → 72 / 318, y 720: move 7 down) |
| Home indicator | **143.7 x 5**, 8.2 above the bottom (on 402); ~134 on 390 class | 134 x 5 at y 789 (today: OK) |
| Live Activity on the lock | screen − 2 x 14.5 wide, 84-160 tall, inner margin **14**, radius ~23 (A + M) | §11 |

- **Unlock (E):** the wallpaper zooms out slightly; clock and widgets fade up; home icons fly in from the edges, ~0.45 s.
- **Wake:** a fade from black, ~0.25 s.
- **Flashlight / camera:** press-and-hold with a **medium** haptic on activation (A behaviour).
- **iOS 27:** an optional compact clock in the top row with the widgets; Now Playing can be cleared.

### 9.2 Home screen
Source: Newsroom "Home Screen customization" image (three straight-on iPhone 16 Pro, 2.0605 px/pt, ±0.5), M; @390 =
x 0.970.

| Element | iOS 26 @402 (M) | @390 | **Starbite** |
|---|---|---|---|
| App icon | **63.5** (squircle; circle-fit radius 16.8) | **61.6** | keep **62** (`ICON_PT`) |
| Columns | 4; left edges 30.7 / 123 / 215.7 / 307.4: **pitch 92.3**, margin 30.6 | pitch 89.5, margin 29.7 → centres 60.5 / 150 / 239.5 / 329 | `COL_CX` 60 + 90 c (OK) |
| Rows | 6 per page, **pitch 100.0** | 100 | `ROW_PITCH` 98 → **100** if 6 rows fit; on 802 tall keep 98 (E) |
| Labels | 12 / 400 white, cap top ~10 below the icon, soft shadow on light wallpapers; hidden in "Large" icon mode | | 12 / **400** (today IT500) |
| Small widgets | **164 x 164** at y 90, gap 20.9, radius **25.7** | HIG: **158** on 390/393 (A); radius 24.8 | `WID_W` 158 (OK); radius **25** (today 24) |
| Search pill | glass capsule **~78 x 23**, centre y 719 (= 155 above the bottom); it turns into page dots while paging | | **78 x 24**, centre **y 647** (today 30 tall at y 640) |
| Dock | glass platter, sides inset ~10-12, top 773.6 (100 above the bottom), **88 tall**, radius ~36-40 (E). 4 icons at pitch 89.4, icon top 12 below the platter top | | x 11..379, **top 702, h 88**, radius **max(32 − 11, 24) = 24**; icons at y 714 (today: dock 688 h 96, icons 705) |
| Icon styles | Default, Dark, Clear light / dark, Tinted light / dark (A); Small / Large icon size | | Default + Dark tonight |

- **Paging:** 1:1 drag, then a spring of ~0.35 s.
- **Jiggle (E):** ±1.5° at ~7 Hz.
- **Haptics:** medium impact for the long-press menu, light on drop.
- **iOS 27:** extra-large widgets for Apple apps, sharper icons.

### 9.3 Widgets (A, HIG table)
| Screen (pt) | Small | Medium | Large | Circular | Rectangular | Inline |
|---|---|---|---|---|---|---|
| **390/393 class (ours)** | **158 x 158** | **338 x 158** | **338 x 354** | **72** | **160 x 72** | **234 x 26** |
| 402 (16/17 Pro), M | 164 | ~349 x 164 | ~349 x 367 | | | |
| 430 class | 170 | 364 x 170 | 364 x 382 | 76 | 172 x 76 | 257 x 26 |

- Content margins **16** (standard) / **11** (tight); text ≥ 11 pt.
- Radius ~25 for a small widget, continuous. Inner elements are concentric (ContainerRelativeShape).
- Rendering modes: full colour, **accented** (tinted / clear: primary and accent groups tinted white), **vibrant** (lock
  screen: greyscale → material).
- Clear / tinted widgets = clear Liquid Glass with a white-tinted content layer.

### Dynamic Island equivalent (proposal; see §11)

---

## 10. Tab bars, toolbars, navigation, search  (M from HIG art; A)

| Item | Value |
|---|---|
| Tab bar | **floating glass capsule**, height **62-64**, inset **21-25** from the sides and above the bottom. On the S9: x 20..(370 − 64 − 8 if a search circle), bottom 802 − 30 + 8 = **780** (it sits in the home band), height 62 |
| Selected tab | a lighter glass **pill behind the item** (capsule height − 4), with the icon + label in the accent colour (systemBlue). The pill **slides with a liquid morph** between tabs (spring response 0.35, damping 0.75) |
| Tab icon / label | 22-24 pt glyph (filled variant when selected); label **10-11 / 500**; unselected **#1A1A1A** light / **#F5F5F5** dark (3P learnui) |
| Badge | red #FF383C circle 18 with white 12-13 / 600 number at the icon's top-right |
| Search tab | a separate **glass circle, the bar's height (62)**, 8 to the right of the bar |
| Minimise on scroll | scrolling down shrinks the bar to the selected tab's circle (62) and slides the accessory inline; scrolling up / tapping restores it (A behaviour; spring 0.40 / 0.85 E) |
| Max tabs | 5 (A) |
| Nav bar | no bar background: content scrolls under a **scroll-edge effect** (soft progressive blur + fade, ~44-60 tall; no hairline) |
| Bar buttons | **44 glass circles** for icons; text buttons are **44-tall glass capsules**, 17 / 500 label, 14 h padding |
| Grouping | related buttons share one capsule (e.g. Edit + Share); ≤ 3 groups; Done in its own group, **prominent = systemBlue glass** with a white glyph/label |
| Large title | 34 / 700 left at margin 20, below the 44 nav row; on scroll it collapses into an inline 17 / 600 centred title (cross-fade over 20 pt of scroll) |
| Search field | bottom: a 44-48 glass capsule with a magnifier, a 17 / 400 placeholder (#BFBFBF light / #404040 dark, 3P) and a trailing mic. Tap: it rises above the keyboard and a 44 "x" glass circle appears to its right |

---

## 11. The Dynamic Island equivalent (Starbite proposal)

What iOS does (A: HIG Live Activities specs, 393 class; island measured on the 16 Pro hero):
- The island at rest is **125.3 x 36.6**, top 13.9, a pure-black capsule.
- **Compact:** leading and trailing views of **52.33 x 36.67** each, hugging the camera.
- **Minimal:** 36.67-45 x 36.67, a separate circle at the right when two activities run.
- **Expanded:** **371 x 84-160** (long-press), radius ~44.
- Animations ≤ 2 s; expand / contract on a bouncy spring (~0.5 s, ~0.7 damping, E) with a blur cross-fade of the
  content.
- Non-island iPhones show a coloured capsule behind the status-bar time (A, Apple's status-icon guide):
  - **green** = call / FaceTime;
  - **red** = screen recording or mic recording;
  - **blue** = hotspot, navigation, Screen Mirroring;
  - **purple** = SharePlay.

  It is ~24 tall, time width + 12 (E); tap returns to the app.

The S9 has no cut-out, so there is no hardware island to wrap. iOS's own non-island iPhones (SE, 16e) put calls,
recording and navigation in a **coloured status-bar capsule** and show Live Activities on the lock screen and as
banners. We propose the same, plus a software "island" that **lives inside the status band** and shows only when
something is live.

- **Status capsule** (always available): a capsule 24 tall, centred on y 30 (`SB_MID`), drawn behind the time at the
  left status column, coloured by activity. It replaces the time column's background, as iOS's call / recording pill
  does:
  - call **green #34C759** with the timer 15 / 600 white;
  - screen or audio recording **red #FF383C**;
  - Survival / SOS **red** with an SOS glyph;
  - navigation **blue #0088FF**;
  - hotspot / USB tether **blue**;
  - heater running **orange #FF8D28**.

  Tap: open the owning app. It appears with a morph from a 6 pt dot (spring response 0.35, damping 0.75).
- **Live pill** (our island, optional): a **black capsule** (not glass, as on iOS) centred at the top, y 6..34.
  - Compact **126 x 28**: leading glyph + trailing value, e.g. the timer "4:59" in orange, the heater "52 °C".
  - Expanded on long-press or for an incoming event: **374 x 160**, radius 44, y 6. It morphs from the compact pill
    on a spring with response 0.45, damping 0.75 (the island's bounce). Contents 15-17 pt white.
  - Minimal (two activities): a 28 pt circle on the right.
  - Because the S9 has no camera hole there, the pill only exists while an activity runs, so it never permanently
    hides status icons.
- **Lock screen Live Activity**: a platter like §5 at the top of the notification list: 358 x 160 max, regular
  glass, 16 padding.

---

## 12. Controls

### 12.1 Buttons (A/E)
- `glass`: a translucent capsule, label in the label colour.
- `glassProminent`: glass tinted with the accent (systemBlue), white label; max 1-2 per view.
- Minimum hit target 44 x 44 (A).
- Icon buttons 44 circles; a large call-to-action capsule 50-52 tall, 17 / 600.
- Press: scale up **~1.04-1.1** (glass grows, it doesn't shrink), with a light that follows the finger (LIQUID-GLASS §5.1).

### 12.2 Segmented control (E)
- Capsule container 32-36 tall, radius h/2.
- The selection indicator is a white (light) / #636366 (dark) capsule with a 2 pt inset. It **slides with a liquid
  stretch** (it elongates in the direction of travel, then settles): spring response 0.35, damping 0.75.
- Text 13-15 / 500. Selection haptic on change.

### 12.3 Slider (A text "narrow lozenge" + E)
- Track 6 tall capsule, filled with the accent from the minimum to the thumb.
- Thumb: a **white lozenge ~38 x 24** (not a circle). While dragged it becomes a clear **glass lens** larger than the
  track, so the track shows through it, magnified.
- Selection ticks at detents.

### 12.4 Toggle (M, HIG @2x art)
- Track **64 x 28** (iOS ≤ 18: 51 x 31). On = systemGreen **#34C759**; off = **#E9E9EA** light / #39393D dark (E).
- Thumb: a white **pill ~38 x 24**, 2 pt inset.
- Press/drag: the thumb turns into a clear **glass lens** that grows past the track edges, ~44 x 32 (E), with the
  track visible through it. Release: it springs to its side and turns white again (response 0.30, damping 0.70).
- Light impact haptic on change.

### 12.5 Lists (general)
- Inset grouped as §6 Settings. Plain lists: rows 44-52; separators inset to the text.
- Swipe actions: full-height colour blocks with 15 / 600 white labels and glyphs; iOS 26 rounds them into capsules
  inside the row (E).

---

## 13. Phone, Camera, App Store  (E unless marked)

- **Phone (iOS 26):** one **Calls** list (favourites, recents, voicemail together) (A).
  - Tabs: Calls / Contacts / Keypad + search.
  - Keypad: **78-80 circles**, gaps 16-18 h / 14-16 v, digit 36-40 / 400 Display, letters 10 / 600 tracking +2, fill
    #E5E5EA / #333333; call button the same size, green.
  - In-call: name 28-34 at the top; a 3 x 2 grid of 72-80 glass circles; end call red, bottom centre.
- **Camera (iOS 26):** the mode bar shows only **Photo / Video** by default; other modes by swiping (A).
  - Settings sit in a top glass row.
  - Shutter: 74-78 white ring with a 62-66 inner disc.
  - Flip camera and thumbnail are 44 glass circles.
- **App Store:** tabs Today / Games / Apps / Arcade + a search circle (M).
  - Today cards: full width − 40, 450-480 tall, radius 20-26 continuous.
  - The card opens by zooming into a full page (matched geometry, response 0.45, damping 0.85).

---

## 14. Typography  (A: HIG Dynamic Type "Large", checked 2026-11-12)

| Style | Weight | Size | Leading | Emphasised | Tracking pt |
|---|---|---|---|---|---|
| Large Title | Regular | 34 | 41 | Bold | +0.40 |
| Title 1 | Regular | 28 | 34 | Bold | +0.38 |
| Title 2 | Regular | 22 | 28 | Bold | −0.26 |
| Title 3 | Regular | 20 | 25 | Semibold | −0.45 |
| Headline | **Semibold** | 17 | 22 | Semibold | −0.43 |
| Body | Regular | 17 | 22 | Semibold | −0.43 |
| Callout | Regular | 16 | 21 | Semibold | −0.31 |
| Subheadline | Regular | 15 | 20 | Semibold | −0.23 |
| Footnote | Regular | 13 | 18 | Semibold | −0.08 |
| Caption 1 | Regular | 12 | 16 | Semibold | 0.00 |
| Caption 2 | Regular | 11 | 13 | Semibold | +0.06 |

- Nav large titles use the **Bold** 34; inline titles are 17 / 600. The default text is 17; the minimum is 11.
- SF Pro tracking by size (A, pt): 6 +0.24 · 8 +0.21 · 10 +0.12 · 11 +0.06 · 12 0 · 13 −0.08 · 14 −0.15 · 15 −0.23 ·
  16 −0.31 · 17 −0.43 · 18 −0.44 · 20 −0.45 · 22 −0.26 · 24 +0.07 · 28 +0.38 · 34 +0.40 · 40 +0.37 · 48 +0.35 · 56 +0.30 ·
  64 +0.22 · 72 +0.14 · ≥ 80 0.
- SF Pro **Text** below 20 pt, **Display** at 20 and up.
- Our face is Inter at opsz 14 (Text) / 32 (Display), per audit P3j-1. Apply the table's tracking **in pt at the drawn
  size**: `tk_text`'s auto tracking should use this table, not a formula, when `ls == 0`.

## 15. Colour  (A: HIG Color, iOS 26 palette, updated 2025-06-09)

| Name | Light | Dark |
|---|---|---|
| Red | #FF383C | #FF4245 |
| Orange | #FF8D28 | #FF9230 |
| Yellow | #FFCC00 | #FFD600 |
| Green | #34C759 | #30D158 |
| Mint | #00C8B3 | #00DAC3 |
| Teal | #00C3D0 | #00D2E0 |
| Cyan | #00C0E8 | #3CD3FE |
| **Blue (accent)** | **#0088FF** | **#0091FF** |
| Indigo | #6155F5 | #6D7CFF |
| Purple | #CB30E0 | #DB34F2 |
| Pink | #FF2D55 | #FF375F |
| Brown | #AC7F5E | #B78A66 |
| systemGray | #8E8E93 | #8E8E93 |
| systemGray2 | #AEAEB2 | #636366 |
| systemGray3 | #C7C7CC | #48484A |
| systemGray4 | #D1D1D6 | #3A3A3C |
| systemGray5 | #E5E5EA | #2C2C2E |
| systemGray6 | #F2F2F7 | #1C1C1E |

Semantic colours (UIKit runtime values; not re-verified on an iOS 26 runtime):

| Name | Light | Dark |
|---|---|---|
| label | #000000 | #FFFFFF |
| secondaryLabel | rgba(60,60,67,.60) | rgba(235,235,245,.60) (iOS 26 may be .70: verify) |
| tertiaryLabel | rgba(60,60,67,.30) | rgba(235,235,245,.30) |
| quaternaryLabel | rgba(60,60,67,.18) | rgba(235,235,245,.16) |
| separator | rgba(60,60,67,.29) ≈ #C6C6C8 on white | rgba(84,84,88,.60) |
| systemBackground | #FFFFFF | #000000 |
| secondarySystemBackground | #F2F2F7 | #1C1C1E |
| systemGroupedBackground | #F2F2F7 | #000000 |
| secondarySystemGroupedBackground | #FFFFFF | #1C1C1E |
| systemFill | rgba(120,120,128,.20) | rgba(120,120,128,.36) |
| secondarySystemFill | rgba(120,120,128,.16) | rgba(120,120,128,.32) |
| tertiarySystemFill | rgba(118,118,128,.12) | rgba(118,118,128,.24) |

Rules (A, HIG "Liquid Glass color"):
- Glass has no colour of its own.
- Small glass flips light/dark by what is under it, and its symbols go monochrome.
- Larger glass is more opaque.
- Tint the **background** of a prominent control (Done), not its text.

## 16. Motion

| Name | Spring | Use |
|---|---|---|
| `.smooth` | duration 0.5, bounce 0 (A) | default UI moves |
| `.snappy` | 0.5, bounce 0.15 (A) | small controls |
| `.bouncy` | 0.5, bounce 0.3 (A) | playful (island, glass morph) |
| legacy `.spring()` | response 0.55, damping 0.825 (A) | |
| Nav push / pop | ~0.45, damping 1.0 (E); the new page slides the full width, the old one moves 30 % under a 10-20 % dim; iOS 26 rounds the moving page's corners concentric with the display (3P Apple forums) | |
| Sheet | 0.45-0.5, damping 1.0 (E) | |
| Keyboard | 0.25 s, curve 7 (≈ spring 0.38 / 1.0) (3P) | |
| Glass morph | 0.4-0.5, bounce ~0.2-0.3 (E) | merge / split / menus |
| Press (glass) | response 0.30, damping 0.6 (audit) | |

- Our springs: `k = (2π / response)²`, `c = 4π·damping / response` (mass 1). SwiftUI's `duration` = response and
  `bounce` = 1 − damping.
- Settle when |x − target| < 0.25 px and |v| < 15 px/s, so the last frame never snaps.
- Reduce Motion: slides and morphs become cross-fades of 0.25 s (A).

## 17. Haptics (UIKit generator → our vibrator pattern)

| iOS generator | Where iOS uses it | Starbite pulse (E, tune on the phone) |
|---|---|---|
| impact .light | key taps, toggles, slider ends | 8 ms at 60 % |
| impact .medium | context-menu lift, long-press commit, CC expand | 12 ms at 80 % |
| impact .heavy | rare | 18 ms at 100 % |
| impact .soft | sheet detents, rubber-band limits | 14 ms at 40 % |
| impact .rigid | hard stops (slider min/max) | 6 ms at 100 % |
| selection | pickers, segmented changes, slider detents, swipe-action threshold | 4 ms at 50 % |
| notification .success | auth / payment success | 10 ms, 60 ms gap, 14 ms rising |
| .warning | refused action | 12 ms, 80 ms gap, 12 ms |
| .error | wrong passcode (with the shake) | 3 x 10 ms, 50 ms gaps |

## 18. iOS 27 deltas (released Sept 2026)
- iOS 27 refines rather than replaces the iOS 26 design (A: HIG change logs 2026-06-08 and 2026-09-09).
- New "Prominent" tab role: an accent-filled tab item.
- Revised dark-mode tab bars, updated guidance on search-as-a-tab and menu-item icons.
- Glass opacity is user-adjustable: iOS 26.1 has the Clear / Tinted switch; iOS 27 has a continuous Liquid Glass slider
  in Settings > Appearance (3P). **Starbite should expose the same slider** (LIQUID-GLASS §6).
- Our project boots iOS 27 on this very S9 (2026-11-11). Native 3x screenshots from it would be the best measurement
  source for Messages, Photos and Phone. See the GAP-LIST's "verify" items.

## Sources
- Apple HIG (fetched 2026-11-12): typography, color, layout, tab-bars, toolbars, search-fields, sheets, alerts,
  action-sheets, toggles, sliders, segmented-controls, buttons, menus, lists-and-tables, materials, motion,
  playing-haptics. https://developer.apple.com/design/human-interface-guidelines/
- Apple Newsroom, "Apple elevates the iPhone experience with iOS 26" (2025-06-09) full-resolution renders (lock
  screen, home screen customisation, Live Activities): https://www.apple.com/newsroom/2025/06/apple-elevates-the-iphone-experience-with-ios-26/
- Apple iPhone User Guide, iOS 26 and iOS 27 editions ("Switch between open apps", "View and respond to notifications",
  status icons). Control Centre page (iOS 26):
  https://support.apple.com/guide/iphone/use-and-customize-control-center-iph59095ec58/ios
- WWDC25 sessions 219 "Meet Liquid Glass", 356 "Get to know the new design system", 284 / 323 (UIKit / SwiftUI with the
  new design); WWDC23 "Animate with springs". https://developer.apple.com/videos/wwdc2025/
- MacStories, "iOS and iPadOS 26: The MacStories Review": https://www.macstories.net/stories/ios-and-ipados-26-the-macstories-review/
- learnui.design iOS guidelines (iOS 26 update): https://learnui.design/blog/ios-design-guidelines-templates.html
- KeyboardKit, "Custom iOS keyboard extensions and Liquid Glass" (2025-07-28).
- MacRumors iOS 18 Control Center guide; MacRumors 2025-06-23 (iOS 26 beta 2 Control Center); 9to5Mac 2025-12-23
  (Liquid Glass Clear / Tinted).
- Study images are kept locally under `build/design-ref/` (git-ignored, never committed).
