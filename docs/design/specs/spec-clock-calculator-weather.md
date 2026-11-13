# Clock, Calculator, Weather (iOS 27) — Starbite build spec

Version 1.0, 2026-11-13 (design-ref Track C, for builder A3). Frame, units and shared components:
`docs/design/IOS-REFERENCE.md` (cited **IR §n**) and `docs/design/LIQUID-GLASS.md` (cited **LG §n**). This file only
adds what is specific to the three apps.

> **Platform decisions (Track A, 2026-11-13; build to these, don't wait):**
> - **Alarms:** the platform holds **32** (trunk 24a6efa8; old records are still read). The Alarm tab's own limit
>   ships with A3's Clock.
> - **City / time-zone list:** Track A builds one city table over `kern/tzdb.c`'s zones, shared by World Clock add-city
>   and Weather search (A2.1a). Code against it; until it lands, use the fixed `WC[5]`.
> - **Scientific maths:** A3 ports the functions it needs from **musl's libm** (MIT).
> - **Forecasts:** **Open-Meteo** (free, no key), cached in a blob, with the location **rounded to ~1 km** before any
>   request.
>- **Currency rates:** **Frankfurter** (the ECB's reference rates; free, no key; Track A's decision). Cache and date them
>   like the forecasts. Convert's units work fully offline.
> - **Haptics:** no haptic on Calculator keys (as iOS); remove the trunk's.

**Tags.** **A** Apple stated · **M** measured (source named) · **3P** a reviewer · **E** estimate.

## Sources and calibration

**Measurement art (M).** All from the Apple iPhone User Guide, iOS 27 edition (support.apple.com/guide/iphone/…/27/ios/27).
Each is a 490 x 1008 px PNG of a **402-pt Pro phone**: the screen starts at x 21.5 px and y 20 px and is 446 px wide,
so **1.1095 px/pt**. The scale was checked against the Dynamic Island, which measures 140 x 40 px = 126 x 36 pt
(A: 125 x 37). The same frame is used in spec-appstore.md. The files are kept git-ignored under
`build/design-ref/apps27/{clock,calc,weather}/`.

| Short name | Guide page | What it shows |
|---|---|---|
| **UG-WC** | "See the world clock" (iph1ac0b4af) | World Clock tab, Edit and +, five cities, tab bar (dark) |
| **UG-AL** | "Set an alarm" (iph2909d3a74) | Add Alarm sheet: wheel, Repeat day circles, Label, Sound, Snooze, Snooze Duration |
| **UG-WU** | "Change your wake up alarm" (iphf2a780f81) | Change Wake Up sheet (Health sleep dial) |
| **UG-SW** | "Use the stopwatch" (iph96b1e460) | Digital stopwatch running, Lap / Stop, page dots, five laps |
| **UG-TM** | "Set timers" (iph8241d6b2a) | Timer sheet: wheel, Label, When Timer Ends, Presets, Recents |
| **UG-CB** | "Use the basic calculator" (iph1ac0b5cc) | Basic calculator after =, history and mode buttons |
| **UG-CS** | "Use the scientific calculator" (iphb977c4c0e) | Scientific calculator in **portrait**, mid-expression |
| **UG-CV** | "Convert units or currency" (iph69d274dde) | Scientific + Convert (USD → EUR) |
| **UG-WM** | "Check the weather" (iph1ac0b35f), first image | Weather location page (Conditions module, 10-day, bottom bar) |
| **UG-WD** | same page, second image | Scrolled location page: compact header, Air Quality, Wind, Moon, Sunset, UV tiles |
| **UG-WL** | "Check the weather in other locations" (ipha39110aad) | Locations list with sky cards and bottom search |

**Text sources (A).** The User Guide iOS 27 pages above, plus "Get started with Calculator", "See previous calculations"
and "View weather maps". **3P:** MacRumors and 9to5Mac on the iOS 26.1 "Slide to stop" change (2025-10-08 and
2025-11-05); MacRumors "iOS 27 Weather" guide and 9to5Mac 2026-06-17 (Highlights; Conditions / Precipitation / Wind);
Tom's Guide (iOS 27 Alarm & Timer volume slider).

**Font size from ink.** SF digits and capitals are **0.705 em** tall and descenders about 0.2 em, so
size ≈ ink height / 0.705 (or / 0.905 with a descender). Text sizes are ±1 pt.

**Converting to the S9 (IR §0).**
- Fixed chrome (buttons, keys, rows, fonts, radii) keeps its pt value.
- Width-filling layouts scale by 390/402 = 0.970.
- The S9 is **72 pt shorter** than the 874-pt Pro. 18 pt of that comes out of the status band (62 → 44). The other 54
  come out of free space or the scrolling content, never out of chrome.
- Every "S9" column below is already converted.

**Appearance.** Clock and Calculator are **always dark** in iOS (A: Clock's art is dark even in the light-mode guide;
Calculator has no light keys). Weather draws its own sky in both appearances. So these three apps need only the dark
palette, plus the light variants of shared sheets and menus.

---

# Part A — Clock

## A1. Information architecture

- Root: a **glass tab bar** (IR §10) with four tabs, in this order: **World Clock · Alarms · Stopwatch · Timers** (A/M
  UG-WC). There is no search tab. The selected tab is **orange**, not blue: Clock's accent is orange.
  - **World Clock**
    - Edit mode: reorder and delete
    - **Choose a City** sheet: search field and A-Z list
  - **Alarms**
    - **Add Alarm** / **Edit Alarm** sheet → Repeat (inline), **Label** (inline text), **Sound** (pushed list),
      Snooze, Snooze Duration (menu)
    - Edit mode
    - Sleep | Wake Up section → Change Wake Up sheet (UG-WU). **Omit:** we have no Health sleep schedule (§A6).
  - **Stopwatch**
    - Two pages, digital and analogue, swiped sideways (A UG-SW)
    - Laps list
  - **Timers**
    - Root with no timer: wheel, Label / When Timer Ends, Presets, Recents
    - Running timers: a list; with one timer, the big ring
    - **+ → Timer** sheet (UG-TM)
    - **When Timer Ends** (pushed sound list)
- Outside the app:
  - Alarm ringing: full screen (§A2.6)
  - Timer done: banner or full screen (§A2.6)
  - Timer **Live Activity** on the lock screen and in the status-band pill (IR §11)
- The app reopens on the last tab used (E: iOS behaviour). Starting the app from Control Centre's Timer opens Timers;
  from the lock-screen alarm widget it opens Alarms.

## A2. Screens

### A2.0 Shared Clock chrome

| Item | iOS (Pro, M) | S9 |
|---|---|---|
| Background | pure black #000000 (M UG-WC) | #000000 |
| Nav row | glass **Edit** capsule 64 x 44 and **+** circle 44, 12 apart, right margin 16; centre y 82 (M UG-WC) | Edit at x 254..318, + at x 330..374, centre y **66** (`NAV_MID`) |
| Large title | 34 / 700, x 16, baseline 152 (M UG-WC) | 34 / 700, x 16, baseline **124** |
| Tab bar | glass capsule 358 x 64, inset 22 from the sides, 21.5 above the bottom (M UG-WC); fill over black ≈ #202020 | x 20..370, y **718..780**, h 62 (IR §10) |
| Selected tab | darker inner pill #090909 with a 1 pt light rim ≈ #3c3c3c, glyph and label **orange #FF9230** (M UG-WC, sampled #FF9936) | pill = tab width − 8, h 54, r 27 |
| Unselected tab | glyph and label white #F5F5F5 (M) | |
| Tab glyphs | globe, alarm clock, stopwatch, timer (SF-like). **Ours drawn originally** (trunk ck_glyph is close) | 24 pt glyph centred y 741, label 10 / 600 baseline 765 (E) |

- Tab bar behaviour: the selected pill slides with a liquid morph (IR §10: response 0.35, damping 0.75). **No haptic**
  on a tab change in iOS (E). The trunk's `sh_haptic` there must go.
- Edit and + are 44-tall glass. Edit is a text capsule, 17 / 500 white. + is a glass circle with a white 20 pt
  plus. Neither is tinted (M UG-WC: both #1F1F1F-#202020 glass over black).

### A2.1 World Clock

**Layout (S9).**
- Nav row: Edit and +.
- "World Clock" large title.
- A plain list of city rows. Each row starts with a 0.5 pt separator at its top, inset 16..374, colour separator
  (IR §15). The list also ends with a separator under the last row (M UG-WC).
- The first row starts at y **142**, 18 below the title baseline (M: 18 on the Pro).
- Row height **98** (M UG-WC: rows pitch 98.2 pt).

Inside a row (offsets from the row's top, M UG-WC):

| Element | Value |
|---|---|
| Offset line | **"Today, +0HRS"** style. 15 / 400, secondaryLabel (≈ #8E8E93 on black). Left 16, baseline +35 |
| City | **≈ 29-30 / 400** (M: Mumbai ink 20.8 → 29.5). Build **30 / 400**, white, left 16, baseline **+70**. One line; long names truncate with "…" before the time |
| Time | **64 / 300 (Light)**, tabular, right-aligned to 374, baseline +70 (M: digits ink 45 → 64) |
| AM / PM | **32 / 300** right after the digits, same baseline (M: "AM" ink 22.5 → 32); none in 24-hour mode |

- **Offset text format** (A/M UG-WC): `<Today|Yesterday|Tomorrow>, <+|->H` then `HRS`, e.g. "Today, +9HRS",
  "Tomorrow, +16HRS". Half-hour zones use `+12:30` with **no** "HRS" (M: "Today, +12:30"). The local zone is
  "+0HRS". The trunk's `ck_city_row` already does this, except that it appends HRS after the minutes: fix that.
- **Empty state** (no cities): "No World Clocks" centred 20 / 600 in secondaryLabel (E iOS 17+). The Edit capsule is
  hidden while the list is empty.
- **Default list:** on first run iOS adds the user's own city (E). Ours: the device time zone's city, then nothing
  else.

**Interactions.**
- Swipe a row left to show a red **Delete** action (IR §12.5: in iOS 26 the action is a capsule inside the row).
  A full swipe deletes. Selection haptic at the threshold.
- **Edit:**
  - Each row shows a red ⊖ badge at the left (22 pt circle, red #FF4245 with a white bar).
  - A ≡ reorder grip appears at the right; the time hides.
  - Tapping ⊖ reveals Delete.
  - Edit becomes **Done**, a prominent orange checkmark circle (iOS 26: the confirm button is a ✓ circle, M UG-AL
    shows the same in a sheet; E here).
  - Drag a grip to reorder; the lifted row scales 1.03 with a shadow, medium impact on lift.
- **+** opens **Choose a City** (A2.1a).
- Tapping a row does nothing (E iOS). Long-press does nothing in iOS (E).
- The times tick on the minute (re-render on the minute boundary only).

#### A2.1a Choose a City (sheet)
- A **large-detent sheet** (IR §3.4). Its title is "Choose a City" (17 / 600). Cancel is a 44 glass ✕ circle at the
  left (E: the iOS 26 sheet convention, M UG-AL).
- A search field at the top of the sheet (E: iOS keeps it in the sheet, not the bottom bar):
  - a 36-tall capsule filled with tertiarySystemFill
  - a magnifier and the placeholder "Search", 17 / 400
  - it gets focus at once and the keyboard rises (IR §4)
- Under it, an **A-Z list of every city**:
  - plain rows 44 tall, 17 / 400, white
  - section index letters on the right edge, 11 / 600 orange
  - section headers "A", "B" … 15 / 600 secondaryLabel
- Typing filters by prefix, ignoring case and accents: "sao" matches "São Paulo, Brazil".
- Rows read "City, Country" (E iOS).
- Tapping a row adds the city to the end of the list. The sheet slides down and the new row fades in at the bottom.
- **No results:** "No Results" 20 / 600 centred (E).
- **Local equivalent: an on-device city database.** About 25 000 cities, each with its IANA time-zone name, from a
  GeoNames-style table we build at image time. Daylight saving comes from our tz rules: extend the trunk's EU/US-only
  `wc_off` to the tz database. Weather's search uses the same database (§C2.4).

### A2.2 Alarms

**Layout (S9; E unless marked. There is no Guide art of the list, so the World Clock geometry is reused: same font
family and row pitch.)**
- Nav row: Edit and + (A UG-AL: "Tap Edit, then tap the alarm time").
- "Alarms" large title.
- iOS 26 shows a **Sleep | Wake Up** section first (bed glyph and "Sleep | Wake Up" 17 / 600). **Omit it** (no Health).
  With it gone, iOS shows no "Other" header either, so the alarm rows follow the title directly (E).
- Alarm rows, sorted by time of day (A behaviour), 98 tall, a top separator inset 16:

| Element | Value |
|---|---|
| Time | **64 / 300**, tabular, left 16, baseline +62. AM/PM 32 / 300 after the digits |
| Subtitle | 15 / 400, left 16, baseline +86: the label, then ", " and the repeat text ("Alarm", "Alarm, weekdays", "Water the plants, every Tue and Sat") |
| Toggle | IR §12.4 (64 x 28, green #30D158 when on), right edge 374, vertically centred |
| Off row | time and subtitle in secondaryLabel (60 % white). The toggle is off |

- Repeat text (E iOS wording, our English):
  - "Never" (shown as nothing)
  - "every day", "weekdays", "weekends"
  - otherwise "every Mon, Wed and Fri", in short day names
- **Empty state:** "No Alarms" 20 / 600 centred, secondaryLabel (E). The Edit capsule is hidden.

**Interactions.**
- Tap a row (outside the toggle) to open **Edit Alarm** (A).
- Toggle the switch: light impact, and the alarm is armed or disarmed at once (A).
- Swipe left to **Delete** (A: the Trash button; iOS 26 shows a red capsule with a trash glyph).
- **Edit:** ⊖ badges and a chevron › on each row; the toggles hide; Edit becomes Done (✓).
- **+** opens **Add Alarm** with the time preset to the current time, rounded (E: iOS presets now).
- When an alarm is armed, a toast-like confirmation is **not** shown in iOS (E). Don't add one.
- **Limit:** iOS has no practical limit. The platform now holds **32** (trunk 24a6efa8). At 32, + shows the alert
  "Can't Add Alarm" / "You can have up to 32 alarms." with OK.

### A2.3 Add / Edit Alarm (sheet) (M UG-AL)

The **large-detent sheet** (IR §3.4) is opaque **#1C1C1E** (secondarySystemBackground dark, M). Its groups are
**#2C2C2E** (M).

| Item | iOS (Pro, M UG-AL) | S9 |
|---|---|---|
| Sheet top | y ≈ 59; top corners 33 (IR §3.4) | y **52** |
| ✕ (Cancel) | 44 glass circle #343437, white ✕ 17 / 600; left 16; centre 40 below the sheet top | centre y 92 |
| Title | "Add Alarm" / "Edit Alarm", 17 / 600, centred on the ✕ row | |
| ✓ (Save) | 44 circle, **orange #FF9230 prominent glass**, white ✓; right 16 | |
| Wheel | 2 columns (hour, minute) + an AM/PM column in 12-hour mode. **Row pitch 32**, selected font **24 / 400** white, tabular (M: "9" ink 17.1). Visible ±3 rows on a cylinder (spacing shrinks: 32 / 27 / 22 from the centre, M) and fading to 25 % | wheel centre y **220**, total height ~176 |
| Selection band | **full group width** (16..374), **35 tall**, r ≈ 10, fill #2C2C30 over #1C1C1E (M: band 35.2 tall) | 16..374, y 202..237 |
| Columns | hour right-aligned at x ≈ 175 on the 402 frame; minute left-aligned at ≈ 205; AM/PM at ≈ 270 (M). The wheel is centred, so the hour/minute pair sits slightly left of centre | hour right edge 170, minute left 199, AM/PM left 262 |
| Repeat group | top 362, r ≈ 22 (E: circle fit), inset 16. Row 1: "Repeat" 17 / 400 left, the summary right in secondaryLabel ("Tue and Sat", "Never", "Every Day", "Weekdays", "Weekends"). A separator inset 16. Row 2: **seven day circles** S M T W T F S, **36 pt** (E ±2), **pitch 50** (M), first centre x 50 on the 402 frame; off = #3A3A3C with a white letter 15 / 600, on = **orange #FF9230** with a white letter | group y 324..440; circles row centre y 410, first centre 48, pitch 49 |
| Options group | Label, Sound, Snooze, Snooze Duration. Rows **52.7** (M) | rows **52**; group top 456, height 208 |
| Label row | "Label" left; the value **right, editable inline** (secondaryLabel, 17 / 400; emoji allowed, M "🌺🌱🪴 Water the plants"). Placeholder "Alarm" | |
| Sound row | "Sound" left; the value + chevron right ("Radial" in iOS, M) → pushes the Sound list | |
| Snooze row | "Snooze" + toggle (on by default, A) | |
| Snooze Duration | "Snooze Duration" left; value **orange** "5 min" right (M): a pop-up **menu** (IR §3.3) with 1 to 15 minutes (3P: up to 15) | shown only while Snooze is on (E) |
| Delete Alarm | Edit Alarm only. A separate group row "Delete Alarm" in red, centred (E: iOS ≤ 18 layout) | group top 680 (scrolls) |

- **The sheet scrolls** if it doesn't fit. On the S9 the Delete row sits below the fold (E).
- **Wheel behaviour** (E, UIDatePicker):
  - The wheels wrap (hours 1-12 or 0-23, minutes 00-59).
  - A flick decays with deceleration 0.998 per ms (UIScrollView "normal"), then snaps to the nearest row on a spring
    (response 0.3, damping 1.0).
  - **Selection haptic + a soft "tick" sound for every row that crosses the band** (iOS picker ticks: A behaviour of
    UIPickerView; system click sound when Keyboard Clicks is on, E).
  - Rows off the band are drawn smaller and dimmer: scale `cos(θ)`, alpha 1 → 0.35 at ±3 rows (E).
  - Tapping a row scrolls it to the band.
- **Save (✓):** writes the alarm, arms it (on = true), closes the sheet. Light impact (E).
- **Cancel (✕):** if anything changed, iOS 26 asks with an action sheet anchored to ✕: "Discard Changes" (red)
  (E). Otherwise it just closes. Swipe-down dismiss follows the same rule.
- **Sound list** (pushed inside the sheet; nav title "Sound", back chevron):
  - A **"Haptics"** row first → a sub-list: "Synchronized (Default)", our patterns, "None".
  - Then section **"Songs"**: **omit** (no Music library alarms in v1).
  - Then section **"Ringtones"**: our own alarm tones, a checkmark (orange) on the chosen one, "(Default)" after the
    default. Tapping a row plays a 3-second preview at the alarm volume.
  - Last: **"None"**.
  - **Our own sounds:** we compose originals; names are ours. Never Apple's ("Radial", "Sprinkles" etc. are their
    names; don't reuse them).

### A2.4 Stopwatch (M UG-SW)

**Digital page.**

| Item | iOS (Pro, M) | S9 |
|---|---|---|
| Digits | "00:41.18": **92 / 100 (Thin)**, tabular, centred; ink 64.9 tall, 348 wide | **88 / 100**, so that the 8-character string (≈ 333 wide) fits within 20 margins; baseline y **172** (Pro 241 − 72) |
| Format | `mm:ss.cc` (minutes, seconds, hundredths) with a **period** (M, US English); past 59:59.99 → `h:mm:ss.cc` and the size drops to fit (E) | Use the region's decimal separator from Settings > General > Language & Region (default "."). Trunk prints "," always |
| Lap / Reset button (left) | **86 circle**, fill **#333333**, label 17 / 400 white; left edge 16; centre y 445 (M) | 84 circle, centre (58, **372**) |
| Start / Stop button (right) | 86 circle. Start: fill **#0F3A1B**-ish dark green, label #30D158 (E: same family as the green play, M #204029 on UG-TM). Stop: fill **#330D0E**, label **#FF4245** (M) | 84 circle, centre (332, 372) |
| Button ring | each disc has a 2 pt gap ring of the same fill around it (E iOS ≤ 18; **not visible in the iOS 27 art**, M): **drop the ring** | none |
| Disabled Lap (before Start) | fill #333333 at 50 %, label 50 % (E) | |
| Page dots | 2 dots of **8**, pitch **19**, centred between the buttons on their centre line; current white, other #727272 (M) | centre y 372, x 186 and 205 |
| Laps list | top separator under the buttons at y ≈ 498 (M); rows **44**, "Lap n" left / time right, 17 / 400 tabular, separators inset 16..374 (M) | list from y **425** (11 below the buttons, as on the Pro) to 714 (above the tab bar's scroll edge) |
| Lap colours | the **shortest** finished lap green **#30D158**, the **longest** red **#FF4245**; only once there are ≥ 2 finished laps; the running lap is always white (A/M UG-SW callouts) | |
| Lap order | newest first; the running lap at the top (M: Lap 5 at the top) | |

**Analogue page** (swipe left on the digits or the dots; A "swipe the stopwatch"; E geometry from iOS ≤ 18):
- A full dial, ~300 pt diameter, centred at y ≈ 205. It replaces the digits; the buttons stay where they are.
- 60 s scale: long ticks every 5 s with numerals 5…60 (17 / 400), minor ticks at 1/5 s, white at 60 %.
- A **sub-dial** for minutes (0-30), 70 pt, above the centre. A small digital readout "00:41.18" (17 / 400) below the
  centre.
- **Hands:** the seconds hand is orange #FF9230, 2 pt wide, with a ring hub. The current lap hand is blue #0091FF,
  shown only after the first Lap.
- The hands move smoothly at 60 fps while running (no ticking).
- The page dots follow the swipe; a page settles on a spring with response 0.35, damping 1.0. Selection haptic on
  the page change (E).

**Interactions.**
- **Start** → running: the left button becomes **Lap**, the right **Stop** (red).
- **Stop** → the left button becomes **Reset**, the right **Start**.
- **Lap:** adds a row with a short insert animation (the rows slide down 44 on a spring with response 0.3, damping
  1.0).
- **Reset:** clears the time and the laps (the rows fade out 0.2 s).
- Buttons press-highlight: the fill lightens 15 % while held (E). **No haptic** on these buttons in iOS (E). Ours
  must match.
- The time keeps running in the background and survives the app closing (A). Persist the start time, not a counter.
- A long laps list scrolls under the tab bar with the scroll-edge effect (LG §5.4).

### A2.5 Timers

**Root, no timer running** (E layout; it uses the same parts as the Timer sheet in UG-TM, which is M):
- Large title "Timers" with + hidden. iOS shows + only once a timer exists (E).
- The **wheel**: 3 columns, values with units. **"0 hours"**, **"30 min"**, **"0 sec"**:
  - the number 24 / 400 white, right-aligned in its column
  - the unit 17 / 600 white on the band (M UG-TM)
  - row pitch 32
- The band **308 wide** (M: 46..355 on the 402 frame), 35 tall, centred, fill #2C2C30. On the S9 it is 300 wide
  (x 45..345).
- Column number right edges at ≈ 123, 238 and 363 on the 402 frame (M). On the S9: 118, 230 and 350, units 4 pt
  after.
- Hours 0-23, min 0-59, sec 0-59. The wheels **do not** wrap for hours (E).
- Under the wheel, two round buttons as on the Stopwatch (A2.4):
  - **Cancel** (grey, disabled until a timer runs)
  - **Start** (green; disabled while the wheel reads 0:00:00)
- On the S9: buttons centre y **330**; wheel centre y **200**.
- **Group** (M UG-TM: **327 wide, centred**, r ≈ 22, #2C2C2E): **Label** (inline text, placeholder "Timer") and
  **When Timer Ends** (value + chevron, e.g. our default tone). Rows 52. On the S9 the group is x 37..353, y 392..496.
- **"Presets"** header (20 / 600, M), then a **horizontally scrolling row of circles**:
  - circles **80** in diameter (M 79.3), pitch **98** (M), first left 16, fill #2C2C2E
  - the number 28 / 400 white with "MIN" or "SEC" 15 / 500 **orange** under it (M: "1 MIN", "2 MIN" …)
  - iOS presets (A: "like 1, 5, or 10 minutes"): **1, 2, 3, 4, 5, 10, 15, 20, 30, 45 min, 1 hr, 2 hr** (E list)
  - tapping a preset **starts that timer at once** (A)
- **"Recents"** header 20 / 600, then rows **≈ 100** tall with separators:
  - the duration **64 / 300 in secondaryLabel grey** ("4:00", M UG-TM: ink 44 → 63)
  - the label under it, 15 / 400
  - a **68 green play circle** at the right: fill #204029, glyph #30D158 (M)
  - tap the play circle to start; tap the row to load it into the wheel (E)
  - swipe left to delete
  - A custom timer is added to Recents when it starts (A). Keep the last 10 (E).
- The page scrolls: the presets and recents sit below the fold on the S9.

**One timer running** (E, iOS 17+ layout):
- The wheel is replaced by a **ring** 280 in diameter, stroke 8:
  - track #3A3A3C
  - progress **orange #FF9230**, draining counter-clockwise from 12 o'clock, round caps
  - the trunk draws r 132, width 7: keep, bump to 8
- In the ring:
  - the remaining time "29:59" 64 / 100 (Thin), tabular, centred
  - under it a 🔔 glyph and the **end time** "10:41 AM" 17 / 400 secondaryLabel
  - the label above the digits (15 / 400) if set
- Buttons: Cancel (grey) / **Pause** (orange: fill #3A250B-ish, label #FF9230, E) → **Resume** (green).
- **Paused:** the ring and digits dim to 50 % and the end-time line hides (E).

**Several timers running** (A: "Set multiple timers"; E layout):
- A list of rows, 98 tall:
  - left: the remaining time 52 / 300 white, then the label (or the duration, "30 min") 15 / 400 secondaryLabel
  - right: a **52 circle** holding a circular progress ring (stroke 3, orange) around a pause ‖ / play ▶ glyph
- Tap the circle to pause or resume. Paused rows go grey.
- Swipe left to **Delete** (A "Remove a timer").
- **Edit** → ⊖ badges → Delete (A).
- **+** at the right of the nav row opens the **Timer sheet** (A2.5a).
- A finished timer's row is removed after it is stopped (E).

#### A2.5a Timer sheet (M UG-TM)
- Large-detent sheet as A2.3: **✕** left, title **"Timer"** centred, and a **green ▶ start circle** at the right
  (44 glass circle, ▶ #30D158 / #5CFC84 highlight, M).
- Then the wheel, the Label / When Timer Ends group, Presets and Recents, exactly as on the root.
- Picking a preset or a recent adds and starts it, and the sheet closes (A: "tap a preset").

**When Timer Ends** (pushed list): the same sound list as the alarm's Sound list. The last row is **"Stop Playing"**:
when the timer ends it stops the audio or video playing, then makes no sound (A). Default = our default timer tone.

### A2.6 Alarm and timer firing (full screen and banner)

iOS 26.1 and later: **stopping** an alarm or a timer needs a **slide** ("Slide to stop"); **Snooze** (alarms) and
**Repeat** (timers) stay single taps (3P MacRumors / 9to5Mac). A setting brings back the one-tap Stop: Settings >
Accessibility > Touch > **Prefer Single-Touch Actions** (3P 9to5Mac).

**Locked or screen off → full screen** (E geometry, 3P structure):
- The screen wakes at full brightness.
- The wallpaper is dimmed by black 40 %, with a heavy blur (σ ~30).
- The status band shows as normal.
- Top block, centred:
  - the alarm's **label** ("Alarm" by default) 28 / 600 white, top y ≈ 110 on the S9
  - the current time 96 / 600 white, the lock clock's style (IR §9.1)
- **Snooze:** a large **orange capsule**, 300 x 64, centred at y ≈ 560, "Snooze" 22 / 600 white (E). It is a tap
  (light impact). It is shown only when the alarm's Snooze is on.
- **Slide to stop:**
  - a **glass track** 334 x 64 (r 32) at y ≈ 660..724 (E)
  - a 56 white knob at the left holding a dark ✕ (or ■) glyph
  - the words **"slide to stop"** centred in the track, 20 / 500 white 70 %, with a shimmering highlight that sweeps
    left to right every 2 s (the iPhone's old slide-to-unlock look, 3P)
  - Drag the knob right: the text fades in proportion.
  - Past **80 %** of the track: release → stop, with a success haptic (E). Below 80 %: it springs back (response 0.35,
    damping 0.7).
  - Rigid haptic when the knob hits the end (E).
- With **Prefer Single-Touch Actions** on, the track becomes a plain glass **"Stop"** capsule of the same size (tap).
- Alarm with Snooze **off**: only the slider. The slider sits at y ≈ 600.
- **Timer:** label "Timer" (or its label), the subtitle "Timer done" 17 / 400. **Repeat** (orange capsule) replaces
  Snooze.
- After **Snooze**, the alarm comes back after the Snooze Duration (A).
- During the snooze, the lock screen shows a Live Activity "Alarm — Snoozed" with a countdown and a **Stop** button.
  **Local equivalent:** the status-band orange pill "⏰ 8:59" counting down to the next ring.

**Unlocked and in use → banner** (A for timers: "you get a notification at the top of your screen"):
- iOS 26 shows an **expanded Live Activity** at the top. On the S9 it is our expanded pill (IR §11): 374 x 160, black,
  r 44, at y 6.
- Contents:
  - the label 17 / 600 white
  - "Alarm" / "Timer" 15 / 400 secondaryLabel
  - at the right, **two 52 circles**: **Snooze** / **Repeat** (orange fill, white ↻ or zzz glyph) and **Stop**
    (grey #3A3A3C, white ✕)
- Unlocked, the banner Stop is a **tap** (E: the slide applies on the lock screen; verify).
- From the Lock Screen a timer can also be cleared by swiping it left, then **Clear** (A).

**Sound and haptics.**
- The alarm sound plays even when the phone is in Silent mode or Do Not Disturb is on, through the speaker and any
  headphones (A).
- The volume comes from Settings > Sounds & Haptics > **Alarms and Timers** (iOS 27, A):
  - a **Match Ringtone Volume** toggle; when it is off, a slider
  - a **Change with Buttons** toggle
- The volume ramps from 30 % to the set level over 15 s (E iOS "gentle wake" for non-Sleep alarms: not verified;
  keep it off by default).
- The haptic pattern repeats with the sound ("Synchronized").
- The sound stops after **15 min** with no action, then auto-snoozes if Snooze is on (E iOS); for timers, after 2 min
  (the trunk already does 2 min).

### A2.7 Live Activity (timer) (E; IR §11 for our pill)
- **Lock-screen platter** (IR §5 / §11): 358 x ~92, regular glass, r 26.
  - Left: the **orange countdown** "4:59" 34 / 500 tabular, the label under it 15 / 400.
  - Right: two 44 circles: **Pause** (orange tint, ‖) / **Cancel** (grey, ✕). Tapping them acts without unlocking.
- **Status-band live pill:** compact 126 x 28, a timer glyph (orange) left and "4:59" orange 15 / 600 tabular right.
  Tap opens Clock > Timers. Long-press expands (IR §11) with the same buttons as the platter.
- When the timer ends the activity turns into the firing UI (A2.6).
- **Stopwatch** in iOS 26+ also shows a Live Activity while running (E). Ours: the pill shows "⏱ 0:41" in white. **Do
  it** (S).

## A3. Clock flows

1. **Set a wake-up alarm:** Alarms → + → spin the hours to 6 and the minutes to 30 (a tick for each row) → tap the day
   circles M T W T F (Repeat reads "Weekdays") → Label "Work" → ✓. The sheet slides down; the new row appears in
   order, toggle on. The lock widget shows "6:30 / Tomorrow" (trunk `sh_alarm` path).
2. **Snooze then stop:** it rings at 6:30 on the lock screen → tap Snooze → the screen sleeps; the pill counts down
   9:00 → it rings again → slide to stop → success haptic, the screen returns to the lock screen.
3. **Two timers:** Timers → Presets "5 MIN" (starts) → + → the Timer sheet → wheel 0 / 12 / 0, Label "Pasta" → ▶ →
   the list shows both rows. The pill shows the one ending soonest. Both end → each fires in turn.
4. **Add a city:** World Clock → + → type "hel" → "Helsinki, Finland" → the row appears at the bottom with
   "Today, +0HRS" (the local zone).
5. **Laps:** Stopwatch → Start → Lap three times → Stop → the longest lap is red, the shortest green → Reset.

---

# Part B — Calculator

## B1. Information architecture
- One screen: **the calculator**. Two glass circles at the top:
  - **left: History** (a clock-arrow glyph). It opens the History sheet.
  - **right: Mode** (a calculator glyph). It opens the Mode menu (A: "Change Mode button").
- Mode menu items, in order (A): **Basic**, **Scientific**, **Math Notes**, then a separator and **Convert** (a toggle
  item with a ✓ when on, E).
  - Math Notes: **omit** in v1. It needs handwriting and Notes integration. A future local equivalent is a typed math
    note in our Notes.
  - Convert works with Basic or Scientific (A).
- **Scientific is in portrait** since iOS 18 (A: the Guide says the Rad/Deg key sits "just above the ÷ key" in
  portrait; M UG-CS). Landscape shows a wider scientific layout (A). **On the S9 we are portrait-only: build portrait
  Scientific**, which answers the brief's "propose": no landscape needed.
- **History sheet** → long-press menu, swipe delete, Edit → Clear All.
- **Convert:** the unit picker sheet (categories + list).
- State survives app restarts: mode, Convert, the displayed value, the history (E iOS).

## B2. Screens

### B2.1 Basic (M UG-CB)

| Item | iOS (Pro, M) | S9 |
|---|---|---|
| Background | #000000 | #000000 |
| History / Mode circles | 44 glass circles (fill ≈ #1F1F1F over black), white 20 pt glyphs; left 16 / right 16; centre y 82 | centre y **66**; x 38 and 352 |
| Keys | **5 rows x 4 circles, Ø 86.5, gaps 8** (pitch 94.6 h, 94.3 v), margin 15.8 | **Ø 84, gap 8, margin 15** (4 x 84 + 3 x 8 + 2 x 15 = 390) |
| Key rows | top 343, bottom 808 (66 above the screen bottom) | rows at y **288, 380, 472, 564, 656**; bottom 740 |
| Row 1 | **⌫ · AC/C · % · ÷** (A/M: delete first, since iOS 18) | |
| Rows 2-5 | 7 8 9 × · 4 5 6 − · 1 2 3 + · **+/− 0 . =** (M: the 0 key is a single circle, not double-wide, since iOS 18) | |
| Function keys (⌫, AC, %) | fill **#616161** (M), glyph white | |
| Digits and +/−, "." | fill **#353535** (M), glyph white | |
| Operators | fill **#FF9200** (M, iOS 26 orange: the HIG says #FF9230 dark; the Guide art samples #FF9200-#FF9300). Glyph white | #FF9500 (E midpoint, check on device) |
| Glyph sizes | digits **36 / 400**; AC, % 32 / 400; ⌫ a 28 pt delete.left glyph; operators **36 / 500** symbols (E from the art) | same |
| Expression line | the full expression "38,670÷50,000", **≈ 27 / 400 secondaryLabel**, right-aligned to 374 (M ink 23.4 incl. comma), baseline ~73 above the result's | baseline y **198** |
| Result | **≈ 69 / 400 (Regular, not Light)**, white, right-aligned to 372 (M: "0.7734" ink 48.7) | **68 / 400**, baseline y **266** (22 above the keys) |

- **Key look (iOS 26/27):** flat discs in Basic. In Scientific the capsules show a faint top specular rim (M UG-CS: a
  1 px lighter top edge). **Our build:** flat fills plus a 1 px top rim at white 10 %. **Not** full Liquid Glass
  (LG §5.5: content keys are not glass).
- **Press:** the key's fill **lightens** while held (digit #353535 → #737373, function #616161 → #D4D4D2, operator →
  #FCC78D), and fades back over 0.3 s on release (E, iOS ≤ 18 values). There is **no haptic and no sound** by default
  (E/3P: the iOS Calculator has no key haptics). **Remove the trunk's `sh_haptic` on keys.**
- **Pending operator highlight:** after tapping an operator and before the next digit, that operator key is drawn
  **white with an orange glyph** (E: iOS ≤ 17 rule; the iOS 27 art shows no pending state, so verify). Keep the
  trunk's behaviour.

**Display behaviour (A/E).**
- **Grouping:** thousands separators per region ("38,670") (M).
- **Digit limit:** 9 digits in Basic portrait, 16 in Scientific (E). After that, digit taps are ignored.
- **Auto-shrink:** the result shrinks to fit the width from 68 down to **44**, then the long expression wraps onto a
  second expression line (E). Results past 9 digits switch to scientific notation "1.23457e14" (trunk OK).
- **AC / C:** shows **AC** when nothing is being entered. It becomes **C** once a digit is typed (M: UG-CS shows C
  mid-entry, UG-CB shows AC after =).
  - **C** clears the last number entered (A), and the key goes back to AC.
  - **Touch and hold C or ⌫** clears everything (A).
- **⌫** deletes the last digit (A). **Swiping left or right on the display** also deletes the last digit (A, long-
  standing; keep).
- **Copy / Paste:** touch and hold the result → an edit menu (the iOS 26 glass capsule menu, IR §3.3, horizontal):
  **Copy** (A), plus **Paste** when the clipboard holds a number (E). Medium impact on the long-press.
- **Repeat =:** tapping = again repeats the last operation on the result (A: "1 × 2 = = = → 4, 8, 16"). A new number
  then = applies the last operation to the new number (A: "100 + 15% = 115, then 150 = → 172.5").
- **%:** in an expression "a + b%" means a + a·b/100 (A example). On its own it divides by 100.
- **Error:** "Error" on division by 0 (iOS 18+ shows "Error", E). Any key clears it.

### B2.2 Scientific (portrait) (M UG-CS)

| Item | iOS (Pro, M) | S9 |
|---|---|---|
| Display | a single expression line that **is** the result line, e.g. "1÷sin(π×e!)", **≈ 62 / 400** white, right-aligned, shrinking to fit (M ink 45 → 64 incl. parentheses) | baseline y **175**, 56 → 32 shrink |
| Function grid | **5 rows x 6 capsules**, each **47 x 37**, pitch 63.1 h / 57.3 v, margin 20, fill **#262626-#272727** (M), labels 17 / 400 white | **46 x 35, pitch 61.2 h / 54 v, margin 19.5**; rows top y **214** |
| Rows (A/M) | `(` `)` `mc` `m+` `m-` `mr` · `2ⁿᵈ` `x²` `x³` `xʸ` `eˣ` `10ˣ` · `¹⁄ₓ` `²√x` `³√x` `ʸ√x` `ln` `log₁₀` · `x!` `sin` `cos` `tan` `e` `EE` · `Rand` `sinh` `cosh` `tanh` `π` `Rad` | |
| Lower block | the Basic keys as **capsules 86.5 x 38**, same 4 columns and colours (M) | **84 x 35**, row pitch 54, rows y 484, 538, 592, 646, 700 (bottom 735) |
| Rad state | tapping **Rad** sets radians: the key reads **Deg** and a small "Rad" 15 / 500 label appears at the display's left (A) | "Rad" at x 20, on the display baseline |
| 2nd | toggles inverse forms: sin → sin⁻¹, eˣ → yˣ, 10ˣ → 2ˣ, ln → log_y, log₁₀ → log₂ (A: "yˣ or powers of 2"); 2ⁿᵈ is drawn highlighted (#616161) while on (E) | |
| Memory | the mc / m+ / m- / mr keys (A). mr is drawn highlighted while memory ≠ 0 (E) | |

- The order of operations is standard: × before + (A). Expressions show in full, with parentheses, before = (A).
- **Maths:** needs double precision maths in the shell: sin / cos / tan / log / exp / pow / gamma for x!. The trunk
  comment notes "the double maths the kernel doesn't have". Port the functions needed from **musl's libm** (MIT; Track A's decision); this is an **L** item. Rand = uniform
  [0, 1) from the kernel RNG.

### B2.3 Convert (M UG-CV; A text)
- Turning Convert on adds **two rows** to the display:
  - the **from** value: white, ≈ 46 / 400, right-aligned, then a unit tag "USD ⌃⌄" (15 / 600 secondaryLabel, a
    chevron pair). M: "1,000" ink 34.3 → 48.
  - a **0.5 pt separator**
  - the **to** value: secondaryLabel ≈ 40 / 400, "876.90", then its own unit tag "EUR ⌃⌄" (M ink 28.9 → 41)
- A **⇅ swap** glyph, orange 20 pt, sits at the left on the separator line (A "Swap Units button"). Tapping it swaps
  the units with a 0.3 s cross-slide (E).
- **Tapping a unit tag** opens a **large sheet "Units"**:
  - category chips across the top that scroll horizontally (A: "Swipe the categories… Speed, Time, Weight"). Chips are
    32-tall capsules: selected = white fill with black text, others = #2C2C2E.
  - Category list (A + E order): Angle, Area, Currency, Data, Energy, Force, Fuel, Length, Power, Pressure, Speed,
    Temperature, Time, Volume, Weight.
  - Below: two columns of rows (**From** / **To**) or one list with a ✓ (E): rows 44, "US Dollar" 17 / 400 + "USD"
    15 / 400 secondaryLabel; a search field at the top.
- Either field can be edited; the other converts as you type (A).
- **Currency: local equivalent.** Rates need the internet.
  - Online: fetch daily rates from **Frankfurter** (the ECB's reference rates, free, no key; Track A's decision) and cache
    them with a timestamp.
  - Offline with a cache: convert and show "Rates from 12 Nov" 13 / 400 secondaryLabel under the to-row.
  - Offline and never fetched: the Currency chip shows "Currency rates need the internet." centred in the list.
    Units always work offline.

### B2.4 History sheet (A text "See previous calculations"; E geometry)
- **Medium detent sheet** (IR §3.4), expandable to large. Its title is "History" 17 / 600. **Edit** (glass capsule) is
  at the right.
- A plain list grouped by day: headers "Today", "Yesterday", then dates, 15 / 600 secondaryLabel.
- Rows 60 tall:
  - the expression "38,670÷50,000" 15 / 400 secondaryLabel
  - the result "0.7734" 22 / 400 white
  - both right-aligned (E)
- **Tap a row:** reloads the calculation into the calculator, "where you left off" (A), and the sheet closes.
- **Touch and hold:** a menu with **Copy Expression** and **Copy Result** (A).
- **Swipe left:** Delete (red) (A).
- **Edit:**
  - selection circles appear on the rows
  - the bottom toolbar shows **Clear All** (left) and **Delete** (right, red, enabled when rows are selected) (A)
  - Clear All → a confirmation action sheet with **"Clear History"** (red) (A)
- **Empty:** "No History" 20 / 600 centred, secondaryLabel (E).
- Keep the last 100 calculations (E). They are stored locally only (no iCloud).

### B2.5 Mode menu (E geometry, A items)
- A glass menu morphing out of the Mode circle (IR §3.3 / LG §5.3: response 0.40, bounce 0.20), anchored top-right,
  ~220 wide.
- Rows 44 tall: **Basic** (calculator glyph), **Scientific** (function glyph), **Math Notes** (pencil glyph; **omit**),
  then an 8 pt section gap, then **Convert** (arrows glyph, with a ✓ when on).
- The current mode has a ✓ at the leading edge.
- **Switch animation:** the keys cross-fade and resize (0.35 s spring, damping 1.0). In Basic → Scientific the 4
  bottom key rows shrink from discs into capsules in place, and the function grid fades in above them (E).

## B3. Calculator flows
1. **12 % tip:** 48.50 + 12 % = → 54.32. The expression "48.5+12%" shows above the result.
2. **Fix a typo:** type 1234 → swipe left on the display → 123.
3. **Copy:** touch and hold 54.32 → Copy → paste it in Messages.
4. **Convert:** Mode → Convert on → tap the unit tag → Length → From: Mile, To: Kilometre → type 26.2 → 42.165 updates
   live.
5. **Reuse:** History → tap "48.5+12%" → it reloads.

---

# Part C — Weather

## C0. What Weather can be on our phone
- iOS Weather is Apple's forecast service. **Local equivalent:** a provider abstraction. When the phone is online,
  fetch from **Open-Meteo** (free, no key; Track A's decision), with the location **rounded to ~1 km** before the
  request. **Cache every fetched location** (current conditions, 24 h hourly, 10 days, the details) with its
  time.
- **My Location:**
  - with a GNSS fix (Starbite location service, when present), else
  - the last fix, else
  - none: the list starts with the user's home city chosen from the city database, A2.1a.
- Omit: severe weather alerts (unless the provider supplies them), news, Report an Issue, and notifications
  (v2, local equivalent: a rain-soon notification from the cached next-hour data).
- Omit: air quality and maps (no tile service) unless the provider supplies them. The map module is described for
  completeness; build it only with a tile source.

## C1. Information architecture
- **Location pages**, swiped left and right (A): My Location first, then the saved locations. Each is a scrolling
  page.
  - Bottom bar (M UG-WM):
    - **Map** circle (left) → the full-screen map (omit without tiles)
    - the **page-dot capsule** (centre)
    - the **Locations list** circle (right)
  - Tapping a module → its **detail sheet** (the iOS 27 hourly / 10-day chart sheet; tile details).
- **Locations list:**
  - large title "Weather"
  - an **… menu**: Edit List, Units, Notifications (omit), Report an Issue (omit)
  - sky cards
  - the **search** field at the bottom (A: "top or bottom of the screen")
  - Search → results → a **preview page** with **Cancel** / **Add** (A)
- Units (A): °C, °F or "Use system setting"; wind km/h, mph, m/s, knots, Beaufort; precipitation mm or in; pressure
  hPa, inHg, mmHg, kPa; distance km or mi. Defaults follow the region (E).

## C2. Screens

### C2.1 Location page (M UG-WM unless marked)

**Sky background (full-bleed, behind everything).**
- One animated scene per **condition x day/night** (A: the icon list: clear, mostly clear, partly cloudy, haze, fog,
  windy, cloudy, thunderstorm, rain, heavy rain, drizzle, snow, heavy snow, sleet / freezing rain / wintry mix; each
  with a night variant).
- **Our own art:** a GPU sky shader per condition. Never Apple's video or images.

| Scene | Base gradient top → bottom (ours) | Motion (E iOS behaviour) |
|---|---|---|
| Clear day | #3F7FC2 → #6FAEE6 (M: the sky in UG-WM samples #4C80B5 high, #5390CB low) | sun glow at the top left, slow lens bloom |
| Partly cloudy day | same + volumetric clouds | clouds drift right at ~4 pt/s, two parallax layers |
| Cloudy | #5E6E80 → #8A98A8 | heavy cloud deck, slow drift |
| Rain / drizzle / heavy rain | #3B4756 → #5D6B7A | streaks at 15° angle: 120 / 300 / 600 drops per screen; **drops land and splash on the tops of the modules** (an iOS 16+ trait, E) |
| Thunderstorm | #262C38 → #424B5A | rain + a lightning flash every 6-15 s (a white flash 0.08 s, then a 0.4 s fade) |
| Snow | #6E7C8C → #A6B2BF | flakes 1-4 pt, drifting, settling on module tops |
| Fog / haze | #8A929A → #B5BAC0 | soft noise layers sliding slowly |
| Clear night | #0B1630 → #1D2D50 | stars that twinkle, a slight moon glow |
| Night cloudy / rain | #121A26 → #26303E | as by day, darker |

- The sky follows the page while it scrolls: it parallaxes at 30 %. During a sideways swipe the two scenes
  **cross-fade** by the swipe progress (E).
- Reduce Motion: a still frame.

**Header (expanded, at scroll 0).** iOS (Pro) → S9 (−18 for the status band; the header is chrome, so nothing more):

| Element | iOS (Pro, M) | S9 |
|---|---|---|
| "MY LOCATION" (only on the My Location page; a home glyph or nothing on others) | 13 / 600 caps, tracking +0.5, white, centred; top 114 | top 96 |
| City | **32-34 / 400** white, centred (M ink 28.8 → 32); cap top 134 | 32 / 400, baseline 139 |
| Temperature | **≈ 100 / 100 (Thin)**, white, "63°": the degree sign hangs right of centre so that the digits stay centred (M: ink 71 tall → 101) | **96 / 100**, digits top 158, baseline 226 |
| Condition | "Partly Cloudy" ≈ 20 / 500 white (M) | baseline 274 |
| H / L | "H:78°  L:53°" ≈ 20 / 500 white (M) | baseline 298 |
| First module top | 384 (M) | **346** (the S9 takes 20 here; the remaining 34 come out of the scrolling content) |

**Compact header (scrolled; M UG-WD).**
- As the page scrolls up, the temperature, the condition and H/L fade out and slide up within the first ~110 pt of
  scroll.
- The header settles as two centred lines:
  - "MY LOCATION" caps (top 77 on the Pro)
  - the city 34 / 400
  - **"63° | Partly Cloudy" 20 / 600** under it (M: "63°  |  Partly Cloudy")
- Compact header bottom ≈ 168 on the Pro, **≈ 150** on the S9.
- The header **stays pinned**. The modules scroll under it and are **clipped at its bottom edge** (no blur band).
  Each module's **title row sticks** at the clip line while its body slides up beneath it and collapses, until the
  next module's title pushes it away (E: iOS 16+ behaviour, matches UG-WD).

**Modules (cards).**
- All modules: **x 20..370 on the S9** (M: margin 20, width 353 on the Pro), **corner radius ≈ 18** (M: circle fit
  16-17, built continuous 18), gap **10** (M 10.8).
- **Fill:** a darkening frosted material over the sky: the backdrop blurred σ ~20 + black 12 % (M: the module samples
  #2F75BA over sky #4C80B5). On dark skies, white 8 % instead. This is a **content material, not Liquid Glass**
  (iOS 26 kept Weather's modules as blur cards, E).
- **Module title row:** a glyph 13 pt + a CAPS title 13 / 600, white 60 % ("💧 PRECIPITATION", "☀︎ UV INDEX"), inset 16,
  top 12. An optional 0.5 pt separator at white 20 % below the title in list modules (E iOS ≤ 18; the iOS 27 hourly
  module has none, M).

Module order, top to bottom (iOS 27, E order; A presence):

1. **Highlights** (iOS 27, 3P):
   - a module with one to three short sentences on the day's notable weather ("Rain likely around 4 PM. Gusts up to
     45 km/h.")
   - 17 / 500 white, left-aligned
   - shown only when there is something to say
   - **Local equivalent:** generate the sentences from rules over the cached hourly data (rain start/stop, wind
     gusts > 40 km/h, temperature swing > 10°, UV ≥ 6).
2. **Hourly module** (M UG-WM), Pro height 182:
   - Title row: **"Conditions"** 17 / 600 white (not caps) and under it the subtitle "Temperature (°C)" 13 / 400 in
     the light accent blue #8FD3FF (M).
   - At the right, a **3-segment glass switch** 130 x 32 (A: Conditions / Precipitation / Wind; M glyphs cloud-sun,
     drop, wind), r 16. The selected segment is a white 25 % pill. Selection haptic on change; the content
     cross-fades 0.25 s.
   - Then a horizontally scrolling row of hours, **column pitch 58** (M):
     - "Now", "10AM" … 15 / 600 (M), with the AM/PM at 11 / 600
     - a 26 pt multicolour condition glyph (our art)
     - the temperature 17 / 500 (M ±1)
     - with a precipitation chance ≥ 10 %, a cyan "30%" 12 / 600 under the glyph
   - **Sunrise / sunset** insert their own column ("06:51", "Sunrise", a sun glyph) (E iOS).
   - In **Precipitation** mode: the glyph row becomes blue bars of chance.
   - In **Wind** mode: arrows rotated to the direction, and speeds.
3. **10-day module** (M UG-WM):
   - Title row "10-DAY FORECAST" with a calendar glyph (E iOS ≤ 26; the iOS 27 art scrolls it off).
   - **Rows 50** (M 50.1). Each row:
     - day ("Today", "Thu"…) 20 / 600 at x 16
     - glyph 24 at x ≈ 120 (precipitation % in cyan 12 / 600 under the glyph when ≥ 10 %)
     - low "53°" 20 / 500 in white 60 % (M: tinted blue #8FD3FF-ish), right-aligned to x 196
     - **range bar**
     - high "78°" 20 / 500 white, right-aligned to 334
   - **Range bar** (M): a 4 pt capsule track 100 long (x 202..302 on the S9) at white 15 %.
     - The filled span covers that day's low → high on a **scale shared by all ten days**.
     - Its gradient maps temperature to colour: ≤ 0 °C #8FD3FF blue · 10 °C #9BD187 green · 20 °C #F8CB06 yellow
       · 30 °C #FF9230 orange · ≥ 38 °C #FF4245 red (M: #9BD187 → #F8CB06 sampled across 53-78 °F).
   - **Today** only: a **6 pt white dot** with a 1.5 pt dark ring at the current temperature (M).
   - The switch in the hourly module also changes this module to precipitation or wind (3P iOS 27).
   - Tap → the 10-day **detail sheet**.
4. **Precipitation map module** (E): a square-ish module (350 x 300) with the map, a blue location dot and the
   temperature in a white capsule. Tap → the full-screen map. **Omit** without tiles.
5. **Air quality** (M UG-WD), full width, ~177 tall:
   - the AQI number 36 / 500
   - the category word 20 / 600
   - a 4 pt rainbow scale bar with a white dot
   - one sentence 15 / 400
   - **Omit** unless the provider gives AQI.
6. **Tiles** (M UG-WD): **two per row, each 171 x ~171** on the Pro (M width 171, gap 11.5). **S9: 170 x 170, gap
   10** (x 20..190 and 200..370). Each tile:
   - a caps title row (glyph + 13 / 600, white 60 %)
   - a big value **34 / 500** white ("4", "8:32PM")
   - a 20 / 600 word ("Moderate")
   - a small graphic
   - a 13-15 / 400 footer sentence at the bottom

| Tile | Big value / graphic / footer (ours, iOS-shaped) |
|---|---|
| **UV INDEX** | "4" / "Moderate"; a 4 pt gradient bar green → violet with a white dot; footer "Use sun protection until 18:00." |
| **SUNSET** (or SUNRISE if before sunrise) | "20:32"; a sine curve with the sun dot on it, the horizon line; footer "Sunrise: 05:51" (M UG-WD) |
| **WIND** (full width, M UG-WD) | left: "Wind 4 km/h", "Gusts 6 km/h", "Direction 141° SE" rows (17 / 500, values in white 60 %); right: a 120 pt compass dial with N/E/S/W and an arrow, the speed in the middle |
| **PRECIPITATION** | "0 mm" / "Today"; footer "Next expected is 2 mm on Fri." |
| **FEELS LIKE** | "61°"; footer "Similar to the actual temperature." / "Wind is making it feel colder." |
| **HUMIDITY** | "72%"; footer "The dew point is 9° right now." |
| **VISIBILITY** | "16 km"; footer "Perfectly clear view." |
| **PRESSURE** | "1 016 hPa"; a gauge arc with a ↑ / ↓ / = trend arrow |
| **AVERAGES** | "+3°" / "above average daily high" (A: tap for details); omit without history data |
| **MOON** (full width, M UG-WD: "WANING GIBBOUS") | rows Illumination / Moonrise / Next Full Moon + a 120 pt moon drawn from the phase. **Computable offline** (astronomy): always available |

- Sunrise, sunset and moon are **computed on the phone** from latitude, longitude and date, so they work offline.
- Tapping a tile opens its detail sheet (A: "Tap a weather detail for more information"): a large sheet with a day
  strip at the top (dates as circles), a chart (24 h), and a short text. **E geometry**; build after the main page.
- The page ends with **"Weather for Helsinki"** and the data source name (13 / 400, white 60 %) + "Last updated
  14:05" (local equivalent of Apple's attribution footer, E).

**Bottom bar (M UG-WM).**
- **Map circle 48** (glass), centre x 51 on the Pro / **x 50** on the S9.
- **List circle 48**, x 340 (S9).
- Between them a **glass capsule 140 x 47** with a ➤ location arrow (the My Location page) and **6.2 pt dots,
  pitch 17**. The current dot is white; the others are white 50 %.
- Centre y 50 above the screen bottom on the Pro → **S9 y 756**.
- No tab bar. The modules scroll under the bar with a soft scroll edge (LG §5.4).
- Tapping a dot pages to that location. Dragging along the capsule scrubs between pages, with selection haptics
  (E iOS).

**Interactions.**
- Swipe left / right → the previous or next location: the page and the sky cross-fade, and the dot moves (spring
  0.40 / 1.0).
- **Pull-to-refresh: not in iOS Weather** (E: it refreshes on open). Ours refreshes on open and every 30 min while
  open and online.
- Tap the hourly or 10-day module → its chart sheet.
- Long-press: none.

### C2.2 States and words (local equivalents)

| State | What shows |
|---|---|
| **Loading** (first fetch of a new location) | the sky of the last known condition (else clear day); the header shows the city and a **"--°"** temperature; each module is a placeholder with its title and a shimmering body (white 8 % → 14 %, 1.2 s loop) (E iOS) |
| **Offline, cache present** | everything from the cache. A small glass capsule under the header: "📶✕ Offline · Updated 2 h ago" 13 / 500. Times are "Updated" relative: "just now", "5 min ago", "2 h ago", "yesterday" |
| **Offline, cache older than 12 h** | as above. "Now" values (temperature, condition) show the cached hour's forecast for the current hour, and the header line reads "Forecast from 12 Nov 08:00" |
| **Offline, no cache** (never fetched) | the sky "Cloudy" scene dimmed; a centred block: a cloud-slash glyph 48, **"Weather Unavailable"** 22 / 600, "Weather needs the internet. Connect to Wi-Fi or mobile data and it will update." 15 / 400 white 70 %, and a glass **"Open Wi-Fi Settings"** capsule (S9 Settings > Wi-Fi) |
| **Location off / denied** (My Location) | My Location's page shows the same block: **"Location Unavailable"**, "Turn on Location Services for Weather in Settings to see the weather where you are." and an "Open Settings" capsule (A wording family) |
| **No fix yet** | the My Location header shows "Finding Your Location…" in place of the city, with the cached city's data under it if any |
| **Provider error** | a toast-free inline line at the top: "Weather couldn't update. Try again later." 13 / 500 in a glass capsule |

### C2.3 Locations list (M UG-WL)

| Item | iOS (Pro, M) | S9 |
|---|---|---|
| Background | dark navy behind the cards (≈ #224A73 from the current sky, darkened) | the current location's sky, blurred σ 40 and darkened 50 % |
| Title | "Weather" 34 / 700 white, x 16 | baseline 124 |
| **…** | 44 glass circle top right (M) | centre (352, 66) |
| Cards | **x 16..386 on the Pro (368 wide), 110 tall, gap 12, r ≈ 20 (E)**; each holds **its own location's sky, still, cropped** (M) | x 16..374 (358 wide), h **108**, gap 12, first top 142 |
| Card text (inset 16) | City **22 / 700** (M ink 19.4 incl. descender); the second line "My Location" or the local time "9:41 AM" 13 / 600; bottom left the condition 13 / 500; top right the temperature **≈ 48 / 300** (M ink 33.5 → 48); bottom right "H:78° L:53°" 13 / 500 | same |
| Alert line | "⚠ Extreme Heat Warning & 2 More" replaces the condition (M). **Omit** without alerts | |
| Search | a **bottom glass capsule 336 x 46** (M), magnifier, placeholder **"Search for a city or airport"** (M) 17 / 400 white 60 %, mic glyph right (**omit the mic**: no dictation) | x 28..362, y 728..774 |

- Tap a card → the list zooms into that location's page (the card expands into the full page, matched geometry,
  response 0.45, damping 0.85).
- Drag (touch and hold, then move) to **reorder** (A). Swipe left → **Delete** (red, trash) (A).
- **… menu** (glass, IR §3.3):
  - **Edit List**: cards shrink into rows with ⊖ and ≡ while the cards stay drawn
  - **Units** (opens a sheet)
  - **Notifications**: omit
  - **Report an Issue**: omit
  - a section with **Celsius** / **Fahrenheit** checkmarks (E iOS ≤ 26 had them in the menu)
- **Search:**
  - Tapping the field raises it above the keyboard; an ✕ circle appears (IR §10).
  - The cards dim and blur.
  - Typing lists matches: rows 44, "Helsinki" 17 / 400 white with the match in bold, "Uusimaa, Finland" 15 / 400
    secondary.
  - Tapping a match opens the **preview** as a large sheet: the location's full page with **Cancel** (✕ left) and
    **Add** (prominent, right) (A).
  - Offline, the preview shows the "Weather Unavailable" block, but **Add still works** and the forecast loads later.
- **Local equivalent of search:** the on-device city database (A2.1a), so search works offline. "Airport" codes
  (IATA) are in the same table, E.

### C2.4 Full-screen map (omit unless we have tiles)
- Layers: precipitation, temperature, air quality, wind (A).
- The layer button is a glass circle at the top right; a list button at the left; a ✕ close.
- A timeline scrubber at the bottom: 1h / 12h for precipitation (A).
- Touch and hold a place → **Add [place]** / **View [place]** (A).
- Without tiles: no map circle in the bottom bar. The bar keeps only the dots and the list circle, with the capsule
  centred.

## C3. Weather flows
1. **First open, online, location on:** a "Finding Your Location…" header → fetch → the sky fades in (0.6 s), and the
   modules rise 12 pt and fade in, staggered 40 ms each (E).
2. **Add a city:** list circle → the search field → "Tamp" → "Tampere" → the preview → Add → the list shows the new
   card at the bottom; the page dots grow to 3.
3. **Offline morning:** open → the cached page with "Offline · Updated 9 h ago"; the 10-day still shows.
4. **Switch to wind:** the hourly module switch → the Wind segment → hourly shows arrows and km/h; the 10-day rows show
   wind ranges.

---

## 4. Motion table

| Trigger | What moves | Spring / duration |
|---|---|---|
| Clock tab change | the selected pill slides and stretches; the page content cross-fades | 0.35 / 0.75 (IR §10); content 0.2 s |
| Add / Edit Alarm, Timer sheet, Choose a City | the sheet slides up from the bottom; the page behind scales 0.92 | 0.45 / 1.0 (IR §3.4) |
| Wheel flick | rows decelerate (0.998 per ms), then snap to the nearest row | snap 0.30 / 1.0 |
| Wheel row crossing the band | selection haptic + tick sound | — |
| Alarm toggle | IR §12.4 | 0.30 / 0.70 |
| Row delete (swipe) | the row collapses its height to 0; rows below move up | 0.35 / 1.0 |
| Edit mode enter / exit | ⊖ badges slide in from the left 30 pt; content shifts right 38; toggles fade | 0.35 / 1.0 |
| Stopwatch Lap | the rows shift down 44, the new row fades in | 0.30 / 1.0 |
| Stopwatch digital ↔ analogue | horizontal paging; the dots cross-fade | 0.35 / 1.0 |
| Stopwatch / timer buttons | the label cross-fades Start ↔ Stop, the fill colour cross-fades | 0.2 s ease |
| Timer start (wheel → ring) | the wheel fades out 0.2 s; the ring draws from empty to full (0.4 s ease-out), then drains linearly | |
| Timer ring drain | continuous, frame-locked (no stepping) | linear |
| Firing full screen appear | the wallpaper dims, the content scales 1.08 → 1.0 and fades in | 0.45 / 0.85 |
| Slide to stop | the knob follows the finger; on release under 80 % it springs back; shimmer sweep 2 s loop | back 0.35 / 0.70 |
| Live pill expand / collapse | IR §11 morph | 0.45 / 0.75 |
| Calculator key press | fill lightens at once; fades back on release | 0.3 s ease-out |
| Calculator result change | no animation (instant); the auto-shrink resizes instantly | — |
| Calculator mode switch | keys morph discs ↔ capsules; the function grid fades in | 0.35 / 1.0 |
| Convert on / off | the to-row slides down from the from-row; the separator draws | 0.35 / 1.0 |
| Swap units (⇅) | the two values cross-slide vertically | 0.3 s |
| Mode menu | morph from the Mode circle (LG §5.3) | 0.40 / bounce 0.20 |
| History sheet | medium detent (IR §3.4) | 0.45 / 1.0 |
| Weather page swipe | page slide + sky cross-fade by progress | release 0.40 / 1.0 |
| Weather header collapse | scroll-driven (no spring): over 0..110 pt of scroll, temp / condition / H-L alpha 1 → 0 and y −40 | scroll-linked |
| Module title pinning | scroll-driven clipping | scroll-linked |
| Sky scenes | continuous animation, 30 fps is enough, paused when the app is in the background | — |
| Locations card → page | card zoom (matched geometry) | 0.45 / 0.85 |
| Search field focus | the field rises with the keyboard (IR §4) | 0.38 / 1.0 |
| Reduce Motion | slides and morphs → 0.25 s cross-fades; skies still | (A) |

## 4b. Haptics and sounds (IR §17 names)

| Event | Haptic | Sound |
|---|---|---|
| Wheel row passes the band | selection | the picker tick (system click; only when Keyboard Clicks is on, E) |
| Alarm / timer toggles | light | — |
| Swipe-action threshold | selection | — |
| Edit-mode drag lift | medium | — |
| Alarm firing | the chosen pattern, repeating ("Synchronized" with the tone) | the alarm tone, looping, ignoring Silent mode / Focus (A) |
| Timer done | the same | the When Timer Ends tone, loop ≤ 2 min |
| Snooze / Repeat tap | light | the tone stops |
| Slide to stop: the end reached | success | the tone stops |
| Stopwatch, Timer and Calculator buttons | **none** (E/3P iOS) | none |
| Weather segment switch, page-dot scrub | selection | — |
| Sheet lands on a detent | soft (IR §3.4) | — |

Our tones (originals; our names):
- **Alarm, default "Daybreak":** a rising three-note motif that repeats.
- **Timer, default "Chime Up".**
- Others: "Beacon", "Ripple", "Ascent", "Glass Bells", "Classic Ring" (like a bell clock), "Pulse".
- Each is a 20-40 s loopable OGG, made by us.

## 5. Words (fixed UI strings, our English)

**Clock:**
- **Tabs and titles:** World Clock · Alarms · Stopwatch · Timers · Edit · Done · Choose a City · Search · Cancel ·
  Add Alarm · Edit Alarm · Timer
- **World Clock:** Today · Yesterday · Tomorrow · HRS · No World Clocks · No Results
- **Alarms:** No Alarms · Alarm (the default label) · Repeat · Label · Sound · Snooze · Snooze Duration ·
  Delete Alarm · Discard Changes · Never · Every Day · Weekdays · Weekends · every · and · min
- **Sounds:** Haptics · Synchronized (Default) · Ringtones · None · (Default) · Stop Playing
- **Day initials / short names:** S M T W T F S · Sun Mon Tue Wed Thu Fri Sat
- **Stopwatch:** Start · Stop · Lap · Reset · Lap 1 …
- **Timers:** Cancel · Start · Pause · Resume · When Timer Ends · Presets · Recents · MIN · SEC · HR · hours · min
  · sec
- **Firing:** slide to stop · Stop · Snooze · Repeat · Timer done · Snoozed
- **Limits:** Can't Add Alarm · You can have up to 32 alarms. · OK

**Calculator:**
- **Modes and history:** Basic · Scientific · Math Notes (omit) · Convert · History · Edit · Clear All ·
  Clear History · Delete · Copy · Copy Expression · Copy Result · Paste · No History · Today · Yesterday
- **Display:** Error · Rad · Deg
- **Convert:** Units · From · To · Search · "Rates from {date}" · "Currency rates need the internet."
- **Unit categories:** Angle · Area · Currency · Data · Energy · Force · Fuel · Length · Power · Pressure · Speed ·
  Temperature · Time · Volume · Weight

**Weather:**
- **Page:** MY LOCATION · Now · Sunrise · Sunset · Today · Conditions · Precipitation · Wind · Temperature (°C) ·
  10-DAY FORECAST · H: · L: · Highlights
- **Tile titles:** UV INDEX · SUNSET · SUNRISE · WIND · Gusts · Direction · PRECIPITATION · FEELS LIKE · HUMIDITY ·
  VISIBILITY · PRESSURE · AVERAGES · MOON phases (New Moon, Waxing Crescent, First Quarter, Waxing Gibbous, Full Moon,
  Waning Gibbous, Last Quarter, Waning Crescent) · Illumination · Moonrise · Moonset · Next Full Moon · DAYS
- **UV words:** Low · Moderate · High · Very High · Extreme
- **List and search:** Weather · Edit List · Units · Celsius · Fahrenheit · Use System Setting · Search for a city or
  airport · Add · Cancel · My Location
- **States:**
  - Offline · Updated just now / {n} min ago / {n} h ago / yesterday
  - Finding Your Location…
  - Weather Unavailable · Weather needs the internet. Connect to Wi-Fi or mobile data and it will update. ·
    Open Wi-Fi Settings
  - Location Unavailable · Turn on Location Services for Weather in Settings to see the weather where you are. ·
    Open Settings
  - Weather couldn't update. Try again later.
  - Forecast from {date time} · Last updated {time}
- **Conditions:** Clear · Mostly Clear · Partly Cloudy · Mostly Cloudy · Cloudy · Haze · Fog · Windy · Breezy ·
  Drizzle · Rain · Heavy Rain · Thunderstorms · Snow · Heavy Snow · Blizzard · Sleet · Freezing Rain · Wintry Mix

## 6. Starbite today (trunk 1c166b85) → gap

**What the trunk has.**
- **Clock** (`shell_apps.inc` 267-985, `shell_time.inc`, `shell_lock.c` 3232+):
  - a 4-tab glass bar at y 718 with drawn glyphs and an orange tint
  - World Clock: a **fixed** 5-city table with EU/US DST rules; rows 96 with city 24 / time 50 / offset 14
  - Alarms: on the platform's **4** alarms (`sh_alarm_*`); rows with time 52 + AM/PM 30, the label + repeat line, a
    toggle
  - Add/Edit Alarm as a **full page** with Cancel/Save capsules, a 2-column 24-h wheel (pitch 36, font 23, a W 12 %
    band), Repeat as an **action sheet** (Never / Every Day / Weekdays / Weekends), and "Label can't be changed yet"
  - Stopwatch: digits 84 regular, `mm:ss,cc` with a comma; 80 discs with a gap ring; laps with best green / worst red
  - Timers: one timer; a 3-column wheel; a ring r 132 orange; "When Timer Ends: Vibrate"; the end alert is "Timer" /
    "The timer has ended." with Stop; the beeps last ≤ 2 min
  - `sh_haptic` on every tab and button
- **Calculator** (`shell_apps.inc` 991-1330):
  - Basic only, 80 discs at pitch 92 from y 318
  - the iOS 18+ key order (⌫ AC % ÷ … +/− 0 . =)
  - colours fn #5C5C5F / digit #2A2A2C / op orange
  - the expression line 26 over the result 86 (shrinks to 36)
  - pending operator lit white
  - 9 digits, then scientific notation
  - landscape = the same 20 keys as capsules
  - `sh_haptic` per key
  - no C state, no history, no modes, no convert, no copy, no swipe-delete
- **Weather:** a placeholder page ("Weather needs the internet / Not available yet"). The home key can open GNOME
  Weather on the Linux side (`shell_appstore.inc` 1248). There are no data, cache or city DB.

**Ranked changes.**

| # | Change | Size |
|---|---|---|
| 1 | **Weather app from zero**: provider abstraction + cache + offline states (C0, C2.2), the location page with the header, hourly, 10-day and computed sun/moon tiles; the bottom bar; the locations list + search | **L** |
| 2 | **City database + tz rules** shared by World Clock add-city and Weather search (A2.1a); replace the fixed `WC[5]` | **M** |
| 3 | **Alarm firing UI**: full-screen slide to stop + Snooze, Snooze Duration, banner/pill when unlocked, Prefer Single-Touch Actions setting (A2.6) | **M** |
| 4 | **Alarm editor as a sheet** (A2.3): ✕ / ✓ glass, inline day circles, inline Label editing (keyboard), Sound list, Snooze + Duration, 12-h AM/PM column, the wheel at pitch 32 / font 24 with a cylinder look + ticks; the platform already holds 32 (24a6efa8) | **M** |
| 5 | **Timers**: multiple timers, Label, Presets, Recents, the Timer sheet, a paused state, the Live Activity platter + pill, Stop Playing, Repeat | **M** |
| 6 | **Calculator Scientific (portrait)** + the double maths library + Rad/Deg, 2nd, memory (B2.2) | **L** |
| 7 | **Calculator** mode menu, History sheet, Convert (units offline, currency cached), AC↔C, hold-to-clear, swipe to delete, the Copy/Paste menu (B2.1-B2.5) | **M** |
| 8 | **Calculator geometry and colours**: keys 84 / gap 8 / margin 15, rows from y 288; fn #616161, digit #353535; result 68 Regular, expression 27; History/Mode circles at y 66; **remove the key haptics** | **S** |
| 9 | **Clock type and geometry**: World Clock rows 98 with city 30 / time **64 Light** + AM 32 / offset 15; the "+12:30" format without HRS; Alarms rows the same; Edit + glass in the nav row; edit mode, swipe-delete, reorder | **S** |
| 10 | **Stopwatch**: digits 88 Thin; the region decimal "."; buttons 84 without the gap ring; Stop colours #330D0E / #FF4245; page dots; the **analogue page**; laps from y 425; a stopwatch pill | **S/M** |
| 11 | Remove `sh_haptic` from Clock tabs, stopwatch/timer buttons and calculator keys; add selection ticks to the wheels | **S** |
| 12 | Our own alarm/timer tones (6-8 originals) and haptic patterns; the Alarms and Timers volume (Settings > Sounds & Haptics) | **M** |
| 13 | Weather sky shaders (9 scenes x day/night) with rain/snow that settles on the modules | **L** (after #1; static gradients first) |
| 14 | Weather detail sheets (hourly/10-day charts, tile details), Highlights rules, the map (only with tiles) | **M** |

## 7. Still estimated

There is no attended iOS 27 capture session, so these values stay **E** until a public iOS 27 source shows them.
Ranked by how much a wrong guess would show:

1. **Alarm full-screen firing layout** (A2.6): slider size and position, Snooze capsule geometry, typography. No Guide
   art; only 3P text.
   - Next sources to try: Apple Newsroom AlarmKit images (WWDC25), the Apple developer AlarmKit docs art, and
     9to5Mac / MacRumors iOS 26.1 screenshots.
2. **Alarms list** (A2.2): row height, time size and toggle position are borrowed from World Clock. No Guide art of
   the list.
3. **Timers root and running states** (A2.5): the single-timer ring size and end-time line, and the multi-timer row
   circle. Only the Timer sheet is M.
4. **Stopwatch analogue page** (A2.4): the dial size and sub-dial. A is only "swipe to switch faces".
5. **Calculator History sheet, Mode menu, Units sheet** (B2.4, B2.5, B2.3 sheet): no Guide art (only button glyphs).
6. **Calculator pending-operator highlight** in iOS 27 (B2.1), the Basic digit limit, the auto-shrink minimum.
7. **Weather Highlights module** (iOS 27): its look and position. 3P text only; not in the Guide art.
8. **Weather detail sheets** and the **chart sheet** (C2.1): not measured.
9. **Weather module corner radius** (18 from a circle fit; ±2) and the module material (blur strength).
10. **Choose a City sheet** and the **Locations-list card radius**.
11. **Key-press colours** (B2.1) and the Start-button green fill (A2.4): iOS ≤ 18 values.
12. **Wheel physics** (deceleration, cylinder projection) and the **tick sound**.
13. **Snooze Duration range** (1-15 min, 3P) and the menu step list.

Values marked M were measured once from 446-px-wide art: expect ±1 pt on geometry and ±1-2 pt on font sizes.
