# Camera and Photos (iOS 27): Starbite build spec

Track C (design-ref), v1, 2026-11-13. For the builder session (A3). Build straight from this file. For shared
components, see [IOS-REFERENCE.md](../IOS-REFERENCE.md) and [LIQUID-GLASS.md](../LIQUID-GLASS.md). Those components
are the tab bar §10, sheets §3.4, alerts and action sheets §3.1 / §3.2, menus §3.3, segmented control §12.2, slider
§12.3, type §14, colour §15, motion §16, haptics §17, press / materialise / morph / scroll edge (LIQUID-GLASS §5).
This file does not repeat them.

**Sources** (fetched 2026-11-13; study copies kept in `build/design-ref/apps27/{camera,photos}/`, git-ignored):
- **UG27**: Apple iPhone User Guide, iOS 27 edition (`support.apple.com/guide/iphone/<page>/27/ios/27`). Camera pages:
  camera-basics, use-camera-tools, take-burst-mode-shots, take-a-selfie, record-a-video, take-live-photos,
  take-panoramic-photos, save-camera-settings, change-advanced-camera-settings. Photos pages: get-started, browse
  library, browse collections, view, see information, delete or hide, share, edit, crop, sort and filter, media types,
  albums, search.
  - Its screen art (490 x 1008 px phone renders, iPhone 17/18 class) is the main measurement source. The screen is
    x 22..467 px. Calibration: **445 px = 402 pt (1.107 px/pt)**, origin (22, 22). Checked against the 4:3 frame
    (402 x 537 pt measured; 402 x 536 expected) and the 16:9 video frame (402 x 717; 402 x 715 expected).
  - The art files, by Apple asset id:
    - Camera: `fe304a31`, `3db30b2a` (Photo mode, focus square), `78a40ff8` (tools menu), `d66d404d` (Video mode),
      `e06bdc1d` (recording), `b11f87f5` (selfie).
    - Photos: `0ff2114e` (Library at rest), `94175a9b` (Library, minimised bar), `98569bee` (Collections), `390a85c6`
      (viewer), `afd27e8b` (info), `aeb36a05` (select), `20f13846` (Edit > Adjust), `bd2111aa` (Edit > Crop),
      `252372be` (share sheet), `9ba0aa86` (Recently Deleted), `2432d8b1` (search), `89f7857e` (Albums).
- **MR26**: MacRumors, "iOS 26 Camera App: New Features and Design Changes". **9to5**: 9to5Mac 2025-06-09 "iOS 26
  dramatically overhauls the camera interface" and 2025-10-30 "iOS 26's camera app hides key features". **MS26**:
  MacStories iOS 26 review (Camera and Photos chapters).
- Trunk code: `git show 1c166b85:starbite/sbos/ui/{shell_camera.inc,shell_media.inc,shell.h}`.

**Tags**: **A** Apple stated it · **M** measured by us (source given; "UG27 art" means the calibration above) · **3P**
a reviewer said it · **E** estimate. Every value is in pt on the S9 canvas (390 x 802) unless it says "iOS".
**Mapping rule:** measured iOS chrome keeps its pt size. The S9 has no island, so the iOS band above the island-level
row (~52 pt on the 874-pt reference) is dropped. Further vertical differences come out of the viewfinder or photo
area, never out of the chrome.

**No attended capture session** (user decision, 2026-11-13). Anything not measurable from public art stays **E** and
is listed in §7.

---

## 1. Information architecture

### Camera
- **Viewfinder** (root, one screen, always dark).
  - Overlay **Camera tools menu**: a bottom glass panel (§2.C3).
  - Overlay **Timer countdown**, **Focus/Exposure square**, **zoom dial**.
  - Mode carousel: `Time-lapse · Slo-mo · Cinematic · Video · **Photo** · Portrait · Pano` (UG27 lists the modes; the
    art shows VIDEO left of PHOTO and CINEMATIC left of VIDEO. Swiping left from Photo reaches Portrait, then Pano
    (A, UG27 pano "swipe left to select Pano")). iOS 27 also lists Spatial, Siri and Reference: **omit** (Vision Pro /
    Apple Intelligence / iPhone 18 Pro hardware). Of the modes we keep:

    | Mode | Starbite status |
    |---|---|
    | Photo | **build** (front camera now; rear when its driver lands) |
    | Video | build when the driver gives a stream + an encoder; until then shown, tap says why (§5) |
    | Portrait | **omit**: needs depth / segmentation we don't have |
    | Pano | **later** (needs the gyro + stitching, M-L); hidden until it exists |
    | Slo-mo | **omit** unless the sensor gives ≥ 120 fps |
    | Time-lapse | later (needs Video); hidden until it exists |
    | Cinematic | **omit** (depth video) |

    Rule: **only modes that work are in the carousel.** iOS shows Photo / Video and hides the rest offscreen anyway
    (A). Ours: `Video · Photo` (+ `Pano`, `Time-lapse` when built).
- Out of Camera: the **thumbnail** opens the Photos viewer in "camera roll" mode (§2.P4, the Camera variant), which
  returns to Camera.
- Entry points (A, UG27 camera-basics): the Home Screen icon; **swipe left on the Lock Screen**; **touch and hold the
  Lock Screen camera button**; the Control Centre camera button. (Action button / Camera Control: the S9 has none;
  local equivalent: **double-press Power** opens Camera, E, a Samsung habit; optional, behind a Settings toggle.)

### Photos
- Tab bar (glass, IOS-REFERENCE §10): **Library** | **Collections** + a separate **Search** circle (A UG27; M art).
  - **Library**: the grid. Years / Months / All accessory. Select mode. Sort & Filter menu.
    - Photo **viewer** → Info panel · Edit (Adjust / Crop, + Filters optional) · Share sheet · Delete dialog.
  - **Collections**: the large-title scroll of sections. Each section header pushes its "see all" page.
    - **Pinned** (Favourites, Recently Saved, Videos, Screenshots, Recently Deleted, a pinned album).
    - **Albums** → album grid (the same grid component, with Select).
    - **People & Pets**: **omit** (no face recognition).
    - **Memories**: **omit**.
    - **Trips**, **Featured**, **Shared Albums**, **iCloud Shared Library**: **omit**.
    - **Recent Days**: a horizontal row of day cards → that day's grid. Build (date grouping only).
    - **Media Types**: Videos, Selfies, Live Photos (omit), Portrait (omit), Panoramas, Screenshots, Screen
      Recordings → each a filtered grid.
    - **Utilities**: Recently Saved, Recently Deleted (locked behind the passcode), Hidden (locked), Duplicates
      (later), Imports (the card, local equivalent below).
  - **Search**: the search field (bottom glass capsule, rising above the keyboard) + suggestions. Our search is
    **local only**: date words ("November", "2025"), album names, media types and file names (card files). No content
    recognition: **omit** "things in photos".
- **Local data model** (Starbite):
  - Library = the Camera store (`sbos-photos`, Track A, newest 32, deletable) **+** the microSD card's DCIM/Pictures
    (read-only today).
  - Card items show a small card glyph badge (E, our addition, the same place iOS draws its shared-library badge) and
    can't be deleted while the card is read-only (§2.P5 words).

---

## 2. Screens

### C1. Camera: Photo mode (the root)

**Layout** (black ground #000; no status bar: iOS hides it in Camera, M UG27 art; privacy dot: see below).

| Item | S9 value | iOS 27 source |
|---|---|---|
| Top band | y **0..72**, black | iOS 0..124 (incl. the 52-pt island band), M UG27 `3db30b2a` |
| Top controls | glyphs **without** circles, 20-22 pt, white; centred row at **y 36**, pitch **44** (2 items: Flash at x 173, Live at x 217; 4 items in the selfie art: pitch 49) | M: flash 74-87 pt, live 216-231 pt on 402; selfie art shows Flash, Live, Center Stage (yellow when on), Night |
| Active-state colour | yellow **#FFD60A** (the Live / Center Stage glyph when on) | M (art yellow #FFD500); iOS dark systemYellow #FFD600 (A) |
| Privacy dot | 6 pt **green #30D158** dot, top-right at (372, 12) while the camera runs | A: "a green dot appears" top right (UG27); the size is E |
| Viewfinder 4:3 | x 0..390, **y 72..592** (520 tall), no corner radius | M: 402 x 537 at y 124 |
| Outside-the-frame | the live preview continues **dimmed** under the top and bottom bands (black at ~65 % over it) when the sensor's field is wider than 4:3 (iOS "View Outside the Frame", A). Our front still is 3:4 exactly, so on Starbite the bands stay solid black | A text; M art (`fe304a31` shows the scene through the bands) |
| Zoom buttons | a row of **34 pt** dark-glass circles; only the **selected** one has a circle, the others are bare labels. Centre y **556** (36 above the frame's bottom), pitch **39**. Labels 13 / 600: selected `1×` in yellow, the others `.5`, `2`, `5` in white. **The front camera on Starbite has one lens → no zoom row**; show the row only when the rear camera has more than one zoom stop (digital 2× counts: show `1×` `2`) | M `3db30b2a`: circle 34-36 pt, labels at x ≈ 155 / 201 / 240 / 280 |
| Shutter | centre **(195, 645)**. Outer **80** circle = dark glass (fill #202020 at ~75 %, a 1 px light rim) with a **68** white disc inside (6 pt band) | M `3db30b2a`: outer 80.4, disc 67.8, centre 157 pt above the bottom |
| Depth button (left) | 48 glass circle at (57, 645), "f" glyph. **Omit** (no depth): leave the slot empty | M: 48 pt at x 57 |
| **Camera tools button** (right) | 48 glass circle at **(333, 645)**, a 3 x 2 dot-grid glyph (6 dots, 3 pt each) | M: 48 pt, x 344 on 402 → 58 from the right edge |
| Mode capsule | glass capsule **214 x 48**, centre **(195, 741)**, fill #151515 at ~85 % (dark glass on black) | M: 221 x 48 on 402 (scaled 214), centre 61 above the bottom |
| Selected mode pill | inside the capsule: **86 x 40** capsule, fill #242424, centred at x 195; label caps **15 / 600**, yellow | M: pill 87 wide, fill (36,36,36); label (255,213,0) |
| Other modes | caps 15 / 500 white at ~85 %, centred at a **pitch of ~78**; clipped by the capsule with a 12-pt fade at each end | M: VIDEO / PHOTO / SIRI centres 145 / 201 / 280 |
| Thumbnail | **48** circle at **(57, 741)**, the newest photo cropped to fill, a 1 px white 20 % rim | M: 48 pt at x 57 |
| Flip camera | **48** glass circle at **(333, 741)**, glyph: two arcs with heads (arrow.triangle.2.circlepath style, our art) | M: 48 pt |
| Home indicator | hidden in Camera (E) | |

**Content and states**
- **Starting:** the viewfinder is black. After 600 ms with no frame, a 13 / 400 "Starting the camera…" appears in
  white 60 % at the frame's centre (E; iOS shows black then the feed).
- **Camera not available** (driver refused / no driver / in use by a call): iOS's ContentUnavailableView in the frame.
  A 44 pt `video.slash`-style glyph (our art), title **"Camera Not Available"** 17 / 600 white, a line 15 / 400 at
  60 %. The line is the driver's reason in plain words (sh_cam_why), or "The camera is in use by another app.", or "The
  camera can't be used during a call." The shutter disc dims to 30 %; mode / flip / tools are dimmed and a tap gives
  the reason as a toast. On the S9 today, "this phone's back camera isn't supported yet" is a valid reason (front
  only).
- **Permission:** on Starbite, the system Camera has no per-app permission. Third-party (Linux) apps: their camera
  request uses the iOS alert (§3.1): title **"“<App>” Would Like to Access the Camera"**, buttons **Don't Allow /
  Allow** (A pattern). The denied state inside such an app is not ours to draw.
- **Front camera** (selfie): the preview is mirrored (A: iOS mirrors the preview; the saved photo is mirrored only if
  **Mirror Front Camera** is on, A). **Flash** on the front = the **front flash**: the screen goes full white at the
  maximum panel level for the capture (trunk has it; iOS "Retina Flash" behaviour). The selfie zoom toggle (two
  arrows in/out, M `b11f87f5`: 28 pt dark circles at the frame's bottom centre) only if the sensor can crop wider.
- **Timer running:** big count digits **96 / 200 (thin) white** at the frame's centre (E), 3-2-1 or 5..1 or 10..1.
  The shutter turns into a **stop** (white rounded square 28 inside the ring, E). The timer glyph in the top row shows
  the seconds.
- **Saving failed** (store full, write error): a toast with the store's reason, "Not saved: <reason>". The photo
  stays as the thumbnail until Camera closes (trunk behaviour; keep).

**Interactions**
- **Tap shutter:** capture (the press animation and flash are in the motion table). **Volume up / down** also capture
  (A).
- **Hold shutter** = **QuickTake video** (A, UG27 record-a-video):
  - Recording starts after ~0.3 s (E). The shutter's white disc becomes a **red** disc, and a lock target appears to
    its right.
  - **Slide right onto the lock** to keep recording hands-free (A).
  - Release without the lock = stop. **Only with a video driver**; without one, holding does nothing beyond a single
    capture.
- **Drag the shutter left** = **Burst** (A, UG27 burst):
  - A counter pill shows above the shutter with the count, 15 / 600 white on a 50 % black capsule (E).
  - Shots at the driver's still rate (iOS ~10/s, 3P; ours whatever the still path allows, likely 1-3/s: say so with
    the count, don't fake it).
  - Lift to stop. The burst is kept as N photos (no burst stacks: **local equivalent**). Settings toggle **"Use Volume
    Up for Burst"** (A).
- **Swipe left / right anywhere on the viewfinder or the mode capsule** = change mode (A). **Tap a mode label** = go
  to it. The carousel follows the finger and snaps to the nearest mode.
- **Tap the selected mode** (PHOTO) **or swipe up from the mode capsule** = open the **Camera tools menu** (A UG27:
  "Tap [Camera Controls] or swipe up from the camera modes"). The tools button does the same.
- **Tap a top glyph:**
  - Flash cycles **Auto → On → Off** (A). A toast-like caption "FLASH AUTO / ON / OFF", 13 / 600 caps in a 50 % black
    capsule, appears under the top row for 1.2 s (E).
  - Live: **omit** (no Live Photos) → don't draw it. Starbite's default top row: **Flash** only, plus **Timer** when
    one is set.
- **Tap the viewfinder** = focus/exposure at the point (C4). **Touch-and-hold** = **AE/AF Lock** (A).
- **Pinch** = zoom (A); **touch-and-hold a zoom button** opens the zoom dial (A; E look: an arc dial 390 wide over the
  bottom of the frame, ticks every 0.1×, labels at the lens stops). Without multi-touch, the dial is the only
  continuous zoom.
- **Flip:** switch front / rear (the preview blurs out and in, motion table).
- **Thumbnail:** opens the Photos viewer on the newest photo (C-variant of P4). Its top-left back circle
  (chevron) returns to Camera, and so does a swipe down (E).

**Haptics and sounds**
- Mode change: **selection** each time a mode snaps (E; iOS gives a tick).
- Shutter: no haptic on a tap (E); AE/AF lock: **impact medium** (E); a zoom-dial stop: **selection** (E).
- **Shutter sound:** our own short click (~120 ms, two transients: the shutter's open and its close, ~ -12 dBFS),
  played on capture. It follows the ring/silent switch except where the law needs it (A: iOS mutes it with the silent
  switch except in some regions). Burst: a shorter tick per shot (E). Video start and stop: a two-tone rising chime /
  a falling one (~250 ms, E).
- Timer: a soft tick each second, and the shutter at zero (E).

### C2. Camera: Video mode (when a video driver exists)
- **Frame 16:9**: x 0..390, **y 9..702** (693 tall) (M: iOS 57..774 on 874, mapped).
- Top: the format readout **"HD"** 13 / 700 over **"30"** 10 / 600, centred at y 30 (M `d66d404d`: centred, white).
  Tap = cycle formats (A).
- The mode capsule, thumbnail and flip as in Photo. The **shutter disc is red #FF4245** (systemRed dark), 68 pt, in
  the same 80 ring (M `d66d404d`).
- **Recording** (M `e06bdc1d`):
  - The disc morphs into a **red rounded square 35 x 35, r ≈ 8**, centred in the ring.
  - The mode capsule, thumbnail and flip **fade out**.
  - A **48 glass Pause** circle appears at the left (57, 645).
  - A **white 39 still-capture disc** sits right of the shutter (centre x 262).
  - The elapsed-time pill: **red #FF383C capsule 95 x 31**, white mono-digits 17 / 600 "00:00:14", centred at y 50
    (M: pill 95 x 31 at y 63-94 on iOS).
- No video driver: the Video label is in the carousel; selecting it shows the ContentUnavailableView with title
  **"Video Not Available"** and the line "This phone can't record video yet." (E words, ours).

### C3. Camera tools menu (iOS 26/27 "camera controls" panel)
- **Panel:** glass bottom panel, side inset **8**, top at ≈ **y 430** in Photo mode (M `78a40ff8`: panel top 499 on
  874, sides ~8). Top corners **r 38** (E). It extends to the bottom edge with concentric bottom corners. It is a
  **grabber sheet**: 36 x 5 grabber at its top (IOS-REFERENCE §3.4).
- **Grid:** **3 columns**, centres x **83 / 195 / 307** (M: 86 / 201 / 316 on 402, pitch 115 → 112 on 390). Row pitch
  **132** (M). Each tool:
  - a **68** circle, dark glass (#000 at ~45 % over the panel);
  - a glyph 24 white (yellow when not default);
  - under it, a label **13 / 500 caps, tracking +0.5**, white, 12 below the circle (M sizes; the font is E).
- **Photo-mode tools** in iOS's order (A UG27 art): Flash, Depth, Live / Timer, Exposure, Styles / Aspect (4:3),
  Night, Format (HEIF). Ours, in that order:
  - **Flash** (Auto / On / Off).
  - **Timer** (Off / 3 / 5 / 10; A: iOS 27 offers 3, 5, 10).
  - **Exposure** (opens an inline slider: the panel's grid cross-fades to a horizontal tick slider −2..+2 EV in 1/3
    steps, with a Back chevron at the top-left and a reset arrow at the right; A behaviour).
  - **Aspect** (4:3 / 16:9 / 1:1; A).
  - **Grid** (our addition from Settings: Off / Grid; E).
  - Depth, Live, Styles, Night, Format: **omit** (not supported).
- **Video-mode tools** (A): Flash (torch), Exposure; Action and Dual Capture: **omit**.
- **Edit** capsule top-left of the screen (glass, ~49 x 22, "Edit" 13 / 500; M `78a40ff8`). Edit mode lets you add and
  remove tools in the top row. Minus badges appear on the top-row glyphs; "Reset to Essentials" (A words) sits at the
  bottom of the panel; Done is a checkmark (A). Build later (S); Starbite's Essentials = Flash.
- **A value picker** (Flash, Timer, Aspect): tapping a tool turns that tool's circle into a horizontal glass capsule
  of its options in place (e.g. `Off  3s  5s  10s`; the selected one yellow). Picking closes the capsule back into
  the circle (morph, LIQUID-GLASS §5.3; E look).
- **Close:** tap the viewfinder, swipe the panel down, or tap the tools button again (A).
- While open, the shutter and mode capsule stay usable (iOS: the shutter still fires, A "Tap the Shutter button" with
  the menu open).

### C4. Focus / exposure square
- **Square 74 x 74**, 1 pt yellow #FFD60A stroke. Each side has a 4 pt tick at its midpoint pointing inwards (M
  `3db30b2a`: 74 pt; ticks E). It is centred on the tap; clamp it so it stays inside the frame.
- **Sun slider:** a sun glyph (a 9 pt disc + 8 rays, ~19 pt overall, yellow) **10 pt right of the square**,
  vertically centred (M: sun x 256-275, square right edge 246). Drag up / down anywhere on the screen after the tap =
  exposure ±2 EV over 150 pt of travel. While it is dragged, a 1 pt yellow vertical line appears through the sun,
  ±64 above and below (E). On the left side of the frame the sun moves to the left of the square (E).
- **Appear:** the square starts at **1.4×** and shrinks to 1.0 with a spring (0.3 / 0.75). It **pulses** twice (alpha
  1 → 0.5 → 1, 0.2 s each). It fades to 50 % after 1.5 s idle and disappears after 5 s (E, long-stable iOS).
- **AE/AF Lock:** touch and hold 0.8 s. The square pulses bigger once and a **yellow capsule "AE/AF LOCK"** (13 / 600
  caps, black text) appears at the top of the frame (A words). Tap elsewhere to unlock (A).
- **Starbite:** the front sensor's AF/AE support decides it. With no AF, show the exposure part only (the sun without
  the square), or nothing.

### C5. Lock-screen camera
- **Swipe left on the Lock Screen** (A): the Camera page follows the finger from the right edge, a rubber-band
  interactive transition. Past 35 % of the width or a fling > 500 pt/s, it commits; otherwise it springs back.
  Spring 0.40 / 0.90 (E).
- **The lock-screen camera button** (bottom-right 50 glass circle, IOS-REFERENCE §9.1): **touch and hold** → a
  **medium impact** and Camera opens with a zoom-from-the-button (A gesture, E motion).
- **The secure camera** (Camera opened from the lock):
  - The thumbnail shows only photos taken in this session. Tapping it opens the viewer limited to those photos.
  - Photos' other tabs need unlocking: Back or "All Photos" asks for the passcode (A behaviour, long-stable).
  - Swipe up (home gesture) returns to the Lock Screen, not Home.

### P1. Photos: Library (grid)

**Layout** (dark-first; light mirrors it with systemBackground). The grid scrolls under the status band.

| Item | S9 value | Source |
|---|---|---|
| Grid | **3 columns, square tiles, 1 pt gutters** → tile **129.3** (390 − 2) / 3. Newest at the **bottom**. Opens scrolled to the end | M (UG27 `0ff2114e`: gutter 1 px @2x ≈ 0.9 pt, row pitch 134.5 on 402); A (newest last) |
| Title | **"Library"** 34 / 700 white with a soft shadow (black 30 %, blur 8) over the photos; left x 20, **baseline 72** | M: Large Title, baseline 90 on iOS (status 62) → 72 |
| Subtitle | 13 / 600 white 85 %, baseline **95**. At the newest end: the item count ("1,964 Items", A art). While scrolling: the date range of the top visible row ("Jul 13, 2025", A art) | M: cap 9 → 13 pt |
| Top scroll edge | a dark progressive blur band 0..120 (LIQUID-GLASS §5.4, the soft edge over photos) so the title stays readable | E |
| Sort & Filter | **44** glass circle, a three-line glyph (wide, mid, short), centre **(273, 62)** | M (`0ff2114e`: 44 circle left of Select) |
| Select | glass capsule **44 tall**, "Select" 17 / 600, right edge at 370, centre y 62 | M |
| Tab bar (rest) | glass capsule **x 20..206, y 718..780** (62 tall), tabs **Library** / **Collections** (glyph 24 + label 11 / 600; selected = a darker inner pill + blue #0091FF label and glyph) | M (`0ff2114e`: 192 x 61 on 402, bottom 25 above the edge; selected label (42,194,255)) |
| Search | **62** glass circle at **x 308..370**, y 718..780, a magnifier 24 | M: 62 circle, right inset 21 |
| Tab bar (scrolled) | **minimises** when scrolling into the past: the capsule shrinks to the selected tab's **56** circle at the left (glyph only). The **Years / Months / All** segmented capsule slides in between it and the search circle | M (`94175a9b`); A (UG27 "Swipe down to browse photos, then tap Years or Months") |
| Years / Months / All | glass capsule ~**220 x 48**, centred at x 195, centre y 749. Labels 17 / 500, selected in a **#3A3A3C** inner capsule (dark) with white text, others white 85 % | M (`94175a9b`); IOS-REFERENCE §12.2 behaviour |

**Content and states**
- **All:** the grid; video tiles show the duration bottom-right ("0:14", 12 / 600 white, shadow). Favourite tiles show
  a white heart 14 at the bottom-left (M art `94175a9b`). Card items show the small card badge top-right (ours, E).
- **Months:** cards full width minus 32 (x 16..374), height ~ **220**, radius **22**. The month's key photo fills the
  card. Over it: "November 2026" 22 / 700 white and the count 15 / 500 white 85 %, bottom-left, padding 16, a bottom
  scrim black 0 → 45 %. Gap 12. Trunk has this; keep its look.
- **Years:** the same cards, one per year, "2026" 28 / 700.
- **Tap a Months card** → All, scrolled to that month. **Tap a Years card** → Months at that year (A; trunk has it).
- **Empty** (no photos anywhere): a centred empty state, a 56 photo-stack glyph, **"No Photos or Videos"** 22 / 700,
  then 15 / 400 secondary **"Photos and videos you take with Camera, or put on a microSD card, appear here."** (A title
  pattern: iOS says "No Photos or Videos"; the line is ours).
- **No card, store empty:** the same, line **"Put a microSD card with pictures in the phone, or take a photo with
  Camera."**
- **Card error** (but store photos exist): the grid shows the store photos. A **banner row** above the newest row,
  13 / 400 secondary, reads **"The microSD card can't be read. Showing photos taken on this phone."** Trunk shows a
  full empty state instead: change it.
- **Loading** (the card being indexed): the grid fills as items are found. The subtitle reads "Looking for photos…
  1,204 found" (trunk words; keep). Thumbnails decode in placeholder grey (#1C1C1E) and fade in over 0.15 s.
- **Sort & Filter menu** (glass menu §3.3, morphing from the button; A items, ours trimmed):
  - Section 1: **Sort by:** "Date Captured" ✓ / "Recently Added" (A: Captured / Added).
  - Section 2: **Filter:** "All Items" ✓, "Favourites", "Edited", "Photos", "Videos", "Screenshots" (A list, minus
    "Shared Library" and "Captured by Me": **omit**).
  - Section 3: **View Options** ▸ (a submenu): "Zoom In", "Zoom Out", "Aspect Ratio Grid" ⇄ "Square Photo Grid", a
    "Screenshots" toggle (A).
  - When a filter is on, the button turns into a **blue glass** circle and the subtitle says "Filtered: Favourites"
    (E). "Remove Filters" at the menu's top (A).

**Interactions**
- **Pinch in / out** = column levels **1 · 3 · 5 · 9**: 3 is the default, pinch out goes 5 → 9, pinch in from 3 goes
  to **1** (full width, aspect-correct). (E: iOS has had 1 / 3 / 5 plus a dense level since iOS 13.)
  Without multi-touch, View Options' Zoom In / Zoom Out are the way (A).
  - The zoom animates by **scaling the grid around the pinch centre**. On release, it cross-fades to the new layout
    with the item under the fingers kept under them (spring 0.35 / 1.0, E).
- **Scroll:** momentum + rubber band at both ends (IOS-REFERENCE §16).
  - **Fast scrubber** (E, long-stable iOS): after > 3 screens of fast scrolling, a scroll handle appears at the right
    edge: a **44 x 32 glass capsule** with an up/down chevron glyph, at x 340..384. Dragging it maps the whole library
    to the screen height. A date bubble ("March 2024", 15 / 600) shows to its left while it is dragged. It fades 1.5 s
    after the scroll stops.
- **Tap the status band** = scroll to the newest end (iOS: "to the top" means the newest end, which is the bottom; E).
  Tapping the selected **Library** tab again does the same.
- **Tap a tile** → the viewer (P4), zooming from the tile.
- **Long-press a tile** → a context menu (IOS-REFERENCE §3.3). The tile lifts to a preview (the photo at its aspect,
  up to 300 wide). Items in order (A set, trimmed):
  - Copy · Share… · Favourite (♡ / "Unfavourite") · Duplicate (store only) · Hide · Add to Album · Edit Date & Time
    (later) · **Delete** (red).
  - Card photos: Copy · Share… · Add to Album · "Delete" dimmed with the line "On the card (read only)" (ours).
- **Select mode** (tap **Select**; M `aeb36a05`):
  - The top-right becomes a grouped **[Sort & Filter | ⋯]** glass capsule + a **44 × close** circle. The title stays
    "Library" and the subtitle stays the date.
  - Tap tiles to toggle. A selected tile shows a **22 blue (#0091FF) circle with a white check** at its bottom-right,
    inset 6, with a 1.5 white ring. The tile itself is **not** dimmed (M art: no dim visible). Unselected tiles show
    nothing.
  - **Drag-select** (A behaviour, long-stable):
    - Touch a tile and drag sideways (> 10 pt horizontal first) to select every tile from it to the finger, in reading
      order; drag back to deselect.
    - Moving vertically keeps extending the range.
    - Near the top or bottom 80 pt, the grid **auto-scrolls** at up to 1,200 pt/s, proportional to the depth.
    - The first tile's state decides whether the drag selects or deselects.
  - Bottom bar (replacing the tab bar):
    - a **48 share circle** at x 57;
    - a centred glass capsule **"Show Selected (6)"** 17 / 600 (M art), or the count "6 Photos Selected" when
      nothing can be shown (E);
    - a **48 trash circle** at x 333.
    - With nothing selected: "Select Items" in the capsule, share and trash dimmed (E words, iOS pattern).
  - ⋯ menu: "Select All" / "Deselect All", Add to Album, Duplicate, Hide, Favourite (A set).
  - Close (×) leaves select mode.

**Transitions** (§4): the tab switch cross-fades the content (0.2 s). The tab bar minimise / restore uses spring
0.40 / 1.0 (LIQUID-GLASS §5.3). Select mode's bars morph in place.

**Haptics:** **selection** on each tile added by a drag-select, on a Years / Months / All change, and on reaching a
pinch zoom level. **Medium impact** on a context-menu lift. No haptic at
scroll ends (E).

### P2. Photos: Collections

**Layout** (M `98569bee`; dark-first, light as art).
- Large title **"Collections"** 34 / 700 at x 20, **baseline 72** (M: same baseline as Library). Top right: a **44 ⋯
  glass circle** (Layout & Reorder menu) at (348, 62). The iOS account avatar left of it: **omit**.
- The body is a vertical scroll of **sections**, each:
  - A header: **name 22 / 700** (Title 2 bold, M cap 16) + a **chevron 17 / 700 in secondary** right after the name
    (tap = "see all"). Header baseline 26 below the previous section's bottom; left x 25 → **x 20** on S9.
  - Right side of the header: an optional "**Edit**" capsule (Pinned: 28 tall, 15 / 600 blue on #E5F0FF light /
    #0A2540 dark, M) and a **28 collapse circle** with a down chevron in blue (tap = collapse the section to its
    header; the chevron rotates −90°, spring 0.3 / 1.0).
  - Content: a **horizontal row of cards** that scrolls sideways, starting at x 20 and running off the right edge
    (peek). Cards radius **22** (E; art r ~ 20-24 at 110), gap **12**, with a 1 pt shadow (black 12 %, blur 12, y 4).
  - Card title: bottom-left inside the card, padding 12, 15 / 600 white (2 lines max), over a bottom scrim.
- **Section order** (A UG27 art order: Memories, Pinned, Albums, People & Pets, …). Ours:
  1. **Pinned**: cards **110 x 110** (M ~110 on 402). Defaults: **Favourites** (heart badge top-right), **Recently
     Saved**, **Videos**, **Screenshots**, **Recently Deleted** (a lock glyph when locked).
  2. **Albums**: 110 x 110 cards; a "+" card last ("New Album"). See-all = the Albums grid (2 columns of 175 cards,
     name 15 / 600 + count 13 / 400 secondary under each card; E from `89f7857e`).
  3. **Recent Days**: cards **233 x 233** (the Memories card size, M 233 square). A day's key photo, "Today" /
     "Yesterday" / "Tuesday" / "Nov 9" 22 / 700 + the count, bottom-left.
  4. **Media Types**: a list card (inset grouped, IOS-REFERENCE §6) of rows: Videos, Selfies, Panoramas, Screenshots,
     Screen Recordings, each with a 22 glyph, the name and the count in secondary + chevron. Hide empty types.
  5. **Utilities**: the same list style. Rows: **Imports** (*local equivalent*: "microSD Card", the card's photos only),
     **Hidden** (lock glyph), **Recently Deleted** (lock glyph), Duplicates (later).
  6. **"Reorder"** text button (17 / 400 blue), centred at the bottom of the scroll (A UG27).
  - Omitted, never drawn: Memories, People & Pets, Trips, Featured Photos, Shared Albums, Wallpaper Suggestions.
- **Layout & Reorder menu** (⋯): "Large Grid", "Small Grid", "Mixed Grid" (default) as a 3-glyph row; "Collapse All";
  "Reorder" (A).
- **Empty:** all sections with no content collapse to their header and a line 15 / 400 secondary. Albums: **"No
  Albums"** and a "New Album" button. The other sections are hidden when empty (E).
- **Interactions:**
  - Tap a card → push its grid (nav push §16). The pushed grid has a 44 back circle, the title inline 17 / 600 centred,
    and Select + ⋯ on the right.
  - Long-press a card → menu: **Pin / Unpin**, Rename (albums), Delete Album (red, albums) (A "touch and hold it, then
    tap Pin").
  - Pinned **Edit** → a sheet (§3.4 large) listing collections with **+ / −** badges. Drag to reorder (touch and hold,
    lift with a medium impact). The close × is at the top-left (A).
- **Locked collections** (Recently Deleted, Hidden): iOS shows the album title and a **"View Album"**
  button (A UG27 "tap View Album, then
  unlock"). Ours: the lock glyph, the album name 22 / 700, the line 15 / 400 "Use your passcode to view this album.",
  and a blue capsule **"View Album"**. The tap → the Starbite passcode sheet (shell_passcode). A Settings > Photos
  toggle **"Use Passcode"** (A: "Use Face ID / Touch ID / Passcode") turns the lock off.

### P3. Photos: Search
- **Tap the Search circle**: the tab bar morphs into a **search field capsule** (§10) at the bottom. It rises above
  the keyboard, which opens at once. The page shows suggestions (A UG27 art `2432d8b1`: suggestion rows under a field).
- **Before typing** (a list on black):
  - "Recent Searches" (≤ 5, with a "Clear" button, 15 / 400 blue);
  - "Media Types" chips (Videos, Screenshots, Selfies, Panoramas) as glass capsules 36 tall;
  - "Albums" (the first 6, as 60 rounded thumbnails with names).
- **Typing:** suggestion rows, 52 tall, each with a 36 rounded thumbnail (r 8), the label 17 / 400 with the matched
  part in 17 / 600, and the count 15 / 400 secondary right-aligned, + chevron:
  - months / years ("November 2026");
  - day names ("Saturday");
  - albums;
  - media types;
  - card folder names ("DCIM/Camera").
- **Return** = a results grid (the same 3-column grid), the title is the query.
- **No results:** a 44 magnifier glyph, **"No Results"** 22 / 700, line 15 / 400 **"Try a new search."** (A pattern
  from system search).
- **Omit:** content search ("dog", "beach"), people, text in photos, Siri suggestions.

### P4. Photo viewer
**Layout** (M `390a85c6`, light art; dark is ours: black ground).

| Item | S9 value | Source |
|---|---|---|
| Ground | black (dark) / white (light) | M (light art: white) |
| Back | **44 glass circle**, chevron.left 17 / 600, centre **(38, 66)** | M: 44 at x 36, y 79.5 on iOS → nav-row centre 66 |
| Date capsule | glass capsule **~180 x 44**, centred at x 195, y 66: the date **15 / 600** ("December 17, 2025") over the time **12 / 500** ("3:42 PM"). For an undated card file: the file name 15 / 600 | M: 2 lines in a 44 capsule, the date cap 66.8-78.6 |
| More | **44 glass circle** "⋯" at (352, 66) | M |
| Badge row | y **112..131** (19 tall): left a **mode badge** capsule (iOS: "PORTRAIT" / "LIVE", both omitted; ours: "PANO" for panoramas, else none), 11 / 500 caps grey stroke; right: the iOS 27 star-rating ☆▾ capsule (**later**, A: iOS 27 adds star ratings) | M (19 tall at y 126 on iOS) |
| Photo | fit inside **y 136..668** (532 tall) × 390, centred; a 3:4 photo is 390 x 520 at y 142..662 | M: 3:4 photo full width at y 164..702 on iOS |
| Filmstrip | y **673..704** (31 tall), centre **689**. Thumbnails **20 x 31** (cropped), gap **2.5**; the current one **31 x 31** with **12 gaps** either side, centred at x 195. Fades to transparent over 24 pt at both edges | M: 19.9 wide, 3.5 pitch gaps, current 30.7, edge fade visible |
| Toolbar | y **724..772** (48 tall), centre **748** (30 above the bottom): **share 48 circle** at x 57; a centred **grouped capsule ~163 x 48** with **heart · info · adjust (Edit)** glyphs at 52 pitch; **trash 48 circle** at x 333 | M: 48 circles, capsule 163 wide, bottom 30 pt above the edge |
| Glyph sizes | 22 (heart, info, sliders, trash), share 22 (box + arrow) | M ~ 20-22 |
| Chrome hidden | tap toggles: all chrome fades out (0.2 s) and the ground turns **black** (light mode too). In dark mode the ground is already black | A (UG27 tip); E for the light → black rule (long-stable iOS) |

**Content and states**
- **Loading the full image** (the card is slow): show the tile's thumbnail scaled up (blurred by its own upscale) and
  swap in the full decode with a 0.15 s cross-fade (trunk has the flight + half/full decode; keep).
- **Decode error:** centred in the photo area, 44 exclamation glyph, title **"Can't Show This Photo"** 17 / 600, the
  reason 15 / 400 secondary ("This kind of picture can't be shown yet." etc.; trunk words, keep).
- **Video item:** a centred 64 glass play circle. The bottom area becomes: the scrubber line (filmstrip of frames, 44
  tall) + play/pause + mute in the toolbar (A UG27 "Play videos"). Build only with a video decoder; until then the
  play circle is dimmed and a tap says **"This video can't be played yet."**
- **Favourite on:** the heart is filled **white** (A pattern; E colour). Toggling gives a light impact.
- **Card photo:** heart, Edit and trash dimmed (40 %). A tap gives a toast: "Photos on the microSD card can't be
  changed yet." (ours).

**Interactions**
- **Swipe left / right** = next / previous (A). The page follows the finger 1:1. It commits past 50 % or on a fling
  > 300 pt/s; spring 0.35 / 1.0. A 16-pt black gap between photos. At the ends: rubber band (×0.3, trunk has it).
- **Swipe the filmstrip** = scrub fast through the library (A). The photo above updates at the strip's centre item
  (half-resolution thumbnail first). Selection haptic per item passed (E).
- **Pinch** = zoom (max **3× the fit size or 1 image px per screen px, whichever is bigger**, E). Pinching below 1× is
  allowed down to 0.7× with resistance, then springs back (rubber band). **Double-tap** = toggle fit ↔ 2.5× centred on
  the tap (E; trunk zooms on a double tap, keep). Zoomed: one finger pans with rubber band at the edges (trunk has it),
  and swipes don't change photos until the edge is reached.
- **Swipe down** (when not zoomed) = **dismiss**:
  - The photo follows the finger and **shrinks** (scale = 1 − 0.35·d/H, min 0.6).
  - The ground's alpha fades with the distance (1 → 0 at 40 % H) and the grid shows through. The chrome fades at once.
  - Release past 100 pt or a fling > 500 pt/s down → it flies into its tile (matched geometry) and the grid's tile is
    revealed. Otherwise it springs back (0.35 / 0.85).
- **Swipe up** = the **Info panel** (A).
- **Toolbar:**
  - share → P7;
  - heart → favourite toggle;
  - info → the Info panel;
  - adjust (sliders) → Edit (P6);
  - trash → the delete dialog (P5).
- **⋯ menu** (A set, trimmed): Copy · Duplicate · Hide · Add to Album · Use as Wallpaper · Adjust Date & Time (later).
  Rotate stays inside Edit, as on iOS.

**Info panel** (M `afd27e8b`, A UG27 see-information):
- The **photo slides up and shrinks** so its bottom sits at ~**y 360** (the photo keeps its width and is cropped at the
  top: iOS shows the photo top-aligned, cut by the screen top). Below it, a scroll of cards on systemGroupedBackground
  (black dark / white light). The toolbar stays; the info glyph becomes filled (M).
- Cards (inset 16 at the sides, radius **22**, fill #1C1C1E dark / #F2F2F7 light; M r ~ 22 at 402):
  1. **Caption** field: a glass-free filled capsule 52 tall, placeholder **"Add a Caption"** 17 / 400 tertiary (A
     words). Kept locally (store metadata). For card files: hidden.
  2. **Details card:**
     - **Row 1:** "Tuesday • Dec 16, 2025 • 5:42 PM" 17 / 600, with **"Adjust"** 17 / 400 blue at the right (later;
       hide until editable).
     - **Row 2:** the file name, 15 / 400 secondary ("IMG_1234" for the card; "Camera 23" for the store, ours). iOS's
       cloud glyph: **omit**.
     - A hairline, then **device row:** "Samsung Galaxy S9 — Front Camera" 17 / 400 + a format chip ("JPEG" / "BMP"
       store) 13 / 600 in a 24-tall capsule (M).
     - A hairline, then **the camera line(s)** 15 / 400 secondary: lens line (from EXIF if the card file has it:
       "Main Camera — 26 mm ƒ1.64"). Line 2: "24 MP • 4284 × 5712 • 3.9 MB" (A format, M from art; ours shows MP •
       W × H • size).
  3. **Map card**: **omit** for now (no map tiles offline). With EXIF GPS: show the place as text "51.50° N, 0.12° W"
     15 / 400 + "No map offline" secondary (ours, E).
  4. **Albums** row: "Albums" + the list (tap opens). Hidden when none.
  - Omit: Look Up, Ask Siri, Image Search, People, Shared by.
- Close: **swipe down** on the panel or tap info again; the photo springs back (0.4 / 0.9).

**Transitions**
- **Open from a tile (zoom):** the tile's rect → the fit rect, matched geometry. The corner radius goes 0 → 0, the
  black ground fades in 0 → 1, the chrome fades in over the second half. Spring **0.40 / 0.90** (IOS-REFERENCE §8). The
  grid stays under it (scaled 1.0, dimmed by the ground).
- **Close:** the reverse, into the **current** photo's tile. If that tile is offscreen, the grid scrolls it into view
  first (instantly, before the flight).

**Haptics:** light impact on favourite. Selection on filmstrip items. None on swipe (A: iOS gives none).

### P5. Delete: the dialog and Recently Deleted
- **Viewer trash** → an **action sheet anchored to the trash button** (IOS-REFERENCE §3.2, the popover morphing from
  the circle):
  - one destructive capsule **"Delete Photo"** ("Delete Video" for a video) and a line above it, 13 / 400 secondary:
    **"This photo will be deleted from this phone. It will be in Recently Deleted for 30 days."** (ours; iOS's line
    names iCloud: **local equivalent**).
  - Tap outside = cancel. After a delete: the photo **shrinks to 0.8 and fades** (0.25 s); the next photo slides in
    from the right (or the previous one if it was the last). **Haptic: none** (E; iOS plays none on delete). Sound:
    none.
- **Select mode trash:** the sheet says "Delete 6 Photos" / "Delete 2 Items" (photos + videos), with the same line in
  the plural. The tiles shrink and fade, and the grid closes the gaps (spring 0.35 / 1.0).
- **Card items in a selection:** the sheet's line becomes "2 photos on the microSD card can't be deleted. The others
  will be in Recently Deleted for 30 days." (ours).
- **Recently Deleted** (Collections > Utilities; locked by default, A):
  - The 3-column grid. Each tile has **"29 days"** (days left; "1 day" on the last day) at its bottom-left, 12 / 600 white
    with a shadow (A behaviour; M art `9ba0aa86`).
  - Its own header: "Recently Deleted" + the line 13 / 400 secondary. iOS's line (E) explains the days-remaining labels and
    that the final removal can take up to 40 days. Ours, shorter: **"Photos are permanently deleted after 30 days."**
  - Select → bottom bar "Recover" (blue) / "Delete" (red). ⋯ → "Recover All" / "Delete All" (A).
  - Delete confirmation: an alert (§3.1): **"Delete 3 Photos?"** / "These photos will be deleted from this phone. You
    can't undo this action." / **Cancel** | **Delete** (red) (E words).
  - Empty: **"No Photos or Videos"** + "Photos and videos you delete stay here for 30 days." (ours).
  - **Starbite:** the store needs a "deleted at" flag and a purge on open (§6, M). Until it exists, a delete is
    permanent: the line must then say **"This photo will be permanently deleted. You can't undo this action."**.
    Never promise a Recently Deleted that doesn't exist.

### P6. Edit (minimal set)
**Layout** (M `20f13846`, `bd2111aa`; the edit ground follows the appearance, dark = black):
- **Row 1** (y 66 on S9; iOS puts it beside the island): **"Cancel"** glass capsule (17 / 600, 44 tall here; iOS 28
  beside the island, M) at the left. **"Done"** capsule filled **yellow #FFCC00 (light) / #FFD600 (dark)** with
  **black** 17 / 600 at the right (M: fill (253,202,0)). Done is dimmed until something changes.
- **Row 2** (y 114): left, a grouped capsule **undo | redo** (44 tall, glyphs 20); centre, the tool name in caps 13 /
  600 secondary ("ADJUST", "CROP"); right, a grouped capsule [**⋯**] (markup: **omit**).
- **Photo:** fit inside **y 142..592**.
- **Tool area:**
  - **Adjust:** a row of **51 circles**, pitch **71**, centre y **626**. The row scrolls sideways and snaps the
    selected tool to the centre. The selected circle's ring shows the value as an arc (yellow #FFCC00) and the value
    number 15 / 500 inside. The others are outline circles with the tool glyph.
    - Below it: a **tick slider**: full-width ticks every 6 pt, 12 tall (major every 5th: 18 tall). It scrolls under
      a yellow centre needle 2 x 24, with a small grey dot at the zero position (M look).
    - The tool's name shows as a caps label in a 50 % black capsule over the photo's bottom ("EXPOSURE", M art).
  - **Tab capsule** (bottom): a glass capsule **~245 x 62**, centre y **741**, items (ours): **Adjust · Filters ·
    Crop**. The iOS items Portrait, Styles, Tools / Clean Up: **omit**. Each item is a glyph 22 + label 11 / 500; the
    selected one is in label colour with a **small yellow triangle marker above it** (M), the others secondary.
- **Adjust tools** (ours, minimal; iOS has 16): **Auto** (magic wand), Exposure, Highlights, Shadows, Contrast,
  Brightness, Saturation, Warmth (8). Omit Brilliance, Black Point, Vibrance, Tint, Sharpness, Definition, Noise
  Reduction, Vignette (later, S each). Range −100..+100, 0 default.
  - **Tap a tool's circle again** = compare it with the original (A "Tap the button to compare").
  - **Tap the photo** = show the original while held (A "To compare … tap the photo"; E: press-and-hold).
- **Filters** (optional, S each once the pipeline exists): Original, Vivid, Vivid Warm, Vivid Cool, Dramatic, Dramatic
  Warm, Dramatic Cool, Mono, Silvertone, Noir (A names). Square previews 60 in a strip, the selected one with a yellow
  ring, and an intensity tick slider below.
- **Crop** (M `bd2111aa`):
  - **Row 2** becomes: [**flip-horizontal | rotate-90°-left**] capsule left · an **"AUTO"** chip centre (straighten
    automatically; ours: **omit** until we have horizon detection) · [**aspect-ratio grid | ⋯**] right.
  - The photo sits inside a **crop frame**: 1 pt white edges, **corner handles L-shaped 3 pt thick, 20 long**, edge
    midpoint handles 20 x 3. While dragging, a rule-of-thirds grid shows at 50 % white. Outside the crop, the photo is
    dimmed 60 %.
  - Under the photo: **Straighten / Vertical / Horizontal** circles (iOS), with the tick dial −45°..+45°. Ours:
    **Straighten only** (−45..45°, the tick dial, needle).
  - The aspect-ratio button opens a strip: portrait / landscape toggle (two outline rects, M), then **FREEFORM ·
    ORIGINAL · SQUARE · WALLPAPER (= screen 390:802) · 9:16 · 4:5 · 5:7 · 3:4 · 3:5 · 2:3** caps 15 / 600, the
    selected in a **#3A3A3C capsule** (M). Corner drags keep the ratio unless Freeform (A).
  - **Reset** (caps 15 / 600 yellow) appears in place of "CROP" in row 2 once changed (E).
- **Cancel with changes** → an action sheet from the Cancel button: **"Discard Changes"** (red) (A words).
- **Done:** saves. Store photos: a non-destructive edit record + a rendered copy (§6). Card photos: **"Save as Copy"**
  only, which keeps a new store photo (ours: the card is read-only).
- **Revert:** an edited store photo's viewer ⋯ menu and Edit show **"Revert"** (A: "tap Edit, then tap Revert"). Its
  confirmation is an action sheet **"Revert to Original"** (red) (A words: E).

### P7. Share sheet (local equivalent)
- iOS (A art `252372be`): a sheet with the photo preview at the top, a row of people / AirDrop, a row of apps, then
  an action list (Copy Photo, Add to Shared Album, Add to Album, Use as Wallpaper, …).
- **Ours:** the IOS-REFERENCE §3.4 sheet at the **medium detent**, glass.
  - Header row: a 44 rounded thumbnail of the photo (r 8) + "1 Photo Selected" 17 / 600 + an "Options ›" 15 / 400
    blue link (Options: **"Include location"** toggle, off by default; ours) + the close × circle.
  - **App row** (60 icons at 76 pitch, name 11 / 400 under):
    - **Messages** (only if MMS is supported; otherwise **omit** the icon, never dim it);
    - **Files** (save to the microSD card: shown only once the card is writable);
    - **Bluetooth**: **omit**.
    - **No people row, no AirDrop.**
  - **Action list** (inset grouped rows, 52 tall, label 17 / 400 + a trailing 22 glyph):
    - **Copy Photo**;
    - **Add to Album**;
    - **Use as Wallpaper** (opens the wallpaper preview);
    - **Duplicate**;
    - **Hide**;
    - **Save to Files** (as above);
    - "Edit Actions…" (**omit**).
  - Multiple: "6 Photos Selected", Copy is "Copy 6 Photos".
- Each action closes the sheet and toasts the result ("Copied", "Added to “Holidays”"; ours, IOS-REFERENCE toast
  style).

### P8. Photos empty and error states (summary; words in §5)
- No photos at all · no card + empty store · card unreadable (banner) · card indexing (subtitle) · store refused
  (safe start: "Photos taken now won't be kept until the next normal start." banner, ours) · decode error in the
  viewer · locked album · no search results · empty album.

---

## 3. Cross-screen flows

**Take a photo and look at it**
1. Home → tap Camera. Camera opens with a zoom from the icon (Home's app-open). The preview starts within the open
   (first frame ≤ 600 ms, else "Starting the camera…").
2. Tap the shutter: the shutter press, the flash, the sound. The thumbnail flies in (motion table). The store save
   happens **after** the thumbnail lands (trunk rule: never block the tap).
3. Tap the thumbnail: the viewer zooms open from the thumbnail circle (the corner radius goes 24 → 0). Swipe left for
   older photos.
4. Swipe down: the viewer shrinks back into the thumbnail circle; Camera is live again.

**Open Camera from the Lock Screen**
1. Locked: swipe left. Camera follows the finger and commits.
2. Take photos. The thumbnail shows only this session's photos.
3. Swipe up from the bottom → back to the Lock Screen (not Home).

**Burst**
1. Photo mode: put a finger on the shutter and drag it left ≥ 30 pt. The shutter disc slides with the finger (max 60
   left, rubber band) and a count pill appears.
2. Each shot: a tick sound, the count +1, a blink of the frame (30 % black, 60 ms).
3. Lift: the disc springs back to the centre (0.3 / 0.7). The thumbnail shows the last shot; N photos are kept.

**QuickTake video** (video driver only)
1. Hold the shutter for 0.3 s: the disc turns red, a lock circle appears 66 to the right, and the timer pill shows.
2. Slide right onto the lock: the disc docks into it. The pause and still buttons show; release keeps recording.
3. Tap the red square: stop, the end chime, and the thumbnail flies in.

**Delete a photo (single)**
1. The viewer → trash: the popover sheet with "Delete Photo".
2. Tap it: the photo shrinks out and the next one slides in. The store marks it deleted (Recently Deleted) and the
   grid tile is gone on return.

**Delete several**
1. Library → Select → drag across the tiles (selection ticks) → trash.
2. The sheet: "Delete 6 Photos" → the tiles shrink out and the grid closes up. Select mode stays on with 0 selected.

**Recover**
1. Collections → Utilities → Recently Deleted.
2. "View Album" → passcode → the grid with days left.
3. Select → tap tiles → **Recover**. The tiles fade and the toast says "Recovered 3 Photos" (ours).

**Crop a photo**
1. The viewer → adjust (sliders) → Edit opens on **Adjust**. Tap **Crop**.
2. Drag a corner; the thirds grid shows while dragging. Release: the crop animates to fill the area (spring 0.4 / 1.0).
3. Done: saved (store) / "Save as Copy" (card).

**Find last November's photos**
1. Search circle → type "nov".
2. A suggestion "November 2025 · 41". Tap it → the results grid.

---

## 4. Motion table

| # | Trigger | What moves | Spring / duration | Tag |
|---|---|---|---|---|
| 1 | Shutter touch-down | the white disc scales **1.0 → 0.88** and darkens to 85 % | 0.12 s ease-out | E |
| 2 | Shutter release (capture) | the disc → 1.0 | spring 0.30 / 0.60 | E |
| 3 | Capture | a **frame blink**: black overlay over the viewfinder 0 → 100 % in 0.04 s, back to 0 in 0.12 s | linear | E (long-stable iOS) |
| 4 | Capture (front flash) | the full screen white + panel max, from 0.15 s before the capture to 0.1 s after | — | trunk |
| 5 | Thumbnail fly-in | the new photo grows from 0.2 → 1.0 at the thumbnail circle (iOS ≥ 26: grows in place; E) with the old one under it | spring 0.35 / 0.80 | E |
| 6 | Mode swipe | the mode labels slide 1:1 with the finger; snap to the nearest | spring 0.35 / 1.0 | E |
| 7 | Mode change | the selected pill **stretches** toward the new label, then settles (liquid, §12.2) | spring 0.35 / 0.75 | E |
| 8 | Mode change (frame) | the preview **blurs out** (σ 0 → 20, 0.15 s) and back in when the new frame arrives; 4:3 ↔ 16:9 frame resize | spring 0.40 / 1.0 | E |
| 9 | Flip camera | the preview blurs to σ 30 and dims 40 % (0.15 s); it **unblurs** when the new camera's first frame arrives (0.25 s). (iOS ≤ 16 did a 3D flip; iOS 26 blurs: E) | 0.15 + 0.25 s | E |
| 10 | Tools menu open | the panel slides up from below the mode capsule; the 68 circles materialise staggered 20 ms | sheet spring 0.45 / 1.0; materialise (LIQUID-GLASS §5.2) | E |
| 11 | Tool value picker | the circle morphs into its option capsule and back | 0.40 / bounce 0.20 (§5.3) | E |
| 12 | Focus square | appears 1.4 → 1.0; 2 pulses; dims at 1.5 s; gone at 5 s | spring 0.30 / 0.75 | E |
| 13 | Exposure drag | the sun moves along its line 1:1 | direct | E |
| 14 | Record start | the white disc → red disc (colour 0.15 s); on recording, disc → rounded square 35 (radius 34 → 8) | spring 0.30 / 0.80 | M (end states), E (timing) |
| 15 | Record bars | the mode capsule, thumbnail and flip fade 0.2 s; pause / still discs fade in | 0.2 s ease | E |
| 16 | Burst drag | the disc follows the finger left (max 60, rubber band); springs back on lift | spring 0.30 / 0.70 | E |
| 17 | Lock-screen swipe | Camera slides in from the right 1:1 | commit spring 0.40 / 0.90 | E |
| 18 | Photos tab switch | content cross-fade; the tab pill slides | 0.2 s / spring 0.35 / 0.75 | IOS-REF §10 |
| 19 | Tab bar minimise | the capsule → 56 circle; Years/Months/All slides in | 0.40 / bounce 0 | LIQUID-GLASS §5.3 |
| 20 | Years / Months / All | the selected pill stretches; the content cross-fades + scales 0.98 → 1 | 0.35 / 0.75; 0.25 s | E |
| 21 | Grid pinch | the grid scales around the pinch centre; on release, a re-layout cross-fade | spring 0.35 / 1.0 | E |
| 22 | Open viewer | tile rect → fit rect (matched geometry), ground 0 → 1 | spring **0.40 / 0.90** | IOS-REF §8 |
| 23 | Viewer swipe | the page follows 1:1; commit / return | spring 0.35 / 1.0 | E |
| 24 | Pinch zoom release | to the clamped zoom / pan | spring 0.35 / 0.85 | E |
| 25 | Double-tap zoom | fit ↔ 2.5× at the tap | spring 0.35 / 0.90 | E |
| 26 | Swipe-down dismiss | scale 1 → 0.6 and ground alpha by the distance; release → into the tile | spring 0.40 / 0.90 | M (behaviour), E (numbers) |
| 27 | Chrome toggle | the bars fade 0 ↔ 1; the light ground ↔ black | 0.20 s ease | E |
| 28 | Info panel | the photo slides up; the cards come up with it | spring 0.40 / 0.90 | E |
| 29 | Delete (viewer) | the photo → scale 0.8 + fade (0.25 s); the next slides in | 0.25 s + spring 0.35 / 1.0 | E |
| 30 | Delete (grid) | the tiles shrink + fade; the grid reflows | spring 0.35 / 1.0 | E |
| 31 | Select toggle | the check circle pops 0.6 → 1.0 | spring 0.25 / 0.60 | E |
| 32 | Context menu | lift 1.03, the rest blurs | IOS-REF §3.3 | |
| 33 | Edit open | the photo moves from the viewer's fit rect to the edit fit rect; the tools rise 40 pt + fade | spring 0.40 / 1.0 | E |
| 34 | Crop release | the crop rect animates to fill the area | spring 0.40 / 1.0 | E |
| 35 | Collection section collapse | the chevron rotates −90°; the cards' row height → 0 | spring 0.30 / 1.0 | E |
| — | Reduce Motion | 22 / 26 / 33 become 0.25 s cross-fades; 7 / 20 lose the stretch | A (HIG) | |

---

## 5. Words (fixed strings, our English; Title Case as iOS)

**Camera**
- Modes (caps in the capsule): `PHOTO`, `VIDEO`, `PANO`, `TIME-LAPSE`.
- Tools: `FLASH`, `TIMER`, `EXPOSURE`, `ASPECT`, `GRID`.
- Values:
  - Flash: `Auto`, `On`, `Off`.
  - Timer: `Off`, `3s`, `5s`, `10s`.
  - Aspect: `4:3`, `16:9`, `1:1`.
- Edit: `Edit`, `Reset to Essentials`, `Done`.
- Captions: `FLASH AUTO`, `FLASH ON`, `FLASH OFF`, `AE/AF LOCK`.
- Zoom: `.5`, `1×`, `2`, `5`.
- Video readout: `HD`, `30`.
- States:
  - `Starting the camera…`
  - `Camera Not Available`
    - `The camera is in use by another app.`
    - `The camera can't be used during a call.`
    - `This phone's back camera isn't supported yet.`
    - driver reason (sh_cam_why)
  - `Video Not Available` / `This phone can't record video yet.`
- Toasts: `Not saved: <reason>`, `The camera is busy`, `The photo didn't come` (trunk).
- Permission alert: `“<App>” Would Like to Access the Camera`, `Don't Allow`, `Allow`.
- Settings > Camera (A names, trimmed):
  - `Grid`, `Level` (if the accelerometer allows), `Mirror Front Camera`, `Use Volume Up for Burst`;
  - `Preserve Settings` → `Camera Mode`, `Exposure Adjustment`;
  - `Lens Cleaning Hints`: **omit**.

**Photos**
- Tabs: `Library`, `Collections`, `Search`.
- Library:
  - `Years`, `Months`, `All`, `Select`, `Cancel`;
  - `Select All`, `Deselect All`, `Show Selected (N)`, `Select Items`;
  - `N Items`, `N Photos`, `N Videos`;
  - `Looking for photos… N found`.
- Sort & Filter:
  - `Sort by`, `Date Captured`, `Recently Added`;
  - `Filter`, `All Items`, `Favourites`, `Edited`, `Photos`, `Videos`, `Screenshots`, `Remove Filters`;
  - `View Options`, `Zoom In`, `Zoom Out`, `Aspect Ratio Grid`, `Square Photo Grid`.
- Collections:
  - `Collections`, `Pinned`, `Edit`, `Albums`, `New Album`, `Recent Days`, `Media Types`, `Utilities`, `Reorder`;
  - `Large Grid`, `Small Grid`, `Mixed Grid`, `Collapse All`;
  - `Favourites`, `Recently Saved`, `Videos`, `Selfies`, `Panoramas`, `Screenshots`, `Screen Recordings`;
  - `microSD Card`, `Hidden`, `Recently Deleted`, `Duplicates`;
  - `Today`, `Yesterday`;
  - `View Album`, `Use your passcode to view this album.`;
  - `Pin`, `Unpin`, `Rename`, `Delete Album`.
- Search: `Search`, `Recent Searches`, `Clear`, `No Results`, `Try a new search.`
- Viewer: `Copy`, `Duplicate`, `Hide`, `Add to Album`, `Use as Wallpaper`, `Favourite`, `Unfavourite`, `Revert`,
  `Revert to Original`.
- Info: `Add a Caption`, `Adjust`, `Albums`, `No map offline`.
- Delete:
  - `Delete Photo`, `Delete Video`, `Delete N Photos`, `Delete N Items`;
  - `This photo will be deleted from this phone. It will be in Recently Deleted for 30 days.`
  - `These N photos will be deleted from this phone. They will be in Recently Deleted for 30 days.`
  - `This photo will be permanently deleted. You can't undo this action.` (until Recently Deleted exists)
  - `N photos on the microSD card can't be deleted.`
- Recently Deleted:
  - `Recover`, `Delete`, `Recover All`, `Delete All`, `N days`, `1 day`;
  - `Photos are permanently deleted after 30 days.`
  - `Delete N Photos?`, `These photos will be deleted from this phone. You can't undo this action.`
  - `Recovered N Photos`.
- Edit:
  - `Cancel`, `Done`, `Discard Changes`, `Save as Copy`, `Reset`;
  - `ADJUST`, `FILTERS`, `CROP`; `Adjust`, `Filters`, `Crop`;
  - `Auto`, `Exposure`, `Highlights`, `Shadows`, `Contrast`, `Brightness`, `Saturation`, `Warmth`;
  - `FREEFORM`, `ORIGINAL`, `SQUARE`, `WALLPAPER`, `9:16`, `4:5`, `5:7`, `3:4`, `3:5`, `2:3`, `STRAIGHTEN`;
  - Filters: `Original`, `Vivid`, `Vivid Warm`, `Vivid Cool`, `Dramatic`, `Dramatic Warm`, `Dramatic Cool`, `Mono`,
    `Silvertone`, `Noir`.
- Share:
  - `1 Photo Selected`, `N Photos Selected`, `Options`, `Include location`;
  - `Copy Photo`, `Copy N Photos`, `Add to Album`, `Use as Wallpaper`, `Duplicate`, `Hide`, `Save to Files`;
  - toasts `Copied`, `Added to “<Album>”`.
- Empty / error:
  - `No Photos or Videos`
    - `Photos and videos you take with Camera, or put on a microSD card, appear here.`
    - `Put a microSD card with pictures in the phone, or take a photo with Camera.`
  - `The microSD card can't be read. Showing photos taken on this phone.`
  - `Reading the Card…`
  - `Can't Show This Photo` (+ reason); `This video can't be played yet.`
  - `Photos on the microSD card can't be changed yet.`
  - `Photos taken now won't be kept until the next normal start.`
  - `No Albums`.

---

## 6. Starbite today → gap

**Trunk 1c166b85, what exists:**
- **Camera:** `shell_camera.inc`, 536 lines. It is **not** an empty page: there is a working front-camera page on
  Track A's driver (`sh_cam_*`). It has:
  - a 360 x 480 mirrored preview drawn 4× into a 3:4 frame at y 112..632;
  - an iOS ≤ 18 layout: a mode strip at y 642 **above** a 78/62 shutter at y 718, PHOTO yellow, VIDEO disabled;
  - 44 circles for the thumbnail and flip; Flash / Live circles at the top (y 66);
  - the front flash (white screen), the shutter blink, a growing thumbnail;
  - store saves after the thumbnail lands, and a "Camera Not Available" empty state with sh_cam_why;
  - a portrait-lock turn (cp7).
  - Missing: the rear camera (another track's driver), zoom, focus, tools, video, burst, a sound, haptics, a lock-screen
    entry.
- **Photos:** `shell_media.inc` (2135 lines, shared with Files and Music):
  - the card's DCIM/Pictures (read-only exFAT) merged with the Camera store (32, deletable), by date;
  - a 3-column grid (128.9 pt tiles, 1.6 pt gaps, half-resolution tiles, cached thumbs), opening at the newest;
  - Years / Months / All (a centred capsule, cards per month/year); a "Library" 22 / 700 header;
  - a dimmed "Select" (toast);
  - the viewer: a tile → viewer flight, swipe between photos with rubber band, double-tap zoom, one-finger pan with
    rubber band, swipe-down dismiss, 44 back circle, bottom 44 circles (share / heart dimmed with toasts; info = a
    toast; delete = an action sheet "Delete Photo" for store photos);
  - empty / error states in words; rotation in the viewer.
  - Missing: tabs, Collections, Search, Select, pinch (the shell has **no multi-touch**), info panel, edit, share,
    favourites, Recently Deleted, filmstrip, the iOS 27 date capsule and toolbar grouping.

**Ranked changes** (S ≤ 1 day, M 2-4 days, L ≥ 1 week):

| # | Change | Size |
|---|---|---|
| 1 | Camera: re-lay out to iOS 27 (C1). Mode capsule **below** the shutter; 80/68 shutter at y 645; 48 thumbnail / flip at y 741; tools button; top glyphs without circles; VF y 72..592; status bar hidden; privacy dot | S |
| 2 | Photos: the **tab bar + Search circle** (Library / Collections), its minimise on scroll with Years/Months/All as the accessory; title 34 / 700 + subtitle; Sort & Filter circle + Select capsule | M |
| 3 | Photos viewer to iOS 27: date capsule, ⋯ circle, **48 toolbar** (share · [heart info edit] · trash), filmstrip, info panel (details card from EXIF / store header), working favourite (store flag) | M |
| 4 | **Select mode** with drag-select + auto-scroll, check circles, the Show Selected / share / trash bar, multi-delete sheet | M |
| 5 | **Multi-touch in the shell** (2 contacts from the touch driver → pinch for Camera zoom, grid levels, viewer zoom). Platform work | L |
| 6 | **Recently Deleted** in the store (a deleted flag + time, a purge after 30 days, recover) + the locked collection behind the passcode; until then, the "permanently deleted" words | M |
| 7 | Camera: focus / exposure square + sun slider + AE/AF Lock (needs the driver's AE compensation; AF if any) | S-M (driver-gated) |
| 8 | Camera: tools menu (Flash, Timer, Exposure, Aspect, Grid) + the timer countdown + volume-key capture + the shutter sound + mode / flip motion | M |
| 9 | Collections page: Pinned, Albums (create / rename / delete; store metadata), Recent Days, Media Types, Utilities; pushed grids | M-L |
| 10 | Edit: Adjust (8 tools) + Crop (rotate, flip, straighten, aspect presets) with a non-destructive edit record; Save as Copy for card photos; Revert | L |
| 11 | Lock-screen camera: swipe-left interactive transition + the hold button + secure-camera limits | M |
| 12 | Search (dates, albums, media types, file names) | M |
| 13 | Share sheet (local targets) once Copy (a pasteboard for images) and wallpaper-from-photo exist | S-M |
| 14 | Burst (drag-left), then QuickTake / Video when a video path exists; Pano / Time-lapse later | M / L (driver-gated) |
| 15 | Rear camera: switch the page to it as the default when its driver lands (flip toggles; the front stays mirrored) | S (UI side) |

---

## 7. Still estimated (no attended capture; these stay E until a public source shows them)
1. Every animation timing in §4 except the end states measured from the art (the record square, the tools panel, the
   viewer layout). In particular: the shutter press scale, the blink, the thumbnail fly-in, the flip blur, and the
   viewer open / dismiss springs.
2. The mode carousel's exact order beyond Video ← Photo → (Siri / Portrait), the label pitch at rest, and the edge
   fade.
3. The tools panel's top on a 4:3 frame (measured once, from one art) and its corner radius; the value-picker capsule
   look.
4. The focus square's tick marks and timings; the exposure line length; the AE/AF Lock badge look.
5. The zoom dial's look; the zoom labels' font.
6. The Photos grid's pinch levels (1 / 3 / 5 / 9 used here), the fast-scrubber handle, and the drag-select auto-scroll
   speeds.
7. The minimised tab bar's circle size (56) and the Years / Months / All capsule's exact width / height (~220 x 48).
8. The Collections card radii (22), the shadows, and the Albums see-all grid.
9. The viewer's chrome-hidden rule for light mode (white → black), the zoom limits, and the dismiss thresholds.
10. The Info panel's photo crop position when open, and the card radii.
11. The Edit adjust-dial value ring, the tick slider spacing, and the crop handle sizes.
12. Every sound (shutter, burst tick, video chimes, timer ticks): described here, ours to make; levels are E.
13. Haptics: all Camera and Photos haptics are E (Apple doesn't document them per app).
14. The delete dialog's message line (iOS's names iCloud; ours is a local rewording) and the Recently Deleted header
    line.
15. Dark-mode Photos colours: the art is light; the dark values come from IOS-REFERENCE §15 semantics.
