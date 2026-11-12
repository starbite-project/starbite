# Starbite OS Next vs iOS 26/27: the gap list

Track C, design-ref session. v1.1 2026-11-12: measured switcher / notifications / home / lock values; icon size
settled at 62. Work list for the UI session (sbos-ui-p1) and, for glass items, the GPU/OS
session (starbite-os).
- **Baseline:** `claude/sbos-ui-p1-921bbf` at ed8175b (P3v), read from code plus 47 host renders. The renders are local
  only, in `build/design-ref/starbite-now/`.
- **Specs:** [IOS-REFERENCE.md](IOS-REFERENCE.md) (§ numbers below) and [LIQUID-GLASS.md](LIQUID-GLASS.md) (G numbers).
- **Builds on:** the 2026-11-11 audit (starbite-ios-audit.md). Audit items already done are not repeated. Open audit
  items are folded in here.

**Size:** S < 1 h, M 1-3 h, L > 3 h. **Owner:** UI = sbos-ui-p1, GPU = starbite-os (lens.h / gpucomp / mesa-compjob).
**Rules that still apply** (sbos-ui-agents-brief):
- heap peak ≤ 135 MiB;
- no glass bake in a tap handler;
- tk_comp.c and kern_next.c frame() belong to the GPU session;
- every film still runs.

---

## Tier 1: tonight's 1.0 candidate (biggest "it looks like iOS" per hour)

| # | Item | Today (where) | Target (spec) | Change | Size | Owner |
|---|---|---|---|---|---|---|
| 1 | **System palette + accent** | ACCENT #6fb3ff (`ACCENT()` shell_lock.c:2005); #0a84ff / #2f7bf6 mixed; app ground #0a0f1f; GROUP #151c33 | iOS 26 dark palette (§15): accent **#0091FF**; red #FF4245, green #30D158, orange #FF9230, yellow #FFD600; grouped background **#000000**, cards **#1C1C1E**; separators rgba(84,84,88,.6); labels white / rgba(235,235,245,.6 / .3) | One `sbtk_colors.h` table (dark set now, light set for later); replace literals. **Decision for the user:** iOS dark is black, not navy (see "Decisions") | S | UI |
| 2 | **Lists / Settings to iOS 26 geometry** | GROUP r16 at x 20 w 350; rows 47; labels 16; icons 29 r7 at x 36; caps headers 13 | §6: cards **x 16 w 358 r 26**; rows **52**; label / value **17 / 400**; icon **30 r 7.5** at card + 16, text at card + 59; separator from the text to the right edge; headers **sentence case 15 / 600**; section gap 35; footers 13 | One `d_group` / `d_row` pair used by Settings, Phone Recents, Notes, App Store info, Wi-Fi, Cellular, Medical Card | M | UI |
| 3 | **Toggles** | 51 x 31, knob jumps (shell_wifi.inc, `sv_switch`) | §12.4: **64 x 28**, white **pill thumb 38 x 24**, on #30D158 / off #39393D; slides on a spring (0.30 / 0.70); light haptic; while dragged the thumb grows into a glass lens 44 x 32 (once G1 lands) | `d_switch` + `tk_spring` per switch | S | UI |
| 4 | **Alert component** | none; `sh_confirm` is a 342 card r32 at dim .55 with a hold bar; 68 toasts | §3.1: **300 wide, r 36**, left-aligned 17 / 600 title, 15 / 400 message, **capsule buttons 48 tall, 132 + 8 + 132, inset 14**, primary blue filled, destructive red; appear 1.10 → 1 on a spring; dim .5 | `sh_alert(title, msg, btns[], cb)`. Restyle `sh_confirm` on it (keep its hold-to-confirm as the primary capsule filling). Use for Remove App, Clear All, call errors | M | UI |
| 5 | **Keyboard iOS 26** | solid #1e2030 panel, keys #6b6e7a r6 33 x 46, words "shift" / "return", no pop-up, appears instantly | §4: **glass panel, top r 30**, keys **31.6 x 44, gap 6, r 8**, all keys the same colour (dark #6B6B6F, light #FFF), ⏎ and shift / delete **glyphs** (the pack has `kb-shift`, `kb-delete`), pop-up balloon (key + 1 key-height balloon, letter 32), predictive strip 40, emoji / mic glyph strip; **slides in** (spring 0.38 / 1.0) and the field rides with it | `kb_draw_r` rewrite; the panel as a lens shape (G16); balloon as its own small layer (no rebake per key) | M | UI |
| 6 | **Control Centre geometry + chrome** | connectivity 160.7 x 156 r 28 with 4 x 52 circles; 70 pt circles; sliders 70 x 156 r 24; no + / power / status row / page dots; only from home | §1: **U 68, gap 14.5, pitch 82.5, x0 37.25, first row y 119**; 2x2 **150.5 r 30** with 55.6 circles + a mini 2x2 cluster (24.7); tall sliders **68 x 150.5** capsules; **+ and Power 28 circles at y 30**; status row at y 92; page icons at x 376; **opens on the lock too**; rows fly in staggered (20 ms per row, scale .9 → 1, response .40 / .80); light haptic at commit; tiles press-grow (G9); the active tile is tinted glass | `build_cc_grid` re-layout from constants; stagger in `set_cc`; gesture enable on the lock | M | UI |
| 7 | **Real glass inside apps (G16)** | every in-app bar, back circle, compose circle, Phone tab bar and search field is `d_lg_flat` (a dark disc over #0a0f1f); NC platters and banners are views without a lens | Every floating control in apps is **lens glass over the app's live content**: back / Done / compose circles, tab bars, search fields, the keyboard panel, sheets, banners and NC platters | `mk(..., glass=1)` + `tk_layer_glass_shape` for these layers; the app page layer counts as the live backdrop (`live` quads). Watch the 127-quad and 3-level limits | M-L | UI (+ GPU if levels run out) |
| 8 | **Liquid Glass shader to iOS 27 (G1 + G4 + G2)** | lens peak 0.2·b ≈ 3 pt, top-left specular + an all-round Fresnel that reads as an outline, `.82c + add` tone | **band h = min(20, .25 short), amount A = min(60, .5 short)**, quarter-circle profile; glint on a **vertical** light, both walls, recolouring, 1.2 pt; face = E + T·S-curve(Y) + K·chroma with light / dark / clear presets and the slider s (default .2) | lens.h `lens_eval` / `lens_shade` + mesa-compjob.c `lens_px` + tk_glass.c constants together; `LENS_GAIN` ramp for the phone check | M | GPU (+ UI for tk_glass.c) |
| 9 | **Springs to iOS** | UNLOCK 320/32, OPEN 380/34, CLOSE 360/30, SHEET 420/36, ROWS 300/30, CC 400/36 (all response ≈ .31-.36) | §16: open / sheet **.45 / 1.0** (k 195, c 27.9); close **.45 / .85** (k 195, c 23.7); unlock + CC **.40 / 1.0** (k 247, c 31.4); press **.30 / .6** (k 439, c 25.1); keep ROWS for lists. Settle at < .25 px and < 15 px/s | `SPR` table in tk_motion.c; try on the phone first (audit "Later") | S | UI |
| 10 | **Haptic vocabulary** | one 10 ms pulse (`sh_haptic`) | §17: light / medium / heavy / soft / rigid / selection / success / warning / error patterns; add the missing uses: CC commit, toggles, tab changes, swipe-dismiss threshold, slider ends, long-press lift | `sh_haptic(kind)`; the kernel pulse takes ms + strength (check the vibrator driver supports strength; otherwise vary the length) | S | UI (+ OS if the kernel API changes) |
| 11 | **Messages: SMS look** | sent bubbles #2f7bf6 blue for SMS, r 20 with an r6 corner, 16 pt text, max 268, 4 / 12 gaps, field jumps 726 → 492, no scroll | §7: **SMS = green #30D158**, **17 / 400**, **r 18**, max **292**, padding 12 / 7.5, **tail on the last bubble of a run** (a hook path, 10 x 16), gaps **2 / 10**, "Delivered" 11 / 600; glass "+" circle 36 + glass capsule field 36; the field **rides the keyboard's spring**; the thread scrolls with momentum | `cm_bubbles` + `cm_field_build` | M | UI |
| 12 | **Status capsule (our Dynamic Island equivalent)** | nothing; toasts at y 6 | §11: a coloured **status capsule** behind the time (call green with a timer; SOS / recording red; heater orange; hotspot blue); optional black **live pill** 126 x 28 at the top, which expanded **is where toasts go** (they become the pill's transient state, as iOS's island does for system events) | `build_apsb` + a pill layer; `toast()` re-targeted at the pill | M | UI |

## Tier 2: next (completes the main surfaces)

| # | Item | Today | Target | Size | Owner |
|---|---|---|---|---|---|
| 13 | **App switcher iOS 26** | ≤ 4 recents; cards scale .6, 234 x 481 r 20 at y 150; pitch 214 (20 pt overlap); flat, no depth | §2 (measured): focused card **scale .69 (269 x 553), centred, centre y 391**; behind card **.65 peeking 70 to the left**; front card **.70 at +209**, stacked on top; radius **30**; icon **25.5** at card + 24, name 17 / 600 on the focused card only; backdrop blur + dim .30; scale interpolates with x; up to 8 recents (snapshot heap permitting); flick-up close with the neighbours closing the gap; sideways swipe on the home bar = previous app | M | UI |
| 14 | **Notification Centre + banners** | platters invisible on the dark wallpaper (no lens); r 26; header 20 / 700; banner y 8, 4 s, no haptic; nothing on the lock | §5 (measured): platters **x 14.5 w 361, r 23, gap 8, height 24.6 + 18 x lines**, icon 38 centred at inset 13.7, text from 65, title / body 15 / 600 and 15 / 400 at 18 leading, time right inset 18.3; real glass (G16); the stack's peeks scale .95 / .90; banner at **y 10** (covers the status band), in on a spring (~.50, slight overshoot), 5-6 s; swipe-left Options / Clear glass actions 56; **notifications on the lock screen** above the hold buttons (bottom-anchored, newest at the bottom); the NC pull also works on the lock | M | UI |
| 15 | **Sheet component with detents** | only the focus sheet (bottom 8, r 24) | §3.4: medium detent **inset 6, top r 33**, bottom r ≥ 26, grabber 36 x 5, 44 glass close circle at 16 / 16; large detent at y 52, opaque, presenting view scales .92; spring present .45 / 1.0, detent change .40 / .85, soft haptic on landing | M | UI |
| 16 | **Menus / popovers / action sheets** | none | §3.2-3.3: glass popover **246 wide, r 26-28**, capsule rows 40 + 8, morphing out of the source button (G11); context menu on long-press (lift 1.03 + blur + medium haptic) for home icons (Quick Actions), messages and list rows | L | UI |
| 17 | **Navigation: push / pop, large-title collapse, edge swipe, scroll edge** | pushed pages appear instantly; titles drawn into the page; most pages don't scroll; mixed "< Photos" text back links | §10 / §16: push slides the full width, the old page moves 30 % under a dim (.45 / 1.0); interactive **left-edge swipe back**; the large title collapses into the 17 / 600 inline title over 20 pt of scroll; **scroll-edge effect** under the status band and bars (G12); every back control is the 44 glass circle | L | UI |
| 18 | **Tab bars everywhere they belong** | Phone only (flat pill, accent text, no selection pill) | §10: floating **glass capsule 62 tall** at x 20, bottom 780; selection **pill** that slides (.35 / .75); a separate **search circle**; minimise on scroll. Phone (Calls / Contacts / Keypad + search, iOS 26 unified), Photos (Library / Collections), Clock (World / Alarms / Stopwatch / Timers), Music, App Store (Today / Apps / Updates + search) | M | UI |
| 19 | **Settings search at the bottom; Spotlight at the bottom** | Search opens at the top (y 56) with Cancel | §6 / §10: a bottom 48 glass search capsule; tap: it rises above the keyboard, with a 44 "x" circle beside it. Same for home Search (iOS 26 Spotlight) | S-M | UI |
| 20 | **Photos iOS 26** | d_h1 "Photos"; viewer fades in .2 s; no zoom or toolbar | §8: no large title in Library (22 / 700 + "Select" capsule over the scroll edge); **zoom from the tile** into the viewer and back (matched geometry, .40 / .90); chrome toggles on tap; bottom glass circles (share, heart, info, delete); pinch and double-tap zoom; tabs per #18 | M | UI |
| 21 | **Home details** | icons 62; press darkens + shrinks to .88; paging rubber ×.3; badges disabled; dock r 24 | §9.2 (measured): keep **62** icons (iOS measures 61.6 at 390); labels 12 / **400**; widget radius **25**; search pill **78 x 24 at centre y 647**; dock **x 11..379, top 702, h 88**, icons at y 714; press: slight dim, **no shrink** (E: .96); `rubber()` on paging; **badges on** (red #FF4245, 18 circle, 12 / 600); long-press → Quick Actions menu (#16) | S-M | UI |
| 21b | **Lock screen positions** | date 19 / 600 at baseline 96; clock IT700 106; hold buttons centre y 720 | §9.1 (measured): date **22 / 600, centre y 82**; clock **Display 600 at 116**, digits top y 108; widgets row top y 214; buttons centre **y 727**; notifications' bottom at y 677 | S | UI |
| 22 | **Type ramp + tracking table** | 23 off-ramp sizes (10.5, 11.5, 12.5, 13.5, 14.5); auto tracking by formula | §14: only ramp sizes (34 / 28 / 22 / 20 / 17 / 16 / 15 / 13 / 12 / 11); tracking from the HIG table by size, not the formula | S | UI |
| 23 | **Continuous corners (G7)** | circular arcs everywhere | p-norm corners (n ≈ 4.5, r × 1.28) in tk_raster `tk_sd_rrect4`, the bake and comp2's SDF; one shared function | M | UI + GPU |
| 24 | **Glass behaviours G6 / G9 / G10 / G12** | none | flip with hysteresis + glyph recolour; press grow + glow; materialise instead of alpha fades; scroll-edge quad | M each | GPU + UI |
| 25 | **CC: expanded connectivity, long-press sliders, Now Playing, edit mode** | none | §1: morph the 2x2 into the expanded panel (6 big circles with labels); long-press slider → full-height slider; Now Playing 2x2 (Music, once audio exists); edit mode later | M | UI |
| 26 | **Phone iOS 26 + call screen** | separate Favourites / Recents tabs; call screen not minimisable; flat buttons | §13: unified Calls list; keypad circles 78-80 glass; in-call a 3 x 2 grid of glass circles; the call can be minimised into the green status capsule (#12) | M | UI |

## Tier 3: later

| # | Item | Note | Size |
|---|---|---|---|
| 27 | **Light appearance** | All of §15's light column; glass presets already have light variants. A Display & Brightness "Appearance" choice. Needs every literal colour behind the table from #1 | L |
| 28 | **Liquid Glass slider + accessibility** (G17) | Settings > Display & Brightness > Liquid Glass (Clear ↔ Tinted); Accessibility: Reduce Transparency / Increase Contrast / Reduce Motion / Reduce Bright Effects | M |
| 29 | **Glass merging (G8) + morph (G11)** | the tab bar's selection and search circle, CC expansion, menus | L (GPU) |
| 30 | **Lock clock iOS 26** | the glass numerals: a lens-glass text mask; adaptive height; weight choice | L |
| 31 | **Camera, Clock tabs, Calculator iOS 26, Weather sky** | audit "Later" items, unchanged | M each |
| 32 | **App Store iOS 26** | Today cards (full width − 40, 450-480 tall, r 20-26) that zoom open; GET → progress morph; momentum scrolling in the store list | M |
| 33 | **Full resolution while moving (audit P3j-3)** | still half-res during big moves; GPU session's call | L (GPU) |
| 34 | **Interruptible app close; sideways app switching on the home bar** | audit "Later" | M |
| 35 | **SF Symbols-like glyph system** | vector glyphs with weights / scales and tinting at compose (helps G6 too) | L |

---

## Decisions for the user (asked once, answer changes the list)
1. **Background colour:** iOS dark mode is pure **black** (#000) with #1C1C1E cards. Starbite is navy (#0a0f1f, #151c33)
   with a blue wallpaper. "Just like iOS" means black in apps; the wallpaper can stay. Recommended: black in apps.
2. **Icon size:** settled by measurement (v1.1). iOS 26 icons are 61.6 pt at 390 wide, so **keep 62**. No decision
   needed.
3. **Lock screen bottom-right button:** iOS has the Camera; Starbite has the Emergency flame. Recommended: keep the flame
   (Survival is the point of the OS) but style it as iOS's 50 pt glass circle.
4. **Lock screen list:** iOS shows notifications there; Starbite shows the OS chooser (it replaces the boot menu).
   Recommended: notifications above, the Systems stack as a lock-screen widget group below the clock.
5. **Light mode:** iOS defaults to light. Recommended: dark first (tonight), light in Tier 3.

## Verify on a real iOS 27 screen (needs the phone; via the main session)
The same S9 boots iOS 27. One attended session of native 3x screenshots (Control Centre, Settings + a sub page,
Messages thread, Photos grid, Phone keypad, the lock with a notification, a sheet, an alert, the keyboard) would turn
every **E** in IOS-REFERENCE into an **M**. It would also calibrate the glass (LIQUID-GLASS §6.3). The images stay in
`build/design-ref/`, never in git.
