# App Store (iOS 27) — Starbite build spec

Version 1.0, 2026-11-13 (design-ref Track C, for Track F). Frame, units, shared components: see
`docs/design/IOS-REFERENCE.md` (cited as **IR §n**) and `docs/design/LIQUID-GLASS.md` (cited as **LG §n**).

**Tags.** **A** Apple stated · **M** measured (source named) · **3P** a reviewer · **E** estimate.

**Main measurement source (M):** the Apple iPhone User Guide, iOS 27 edition, "Get apps in the App Store", product page
art (490 x 1008 px PNG, a 402-pt Pro phone, **1.1095 px/pt**; screen x from 22 px, y from 20 px; calibrated by the
44-pt back circle = 45 pt and the Dynamic Island = 126 pt). Kept at
`build/design-ref/apps27/appstore/page27.png` (git-ignored). Width-filling values are scaled 390/402; fixed chrome is
kept in pt (IR §0).

**What iOS 27 is here (A/3P):**
- The User Guide, iOS 27 edition, lists the tabs as **Today, Games, Apps, Arcade, Search**. Its art shows **Search
  as a fifth tab inside the one glass capsule**, not a separate circle (M, page27.png). A/M.
- This came in iOS 26.4: Search went back into the tab bar and its field moved back to the **top** of the Search tab
  (3P: 9to5Mac 2026-02-23; MacRumors iOS 26.4 guide). iOS 26.0-26.3 had the separate search circle. **Build iOS 27:
  Search is a tab.**
- iOS 26.4 also added a reworked account page with a dedicated **App Updates** section, two taps away (3P: MacRumors).
  The User Guide, iOS 27 edition, says: tap the account button or your picture, then **App Updates**, then Update or
  Update All. Updated apps then show under **Updated Recently** (A).
- iOS 27 adds Personalized Collections and "App Notes", a line saying why an app is recommended (A: Apple Newsroom
  2026-06-08). We render App Notes as our local equivalent, "Because you installed X" (§2.4).
- The Get button is **"Get"** in title case: a filled blue capsule on the product page (M). The all-caps "GET" is
  iOS ≤ 18.

**Local equivalents (our catalogue):**
- Free Linux apps for the big cores. Each app offers one to three **install methods**: Latest Release, Build from
  Source on the M3 cores, Debian Package (`starbite/appstore/FORMAT.md`; `sh_lpkg_t.methods`).
- **Omit:** Arcade, prices, in-app purchases, Face ID or side-button purchase confirmation, ratings and reviews written
  by users, gift cards, the Apple Account, Family Sharing, subscriptions, the age-rating questionnaire.
- **Local equivalents:**
  - the account picture → the **Account button** (owner initial, update badge);
  - the purchase sheet → the **Install sheet**, which chooses the method;
  - ratings → our **Touch** rating (Good / OK / Poor);
  - age rating → **Licence**;
  - Privacy "Data Used to Track You" → **What It Can Access** (`needs`).

**Build order (Track F, 2026-11-13).** §2.1 Structure → §2.2 Buttons → §2.3 Product page → §2.4 Today → §2.5 Search →
§2.6 Games / Apps tabs → §2.7 Account and Updates. Then §3 flows, §4 motion, §5 words, §6 gap, §7 captures.

---

## 1. Information architecture

**Tab bar (iOS 27: Today / Games / Apps / Arcade / Search). Ours, four tabs in one capsule:**

| # | Tab | iOS 27 | Ours | Glyph (our art) |
|---|---|---|---|---|
| 1 | **Today** | editorial feed | the editorial feed from the catalogue's `featured` + stories | a page with a top band (calendar-sheet look) |
| 2 | **Games** | games charts, categories | the catalogue's **Games** category, laid out like iOS Games | a rocket |
| 3 | **Apps** | apps charts, categories | every non-game category (Internet, Office, Graphics, Media, Utilities, Development, Education, Science, System) | three stacked layers |
| — | Arcade | subscription games | **omit** (no tab) | — |
| 4 | **Search** | search, as a tab | the same | a magnifier |

- Four tabs: the capsule gets wider tab slots, with the same geometry rules (§2.1). E.
- **Updates is not a tab**, as in iOS 27. It lives behind the Account button, top right of every root tab, which
  carries a red count badge. A **Prominent tab** (IR §18) is not used. Starbite today has an Updates tab, which this
  replaces (§6).

**Navigation tree**
- Today
  - Story (zoom from its card, §2.4.3) → Product page
  - "See All" list → Product page
- Games / Apps
  - Product page
  - "See All" (a chart or collection list) → Product page
  - Category → category page → Product page
- Search
  - Results → Product page
  - Suggestion / recent → Results
- Account button (any root tab) → **Account sheet** (large detent):
  - App Updates (pending, Update All, Updated Recently)
  - Installed (ours; iOS: Apps / Purchased) → Product page
  - Storage and Linux (ours)
- Product page (pushed over the tab that opened it; the tab bar stays)
  - Ratings strip cell → its section on the page (Touch → "Touch" section, Size → Information)
  - What's New → **Version History** (pushed)
  - Description "more" → expands in place
  - Information → Install Method menu, Licence (pushed text), What It Can Access (pushed list)
  - Get → the Install sheet when the app offers more than one method (§2.2.5)
  - Share → **omit** in v1 (no share target yet). Keep the glass circle slot for later. E.

Pushed pages keep the tab bar visible (iOS: product pages show the tab bar, M page27.png). The story view (§2.4.3)
covers it.

---

## 2. Screens

### 2.1 Structure: the tab bar, titles, Account button, scroll edge

#### 2.1.1 Tab bar (one Liquid Glass capsule; IR §10, LG §5.3)
| Item | iOS 27 measured (402 phone) | **S9 value** | Tag |
|---|---|---|---|
| Capsule | x 18.9 .. 383, top y 788.7, height ~62, bottom gap ~23 | **x 18, w 354, y 718, h 62, r 31** (bottom 780) | M page27 / IR §10 |
| Slots | 5 equal slots in 365 pt minus 4+4 inner padding | **4 slots of (354 − 8) / 4 = 86.5**, starting at x 22 | M → E |
| Selected pill | a lighter glass pill behind the selected item (Search: blue glyph + label area 321..386 = 65 wide) | **pill = slot width − 0 = 86.5 x 54, r 27, inset 4 top / bottom**. Fill: light `#FFFFFF` 55 % over the glass / dark `#FFFFFF` 14 % | M (width) / E (fill) |
| Glyph | ~24-26 pt, filled variant when selected | **25 pt box**, centre y = capsule top + 22 (**740**) | M (≈) |
| Label | under the glyph, caps ~8 pt high → 10 pt font | **10 / 600** (Caption 2 bold-ish), baseline capsule top + 49 (**767**) | M |
| Colours | selected: systemBlue glyph + label; others: label colour | selected **#0088FF** light / **#0091FF** dark; unselected **#1A1A1A** light / **#F5F5F5** dark | IR §10 |
| Badge | — (iOS badges the account picture, not a tab) | none on tabs | A (User Guide) |
| Pill move | the pill slides between tabs with a liquid stretch | spring **response 0.35, damping 0.75**; during the move the pill stretches to 1.15x width in the direction of travel and settles (LG §5.3 merge) | IR §10, E |
| Press | the touched item's glass grows | touch-down: the capsule area under the finger lights (LG §5.1 glow); the pill does **not** jump until release | E |
| Haptic | selection on change | **selection** | IR §17, E |
| Re-tap selected tab | scroll to top (if at top on a pushed page: pop to root) | same: pop to root with the push spring, else scroll to top over 0.45 s ease-out | A (long-standing UIKit), E |
| Minimise on scroll | App Store does **not** minimise its tab bar (the art shows it full over scrolled content) | **no minimise** | M (page27, mid-scroll content under it) / E |

- **Scroll edge, bottom:** content passes under the capsule. Use LG §5.4 soft edge: a band of 62 + 16 = 78 pt above
  the bottom safe band (y 702..802). E.

#### 2.1.2 Large title and date header (root tabs)
| Item | Value | Tag |
|---|---|---|
| Date line (**Today tab only**) | weekday, day month, e.g. "THURSDAY 13 NOVEMBER", Footnote **13 / 600**, uppercase, tracking +0.3, secondaryLabel; baseline **LARGE_BASE − 38 = 86** | E (iOS 11-18 layout, kept in 26/27) |
| Large title | **34 / 700** (IR §14), x 20, baseline **124** (frame). "Today" / "Games" / "Apps" / "Search" | IR §10 |
| Content start | first card / section top = **title baseline + 18 = 142** | E |

#### 2.1.3 Account button (every root tab, top right)
- **Geometry:** a **36 pt circle**, its right edge at x 370 (margin 20), centred vertically on the large title's
  cap middle, **y centre 112**. E (iOS 18 layout, unchanged in 26 art seen by 3P).
- **Our art:** a disc filled with systemGray (#8E8E93) and the owner's initial in white, 17 / 600, centred. The name
  comes from Settings > About > Name; with none, a person glyph. Never a photo in v1.
- **Badge:** a red **#FF383C** circle, **18 pt** (a capsule if 2 digits), white 12 / 600 number. It overlaps the top
  right with its centre at (circle right − 3, circle top + 3). The count is the number of updates pending (`aq_badge`).
  IR §10.
- **Tap:** opens the **Account sheet** (§2.7), with a light impact on touch-down (E).
- **On collapse (§2.1.4):** it moves into the nav row as a **32 pt** circle centred at y 66, x centre 352, and its
  scale follows the title cross-fade. E.

#### 2.1.4 Collapse to the inline title (Games, Apps, Search, pushed lists)
- Scroll offset `s` (0 = at rest).
- For `s` in 0..44, the large title moves up 1:1 with the content.
- From `s` = 24 to 44 (**20 pt**), the large title cross-fades out and the **inline title** fades in: **17 / 600**,
  centred at x 195, baseline **71** in the nav row (y 44..88). IR §10.
- **Today has no inline title:** its header (date + "Today" + Account button) scrolls away with the content, and
  only the top scroll edge remains. M (iOS 18 behaviour) / E for 27.
- **Overscroll (pull down at the top):** the large title stays pinned to the content and rubber-bands with it. iOS
  doesn't enlarge it. E.
- **No pull-to-refresh** on Today, Games or Apps; iOS has none (E). The catalogue refreshes on open.

#### 2.1.5 Top scroll edge
- LG §5.4 **soft**: a band from y 0 to **SAFE_TOP + 44 + 16 = 104** when an inline title or nav buttons show, and
  **0..60** on Today (status band only).
- Blur ramps 0 → 1 toward the top. Light: alpha toward the background 0 → .6. Dark: darken 0 → .25. No hairline.
- **Over the zoomed story's hero:** no edge band, so the photo runs to the top. E.

#### 2.1.6 Push / pop of pages inside a tab
- IR §16 nav push: response **0.45**, damping **1.0**. The new page slides the full width. The old page moves 30 %
  under a dim of black 10 % (light) / 20 % (dark). The moving page's corners are rounded to **32** (our display
  corner) during the move.
- **Back:** a **44 glass circle** with a 17 / 600 chevron at **x 16, y 44..88** (centre 38, 66). M: page27 shows it
  at x 14.4, w 46. Plus the interactive **edge swipe** from x < 20 (§4).

---

### 2.2 Buttons: Get / Open / Update, waiting, progress, the Install sheet

#### 2.2.1 Sizes by context
| Context | Size (w x h) | Fill | Label | Tag |
|---|---|---|---|---|
| **Product page header** | **min 74 x 32**, r 16; width = label + 2 x 20 | **systemBlue filled** (#0088FF / #0091FF) | **17 / 600 white**, title case | M page27 (74 x 32.5 at x 153, bottom flush with the icon bottom) / E (type) |
| **Lists, search results, charts** | **min 74 x 30**, r 15; width = label + 2 x 18 | **tertiarySystemFill**: light rgba(118,118,128,.12) / dark rgba(118,118,128,.24) | **15 / 600 systemBlue** | E (iOS 18: 72 x 28 GET 15/700; iOS 26 title case, slightly larger) |
| **On a dark card / story hero** (Today) | 74 x 30, r 15 | white 24 % (dark text-on-image cards) or white 100 % on light | 15 / 600 white (on 24 %) / systemBlue (on white) | E |
| **Nav bar compact** (after the header scrolls away, §2.3.2) | **64 x 28**, r 14 | systemBlue filled | 15 / 600 white | E |

- The label set (title case, ours): **Get · Open · Update · Retry · Install** (Install appears in the Install sheet
  only).
- **Space for "Update":** the capsule widens to label + padding. It never shrinks under the min width. All
  label changes animate the width with a spring of **0.35 / 1.0** while the label cross-fades over 0.15 s. E.
- **Disabled** ("Not Enough Space" on the page): the fill becomes tertiarySystemFill and the label tertiaryLabel. The
  reason is shown in a note under the header (§2.3.1).
- **Under the button (iOS "In-App Purchases" caption): omit.** Our slot holds the method word in **Caption 2 11 /
  400 secondaryLabel**, centred under the capsule, 4 pt below it: **"Release" · "Source" · "Debian"**. Only on the
  product page, and only when the app offers more than one method. E (our use of iOS's slot).

#### 2.2.2 Press state
- Touch-down: the label and fill go to **alpha 0.55** in 0.08 s. Release: back to 1 in 0.25 s ease-out. No scale.
  This is a **content-layer** button, not glass (LG §5.5). E (UIKit highlighted button).
- Drag off by more than 30 pt: cancel the highlight. Act on touch-up inside.
- **Haptic:** none on Get in iOS. iOS gives the purchase confirmation **notification success** after Face ID. Ours:
  **impact light** on touch-up when an install starts. E.

#### 2.2.3 The state machine and the morph (one control)
```
Get ──tap──► [Install sheet if >1 method or source default] ──Install──► Waiting ──bytes or build start──► Progress
 ▲                                                                                     │       │
 │◄──────────── Cancel (tap the stop square) ◄─────────────────────────────────────────┘       │
 Retry ◄── failure (note under header; Retry = Get again with the same method)                 ▼
                                                                                      Open (installed)
Open ──(catalogue says newer)──► Update ──tap──► Waiting ──► Progress ──► Open
```
| State | Shape | Content | Tag |
|---|---|---|---|
| **Get / Open / Update / Retry** | the capsule (§2.2.1) | label | M/E |
| **Waiting** (`SH_PKG_QUEUED`, or INSTALLING with phase 0 / VERIFYING) | the capsule **shrinks to a circle** of the capsule's height (30 or 32), fill clears to **none** | an **indeterminate ring**: diameter **26** (list) / **28** (page), stroke **2.5**, a **25 % arc of systemBlue** over a **systemGray5 track** (#E5E5EA / #2C2C2E), rotating **one turn per 0.9 s**, linear | E (iOS 18 behaviour; 27 same, 3P captures) |
| **Progress** (DOWNLOADING with total, BUILDING, INSTALLING with pct) | circle, no fill | **determinate ring**: same diameter and stroke, track systemGray5, progress **systemBlue** from 12 o'clock clockwise, round caps. Centre: a **stop square 8 x 8 (list) / 9 x 9 (page), r 1.5**, systemBlue | E |
| Ring progress motion | — | the arc animates to each new value over **0.25 s ease-out**, never backwards (clamp; a phase change to Verifying goes back to the indeterminate spin) | E |
| **Done** | the circle **grows back into the capsule** | "Open" cross-fades in | E |
| Removing | circle | indeterminate ring, **no** stop square | ours |

- **Morph springs:** capsule → circle **response 0.30, damping 0.9** (width and the radius together, the centre
  stays on the capsule's centre for lists, or the **leading edge** stays put on the product page). Circle → capsule
  **0.35 / 0.85**. The label fades out in 0.1 s before the shrink. E.
- **Hit area:** at least 44 x 44 around the ring. Tapping the ring **stops**: in lists, immediately; on the product
  page as well (iOS doesn't confirm). For a **source build more than 50 % done** (ours), confirm with an alert:
  "Stop Building?" / "The build will start again from the beginning." / [Keep Building] [Stop] (Stop = destructive).
  E.
- **Status line (ours, iOS has none in the button):** the row's subtitle (lists) or a Footnote line under the header
  (page) shows the phase in words. Keep `aq_status`'s words (§5). iOS shows only the ring, so ours adds this text
  because builds take minutes. E.
- **The same app everywhere:** every place that shows the app (card, row, header, nav compact button) shows the same
  state, live.

#### 2.2.4 Redownload / installed-before
- iOS shows a **cloud with a down arrow** (systemBlue glyph 24 pt, no capsule) for apps bought before.
- **Local equivalent:** shown when the app is not installed but its files are still cached on the memory card from
  an earlier install, so reinstalling needs no network. Tap = install from the cache with the method used last
  time, no sheet. E (ours).

#### 2.2.5 The Install sheet (ours; iOS's purchase confirmation sheet, re-used for the method choice)
iOS slides up a sheet on Get with the app's icon and name, the account, and a "Double Click to Install" prompt. We
have no payment or authentication, so the same sheet becomes the **method choice**.
- **When it appears:**
  - Get on the product page when the app offers **more than one** method;
  - Get anywhere when the default method is **Build from Source**, so a long build never starts by surprise.
- **When it doesn't:** single-method apps, list Get with a Release / Debian default, Update and Redownload. These
  start at once.
- **Geometry (IR §3.4 medium detent, fitted to content):** side and bottom inset **6**, top radius **33**, bottom
  corners **26**, Liquid Glass. Grabber **36 x 5**. Height = content (≈ **360**: y 436..796). E.
- **Content, top to bottom:**
  1. Header row (y +16): **44 glass "x" circle** at left (16 from the edges); centred title **"Install"** 17 / 600.
  2. App line (y +72): icon **60 pt** (radius 13.4, §2.3), name **Headline 17 / 600**, under it subtitle 15 / 400
     secondaryLabel. Left 20, text at x 92.
  3. **Method list** (y +148): an inset grouped card (IR §6: x 16 w 346 inside the sheet, r 26). One **52-pt row per
     offered method**, ordered **Latest Release, Build from Source, Debian Package**. Each row: title 17 / 400 + a
     second line 13 / 400 secondaryLabel (`aq_mver` + size or the build time: "2.4.1 · 38 MB", "About 12 min on the
     M3 cores · 2.5.0-dev", "2.3.0 · Debian trixie"), a trailing **checkmark** systemBlue 17 / 600 on the chosen
     row. Rows are 64 tall with two lines. The preselected row is `def_method`.
  4. A footnote (13 / 400 secondaryLabel) under the card for the chosen method:
     - Release: "Downloads the developer's newest release."
     - Source: "Builds the newest code on the big cores. Your phone stays usable but warm."
     - Debian: "Installs Debian's tested package."
  5. **Install** button: a full-width (sheet − 2 x 20) **50-tall capsule**, systemBlue prominent glass, label **17 /
     600 white**, 20 above the sheet's bottom edge.
- **Interactions:**
  - Tap a row: the checkmark moves, with a **selection** haptic.
  - Install: the sheet dismisses (spring 0.45 / 1.0), and the source button goes straight into **Waiting**, so the
    morph is seen on the page.
  - "x", swipe-down or a tap on the dimmed page cancels: nothing starts.
- **Remember:** the method last chosen for an app is preselected next time for that app. E.

#### 2.2.6 Haptics and sounds (buttons)
- Start install: impact light. Selection change in the sheet: selection. Install finished: **none** (iOS is silent;
  the app's icon appears on the home screen). Failure: **notification error**, once, when the button turns into
  Retry. Stop: impact light. E.
- Sounds: none.

---

### 2.3 Product page (pushed; the tab bar stays)

Measured from page27.png (M, 402 phone). S9 y values are measured **relative to the back circle**, so the S9's
shorter status band is already taken out. All sections use margin **20**.

#### 2.3.1 Header
| Item | iOS 27 (402 phone) | **S9** | Tag |
|---|---|---|---|
| Nav row | back circle 44-46 at x 14, y 59..104 | back **44 glass circle** x 16, y 44..88. Right: **share 44 glass circle** at x 330 (**omit in v1**, keep the slot empty) | M |
| Icon | **118 x 118** at x 19.8, top 20 below the back circle | **118 x 118 at x 20, y 108**; continuous corners, the home-screen icon shape scaled (r = 0.2237 x 118 = **26.4**); a 0.5-pt hairline rim of black 8 % (light) | M (size, place) / E (r; the measured corner span ≈ 25 pt suggests a smaller visible radius, so check on the S9 capture) |
| Name | cap top 5 below the icon top; cap height 15.3 → **22 / 700** (Title 2 bold) | x **155**, baseline **y 129** (icon top + 21); up to 2 lines (leading 28); then ellipsis | M |
| Subtitle | the developer / subtitle in grey under the name | **15 / 400 secondaryLabel**, baseline 151, 1 line. Ours: the app's `summary` (short line), else the category | M (place) / E (type) |
| Get | **74 x 32** at x 153, **bottom flush with the icon bottom** | x **155**, y **194** (226 − 32); §2.2.1 page size. The method word goes under it (§2.2.1). | M |
| Status line (ours) | — | when busy or failed: **Footnote 13 / 400**, x 155, baseline 244 (below the icon), secondaryLabel / systemRed for failures. `aq_status` words. | E |
| Note band (ours) | — | the space / network / memory notes: a **13 / 400** line with a 15-pt glyph at the left, x 20, under the icon, before the strip; orange #FF8D28 for warnings, red for errors. Only when needed. | E |

#### 2.3.2 The nav bar when the header scrolls away (iOS behaviour)
- Once the **Get button's bottom passes under the nav row** (scroll `s` ≥ 194 + 32 − 88 = **138**), these cross-fade
  in over 16 pt of scroll in the nav row:
  - a **small icon 30 x 30** (r 6.7), centred at x 195, y 66;
  - the **compact Get** (§2.2.1, 64 x 28), right-aligned at x 370 (where the share circle is, or left of it if
    share exists);
  - the back circle stays.
- Scrolling back reverses it.
- The compact button mirrors the header button's state, including the ring (diameter 24, stroke 2.2).
- The top scroll edge (§2.1.5) is on whenever `s` > 0.
- iOS behaviour since iOS 11, unchanged in 26 (3P captures). E for the numbers.

#### 2.3.3 Info strip (horizontally scrolling "badges" row)
- **Position:** a 0.5-pt separator at y **246** (x 20..370). The strip runs y 246..340, with a separator at 340 (both
  separators: the separator colour, IR §15).
- **Columns:** **108 wide** (fixed), first centre at **x 68**. Separators between columns are **0.5 x 44 pt**,
  vertically centred, at x 122, 230, 338, … The strip scrolls horizontally with content insets 14 / 20. No paging,
  free scroll, no indicator. M (centres 68 / 176 from page27).
- **Three lines per column, centred:**
  1. Label: **11 / 600 uppercase**, tracking +0.06, secondaryLabel, cap top at **262**, baseline **270**.
  2. Value: **20 / 700** (Title 3 bold), **secondaryLabel** (grey, not black; M: the "4.2" is grey), baseline **302**.
     Or a **22-pt glyph** in secondaryLabel.
  3. Sub-line: **13 / 400** secondaryLabel (stars in iOS: 11 pt), baseline **326**.
- **iOS 26/27 order:** Ratings, Age Rating, Category, Developer, Language, Size (A, the User Guide's list; M, art).
- **Ours, in this order:**

| # | Label | Value | Sub-line | Tap → |
|---|---|---|---|---|
| 1 | **TOUCH** | Good / OK / Poor / — | "with touch" / "not rated" | the "Touch" section (§2.3.7) |
| 2 | **LICENCE** | the first SPDX id ("GPL-3.0", "MIT") | "open source" / "or later" / "or Apache-2.0" | the Licence page (pushed text) |
| 3 | **CATEGORY** | a 22-pt category glyph | the category name | the category page |
| 4 | **DEVELOPER** | a 22-pt person glyph | the project / developer name (`source`) | — (v1) |
| 5 | **SIZE** | number ("38") | unit ("MB") — the chosen method's download (Release) or install size | Information |
| 6 | **MEMORY** | number ("210") | "MB in use" | Information |
| 7 | **BUILD** (only if Source is offered) | "~12" | "min from source" | the Install Method row |

- Tap: a cell highlights (systemFill behind the column, r 12, for 0.1 s). The page then scrolls to the target
  section with the scroll-to spring 0.5 / 1.0. E.

#### 2.3.4 Screenshots carousel
- Starts at y **356** (strip bottom + 16). Its own section title is **not shown** in iOS 26/27 (M: the art shows the
  shots straight under the strip). E.
- **Shot size:**
  - iOS: **235 wide** (portrait phone shots), **9 gap**, x 19.8, continuous corners with a ~34-pt curve span, which
    is **r ≈ 22** (M).
  - **Ours:** a Linux window is 720 x 1344 (0.536). Use **w 235, h 439**, r **22**, 0.5-pt hairline rim of black 10
    % / white 10 %.
  - Landscape shots: h 232, w = h x aspect.
- **Scrolling:** horizontal, **snaps** to each shot's leading edge at x 20 (decelerate then snap to the nearest; a
  flick moves at most 2 shots). Spring 0.4 / 1.0 for the snap. E (iOS behaviour).
- **Tap a shot:** a full-screen viewer (zoom from the shot, matched geometry, 0.45 / 0.9). Black background, the
  shots paged at full width, a 44 glass "x" at top right. Swipe down to close (the shot shrinks back). E.
- **No shots:** the carousel is omitted. Never a placeholder.
- **Our source of shots:** catalogue `screenshots[]` (PNG on the card). Add to FORMAT.md (§6).

#### 2.3.5 Description
- Starts 24 below the carousel.
- Type: **15 / 400**, leading 20, label colour (E: iOS 18 uses 15 pt here).
- Shows **3 lines**. If longer, a **"more"** link (15 / 400 systemBlue) is right-aligned on the third line, with a
  fade of the text under it (a 60-pt gradient to the background behind "more").
- Tap "more" (or anywhere in the text): it **expands in place** to full height. Height spring 0.4 / 1.0; the text
  below slides down. No collapse back (iOS behaviour). E.
- Under the text, a developer row: the developer name 15 / 400 systemBlue, the role "Developer" 13 / 400
  secondaryLabel under it, and a chevron right. Ours: the project name + "Project". Tap → **omit in v1** (later: more
  by this project). E.

#### 2.3.6 What's New
- **Section header:** "What's New" in **22 / 700** (Title 2 bold) at x 20, as the existing `aq_sec`. "Version History" link right-aligned, 17 / 400 systemBlue, on the
  header's baseline. E.
- **Line under the header:** the version ("Version 2.4.1", 15 / 400 secondaryLabel) at left, and the relative date
  ("2d ago", "3w ago", "Mar 4", 15 / 400 secondaryLabel) at right.
- **Notes:** 15 / 400 label, 3 lines + "more" (as §2.3.5).
- **Placement:** after the description (E; the iOS 27 art puts the shots right under the strip).
- **Ours:** the release notes of the newest version of the **chosen method** (Release: the tag's notes; Source: the
  last 3 commit subjects + "Built from <short hash>"; Debian: the changelog's top entry).
- **Version History page (pushed):** an inset list. Each row has the version (Headline 17 / 600) with the date
  (secondaryLabel 15) at right, and the notes under it (15 / 400, 3 lines + more). Plain list, separators inset 20.
  Large title "Version History".

#### 2.3.7 Ratings & Reviews → ours: **Touch**
- **Omit** user reviews (no service).
- **Local equivalent:**
  - A section "Touch" with a card (inset grouped, r 26) that shows how the app works on the phone. A big **48 / 700**
    word ("Good"), and three checks with SF-style glyphs (13 / 400): "Fits the screen", "Works with touch", "Uses the
    keyboard".
  - These come from the catalogue's `touch` notes.
  - The card has no stars.

#### 2.3.8 What It Can Access (iOS "App Privacy")
- **Section header:** "What It Can Access", with a "Details" link (17 / 400 systemBlue).
- **Card:** an inset grouped card with one 52-pt row per need. Each row has a 30-pt rounded-square glyph tile (r 7.5,
  IR §6) in a colour, a title 17 / 400 and a 13 / 400 line. The needs and their words:
  - Network: "Connects to the internet"
  - Audio: "Plays or records sound"
  - GPU: "Uses the graphics cores"
  - Camera: "Can use the camera"
  - Files: "Reads and writes your files in Linux"
  - Keyboard: "Needs the keyboard"
- **None:** a single row "Works fully offline, with no special access."
- iOS uses large cards with a privacy-label look; ours is the list (E).

#### 2.3.9 Information (iOS: Provider, Size, Category, Compatibility, Languages, Age Rating, Copyright, …)
- **Section header:** "Information" (22 / 700).
- **Layout:** a plain list (iOS style here is NOT a grouped card). **Rows 44 tall** (E: iOS 18 ≈ 44). Label 15 / 400
  secondaryLabel at x 20; value 15 / 400 label, right-aligned at x 370. Separators inset 20..370. Long values have a
  chevron down, and a tap expands the row in place (iOS behaviour for Compatibility, Languages).
- **Rows (ours, in order):**
  1. **Install Method** — the value is the chosen method with a **pop-up button glyph (chevron up/down, 13 pt)** in
     systemBlue. Tap → a **pull-down menu** (IR §3.3, morph from the value) with the offered methods, each with a
     second line (version · size / build time) and a check on the current one. Choosing one:
     - not installed: sets the method for Get (the method word under Get updates).
     - installed with another method: an alert **"Reinstall from Source?"** / "Your settings and files in Linux are
       kept." / [Cancel] [Reinstall].
  2. **Installed** — version · method (only when installed).
  3. **Newest** — the newest version for the chosen method.
  4. **Developer** — the `source` project.
  5. **Size** — download / installed sizes ("38 MB download · 112 MB installed").
  6. **Memory** — "About 210 MB in use".
  7. **Category**.
  8. **Licence** — value + chevron right → the licence text, pushed.
  9. **Package** — the package id (selectable, 13 / 400 monospaced).
  10. **Verified** — "Signature checked" (when `SH_PKG3_VERIFIED`) / "From the card's mirror" (MIRROR).
- **Under Information** (installed apps only), two **text-button rows**, each a 52-pt inset grouped card with a
  centred label (iOS has none; ours):
  - "Roll Back to <version>" (systemBlue);
  - "Remove App" (systemRed). Remove → an action sheet (IR §3.2, anchored): "Remove <Name>? Its files in Linux are
    kept." [Remove App] (destructive). System apps show the footnote "Part of the system: it can't be removed."
    instead.

#### 2.3.10 Bottom of the page
- **"You Might Also Like":** a horizontal shelf of app lockups. A lockup is a column: icon 60 (r 13.4), name 13 /
  400 (2 lines), category 13 / 400 secondaryLabel, Get list pill.
  - Use the **row shelf** (§2.6.2): 3 rows per column, column width 390 − 40 − 24 (the next column peeks 24).
  - Content: same-category apps.
- Then **120 pt** of space under the last section, so the end clears the tab bar.

#### 2.3.11 States
- **Loading** (catalogue not read yet): the header with the icon placeholder (systemGray5 rounded square) and grey
  bars (r 4) for the name/subtitle, shimmer off; then fades in when ready (0.25 s). E.
- **Linux not running:** the existing "Starting Linux" screen stays, but as a **centred empty state** (§2.8).
- **App removed from the catalogue** while open: the page stays, Get becomes disabled, note "No longer in the
  catalogue.".

---

### 2.4 Today

#### 2.4.1 Layout (S9)
- **Header (§2.1.2–2.1.3):** the date line at baseline 86, "Today" at baseline 124, the Account button at (352, 112).
- **Cards:**
  - x **20**, w **350**, h **440**, continuous corner r **24**. iOS: full width − 40, 450-480 tall, r 20-26 (IR
    §13). The S9 is 50 shorter, so 440.
  - The first card's top is at **y 142**; **24** vertical gap between cards. iOS 18: 30-ish. E.
  - Shadow: light mode black 12 %, y 6, σ 20; dark mode none. E.
- **Day sections:** Today scrolls back through earlier days. Each new day starts with its own date line + a
  **"Yesterday" / weekday 28 / 700** title (Title 1 bold), 32 above the next card. Ours: the catalogue's stories of
  the last 7 days; then the end. E.
- **End of the feed:** a centred footnote, "You're all caught up." (13 / 400 secondaryLabel), 40 above the tab-bar
  band. Ours.

#### 2.4.2 Card types (our art; Today content from the catalogue, §6)
All text on the cards is white over art, unless the card is a "light" list card. Text inset **20** from the card
edges.

| Type | Content and layout | Type sizes | Tag |
|---|---|---|---|
| **Story (hero)** | Full-bleed art. **Top:** the label (e.g. "GET STARTED", "BEHIND THE CODE") then the title (≤ 3 lines). **Bottom:** the subtitle line. A **scrim** behind each text block: a linear gradient black 0 → 35 % over 140 pt (top block: from the top down; bottom: from the bottom up). | label **13 / 600 uppercase**, tracking +0.3, white 70 %, baseline 36; title **28 / 700** (Title 1 bold), white, leading 32, first baseline 70; subtitle **15 / 400** white 85 %, baseline card bottom − 20 | E |
| **App of the Day** | Full-bleed art. **Lower-left, above the band:** "APP / OF THE / DAY" stacked on three lines (**44 / 800**, leading **38**, tighter than the size; white, or black on light art), inset **22**, its last baseline 14 above the band. **Bottom band 78 tall** (0.224 x card width): a blur strip over the art (a dark or light material picked from the art, not Liquid Glass: tk blur σ 20 + black 25 % / white 60 %), with **icon 48** (r 10.7) at x +16, name **17 / 600** + subtitle **15 / 400** at 70 % at x +76, and the card Get (§2.2.1) at the right − 16 | as given | M (from apple.com/app-store marketing art "download_instantly", the iOS 13-18 card design: proportions scaled to a 350 card) / E (unchanged in 26/27) |
| **Collection list** | A **plain card** (secondarySystemGroupedBackground: white / #1C1C1E). Top: the label (13 / 600 uppercase secondaryLabel) + title (**28 / 700** label colour) at inset 20. Then **4 app rows** (§2.6.1 row, 64 tall, separators inset to the text). The card's height grows to fit, max 440. Tap a row → its product page (push, not zoom). Tap the header → the story with the full list (zoom). | as given | E |
| **Single app ("the app card")** | A small card **h 200**: art band on top (h 120), the app row under it (icon 48, name, subtitle, Get) | E | E |
| **App Notes (iOS 27)** | Not a card. A one-line **13 / 400 secondaryLabel** note with a sparkle glyph, under a collection card's title: "Because you installed GIMP". | | A (feature) / E (look) |

**Fallback when the catalogue has no stories** (today's case), generated from the data:
1. App of the Day = a featured app, rotated by date.
2. A collection "Top Apps in <biggest category>".
3. Featured app cards.
4. "Recently Updated" (a collection of apps with a new version in 7 days).
5. "On Your Phone" (installed apps with updates; only if any).

#### 2.4.3 Press and the card-to-story zoom (the signature motion)
**Press:**
- Touch-down on a card (not on its Get): the card **scales to 0.96** around its centre.
- Spring for the press: response **0.25**, damping **1.0**. Release: back to 1.0 with **0.35 / 0.7**, a slight
  overshoot.
- Moving the finger more than 10 pt (a scroll starts) cancels the press: the card springs back and the list scrolls.
- No haptic. E (iOS 11-18 behaviour, kept in 26/27).

**Open (tap):**
1. The **story view** starts exactly at the card's on-screen rect: x, y, w, h, r 24, scale 1, from the pressed scale.
2. One spring, **response 0.5, damping 0.82** (≈ duration 0.5, bounce 0.18), animates its frame to the full screen
   **(0, 0, 390, 802)**, with the corner radius **24 → 32** (our display corner).
   - IR §13 says 0.45 / 0.85.
   - iOS shows a visible small overshoot: the top edge goes ~4 pt past and settles. E.
3. **Hero geometry:**
   - The card art becomes the story's hero at the top: its width grows 350 → 390, and its height to **490** (440 x
     390 / 350), so the art keeps its aspect and its anchor (top-left of the card ↔ top-left of the screen).
   - The label / title / subtitle **stay pinned to the hero** at the same inset (20), so they ride along with no
     cross-fade.
4. **Body:** it sits below the hero, starting at y 490 + 24. It **fades in over the second half** of the spring
   (0.25 s, delay 0.15 s) and slides up 20 pt as it does.
5. **The rest of the feed:** it stays where it is behind, dimmed by black 0 → 30 % (light) / 0 → 50 % (dark). E.
6. **Tab bar:** it slides down **out of the screen** (y + 100) with the same spring, and the status bar stays.
   **Today's header scrolls nothing.**
7. **Close button:** a **36 glass circle** with an "x" 15 / 700, at **x 338..374, y 52..88**. It materialises (LG
   §5.2) once the spring passes 60 %.
   - In iOS ≤ 18 it was a 30-pt dark translucent circle; 26/27 make it a Liquid Glass circle (3P captures). E.
   - The circle flips light / dark by the art behind it (LG §3).
8. **Haptic:** none. Sound: none.

**Story view contents (scrolls vertically):**
- The hero (490).
- Body text, **Body 17 / 400**, leading 25, x 20, w 350.
- Inline art (w 350, r 16).
- App lockup rows (§2.6.1, inside an inset card r 26).
- At the end, a **large app lockup**: a card with icon 60, name, subtitle and the Get list pill, then 60 pt of space.
- **The story has no tab bar and no nav bar:** only the close circle, which stays fixed at the top right while the
  story scrolls. Once the hero has scrolled away, the top scroll edge appears (§2.1.5).

**Close:**
- **Tap the close circle:** the reverse morph to the card's current rect (the feed may not have moved). The spring
  is **response 0.45, damping 0.9** (less bounce). The body fades out over the first 0.15 s and the tab bar slides
  back in.
- **Drag to dismiss** (only when the story is scrolled to its top, or from the left edge anywhere):
  - **Pull down at scroll 0:** the story stops scrolling and the **whole view scales down** with the finger:
    `scale = 1 − 0.25 · min(1, d / 400)` (d = drag distance down, pt), anchored at the screen centre. Its corners
    round 32 → 40 at most. The view also follows the finger by d · 0.15 vertically.
  - **Left-edge pan** (start x < 20): the same, with `d` = the horizontal distance, and the view moves right by
    d · 0.15.
  - **Release:** if `scale ≤ 0.90` (d ≥ 160), or the release speed is > 600 pt/s in the drag direction, it
    **dismisses**. The view morphs from its current (scaled) frame into the card's rect with **0.45 / 0.9**. Else it
    springs back to full screen with **0.35 / 0.85**.
  - **Haptic:** **impact soft** once when the threshold is crossed (ours; iOS has none, so keep it subtle). E.
- **iOS feel:** the shrink starts at once with the finger (no dead zone), and the scrolled-down content does not
  rubber-band during the dismiss drag. E.

---

### 2.5 Search (a tab in iOS 27; field at the top)

**iOS 27:** Search is the last tab in the capsule, and its field sits **at the top** under the large title, as in
iOS 18 (3P: 9to5Mac / MacRumors on 26.4; A/M: the iOS 27 User Guide art shows the Search tab inside the bar).

#### 2.5.1 Layout (S9)
- **Large title** "Search", baseline 124, with the Account button.
- **Field:**
  - y **138..182**. A **44-tall capsule** (iOS 26 search fields are capsules) at x 16..374, r 22, fill
    **tertiarySystemFill**. E (shape A from IR §10; place E).
  - Magnifier glyph 17 pt secondaryLabel at x 30. Placeholder **17 / 400**, secondaryLabel, at x 54: "Apps, Games
    and More". A **mic** glyph at the right: **omit** (no dictation yet).
- **Focused (tap the field):**
  - The large title collapses (the §2.1.4 cross-fade, done at once over 0.3 s) and the field moves up to the **nav
    row, y 44..88**.
  - A **44 glass "x" circle** appears to the right (x 330..374), and the field shrinks to x 16..322. Spring 0.4 /
    0.9.
  - The keyboard rises (IR §4, Return key **"search"**, systemBlue).
  - Typing shows a **clear (x) button** 17 pt in the field's right end.
  - The "x" circle cancels: it clears the text, ends editing and goes back to the start state.
- **Idle content (not focused, field empty):**
  1. **"Discover"** (22 / 700 header): **6 rows of trending terms**. Each row is 44 tall, with a magnifier glyph 15
     pt systemBlue at x 20 and the term **17 / 400 systemBlue** at x 46, and separators inset 20. Ours: the
     catalogue's `trending` list, else the existing AQ_SUGG.
  2. **"Suggested"** (22 / 700): 4 app rows (§2.6.1 standard row).
  3. **"Categories"** (ours, iOS 26 shows them here too, 3P): a 2-column grid of category tiles.
     - Tile w (350 − 10)/2 = 170, h 100, r 16; gap 10. The category colour (AQ_CATC) fill, with a
       **17 / 600 white** name at bottom-left inset 12, and a 40-pt white glyph at top-right at 60 % alpha.
     - Tap → the category page.
- **Focused, field empty:**
  - **"Recent"** (17 / 600 header, a **"Clear"** link 17 / 400 systemBlue at right).
  - Up to 8 recent terms as rows (44, a clock glyph 15 pt secondaryLabel, the term 17 / 400 label). Tap → search it.
    Swipe a row left → **Delete** (red, 15 / 600).
  - With no recents: the Discover rows.
- **Typing (live, on every key, debounce 120 ms):** suggestion rows, 44 tall. Each has a magnifier 15 pt
  secondaryLabel; the **typed part in label colour 17 / 400**, the completion in **17 / 600**. Up to 8 rows, plus
  up to 2 direct **app rows** (compact row with Get) when a name starts with the text. E.
- **Submit** (Return or a suggestion): the keyboard goes down. The **results** replace the content; the field stays
  in the nav row with the text and the "x" circle.

#### 2.5.2 Result rows
- **A result block** per app, separators between blocks inset 20:
  - The standard row (§2.6.1): icon 64, name, subtitle, Get.
  - **Then a screenshot strip:** up to 3 portrait shots side by side under the row, each **(350 − 2 x 8) / 3 =
    111 wide, h 207** (our 0.536 aspect), r **12**, gap 8, at y row bottom + 8.
  - With 1 landscape shot: one shot w 350, h 197.
  - Block height = 84 + 8 + 207 + 20.
- Results with no shots: the row only.
- **Ordering:**
  1. A name match;
  2. a summary / keyword match;
  3. a category match.
  Installed apps show Open. E.
- **No results:** a centred content-unavailable state (UIKit's search empty state) in the free space above the
  keyboard:
  - a magnifier glyph **48** tertiaryLabel;
  - 12 below it, **"No Results for “<q>”"** in **22 / 700**, label;
  - 8 below that, **"Check the spelling or try another word."** in 15 / 400 secondaryLabel.
  - A (pattern) / E (sizes).
- **Search is offline-safe:** the catalogue is local, so there is no network error here.

#### 2.5.3 Haptics
- Keyboard per IR §4. A selection haptic on "Clear". Nothing on submit. E.

---

### 2.6 Games and Apps tabs; shared rows and shelves

#### 2.6.1 App rows (one component, two sizes)
| Size | Height | Icon | Text | Get | Tag |
|---|---|---|---|---|---|
| **Standard** (search results, charts, See All lists) | **84** | **64** at x 20, centred vertically, continuous r 14.3 (0.2237 x 64) + 0.5-pt rim | x **96**: name **17 / 400** label (2 lines max, leading 22), subtitle **13 / 400** secondaryLabel (1 line); a chart row puts the **rank 17 / 600** in front of the name at x 96 and moves the name to x 122 | list pill, right edge 370, centred vertically | E (iOS 18 layout) |
| **Compact** (shelves, cards, Updates) | **64** | **48** at x 20, r 10.7 | x **80**: name **16 / 500** label, subtitle **13 / 400** secondaryLabel | list pill | E |

- **Row separators:** 0.5 pt, from the text's x to 370; the last row has none.
- **Tap a row:** highlight systemFill (or systemGray5) behind the row for the press, then push the product page.
  Highlight in 0 s, out in 0.25 s. E.
- **Long-press a row or card (iOS: a context menu with the app):** **omit in v1**. Later: Share, "Install from
  Source…". E.

#### 2.6.2 Layout of Games / Apps (S9)
1. Large title "Apps" / "Games" + the Account button (§2.1).
2. **Hero carousel**:
   - Paging cards **w 350, h 260, r 24**, gap 10. The next card peeks 10 pt at the right.
   - Content: art, the label 13 / 600 uppercase, title **22 / 700** white and a bottom band with a compact row (white
     text, dark-card Get).
   - Snaps per card. Spring 0.4 / 1.0. Press 0.96 (as §2.4.3). Tap → product page (push, **not** zoom). E.
3. **Shelves** (repeat):
   - A **section header**: **22 / 700** at x 20, with "See All" **17 / 400 systemBlue** right-aligned at 370 on the
     same baseline. Header block 44 tall, 24 above it.
   - Then a **row shelf**: columns of **3 compact rows**, column width **326** (390 − 40 − 24), gap 10. It scrolls
     horizontally and snaps per column; the next column peeks.
   - Ours:
     - "Top Apps in <category>" (by the catalogue's `rank`, else alphabetical);
     - "Recently Updated";
     - "Build It Yourself" (source-capable apps; ours);
     - "Works Great With Touch" (touch = Good).
4. **"Top Categories"** (Apps tab only): a list of categories. Each row is 52: a 30-pt tile (r 7.5, the category
   colour) with a white glyph 18 pt at x 20, the name **17 / 400** at x 62, and a chevron. Then a "See All" row. Tap
   → the category page.
5. **Category page** (pushed): large title = the category, then standard rows, no shelves.
6. **Games tab, small catalogue:** if the catalogue has fewer than 8 games, no shelves: the hero carousel + one
   standard list "All Games". E (ours).

#### 2.6.3 States
- **Empty category:** "No Apps" 22 / 700, with "Apps in this category will appear here." 15 / 400 secondaryLabel,
  centred.

---

### 2.7 Account sheet and App Updates (iOS 26.4/27: the account page with an App Updates section)

- **Open:** tap the Account button. A **sheet, large detent** (IR §3.4: top y **52**, r 33, opaque grouped
  background). The tab content behind scales to 0.92, rounds its corners and dims 25 %.
- **Sheet chrome:** a centred title "Account" → ours **"Your Apps"** (17 / 600). A **Done** circle at the right: a
  **44 glass circle with a checkmark**, prominent systemBlue (iOS 26 sheet style, IR §3.4). Swipe down closes it.
- **Content (an inset grouped list, IR §6, rows 52):**
  1. **Owner card** (iOS: the account picture + name + email): a 60-pt initial circle, the name 20 / 600 and "Starbite
     OS · Galaxy S9" (15 / 400 secondaryLabel). Not tappable in v1.
  2. **App Updates** (a row with a 30-pt blue tile + arrow-down glyph; the count as a value; chevron) → pushes **App
     Updates**.
  3. **Installed** (count, chevron) → a list of the installed apps (standard rows with Open, swipe left → **Remove**,
     red), sorted by name. iOS: "Apps" / Purchased.
  4. **Storage** — "<free> free on the memory card" (value, no chevron).
  5. **Linux** — "Running" / "Starting…" / "Stopped" (value).
  6. **Install Method for New Apps** — a menu row: Ask Each Time / Latest Release / Build from Source / Debian
     Package. Ours: it sets the default for list Get. E.
- **App Updates page (pushed inside the sheet), title "App Updates":**
  - **Pull to refresh** = check for updates. A spinner (the system activity indicator, 20 pt) appears under the nav;
    the **"Checked just now"** footnote updates. E (iOS supports pull-to-refresh here, 3P).
  - **"Pending"** section (iOS: "Upcoming Automatic Updates" / "Pending"):
    - Header 22 / 700 with **"Update All"** (17 / 400 systemBlue) at right, shown when 2 or more.
    - Rows: the **compact row** with the date ("2 days ago") as subtitle and an **Update** list pill.
    - Under each row, the update's notes **15 / 400, 2 lines + "more"**.
    - Then a footnote under the version: "2.4.0 → 2.4.1 · Latest Release" (13 / 400 secondaryLabel). Ours.
  - **"Updated Recently"** section: rows with **Open** pills and the date the update was installed; notes the same.
    7 days. A (section name) / E.
  - **"Roll Back"** (ours): rows of apps that can roll back, each with a "Roll Back" text button 15 / 400 systemBlue.
  - **No updates:** under "Pending", a centred footnote **"All apps are up to date."** (15 / 400 secondaryLabel).
  - **Offline:** a footnote row: "Not connected. Showing what the memory card's mirror knows." / "Not connected.
    Connect to Wi-Fi or mobile data to check for updates.".
- **Update All:** every pending row goes to **Waiting** at once. The rows run one at a time (`phone jobs one at a
  time`): each row's ring fills, then it slides from Pending to Updated Recently. The row moves with the list's
  default move: 0.35 s, the others close up. E.

---

### 2.8 App-wide states and words
| State | Where | Look | Words |
|---|---|---|---|
| Linux starting | whole app, first open | centred: a 20-pt activity spinner, then **17 / 600** title and **15 / 400** secondaryLabel line | "Starting Linux" / "The App Store opens when Linux is up, in a few seconds." |
| Linux failed | whole app | the content-unavailable style: a warning glyph 48, a title 22 / 700, a message, and a **"Try Again"** capsule (glass, 44 tall) | "Linux Didn't Start" / "The big cores did not start." / Try Again |
| Catalogue missing | whole app | the same style | "No Catalogue" / "The App Store's catalogue isn't on the memory card yet." |
| Reading the catalogue | root tabs | skeleton: grey cards (systemGray6 / #1C1C1E, r 24) in the card frames, no shimmer | — |
| No network, app needs it | product page note + Get stays enabled | note line with a wifi-slash glyph 15, orange | "Needs the internet to download. Connect to Wi-Fi or mobile data." |
| No network, mirror present | note, grey | — | "No network: it installs from the memory card's mirror." |
| Not enough space | product page | disabled main button + red note | "Not Enough Space" · "It needs 112 MB on the memory card; 40 MB is free." |
| Install failed | button → Retry; red status line; hint lines | — | "Didn't install: <engine words>" + `hint` |
| Too big for memory | orange note | — | "Uses about 900 MB of memory; Linux has 1.5 GB. It may run slowly." |
| iOS "Cannot Connect to App Store" | **omit** | (the catalogue is local) | — |

---

## 3. Cross-screen flows

**F1. Install an app with one method, from a list**
1. Tap Get in a row. The pill highlights, then on touch-up an impact light.
2. The pill morphs to the waiting ring (0.30 / 0.9). The subtitle reads "Waiting…".
3. Downloading: the ring fills, and the subtitle reads "Downloading 12.3 of 40 MB".
4. Verifying: the ring spins again, "Verifying…". Installing: the ring fills, "Installing 80 %".
5. Done: the ring grows into **Open** (0.35 / 0.85). The subtitle goes back to the summary. The app's icon appears in
   the home screen's Linux page.
6. Tap Open → `lx_open_app`: the app opens over the store with the app-open zoom (home screen spec).

**F2. Install from source on the product page**
1. On the product page, tap Get. The **Install sheet** slides up (0.45 / 1.0).
2. Tap "Build from Source": the checkmark moves (a selection haptic) and the footnote changes.
3. Tap Install: the sheet goes down, and the header button morphs to Waiting, then to the determinate ring.
4. The status line reads "Building 34 % · about 8 min left". The compact nav-bar button shows the same ring when
   scrolled.
5. Leave the page or the app: the progress continues. The Account button's badge does not count installs; the home
   icon shows the iOS-style **download ring on the icon** (home screen spec).
6. Done → Open.

**F3. Switch the method of an installed app**
1. Information → Install Method → the menu → "Latest Release".
2. An alert: "Reinstall from Latest Release?" / "Your settings and files in Linux are kept." / Cancel ·
   **Reinstall**.
3. Reinstall: the header button → Waiting → ring → Open.

**F4. Update**
- From the Account button: Your Apps → App Updates → Update (or Update All).
- From the product page: Update in the header.
- Each runs as F1. The badge count drops when each one finishes.

**F5. Open a Today story and go to an app**
1. Tap a card: press 0.96, then the zoom (§2.4.3).
2. Scroll the story. Tap the lockup's Get (installs, F1) or the lockup row: **the product page is pushed inside the
   story's context**. It covers the story with the nav push; Back returns to the story.
3. Close the story: drag down, or tap the "x" circle.

**F6. Search**
1. Tap the Search tab; tap the field; type "gimp"; tap a suggestion.
2. The results appear with shots. Tap a result → the product page (push). Back → the results, with their scroll kept.
3. Tap the Search tab again: if on a pushed page, pop to the results; again: clear and back to idle with the field
   unfocused (E).

**F7. Remove**
- Product page → Remove App → the action sheet → Remove App.
- The button goes through the removing ring (no stop square) to Get.

---

## 4. Motion table
| # | Trigger | What moves | Spring / duration | Tag |
|---|---|---|---|---|
| 1 | Tab change | selection pill slides + stretches | 0.35 / 0.75 | IR §10 |
| 2 | Tab change | content cross-fade (no slide) | 0.2 s ease | E |
| 3 | Root title collapse | large → inline title cross-fade over 20 pt of scroll; Account button 36 → 32 into the nav row | scroll-driven | IR §10, E |
| 4 | Push product page | full-width slide, old page −30 % + dim, moving page corners 32 | 0.45 / 1.0 | IR §16 |
| 5 | Edge-swipe back (x < 20) | page follows the finger 1:1; old page parallax 30 % → 0; release: finish if moved > 50 % width or speed > 500 pt/s, else cancel | finish / cancel 0.45 / 1.0 from the release speed | E (UIKit) |
| 6 | Product header scroll-away | small icon + compact Get fade into the nav row over 16 pt | scroll-driven | E |
| 7 | Card press | scale 0.96 | down 0.25 / 1.0; up 0.35 / 0.7 | E |
| 8 | Card → story | frame, corner 24 → 32, hero 350 → 390 wide | 0.5 / 0.82 | E (IR §13: 0.45 / 0.85) |
| 9 | Story body | fade in + up 20 | 0.25 s, delay 0.15 | E |
| 10 | Story open | tab bar slides out (y + 100); feed dims | same spring as 8 | E |
| 11 | Story close (tap) | reverse morph | 0.45 / 0.9 | E |
| 12 | Story drag dismiss | scale 1 → 0.75 with the finger; threshold 0.90 / 600 pt/s | interactive; finish 0.45 / 0.9; cancel 0.35 / 0.85 | E |
| 13 | Close circle | materialise (lensing ramp, 0.8 → 1) | 0.35, no bounce | LG §5.2 |
| 14 | Get → waiting | capsule → circle, label out 0.1 s | 0.30 / 0.9 | E |
| 15 | Ring progress | arc to the new value | 0.25 s ease-out | E |
| 16 | Spinner | 25 % arc rotating | 0.9 s / turn, linear | E |
| 17 | Done → Open | circle → capsule, label in | 0.35 / 0.85 | E |
| 18 | Label width change (Get ↔ Update) | width | 0.35 / 1.0 + 0.15 s cross-fade | E |
| 19 | Install sheet | slide up / down | 0.45 / 1.0 | IR §3.4 |
| 20 | Account sheet (large) | slide up; content behind → 0.92 + r 32 + dim 25 % | 0.45 / 1.0 | IR §3.4 |
| 21 | Search focus | title collapse, field to the nav row, "x" circle in | 0.4 / 0.9; keyboard IR §4 | E |
| 22 | Description "more" | height expands, content below slides | 0.4 / 1.0 | E |
| 23 | Carousel snap (shots, hero, shelves) | to the nearest item edge | 0.4 / 1.0 | E |
| 24 | Information menu | value → menu morph | 0.40, bounce 0.20 | LG §5.3 |
| 25 | Row moves Pending → Updated Recently | row slides, others close up | 0.35 s ease-in-out | E |
| 26 | Reduce Motion | 8 / 11 / 12 become a 0.25 s cross-fade; 1 slides with no stretch | — | IR §16 |

---

## 5. Words (our English; title case where iOS uses it)
- **Tabs:** Today · Games · Apps · Search
- **Today:** <WEEKDAY DAY MONTH> · Today · Yesterday · APP OF THE DAY · GET STARTED · BEHIND THE CODE · You're all
  caught up.
- **Buttons:** Get · Open · Update · Retry · Install · Update All · See All · Version History · more · Done · Cancel ·
  Try Again · Clear
- **Method words:**
  - Latest Release · Build from Source · Debian Package
  - under Get: Release · Source · Debian
  - Install sheet footnotes, as §2.2.5.
- **Install sheet title:** Install
- **Strip labels:** TOUCH · LICENCE · CATEGORY · DEVELOPER · SIZE · MEMORY · BUILD
- **Strip sub-lines:** with touch · not rated · open source · or later · MB in use · min from source
- **Sections:** What's New · Touch · What It Can Access · Information · You Might Also Like · Discover · Suggested ·
  Categories · Recent · Top Categories · Top Apps in <Category> · Recently Updated · Build It Yourself · Works Great
  With Touch
- **Information labels:** Install Method · Installed · Newest · Developer · Size · Memory · Category · Licence ·
  Package · Verified
- **Account:** Your Apps · App Updates · Installed · Storage · Linux · Install Method for New Apps · Ask Each Time ·
  Pending · Updated Recently · Roll Back · All apps are up to date. · Checked just now · Checked <n> min ago
- **Status:** Waiting… · Downloading… · Downloading 12.3 of 40 MB · Verifying… · Building 34 % · Building 34 % · about
  8 min left · Building 34 % · under a minute left · Installing… · Installing 80 % · Removing… · Didn't install
- **Alerts:**
  - Stop Building? / The build will start again from the beginning. / Keep Building · Stop
  - Reinstall from <Method>? / Your settings and files in Linux are kept. / Cancel · Reinstall
  - Remove <Name>? Its files in Linux are kept. / Remove App
- **Search:** Search · Apps, Games and More · No Results for "<q>" · Check the spelling or try another word.
- **Needs:** Connects to the internet · Plays or records sound · Uses the graphics cores · Can use the camera · Reads
  and writes your files in Linux · Needs the keyboard · Works fully offline, with no special access.
- **States:** as the table in §2.8.

---

## 6. Starbite today → gap (trunk 1c166b85: `shell_appstore.inc` 1562 lines, `shell_linux.inc` 990)

**What it has:**
- A glass tab bar: Today / Browse / Updates (badge) + a separate search circle (the iOS 26.0 layout), TB_Y 718, h 62.
- Today: the date line + title, featured cards 350 x **264** r 22 with a gradient from the icon's mean colour, and
  "Top Apps in …".
- Browse categories; Search with the keyboard and suggestions, plus "No Results".
- Updates: Check, Update All, Roll Back.
- The product page: a 100-pt icon, 22 / 700 name, a **GET** capsule 84 x 32 (all caps), a 4-cell strip (SIZE /
  MEMORY / TOUCH / LICENCE), an **inline segmented Install Method** control + card, the description, Information,
  Roll Back and Remove.
- Rows 76 with 48 icons and a 74 x 30 pill (all-caps GET / OPEN / UPDATE / RETRY).
- A progress ring (r 13, stroke 2.6, stop square 8).
- Chunked painting, live layers for progress, and momentum scrolling.

**Ranked changes:**
1. **(M) Tabs → iOS 27:** Today / Games / Apps / **Search inside the capsule**, 4 slots, the sliding pill (§2.1.1).
   Remove the separate search circle and the Updates tab. Add the **Account button** with the badge and the **Your
   Apps sheet** with App Updates (§2.1.3, §2.7). Browse becomes the Apps tab.
2. **(M) Button words and styles:** title case "Get / Open / Update / Retry". Page button 74 x 32 filled blue 17 /
   600; list pill tertiarySystemFill + blue 15 / 600. The waiting spinner state and the capsule↔circle morph (§2.2.3)
   are new; today it swaps instantly.
3. **(L) Card-to-story zoom** (§2.4.3): new. It needs a story view layer that animates frame / radius, the press
   scale 0.96, and the drag-dismiss. The cards themselves go to 350 x 440 r 24 with the card types of §2.4.2.
4. **(M) Method choice → the Install sheet + Information menu** (§2.2.5, §2.3.9). Replace the inline segmented
   control and "GET installs it the way chosen above." Add the method word under Get.
5. **(M) Product page header to measured geometry:**
   - icon 100 → **118**; name x 155;
   - Get bottom-aligned with the icon;
   - the strip → 108-pt scrolling columns with 7 cells, values grey 20 / 700;
   - the nav-bar compact icon + Get on scroll (§2.3.2).
6. **(M) Screenshots carousel, What's New + Version History, What It Can Access, description "more"** (§2.3.4–2.3.8).
   They need catalogue fields: `screenshots[]`, `notes` per version, `trending[]`, `stories[]`, `rank`, `developer`.
   That is a FORMAT.md change (Track F's store side).
7. **(S) Search to top-field iOS 27 behaviour:** the field under the title → the nav row on focus, the "x" circle,
   Recent + Clear, result blocks with 3 shots, the content-unavailable empty state.
8. **(S) Rows:** a standard 84 / icon 64 row for search and charts; a compact 64 / icon 48 row for shelves.
9. **(S) Games / Apps shelves:** the hero carousel and 3-row column shelves that snap.
10. **(S) States:** the content-unavailable style for Linux starting / failed / no catalogue; skeleton cards.

---

## 7. Still estimated (worth re-checking against public sources later)
1. **Today card size, radius and type** (350 x 440, r 24, title 28 / 700, label 13 / 600): look for an iOS 27 User
   Guide or Newsroom image of the Today tab ("Get apps" iOS 26 edition art; App Store marketing pages).
2. **Zoom and dismiss springs** (0.5 / 0.82 open, 0.45 / 0.9 close, 0.90 scale threshold): frame-step a public screen
   recording (MacStories / 9to5Mac iOS 26/27 videos) at 60 fps.
3. **List Get pill** (74 x 30, tertiarySystemFill, 15 / 600 blue) and the **progress ring** (26-28 dia, stroke
   2.5, stop square 8-9): a review screenshot of an iOS 26/27 search results list mid-download.
4. **Search tab on 26.4/27**: the field's height and shape at the top, the "x" circle, Recent/Discover headers — the
   9to5Mac 26.4 beta screenshots.
5. **Account sheet / App Updates layout** (26.4 hub): MacRumors' 26.4 guide images.
6. **Tab bar pill fill and label size** in light / dark (iOS 27 "revised dark-mode tab bars", IR §18).
7. **Product icon corner** (the page27 corner span suggests ~18 rather than 26.4): re-measure on a larger iOS 27
   render.
8. **Info strip type sizes** (20 / 700 grey value, 11 / 600 label) at a higher resolution.
9. **Description and What's New body size** (15 vs 17).
10. **Close circle size on stories** (36 glass vs 30).

---

## Sources
- Apple iPhone User Guide, iOS 27: "Get apps in the App Store on iPhone" (the tab list, product page facts, the
  Redownload button; product page art measured), https://support.apple.com/guide/iphone/get-apps-iphc90580097/ios ·
  "Update apps from the App Store on iPhone" (account → App Updates → Update All, Updated Recently),
  https://support.apple.com/guide/iphone/update-apps-iph98709f167/ios
- 9to5Mac, "iOS 26.4 makes two app updates that bring back iOS 18 designs" (2026-02-23), and "New iOS 27 designs
  reportedly coming to these iPhone apps" (2026-06-05).
- MacRumors, "iOS 26.4 Features" guide (search back in the tab bar, field at the top, account hub, Updates section).
- Apple Newsroom, "Apple expands App Store capabilities…" (2026-06-08): Personalized Collections, App Notes,
  Creative Assets.
- IR (IOS-REFERENCE.md) §3, §6, §10, §12-§18; LG (LIQUID-GLASS.md) §5.
