# Phone, Messages, Contacts (iOS 27) — Starbite build spec

Version 1.0, 2026-11-13 (design-ref Track C, for builder A2). Frame, units and shared components: see
`docs/design/IOS-REFERENCE.md` (cited **IR §n**) and `docs/design/LIQUID-GLASS.md` (cited **LG §n**). This file
replaces the short IR §7 (Messages) and IR §13 (Phone) entries where they differ; differences are flagged "**(IR fix)**".

> **Decisions (Track A, 2026-11-13; build to these):**
> 1. **Tap to dial**, as iOS does. The trunk's 0.6 s hold (a testing-phase guard) goes. The guard that matters stays:
>    self tests never place a call. Track A has told the main session in case the user prefers the hold.
> 2. **Hold** takes FaceTime's slot in the in-call grid.
> 3. **SMS reactions are sent as text** (Loved "…" etc.), as iOS does for SMS.
> 4. **The Contacts store exists** (trunk 9910ebf9): each contact is one document in the shell's document store
>    (shell.h `sh_doc_put / get / count / id_at / new_id / usage`, collection **`SH_DOC_CONTACTS`**): up to 32 KiB
>    under a stable id, within a **64 KiB** collection budget, saved with the settings. Use it for **names
>    everywhere** (Calls, Messages, banners, in-call, the lock's notifications): look a number up through one helper,
>    and match on normalised digits. Spec and build Contacts as if the store is there, with a **full state**: a
>    "Storage Full" alert ("Delete some contacts or notes to make room.", OK). A save that fails keeps the edit form
>    open with a footer "Not saved: storage full" (13 / 400, systemRed). Names then appear everywhere iOS shows them:
>    Calls, Messages, banners, the in-call screen.

**Tags.** **A** Apple stated · **M** measured (source named) · **3P** a reviewer · **E** estimate.

**Measurement sources (M).** Every one is a public image, kept git-ignored under `build/design-ref/apps27/`:

| Code | Image | Phone / scale | What it shows |
|---|---|---|---|
| **SUP** | Apple Support 126076 "Use Unified layout in the Phone app", `ios-26-iphone-17-pro-phone-calls-unified.png` (640 x 1320) + the Filter menu crops | 402-pt 17 Pro; screen 27..612 px → **1.4577 px/pt** (screen width and height both agree) | Calls tab: favourites grid, Recents rows, call buttons, tab bar, Filter menu |
| **UGc** | iPhone User Guide iOS 27, "Make a call" art (826 x 1016) | 402-pt; 1.1095 px/pt (the App Store spec's calibration: island 126, back circle 45) | iOS 27 Calls tab (same layout as SUP) |
| **UGi** | User Guide iOS 27, "While on a call" art: Hold Assist + Call Context (490 x 1008 each) | 402-pt, 1.1095 px/pt, screen x from 22, y from 20 | iOS 27 in-call screen |
| **UGm** | User Guide iOS 27, "Send and reply to messages" art: list (862 x 1008), thread (812 x 1146), green SMS thread (490 x 1008), "+" menu | 402-pt, 1.1095 px/pt | iOS 27 Messages |
| **UGk** | User Guide iOS 27, "Add your contact info" art: contact card (490 x 1008) | 402-pt, 1.1095 px/pt | iOS 27 contact card |
| **TB** | TidBITS 2025-11-10 "Comparing the Classic and Unified Views…", real iPhone 17 screenshots in mock frames (2560 x 2560 each) | 402-pt; screen 845 display px → **2.102 px/pt** (±1 pt) | Keypad (both layouts), Contacts tab, Filter menu, Recents |

The 402 → 390 conversion: fixed chrome (circle sizes, bar heights, fonts) stays in pt. Width-filling layouts (grids,
cards) scale by 390/402 = 0.970. Vertical: the S9 is 802 tall vs 874. The status band is 44 vs 54-62, so take 10 off
everything top-anchored. Bottom-anchored items keep their distance from the bottom edge. The leftover ~60 pt comes
out of scrolling content (IR §0).

**What iOS 27 is here (A/3P).**
- **Phone:** iOS 26 introduced the **Unified** layout and kept **Classic** as an option. The User Guide, iOS 27
  edition, still documents both; Unified is the default (A: Support 126076, UG27 "Make a call"). No iOS 27 Phone
  redesign is reported (3P: 9to5Mac 2026-07-23 lists none). iOS 27 adds Apple-Intelligence extras (Call Context
  card, wait-time prediction). **Omit** them.
- **Messages:** the iOS 26 design stays. iOS 27 changes that we take (3P 9to5Mac 2026-07-23):
  - failed messages **retry automatically**;
  - the right end of the text field holds the **audio button**, configurable as **Record Audio / Start Dictation /
    None** (Settings > Apps > Messages > Show in Text Field);
  - search finds conversations by phone number.
- **Contacts:** the iOS 26 design stays. Its search field sits at the **bottom** of the list (A: UG27 "Add and use
  contact information").

**Local equivalents (our modem, SMS only, no accounts).**
- **iMessage → SMS:** bubbles are always green, and the placeholder says "Text Message • SMS".
- **FaceTime → omit.**
- **Voicemail → omit** (no visual voicemail): no Voicemail filter item and no inline voicemail rows. Hold the "1" key
  to call the carrier's voicemail number when one is set (E). Today we have none.
- **Call Screening, Hold Assist, Call Context, Live Voicemail, call recording, Live Translation, screen sharing →
  omit.**
- **Contact posters → local simplified poster:** a tile or page tinted from the contact's colour, with the initials
  monogram (§2.12). There is no poster editor in v1.
- **Spam → omit** (no carrier spam data). **Unknown Callers / Unknown Senders → local equivalent:** numbers not in
  our Contacts.
- **Tapbacks → local equivalent:** stored with the message and drawn as on iOS. For SMS they are sent as a text, as
  iOS does for SMS (§2.9).
- **Backgrounds, polls, stickers, Memoji, Genmoji, Digital Touch, Check In, Apple Cash, inline replies, Send Later →
  omit in v1.** Backgrounds are an optional local-only extra (§2.10).

---

## 1. Information architecture

### Phone (Unified layout; Classic noted)
- Tab bar (IR §10): **Calls · Contacts · Keypad** in one glass capsule, plus a separate **Search** glass circle on the
  right (A: Support 126076, M: SUP/TB). Default tab: Calls.
- **Calls** (root). Nav row: **Edit** capsule (left), title "Calls" inline (centre), **Filter** circle (right).
  - Favourites grid → tap: call that favourite by its default method (A). Long-press: menu (§2.2).
    - "Show All" tile → Favourites list (pushed).
  - Recents list → tap the row: **contact card / caller info** (pushed) (A: UG27). The trailing call button calls (A).
  - Edit → menu: **Edit Favorites**, **Select** (A: UG27 "Edit, then Edit Favorites"; "Edit, tap Select"). With no
    favourites the first item is **Add Favorite** (A).
  - Filter → menu: Classic / Unified picker; Calls / Missed / Unknown Callers; Manage Filtering (§2.3).
- **Contacts** (tab): the same list as the Contacts app (§2.13), with its search field at the top (M: TB) and a "+"
  circle.
- **Keypad** (tab) (§2.4).
- **Search** (the circle): searches calls and contacts. Calls first, five results per group, then "See All" (3P TB).
- Call screens (over everything): Outgoing / In call (§2.5), Incoming full screen (§2.6), Incoming banner (§2.7). The
  status capsule shows while you are away from the call screen (§2.8).
- **Classic layout (option):** five tabs **Favorites · Recents · Contacts · Keypad · Voicemail**, no search circle.
  Recents has an always-visible **All / Missed** segmented control, rows carry an **(i)** info button, and a row tap
  calls (A/3P: TB, Support 126076).
  - **Build Unified only in v1.** Keep the Filter menu's Classic/Unified picker visible but disabled, with a
    "Classic layout isn't available yet" footer (E). Classic is a later item (§6 #18).

### Messages
- **Conversation list** (root): large title "Messages"; **Edit** capsule (left) and **Filter** circle (right) in the
  nav row; a bottom toolbar with a **Search** field (glass capsule) and a **Compose** glass circle (A/M: UGm list
  art).
  - Pinned conversations (a grid on top) → thread.
  - Rows → thread (push).
  - Compose → **New Message** (sheet, large detent).
  - Filter → menu (§2.9.3).
  - Search → results page in place (§2.9.4).
- **Thread** (pushed): avatar and name capsule header → tap: **conversation details** (pushed, §2.11).
- Bubble long-press → the **reaction bar** and menu (§2.10.6).

### Contacts (app)
- Root: **Contacts** list, large title, "+" circle top right, **Lists** back capsule top left (E: iOS 26 Contacts
  opens inside Lists). With a single local account we **omit** Lists, and the title is the root.
  - My Card (top) → my card.
  - Contact row → **contact card** (push) → **Edit** (in place) / quick actions.
  - "+" → **New Contact** (sheet, large detent).
  - Search field at the bottom (A).

---

## 2. Screens

Colours: IR §15 names. Glass: LG. Type: IR §14 style names (size / weight). The values below are S9 pt (390 x 802)
unless they say "@402".

### 2.1 Phone: Calls tab (Unified)

**Layout (top to bottom)**

| Element | iOS @402 (M SUP unless noted) | **S9** |
|---|---|---|
| Nav row | Edit glass capsule **62 x 44** at x 16.5, centre y 83; Filter glass circle **44** at x 342..386 (16 from the right); title "Calls" Headline 17 / 600 centred on the row | Edit capsule 62 x 44 at x 16, centre **NAV_MID 66**; Filter circle 44 at x 330..374; title centred at 66. **No large title** (A: SUP art). **(IR fix:** today's code draws a large "Calls" title.) |
| Favourites grid | **4 columns**, tile **81 x 122.8**, side margin 15.8, gap 15.8 (pitch 96.6), first tile top 122 (17 below the nav row); row pitch **158.5** | tile **77.6 x 117**, margin 16, gap 15.8 (pitch 93.4); column x 16 / 109.4 / 202.8 / 296.2. Row 1 top **105**, row 2 top **258.5** |
| Tile | rounded rect, radius ≈ **20** (M TB ±3; SUP suggests 14-18), the contact poster fills it. Without a poster photo: a **pale tinted tile with the contact's circular photo or initials disc Ø 49 centred** (M TB "EE", "Chris") | local poster (§2.12): tinted tile + initials disc Ø 49 at the centre |
| Tile label | phone glyph (12 pt, secondaryLabel) + first name, Subheadline **15 / 400 secondaryLabel**, centred, centre 12-13 below the tile | same; one line, middle-truncated |
| Max tiles | **up to 8**. With more favourites the 8th spot is **"Show All"**: a 2 x 2 cluster of small tiles / discs (≈ 37 each) in the tile's place, labelled "Show All" without a glyph (A: Support 126076; M TB) | same |
| "Recents" header | Title 2 **22 / 700**, x 18.5 (≈ 20 with the glyph bearing), centre 38 below the last tile row's bottom | 22 / 700 at x 20, centre y **413** |
| Recent row | **74.1** pitch; avatar **50** circle at x 16; text x **76**; separator 0.5 pt from x 76 to 386 (16 from the right) | row height **74**; avatar 50 at x 16; text x 76; separator x 76..374. First row top **433** |
| Row line 1 | name Headline 17 / 600, label colour; **missed = systemRed** (M: #FF4245 sampled) | same |
| Row line 2 | direction glyph (↗ outgoing, ↙ incoming, 12 pt) + number label ("mobile", "iPhone", "home", or a place for unknown numbers) + count "(2)", Subheadline 15 / 400 secondaryLabel (M SUP/TB) | same; the number label comes from the contact's field label, else **"unknown"** (M TB) |
| Time | right-aligned 15 / 400 secondaryLabel, centred on line 1-2, ending 14 before the call button | ends at x 316 |
| Call button | **44 glass-fill circle**, light fill #F3F3F5 (M), systemBlue phone glyph (17 pt, filled) at x 342..386 | x 330..374, centre on the row |
| Bottom | tab bar (IR §10), Calls selected (systemBlue icon + label, lighter pill) and the Search circle | IR §10 values |

- **Date formats** (M SUP/TB): today → time ("9:41 AM"); yesterday → "Yesterday"; the last 6 days → weekday
  ("Wednesday"); older → numeric short date ("1/20/26"). Our locale's 12/24 h rule applies (`sh_timefmt.h`).
- One row per **run** of consecutive calls with the same number, direction and missed state; the run size shows as
  "(n)" (M TB "T-Mobile (2)").
- Rows that fit on the S9: 4-5 below the favourites (3P TB says ~4 on an iPhone 17). Scrolling moves the whole page.
  The favourites scroll away with it (E).

**Content and states**
- **No favourites:** the grid is absent and "Recents" starts at y 105. Edit offers **Add Favorite** (A).
- **No calls:** below the header, centred: title "No Recent Calls" (Title 3, 20 / 600), body "Calls you make and
  receive appear here." (15 / 400 secondaryLabel) (E words).
- **No favourites and no calls:** the empty state alone, centred in the content area.
- **Cellular off:** the empty-state body becomes "Turn on Cellular to make and receive calls." plus a "Cellular
  Settings" glass button (keep today's behaviour; local).
- **Filtered** (Missed / Unknown Callers): the favourites grid hides. The header reads "Missed" / "Unknown
  Callers". The Filter circle's glyph turns **systemBlue** and its fill tints (A: Messages "the icon turns blue"; E
  for Phone).
- **Editing (Select):** Edit becomes **Done** (prominent blue glass capsule). Rows get a 22 selection circle at x 16
  (avatars shift right 38). The bottom bar shows "Delete" (red, disabled until one is selected) and "Clear All" (E).

**Interactions**
- Tap a row → the caller's **Info** page (§2.5.6): a contact's card, or the number's page with **Create New
  Contact / Add to Existing Contact** (A). Tap the call button → dial at once.
- **Swipe left** on a row (A/3P; M TB "Reminders" screenshot): the row turns into a **light grey rounded card**
  (systemGray6, radius ≈ 24, full row height) that follows the finger left. Two **round action buttons Ø 50** are
  revealed in the freed space, not full-height blocks: **Remind Me** (systemBlue, white clock glyph 24) and
  **Delete** (systemRed, white trash glyph 24). Delete is outermost, its right edge **11** from the screen edge, gap
  **10** between the circles.
  - The S9 centres are x **294 / 354**, on the row's centre line.
  - A full swipe (past 60 % of the width) leaves only the red circle and deletes (A: "Swipe a call all the way to
    the left").
  - Remind Me opens a glass menu over the row (M TB). Header "**Call Back Reminder**" (15 / 400 secondaryLabel),
    then **Remind Me in 1 Hour**, **Remind Me Tonight**, **Remind Me Tomorrow**, **Remind Me Later…** (17 / 400, row
    pitch 42). Later… opens a date-time picker sheet.
  - The result is a local reminder that posts a notification. Reminder rows show at the top of Calls (3P TB): a 44
    row with a clock glyph, "Call back <name>" and the time (E).
- **Long-press a row** (M TB) → the row lifts as a **white capsule card** (≈ 358 x 67, radius ≈ 33, shadow), the rest
  dims grey (≈ black 20 %). A glass menu sits above it, aligned right: **Message ›**, **Call ›** (submenus listing the
  contact's numbers), (iOS: Mail › → **omit**), separator, **Delete** (red, trash glyph).
  - Unknown number: **Message**, **Call**, **Create New Contact**, **Add to Existing Contact**, separator, **Delete**
    (A: the add items exist; E: order).
  - Menu rows pitch 42, 17 / 400 label, 22 pt glyph at x +24, chevron at the right inset 24.
- Favourite tile: tap = call (A: "To contact someone in your Favorites, tap their name"). Long-press → menu: the
  contact's numbers and methods ("Call mobile", "Message mobile"), "Edit Favorites", "Remove from Favorites" (red)
  (E). In Edit Favorites, tiles jiggle (IR §9.2) with a **"−" badge** top-left; drag to reorder; "+" top right adds
  (A: UG27 "the Remove button in the upper-left corner", Add button).
- **Setting (A):** "Tap Recents to Call" (Settings > Apps > Phone): when on, a row tap dials. Default off.
- Pull-to-refresh: none (A: none documented).

**Transitions:** row → Info: push (IR §16). Filter → menu morph (LG §5.3). Edit → menu morph.

**Haptics:** swipe threshold → selection; long-press lift → medium impact (IR §17).

### 2.2 (reserved: Favourites list, "Show All")
A plain pushed list: inline title "Favorites", rows 60 (as Contacts §2.13) with the method ("mobile") as subtitle,
and Edit in the nav row. Reorder handles appear in Edit (E).

### 2.3 Phone: Filter menu

Glass menu morphing out of the Filter circle (LG §5.3). Measured (M SUP filter crop, 402):

| Item | Value |
|---|---|
| Panel | **249 wide**, top aligned with the button top (y 61 @402 → S9 **y 44**), right edge 12 from the screen edge, radius ≈ **24** (M ±3), regular glass, nearly opaque (IR §3.3) |
| Layout picker | two phone glyphs **41 x 81** (our art: a phone outline with list lines; Unified shows a 2 x 4 grid on top), labels "Classic" / "Unified" 15 / 400 below, a **22 radio circle** under each (selected = filled systemBlue with a white check). Glyph-row centre 78 below the panel top |
| Separator | a 1 pt line, inset 20, below the picker (~120 below the panel top) |
| Rows | pitch **42** (M): a check column at x +19 (Body "✓" for the active filter), 20 pt glyph at x +47 (centre), label Body 17 / 400 at x +71 |
| Rows, in order (A: Support 126076) | **Calls** (phone glyph) · **Missed** (missed-call glyph: a rounded square with a dot) · ~~Voicemail~~ (omit) · **Unknown Callers** (person with "?") · ~~Spam~~ (omit) |
| Footer | separator, then **Manage Filtering** (no glyph, x +58), which opens Settings > Apps > Phone > Unknown Callers (local) |

- The current filter carries the check. Choosing one closes the menu (collapse 0.35, bounce 0.1) and swaps the list
  with a 0.2 s cross-fade (E).
- Unknown Callers (local): calls from numbers not in Contacts. In that view each row gets a "Mark as Known" capsule
  instead of the time (A wording).

### 2.4 Phone: Keypad tab

Measured on TB (real iPhone 17 screenshots, 402, ±1 pt). The S9 keeps the circle sizes and the bottom offsets and
compresses only the space above the keys.

| Element | iOS @402 (M TB) | **S9** |
|---|---|---|
| Add-to-contacts button | person-plus glyph 24 pt, label colour, centre (369, 86); shown only when a number is typed | centre (**358, NAV_MID 66**), 44 hit box |
| Number | **~38 / 400** (digit cap ≈ 27; SF Display), label colour, centred, centre y 116; formatted as you type (US: "867-5") | 38 / 400, centre **y 120**. Shrinks to 30 when it doesn't fit 330 wide, then truncates at the head (E). Format: group by the SIM country's rule, else raw (E) |
| T9 suggestion | glass capsule **281 x 43**, white with a soft shadow, centred, centre y 168. Inside: person glyph 20 + "Jenny T… (555) 867-5309" Body 17 / 400; the **typed digits in label colour, the rest secondaryLabel**; the name truncates first | 281 x 43 capsule, centre **y 168**. Hidden when there's no match. Two or more matches: the best one + " and n more" (E) |
| Keys | **3 x 4 glass circles, Ø 78** (M 77-79); fill ≈ #F9F9F9 on white, soft shadow (light); column pitch **101.8**, row pitch **96**; row-1 centre y 305, row-4 centre y 594 | Ø **78**; columns **x 96 / 195 / 294** (pitch 99 = 101.8 x 0.97); rows centres **y 234 / 330 / 426 / 522** |
| Digit | ~**37 / 400** (cap 26; regular, not medium), centre 6 above the key centre | 37 / 400, centred at key centre − 6. **(IR fix:** today 32 / 500.) |
| Letters | ~**10 / 700**, tracking ≈ +2, label colour, centre 17.6 below the key centre: "ABC DEF GHI JKL MNO PQRS TUV WXYZ"; "0" shows "+"; "1", "*", "#" have none | same |
| "*" "#" | glyph larger (≈ 44 pt) and centred | same |
| Call button | **Ø 78 systemGreen** (M #38D15F ≈ dark-mode green; use #34C759 light / #30D158 dark), white phone glyph 34 pt, centre column, centre **183 above the bottom** | centre **(195, 619)** |
| Delete | "delete.left" glyph **33 x 24**, light grey key shape with a dark ×, centre at the right column, call row; **only when a number is typed** | centre **(294, 619)** |
| Tab bar | IR §10, Keypad selected | |

Dark mode (E): keys glass over black ≈ #2C2C2E with a 1 px light rim; digits white; letters white 80 %.

**Interactions**
- **Tap a key:** append the character; play its **DTMF tone** (~100 ms, at the dial-pad sound setting) (A: tones;
  E: length). Press animation: glass press (LG §5.1), the key brightens. Haptic: **none** by default (E: iOS keypad
  has no haptic unless keyboard haptics are on); we use **light impact** only when Keyboard Feedback > Haptic is on.
- **Hold "0"** → "+"; **hold "*"** → "," (soft pause); **hold "#"** → ";" (hard pause) (A: UG27).
- **Hold "1"** → call voicemail if a number is set (E/local).
- **Delete:** tap removes one character; hold repeats after 0.5 s at 10/s, then whole words after 2 s (E).
  Long-press the number → **"Paste"** edit menu (A: "Tap the phone number field… then tap Paste"; TB: hold the blank
  area above the keys).
- **Call with an empty number:** fill in the **last dialled number** (no call). The second tap calls (A).
- **Call:** dial at once → §2.5. **Decided (box at the top): tap to dial**, as iOS does (A). The trunk's 0.6 s hold goes.
- Tap the T9 capsule → the number field takes that contact's number (E). Then the call button calls it.
- Add-to-contacts → action sheet anchored to it (IR §3.2): **Create New Contact**, **Add to Existing Contact** (A
  wording from "Use contacts from the Phone app").
- Emergency numbers: keep today's red line and never send down (local).

### 2.5 Phone: Outgoing / in-call screen (iOS 27)

Measured on UGi (402 @1.1095). Full screen over everything, the lock screen too.

| Element | iOS @402 (M UGi) | **S9** |
|---|---|---|
| Background | full-bleed gradient (the contact poster's colours; for a bare number a deep **teal → blue** gradient: top #3C6887, bottom #005987 sampled). Status bar white | local: a vertical gradient from the contact's monogram colour at 45 % brightness (top) to 30 % (bottom). Bare number: **#3C6887 → #005987**. Light/dark independent |
| Status line | **above** the name: "Calling…" / "Ringing…" / the timer "00:46", ~**22 / 500**, white 70 % (M #A7B7CB), centred, centre y 127 (digit height 15.3) | centre **y 117** |
| Name / number | **34 / 700** (digit height 24.4), white, centred, centre y 163; one line, shrinks to fit then truncates (the Call Context art clips it with a fade) | 34 / 700, centre **y 153**, max width 350 |
| With a photo (alt. layout) | when there is an image: a **58 x 58** rounded image at x 42, the timer 22 / 500 and the name 34 / 700 left-aligned at x 113 (M UGi Call Context) | our poster has no photo: always the centred layout |
| Controls | **3 x 2 glass circles Ø 78** (M 77-78.5); column centres **80.2 / 201 / 321.8** (pitch 120.8); row centres **244 and 117.7 above the bottom** | Ø 78; columns **x 77.8 / 195 / 312.2**; rows **y 558 / 684** |
| Labels | **14 / 400-500**, white, centre 53 below the circle centre (M by text width: "Speaker" 51.4 pt, "End" 22.5 pt) | 14 / 500 white, centre y 611 / 737 |
| Row 1 | **Speaker** (when Bluetooth audio exists the label becomes **Audio** with a route glyph, tap → route menu), **FaceTime** (dimmed when unavailable), **Mute** | **Speaker / Audio**, **Hold** (local equivalent in FaceTime's slot, see below), **Mute** |
| Row 2 | **More** (•••), **End** (red), **Keypad** (3 x 4 dots) | same |
| End | Ø 78 red, measured **#CF1E20** (darker than systemRed), white handset-down glyph 30 pt | **#CF1E20** both modes |
| Glass circles | clear glass over the gradient, white glyph 26 pt; **on** state (Speaker, Mute, Hold) = **white fill, dark glyph** (E: unchanged since iOS 7) | same |
| Banner slot | a glass card **332 x 77**, r ≈ 18, inset 30, sits 32 above row 1 (Hold Assist / Call Context) | used only for **our** notices: "No audio yet" and the kernel's audio reasons (today's text), as this card: title 17 / 600 white + subtitle 15 / 400 white 70 %. Card x 29..361, bottom **y 487** |

- **Hold slot (local; decided):** FaceTime is omitted. We put **Hold** there (modem `AT+CHLD=2`; lit when held, the
  status line says "On Hold"). If the modem refuses hold, the circle shows dimmed, as iOS dims an unavailable
  FaceTime (M UGi shows FaceTime dimmed with a "?" video glyph). Mute long-press also toggles Hold (A).
- **More** → glass menu (morph from the circle): **Add Call** (only if the modem supports a second call), **Send
  Message** (opens the Messages thread with the call screen minimised), **Contact** (the card). iOS items Hold
  Assist, Share Screen, Record → **omit**.
- **Keypad** → the 3 x 4 keys (as §2.4, white glyphs on glass, Ø 78, pitch 99 x 96) replace the name area. The typed
  tones replace the name in the same 34 / 700 line. The bottom row becomes [blank · End · **Hide**] with Hide as a
  17 / 400 text button (E). Rows on S9: centres y 245 / 341 / 437 / 533 (the last 25 above row 2's 558-row slot:
  that slot is hidden while the keypad shows).
- **States and words:** "Calling…" (dialling) → "Ringing…" (alerting) (E: iOS 26 shows "calling…" then the timer;
  our modem reports alerting) → timer "0:05" (m:ss; h:mm:ss after an hour) → "Call Ended". "On Hold" while held.
  "Ending…" is ours (local).
- **Call ended:** the status reads "Call Ended" in white 70 %, all controls dim to 40 % and stop taking touches; the
  screen closes after **1.5 s** (E), with a 0.25 s fade, back to where you were.
- **Leaving the call screen:** a home swipe or the app switcher minimises it into the **green status capsule** (§2.8).
  Tapping the capsule restores it (A: "tap the green call indicator").
- **Proximity:** the panel goes dark at the ear unless Speaker is on (today's Track B behaviour; keep).

### 2.5.6 Caller Info page (from a Recents row)
Pushed. It is the contact card (§2.14) when the number is a contact. For a bare number: the local poster header with
the formatted number as the name, quick actions **Message · Call** (§2.14), then a card listing this number's
recent calls (date header 15 / 600, rows "Outgoing Call 9:41 AM 2 minutes" 15 / 400) (E). Then the action rows
**Create New Contact**, **Add to Existing Contact**, **Block this Caller** (red) (A wording: "Create New Contact",
"Add to Existing Contact", "Block Contact").

### 2.6 Phone: Incoming call, full screen

Shown when the phone is locked, or when the user taps the banner (§2.7). Same background, name and status
positions as §2.5. The status line reads **"mobile"** (the number's label) or **"Phone call"**, not a timer (E).

| State | Controls (S9) |
|---|---|
| **Unlocked** (after tapping the banner) | Row 1 (y 558): **Remind Me** (alarm-clock glyph) at x 77.8 and **Message** (bubble glyph) at x 312.2, glass Ø 78 with 14 / 500 labels. Row 2 (y 684): **Decline** Ø 78 red #CF1E20 at x 77.8 and **Accept** Ø 78 systemGreen at x 312.2, labelled. The middle column is empty (E: iOS ≤ 18 layout, kept in 26/27 renders) |
| **Locked** | Row 1 as above (Remind Me, Message). Instead of row 2, a **"slide to answer"** glass track **310 x 72**, r 36, centred, centre **y 684**. Thumb Ø 62 white, systemGreen handset glyph. Label "slide to answer" 20 / 400 white 70 %, with a light shimmer sweeping left → right every 2.4 s (E) |

- **Answer:** tap Accept, or drag the thumb past **75 %** of the track (release earlier springs back 0.35 / 0.8) (A:
  "drag the slider"). Haptic: **medium impact** on answer (E).
- **Decline:** tap Decline, or press the power key **twice** (A). One press of power or a volume key **silences the
  ringer** (A); the screen stays.
- **Remind Me** → menu: "In 1 hour", "When I leave" (omit: no geofencing), "Tonight", "Tomorrow" (E). It declines
  and schedules a local notification.
- **Message** → menu of replies: **"Sorry, I can't talk right now."**, **"I'm on my way."**, **"Can I call you
  later?"**, **"Custom…"** (A that defaults exist and are editable in Settings > Apps > Phone > Respond with Text; E
  for the exact words, the iOS defaults since iOS 6). It declines and sends the SMS.
- Ringtone plays and the vibrator pattern runs until answered or declined (today's `snd_ring`).
- **Missed:** if the caller hangs up, the screen fades out (0.25 s). A "Missed Call" notification posts: title the
  caller, body "Missed Call" (E). The Phone icon badge counts missed calls since Calls was last viewed (today's
  behaviour).

### 2.7 Phone: Incoming call banner (unlocked, in use)

When the phone is unlocked and in use, iOS shows a **compact call banner**, not a full screen (A: "Swipe up on the
call banner", "tap the call banner for more options").

| Element | **S9** (E unless noted) |
|---|---|
| Platter | IR §5 banner geometry: x 8..382 (374 wide), top **y 10**, height **76**, radius **38** (a capsule; concentric clamp), regular glass, dark tint |
| Avatar | monogram Ø 48 at x 22, centred |
| Text | name Headline 17 / 600 white; below it "mobile" (or "Phone call") 15 / 400 white 70 %; x 80, max width 160 |
| Buttons | **Decline** Ø 44 red #CF1E20 at x 270..314 and **Accept** Ø 44 systemGreen at x 324..368, white glyphs 20 pt |
| In | drops out of the status band (IR §5): spring 0.50 / 0.8 |
| Swipe up | **declines** the call (A) and slides the banner out (0.35 / 1.0). (A normal notification banner would only hide.) |
| Tap (not on a button) | expands to the full-screen incoming screen (§2.6): a morph of the platter into the screen, 0.45 / 0.85 |
| Accept on the banner | the banner morphs into the full in-call screen (same spring) |
| Persistence | stays until answered, declined or the caller gives up. No auto-dismiss |

**(Gap:** today every incoming call takes the full screen.)

### 2.8 Status capsule while in a call
IR §11 status capsule: **green #34C759** behind the time, 24 tall, with the timer in 15 / 600 white. It appears on a
morph from a 6 pt dot (0.35 / 0.75). Tap → the call screen (A behaviour on non-island iPhones). It is already in the
trunk (B7). Keep it; check the colour and the timer font.

### 2.9 Messages: conversation list

**Layout** (M UGm list art @402 unless noted)

| Element | iOS @402 | **S9** |
|---|---|---|
| Nav row | **Edit** glass capsule (≈ 62 x 44) left at x 16; **Filter** glass circle 44 right at x 342..386 (line.3.horizontal.decrease) | Edit at x 16, Filter x 330..374, both centred on **NAV_MID 66** |
| Large title | "Messages" Large Title **34 / 700**, x 20, below the nav row | baseline **LARGE_BASE 124** (IR §6). Collapses to an inline 17 / 600 title on scroll (IR §10) |
| Pinned | (M UGm filter / swipe art) up to 9 **circles Ø 90**, 3 per row, column centres 71 / 200 / 330 (pitch 129.4), row pitch **127**. Name **12 / 400 secondaryLabel** centred 57.6 below the circle centre, preceded by a **Ø 9 systemBlue dot** when unread. A recent incoming message or reaction shows as a small white speech bubble overlapping the circle's top-right (e.g. "❤️") | Ø 90, centres x **69 / 195 / 321**, first row centre **y 170** (SAFE_TOP + 44 + 82), row pitch **124** |
| Row | pitch **88.7**, the same for one- and two-line previews (M: "Animation Team" has no preview but the same height) | **88** |
| Unread dot | **Ø 10.9 systemBlue** at x 8..19 (centre x **13.5**), centred on the name line (M) | Ø 11, centre (13.5, row top + 23) |
| Avatar | **Ø 46.8** at x 27, vertically centred on the row (M) | **Ø 47 at x 27** |
| Text column | x **85.7** (avatar + 11.7) | x **86** |
| Name | Headline 17 / 600 label, top line (baseline ≈ row top + 28) | same |
| Time + chevron | time 15 / 400 secondaryLabel right-aligned; chevron 13 / 600 tertiaryLabel at the right inset 16 (time ends 10 before it) | chevron right edge x 374; time ends x 356 |
| Preview | Subheadline **15 / 400 secondaryLabel**, up to **2 lines**, below the name | same; "You: " is **not** iOS (iOS shows the bare last message) → drop it (§6) |
| Separator | 0.5 pt separator colour from the text x to 16 from the right | x 86..374 |
| Bottom toolbar | **Search** glass capsule **≈ 48 tall** from x 27 to 305 (magnifier, placeholder "Search" Body 17 / 400 at the placeholder colour, mic glyph at the right end); **Compose** glass circle **Ø 48** at x 317..366 (square.and.pencil). Bottom ≈ 28-32 above the screen bottom (M ±3) | search x 27..302, compose x 314..362, both **bottom y 774**, centre y 750 |
| Scroll edge | content dissolves under the nav row and the bottom toolbar (LG §5.4) | |

**Content and states**
- **Empty:** centred "No Messages" (Title 3, 20 / 600) + "Your conversations appear here." 15 / 400 secondaryLabel
  (E words). Cellular off: "Turn on Cellular to send and receive text messages." + the "Cellular Settings" button
  (keep).
- **Unread:** the blue dot; the name stays 17 / 600 (iOS doesn't bold unread rows differently) (M).
- **Failed last message:** the preview starts with a red "!" glyph 13 pt + "Not Delivered" (E).
- **Draft:** the preview reads "Draft: …" with "Draft" in systemRed (E: long-standing).
- **Filtered** (Unread, Unknown Senders…): the large title changes to the filter name ("Unread"), and the Filter
  glyph turns **systemBlue** (A: "the icon turns blue").
- **Select mode** (Edit > Select Messages): Edit → **Done** (prominent). Rows shift right 38 with 22 selection
  circles. The bottom toolbar becomes **Read All** (left; "Read" once something is selected) and **Delete** (right,
  red) (A: "tap Read All", "tap Unread in the bottom-left corner", "Delete").

#### 2.9.1 Interactions
- Tap a row → thread (push).
- **Swipe style (M UGm swipe art):** the row becomes a **grey rounded card** (systemGray5 light #E5E5EA, radius
  ≈ 23, full row height 88) that slides with the finger. The revealed actions are **Ø 50 coloured circles** with
  white glyphs, 10 from the screen edge, not full-height blocks. Same component as Phone §2.1.
- **Swipe right** (leading): one **systemBlue Ø 50** circle with the **unread glyph** (a speech bubble with a dot;
  read glyph when unread), centre x **35**. A full swipe toggles (A: "Swipe right on a conversation to mark it as
  unread").
- **Swipe left** (trailing), outermost first: **Delete** (systemRed, trash) at centre x **355**, then **Hide Alerts
  / Show Alerts** (systemIndigo #6155F5, bell.slash) at x 295 (A: Delete; E: Alerts and its colour).
  - A full swipe deletes: the row slides out and the conversation goes to **Recently Deleted** for 30 days (A).
  - Local: we keep a Recently Deleted list (the filter item) instead of erasing at once.
- **Long-press a row** → the thread preview lifts (a 300 x 360 glass-edged card showing the last bubbles) + menu
  below: **Pin**, **Mark as Unread / Read**, **Hide Alerts**, **Delete** (red) (A: Pin, Delete; E: order and the
  rest). Haptic: medium impact.
- **Edit** → menu: **Select Messages**, **Edit Pins** (A: Select Messages; E: Edit Pins). Omit "Set Up Name & Photo"
  (iMessage only).
- Pinned item long-press → menu "Unpin", "Hide Alerts", "Delete"; or drag it down into the list to unpin (A).
- Compose → New Message sheet (§2.11).

#### 2.9.3 Filter menu
Glass menu from the Filter circle, same component as §2.3 (no layout picker).
- Measured (M UGm filter art): panel **254 wide**, right edge 10 from the screen edge, top at the button's top, radius
  ≈ 27, rows pitch **43**, check at x +27, glyph at x +62, label 17 / 400 at x +88. The default iOS 27 rows are
  **Messages** (two-bubbles glyph, checked) · **Spam** (bin-with-x) · **Recently Deleted** (trash), then a separator
  and **Manage Filtering** (no glyph).
- **Ours:** **Messages** · **Unread** (A: "tap the Filter button, then tap Unread") · **Unknown Senders** (only while
  Screen Unknown Senders is on; A) · **Recently Deleted**, then separator, **Manage Filtering** (→ Settings > Apps >
  Messages > Screen Unknown Senders). Spam is **omitted** (no spam data), and so are Transactions and Promotions.
- With Screen Unknown Senders on, messages from numbers not in Contacts go to Unknown Senders and their notifications
  are muted (A behaviour, local equivalent).

**Recently Deleted** page: title "Recently Deleted", rows as the list with "n days" left, select mode with
**Recover** and **Delete** (red) at the bottom. Delete asks "Permanently delete n conversations? This can't be
undone." with **Delete** (red) / Cancel (A: Recover / Permanently Delete exist; E: words).

#### 2.9.4 Search
- Tap the bottom search capsule → it rises above the keyboard (IR §10). The compose circle morphs into a **44 "x"
  glass circle** (cancel). The large title collapses (E).
- Empty query: suggestions: "Conversations" (recent avatars in a row) (E).
- Typing: results in sections **Conversations** (matching name or number, A for iOS 27: search by phone number) and
  **Messages** (rows: avatar 40, name 15 / 600, the matching text with the match in label colour + bold, the date
  right) (E). Tap → the thread scrolled to that message, which flashes (a 0.6 s highlight pulse) (E).
- No results: "No Results" (Title 3) centred (E).

### 2.10 Messages: thread

**Header** (M UGm thread art @402)

| Element | iOS @402 | **S9** |
|---|---|---|
| Back | glass circle 44, chevron, centre (37.9, 81) | x 16..60, centre y **66** (BACK_X → 16, see §6) |
| Avatar | **Ø 56-58**, centred, centre y 88 (it sits in the nav row, overlapping the scroll edge) | **Ø 56**, centre **(195, 78)** |
| Name capsule | glass capsule **≈ 120 x 30** (hugs its text: name **17 / 600** + a "›" chevron 13 / 600 secondaryLabel), centre y 131.6; it touches the avatar's bottom | height 30, padding 12, centre **y 121** |
| Right side | none for an SMS conversation (M: the green art has no button; iMessage shows a FaceTime video circle) | none |
| Scroll edge | bubbles blur and fade under the header (LG §5.4) | band to y 140 |

**Bubbles** (M UGm green SMS art, ±1; replaces IR §7 values)

| Item | Value |
|---|---|
| Text | Body **17 / 400**; white on sent green, label colour on received grey |
| Sent colour (SMS) | systemGreen: light #34C759 / dark #30D158; the art renders it brighter (#8FEF8A highlights / #34D75B fill) — use the system colour |
| Received | light **#E9EAEA** (M) / dark #262628 |
| Height | **1 line 40.5**, **2 lines 61.2**, then +20.7 per line → build **h = 19.5 + 21 · lines** (padding 9.75 top/bottom, line pitch 21) **(IR fix:** 36 / 22) |
| Text inset | **14.4** from the bubble's left/right edge **(IR fix:** 12) |
| Radius | **20** (a one-line bubble is a full capsule) **(IR fix:** 18) |
| Max width | **68 %** of the screen: 275 @402 → **266** on S9 (text width 237) **(IR fix:** 292) |
| Side margin | bubble edge **16** from the screen edge (M) |
| Tail | only on the **last bubble of a run** (M). iOS 26/27 shape: a small **hook below the bottom-outer corner**, pointing down and out. Its base spans x edge+8 .. edge+21 along the bottom, its tip sits **7.9 in from the outer edge and 6.7 below the bottom** (M zoom) **(IR fix:** today's tail is the older side curl) |
| Same-sender gap | **4.5** (M) **(IR fix:** 2) |
| Sender change gap | **9.9** (M) |
| Time header | "Yesterday 6:00 PM": **11 pt**, the day **600** + the time **400**, secondaryLabel, centred; **12 above / 12 below** (35 pt band) (M). Shown at the first message and before any message ≥ 60 min after the previous one (E: keep today's rule) |
| Intro line | at the top of a conversation: "Text Message • SMS" 11 / 500 secondaryLabel, then the time header of the first message (E: iOS shows "Text Message • SMS" + date) |
| Status | under the **last sent** bubble only: **11 / 600 secondaryLabel**, right-aligned to the bubble's right edge, centre **12.8 below** the bubble (M "Read"). Words: "Sending…" → "Sent" (SMS default) → "**Delivered**" when the network reports delivery (local; iOS shows Delivered/Read only for iMessage/RCS) |
| Failed | the bubble at 55 % opacity, a **red "!" circle Ø 22** to its right (edge 8 from the bubble, the bubble moved left 30), status "**Not Delivered**" in systemRed. iOS 27 **retries automatically** first (3P) → we retry 3 times (after 5 s, 30 s, 2 min) while "Sending…" shows; only then Not Delivered (E policy) |
| Tapback badge | a **Ø 32 circle** on the bubble's **top outer corner**: centre 2 in from the bubble's corner edge, 11.5 above its top; emoji 20 pt; fill = the reactor's colour (**mine: systemGreen** M #34D75B; theirs: systemGray5 with a 1.5 pt rim of the background colour); two "thought" dots (Ø 5 and Ø 3) trailing down toward the bubble (M UGm) |
| Next bubble after a tapped-back one | +22 extra top space so the badge doesn't collide (M: 38 vs 10) |

**Input bar** (M UGm thread / green art @402, ±1.5)

| Element | Value @402 (M) | **S9** |
|---|---|---|
| "+" button | glass circle **Ø 40-42**, "+" glyph 20 / 500 label colour, left edge 27 | Ø **40** at x 27..67 |
| Field | glass capsule **40 tall** (grows by 21 per line to 5 lines, then scrolls), x 80.5 → 373 (right inset 29) | x **79 → 363**, 40 tall |
| Placeholder | "**Text Message • SMS**" Body 17 / 400 tertiaryLabel, x +16 inside | same **(IR fix:** "Text Message") |
| Trailing (empty field) | the **audio button**: a waveform glyph 18 pt secondaryLabel, 16 from the field's right end. iOS 27 setting **Show in Text Field: Record Audio / Start Dictation / None** (3P) | local: **Start Dictation** default → our dictation if present, else **None** (no glyph). Audio messages: omit (SMS) |
| Send (text present) | the trailing glyph is replaced by a **Ø 30 green circle** (SMS green) with a white up-arrow 15 / 700, 5 from the field's right end (E size; A: colour follows the service) | Ø 30, centre (field right − 20, field bottom − 20) |
| Position | bottom ≈ 28-33 above the screen bottom with no keyboard; **8 above the keyboard** when it shows (E) | bottom **y 774**; with the keyboard: keyboard top − 8 |
| Material | the bar floats over the bubbles; scroll-edge blur behind it (LG §5.4) | |

**"+" menu** (M UGm plusmenu @402): a tall glass panel **≈ 326 wide**, x 11 from the left, radius ≈ 27, rising from
the + button. Rows pitch **≈ 68** with a **Ø 38 coloured icon disc** and a **≈ 22 / 400** label. iOS 27 items include
Camera, Photos, Stickers, Audio, Store, #images, Digital Touch, Drawing, Location, Memoji, Poll, Send Later…
(A/M). **Ours v1 (local):** **Send Later** (orange disc, clock glyph; a time picker sheet; the SMS goes at that time
while the phone is on) and **Contact** (grey disc; sends a chosen contact's name and number as plain text). Camera
and Photos appear only once MMS exists. Tap outside or swipe down closes it (collapse 0.35 / 0.9).

#### 2.10.1 Thread interactions
- **Swipe left anywhere on the thread** → every bubble slides left by up to **64** (rubber-banded past that, E) and
  each message's **time** (13 / 400 secondaryLabel, "9:41 AM") appears at the right edge, centred on its bubble.
  Sent bubbles move too. Release → spring back 0.35 / 0.9 (A: "Swipe left on the message bubble to see timestamps";
  E numbers).
- **Double-tap or long-press a bubble** → **reaction mode** (§2.10.6). Haptic: medium impact.
- **Tap a link / number** in a bubble → open it / offer Call, Message, Add to Contacts (E: data detectors). Numbers
  are underlined like links.
- **Tap the status "Not Delivered" or the red "!"** → action sheet: **Try Again**, **Delete Message** (red) (A: "tap
  the Send Failure button to try sending the message again").
- **Pull down at the top** → loads older messages (spinner 20 pt centred) (E).
- **Keyboard:**
  - Tap the field → the keyboard rises (IR §4) and the thread scrolls so the newest bubble sits 8 above the bar.
  - **Interactive dismiss:** dragging the thread downward with the finger crossing the input bar drags the keyboard
    with it 1:1. On release, below half its height or flicked down (> 300 pt/s) → it dismisses (spring 0.38 / 1.0),
    else it returns (E: UIKit `.interactive`).
  - Tapping a bubble area does **not** dismiss (E).
- **Typing indicator: omit** (SMS has none).

#### 2.10.2 Send
1. Tap Send (or Return when "Send with Return" is set: omit, iOS has no such setting).
2. The text leaves the field at once. A bubble appears at the bottom, **growing out of the field's text rect**: it
   starts at the field's text position and size (scale ≈ 0.85, the field's corner radius), then moves and resizes
   to its final place with a spring of **response 0.35, damping 0.75** (IR §7). The bubble colour fades from the
   field colour to green over the first 0.15 s (E).
3. Earlier bubbles slide up by the new bubble's height + gap on the same spring.
4. The **Sent sound** (a short upward "whoosh", our own sound) plays at the start of the move (A: iOS has a
   "Sent Message" sound in Messages; E: our sound design). **No haptic** (IR §7) **(gap:** today calls `sh_haptic`
   on send).
5. "Sending…" appears under it, then "Sent" / "Delivered".

**Receive (thread open):** the new bubble pops in at the bottom: scale 0.6 → 1 from its tail corner, 0.4 / 0.75 (E).
The **Received sound** (a soft two-note ding, ours) plays, and no notification posts (A behaviour: in-conversation
sound, no banner). The thread auto-scrolls only if you were already at the bottom. Otherwise a **"↓ New Message"**
glass capsule (E) appears above the input bar; tap → scroll to the end.

#### 2.10.6 Reaction mode (long-press / double-tap)
- The pressed bubble lifts (scale 1.03, shadow σ 16 black 20 %) and stays in place. The rest of the screen blurs
  (σ ≈ 20) and dims 25 % (IR §3.3, E).
- **Reaction bar** (M UGm tapback art @402): a glass capsule **63 tall**, starting **10** from the left screen edge
  and running past the right edge. It **scrolls horizontally**: swipe left for suggested emoji (A). Its bottom sits
  **6 above the bubble's top** (below the bubble when there isn't room, E).
  - Items: **❤️ 👍 👎 "HA HA" ‼️ ❓**, then recent / suggested emoji, at a pitch of **50**, each ≈ 27 pt, the first
    centred 41 from the bar's left end.
  - The current reaction sits in a **systemBlue Ø 45 disc**.
  - Our art draws the six classics as our own glyphs (heart, thumb up/down, "HA HA" lettering, double "!", "?"),
    not Apple's.
  - Separately, a **Ø 36 glass emoji button** (a smiley) sits on the bubble's top-outer corner with two thought
    dots. It opens the emoji picker for any emoji (A: "tap the Emoji button").
  - Selection haptic when the finger slides across items.
- **Double-tap** shows the reaction bar only (M: the art shows no menu). **Long-press** adds the menu below (E).
- **Menu** below the bubble (IR §3.3 menu, 250 wide), items in order (E; A for Reply/Copy/Select/More):
  1. **Reply** → **omit** (SMS has no threads).
  2. **Copy** (doc.on.doc)
  3. **Select** (text selection inside the bubble) (A: "tap Select, select the text you want to copy").
  4. **Translate** → **omit**.
  5. **More…** → select mode: check circles at x 16 beside every bubble (the bubbles shift right 34). The bottom bar
     shows **trash** (left, red) and **Forward** (right, arrowshape.turn.up.right) (A: "tap More… select… tap
     Delete").
  - Received bubbles with a number or link add **Call**, **Send Message**, **Add to Contacts** at the top (E).
- Tap a reaction → the badge appears on the bubble with a pop (scale 0 → 1.15 → 1, 0.35 / 0.6) and the mode closes
  (0.3 s). Tapping your own reaction again removes it (A). One reaction per message from me (A).
- **SMS send of a reaction (local, as iOS does for SMS):** we also send a text such as **Loved "Thanks for the
  puzzles!"**, **Liked "…"**, **Disliked "…"**, **Laughed at "…"**, **Emphasized "…"**, **Questioned "…"**, or
  **Reacted 😃 to "…"**. The quote is truncated to 60 characters. Incoming texts in exactly these forms are folded
  into a badge on the matching bubble instead of a new bubble (E; iOS does this for Android SMS). Setting: Settings
  > Apps > Messages > **Send Reactions as Text** (default on, local).

#### 2.10.7 Chat backgrounds — optional
iOS 26 backgrounds are iMessage-only (A). **Local, optional, v2:** conversation details → **Backgrounds**: None,
Color (a gradient from two swatches), Photo. Stored only on this phone. Bubbles stay the same. The time header and
status get a glass pill behind them for legibility. Not in v1.

### 2.11 Messages: New Message (compose)
- **Sheet, large detent** (IR §3.4): close **"x" glass circle 44** at the top left, title "New Message" 17 / 600
  centred on the sheet's nav row (E: iOS 26 sheets use the circle instead of "Cancel").
- **To: row** below the nav row: "To:" 17 / 400 secondaryLabel at x 20; the entry field after it; a systemBlue
  **"+" (plus.circle)** at the right (x 350, 44 hit) → the contact picker (A: "tap the Add Contact button in the To
  field"). Separator 0.5 below. Height 44.
- Typing names or digits → a **suggestion list** replaces the empty area. Rows 60 (avatar 40, name 17 / 400, then
  "mobile  +44 7700 900123" 15 / 400 secondaryLabel), matched characters in label colour. Tap → the recipient becomes
  a **token** (a systemBlue-tinted capsule 28 tall, name 17 / 400 blue). Backspace on an empty field selects the
  last token (blue fill, white text), a second backspace deletes it (E: long-standing iOS behaviour).
- A typed number that isn't a contact becomes a token on Return or on focusing the message field (E).
- **One recipient only in v1** (no group MMS). A second token shows the alert "Group messages aren't supported" (OK)
  (local).
- The input bar as in the thread. Send creates or opens the thread, and the sheet turns into the pushed thread
  (cross-fade 0.25 s, E).
- Keyboard: the To: field opens on the alphabetic layer (iOS) **(gap:** today 123 first).

### 2.12 Local contact poster and monogram (Phone, Messages, Contacts)
- **Monogram:** a circle with a vertical gradient: light #A5ABB8 → #858994, dark #6E7380 → #4E525C (E: iOS grey). The
  initials (first + last, max 2) are in **SF Rounded-like 600 white**, size = 0.42 × diameter. With no name: the
  person glyph (white 60 %), as today's `d_monogram` (M TB: initials discs "TE", "JW" use this look; some are
  tinted per contact).
- **Contact colour (local):** a hue from a hash of the contact id. It picks one of 8 iOS-like tints (blue-grey,
  teal, green, orange, pink, purple, indigo, brown at 35 % saturation). It is used for the favourites tile, the card
  header and the call background. The user can change it in Edit → "Poster colour" (local; replaces Apple's poster
  editor).
- **Poster tile** (favourites): the tint as a pale soft vertical gradient (top +10 % light; M TB tiles are pale
  beige / grey / sage), monogram disc Ø 49 at the tile centre. Name under the tile, not on it (M SUP/TB).

### 2.13 Contacts: list (Contacts app and Phone's Contacts tab)

| Element | iOS @402 (M TB unless noted) | **S9** |
|---|---|---|
| Nav | Phone tab: back circle 44 (to Lists), title "Contacts" 17 / 600 centred, **"+" glass circle 44** at the right | Contacts app (no Lists): **large title "Contacts"** at LARGE_BASE; "+" circle at x 330..374, NAV_MID (E) |
| Search | Phone tab: glass capsule **368 x 42** at x 17, centre y 139 (magnifier, "Search", mic) — **top**. Contacts app: at the **bottom** (A UG27) | Phone tab: top, x 16..374, 42 tall, centre y 110. Contacts app: bottom toolbar capsule as §2.9 (x 27..363, bottom y 774), no compose circle |
| My Card | top row: avatar 60, name 22 / 600, "My Card" 13 / 400 secondaryLabel (E) | height 84, then a 0.5 separator |
| Section header | the letter, **15 / 400 secondaryLabel**, x 19, with a full-width separator below (x 16..386) ~40 tall (M) | x 20, band 40 |
| Row | pitch **60**; avatar **Ø 40** at x 18; name Body **17 / 400** at x 68.5; separator from x 68.5 to 16 from the right (M) | 60; Ø 40 at x 18; name x 68; separator x 68..374. The **sort key in 600** ("Ken **Zeman**") (E: long-standing) |
| Index | **A–Z and #**, Caption 2 **11 / 600 systemBlue**, centre x 8 from the right edge, pitch **13.8**, vertically centred in the list area (M) | centre x 382; 27 entries x 13.8 = 373 tall, centred between y 140 and 760 |
| Tab bar | (Phone tab only) IR §10 | |

**States**
- **Empty:** "No Contacts" (Title 3) + "Contacts you add appear here." + a **"Create New Contact"** glass capsule (E
  words). **(gap:** today "Contacts can't be added on this phone yet.")
- **Search:** typing filters live by name, number (digits ignore spaces/dashes) and company. Results are plain rows
  without section letters; "No Results" when empty (E).

**Interactions**
- Tap row → card (push). Long-press row → context menu: **Call**, **Message**, **Copy Number**, **Share Contact**
  (omit v1), **Make this My Card** (A wording), **Delete Contact** (red) (E order).
- **Index scrubber:** a touch or drag on the index jumps to that section's header (no animation, instant). A
  **selection haptic** fires on each new letter. The index has a 30 pt hit width (E).
- Swipe actions: none in iOS Contacts (E).
- "+" → New Contact sheet (§2.15).

### 2.14 Contacts: contact card (iOS 26/27 poster-style)

Measured on UGk (@402, 1.1095 px/pt). The whole page takes the contact's poster colours; there is no white
background.

| Element | iOS @402 (M UGk) | **S9** |
|---|---|---|
| Background | full-bleed poster colour (art: teal #55848A, a soft diagonal gradient); status bar white | the local contact colour (§2.12) as a 2-stop gradient, top 55 % → bottom 40 % brightness |
| Nav | back glass circle 44 at x 16, centre y 81; **"Edit"** glass capsule ≈ 62 x 44, right inset 18 | back x 16..60, Edit x 312..374, NAV_MID 66 |
| Photo | a **Ø 242 circle** (60 % of the width), centre y 268 (top 147) | Ø **235**, centre **(195, 248)** (top 130) |
| Name | **24 / 700** white, centred, centre y 420 (M: text height 18) | centre y **392** |
| Subtitle | pronouns / company 13 / 400 white 80 %, centre 25 below the name | centre y 417 |
| Quick actions | **4 glass circles Ø ≈ 63, pitch 72**, no labels, white glyphs 22 pt: **message, call, video, mail** (M); unavailable ones dimmed (M: video dimmed) | **2 circles: message, call**, Ø 63, centres x **159 / 231**, y **466** (video and mail omitted; we have no FaceTime and no email yet) |
| Field cards | each field is **its own glass card**: x 16..384 (368 wide), radius ≈ **20**, gap **7.2**; heights 75.7 (photo row), **68.5** (label + value) (M) | x 16..374 (358 wide), r 20, gap 7; height 68 |
| Field text | label **15 / 400 white 70 %** ("mobile") at x +18, top line; value **17 / 400 white** below (M); trailing action glyph 17 pt white at right inset 18 (phone, envelope, map pin) | same |
| First card | "**Contact Photo**" row: avatar Ø 48 + label 17 / 400 + chevron (M) | **"Poster colour"** row (local) with the monogram Ø 48 and a chevron |
| Order of cards | Contact Photo · phone numbers · emails · addresses · birthday · notes · then action rows (E) | Poster colour · numbers · notes · actions |
| Action rows (E) | separate cards, 52 tall, 17 / 400 white: **Send Message**, **Share Contact**, **Add to Favorites**, **Add to Emergency Contacts**, **Share My Location**, then **Block Contact** (in red) (A: Block Contact, Add to Favorites exist) | **Send Message**, **Add to Favorites / Remove from Favorites**, **Block Contact** (red, #FF6961 on the tinted background) |
| Scroll | the photo shrinks and fades as you scroll; at 200 pt the name moves into the nav row as 17 / 600 white (E) | same |

**Interactions**
- Message → the thread (cross-app, push in Messages). Call → dials the default number. With several numbers, a menu
  of the numbers appears first ("mobile +44…", "home …"). **Long-press** a quick action → choose the default number
  (A: "touch and hold the button… then tap a selection").
- Tap a number card → call. Long-press → menu **Copy**, **Send Message**, **Call** (E).
- Block Contact → confirmation sheet: "You will not receive phone calls or text messages from people on the block
  list." with **Block Contact** (red) / Cancel (E words; local: our modem rejects the calls, and SMS are dropped
  to a hidden list).

**Edit mode** (Edit tapped)
- The page turns into the grouped edit form, in place (cross-fade 0.25 s, E). The background stays tinted. The nav
  shows **Cancel** (glass capsule, left) and **Done** (prominent systemBlue glass capsule, right; disabled while
  nothing has changed) (E: IR §10 rules).
- Photo Ø 120 + "Edit" text button below it (local: opens the poster colour picker: 8 swatches + 'Initials' or
  'Glyph').
- Cards (inset grouped, IR §6 geometry): **First name**, **Last name**, **Company** (one card, three rows of 52 with
  separators). Then **"add phone"**: a row with a green ⊕ (Ø 22, systemGreen) + "add phone" 17 / 400 systemBlue.
  Each added phone row: red ⊖ (Ø 22) + label button ("mobile" + chevron, systemBlue, opens the label picker:
  mobile, home, work, school, iPhone, main, home fax, work fax, pager, other, Custom Label) + number field (phone
  keypad layer). Then **"add email"** (omit v1), **"add birthday"**, **"add notes"**.
- Tapping ⊖ slides in a red **"Delete"** button at the row's end (iOS ≤ 18 style, E for 26).
- Bottom: **Delete Contact** (red, its own card) → action sheet "Delete Contact" (red) / Cancel (A: "tap Delete
  Contact again to confirm").
- Done saves → back to the card (0.25 s cross-fade). Cancel with changes → action sheet **Discard Changes** (red) /
  **Keep Editing** (E).

### 2.15 Contacts: New Contact
- Sheet, large detent: Cancel (left capsule) · title "New Contact" · Done (prominent, right; enabled once any name or
  number is entered) (A: "Tap the Add button… then tap Done").
- Body = the edit form above, with the photo placeholder (the person monogram Ø 120 + "Add Photo" → local poster
  colour) and empty fields. First name gets focus with the keyboard up (E).
- From the Phone keypad / an unknown caller: the number is prefilled in the first phone row as "mobile" (A).
- "Add to Existing Contact" → the contact picker (the list in a sheet). Pick a contact → its edit form with the new
  number appended (E).

---

## 3. Cross-screen flows

**F1. Call a number.** Phone → Keypad → digits (tones; T9 capsule updates) → tap Call → the call screen slides up
(0.45 / 1.0, the keypad page stays under it): "Calling…" → "Ringing…" → the timer → End → "Call Ended" for 1.5 s →
fades back to Keypad (the number cleared) (A/E). A Recents row is added.

**F2. Call back a missed call.** Notification "Missed Call" → tap → Phone, Calls tab, the list at the top →
tap the row's call button → F1 from "Calling…". The missed badge clears when Calls is shown.

**F3. Incoming while using the phone.** Banner drops (§2.7) + ringtone → Accept → the banner morphs into the in-call
screen → home swipe → the green capsule (§2.8) → tap → back to the call. Decline → the banner slides up; a Recents
row "Declined" is added (local word; iOS just shows the call as incoming without a duration) (E).

**F4. Incoming while locked.** Full screen (§2.6) over the lock screen → slide to answer → in-call. End → the lock
screen (not unlocked).

**F5. Send a message to a new number.** Messages → Compose → To: digits → Return (token) → message field → Send →
the sheet becomes the thread; the bubble flies (§2.10.2) → "Sending…" → "Delivered".

**F6. Reply from a notification.** A banner arrives (IR §5) → pull it down → an inline reply field (the input bar
minus "+") → Send. The banner shows "Sent" for 1 s and dismisses (E). (Today: the banner only opens the thread.)

**F7. React to a message.** Long-press a bubble → bar + menu → ❤️ → the badge pops, mode closes → the SMS "Loved
"…"" is sent (setting on).

**F8. Save a caller as a contact.** Calls row tap → Info → Create New Contact → New Contact sheet with the number →
type a name → Done → the row and every thread now show the name.

**F9. Find a contact and text them.** Contacts → index "K" → row → card → message circle → thread opens in Messages
(app switch with the zoom-from-icon motion skipped: a cross-fade, E).

**F10. Delete a conversation.** List → swipe left → Delete → the row collapses (0.3 s) → it moves to Recently Deleted
(30 days, local).

---

## 4. Motion table

| # | Trigger | What moves | Spring / duration | Tag |
|---|---|---|---|---|
| 1 | Tab change (Phone) | selection pill slides + stretches; content cross-fades | 0.35 / 0.75; fade 0.2 s | IR §10 |
| 2 | Filter / Edit menus | button → glass panel morph; rows fade in over the second half | 0.40, bounce 0.20; close 0.35, bounce 0.10 | LG §5.3 |
| 3 | Filter change | list cross-fade | 0.2 s ease | E |
| 4 | Push (row → info / thread / card) | IR §16 push | 0.45 / 1.0 | IR §16 |
| 5 | Swipe actions | row turns into a grey rounded card and follows the finger 1:1; Ø 50 action circles scale 0.5 → 1 as they are revealed; past 60 % width → full-swipe commit (selection haptic at the threshold); release → snap open (circles' width + 10) or closed | 0.35 / 0.85; commit: row slides out 0.25 s, list closes up 0.3 s | M (shape) / E (timing) |
| 6 | Long-press preview / context menu | content lifts 1.03 + shadow; background blur 0 → 20; menu grows from the touch side | 0.35 / 0.8 | IR §3.3, E |
| 7 | Keypad key press | glass press (grow ≈ 1.06, light) | down 0.25 b .2; up 0.40 b .35 | LG §5.1 |
| 8 | T9 capsule appear / change | materialise (0.8 → 1, lensing ramp); text cross-fade 0.15 s | 0.35, no bounce | LG §5.2 |
| 9 | Delete key / add-contact appear | fade + scale 0.8 → 1 | 0.25 / 1.0 | E |
| 10 | Call screen in (outgoing) | slides up from the bottom over the app | 0.45 / 1.0 | E |
| 11 | Call screen out (ended) | fade | 0.25 s | trunk |
| 12 | Incoming banner in / out | drop from the status band + scale 0.9 → 1 / slide up | 0.50 / 0.8; out 0.35 / 1.0 | IR §5, E |
| 13 | Banner → full call screen | platter morphs to full screen; content cross-fades | 0.45 / 0.85 | E |
| 14 | Slide to answer | thumb follows the finger; release before 75 % → back; shimmer loop | back 0.35 / 0.8; shimmer 2.4 s linear | E |
| 15 | Call minimise → capsule | screen shrinks into the status capsule; capsule from a 6 pt dot | 0.45 / 0.85; capsule 0.35 / 0.75 | IR §11, E |
| 16 | Call controls on/off | circle fill glass ↔ white, glyph colour | 0.2 s ease | E |
| 17 | Send bubble | field text rect → bubble rect (x, y, w, h, radius), colour field → green | 0.35 / 0.75 | IR §7 |
| 18 | Others shift up on send / receive | translate y | 0.35 / 0.75 | E |
| 19 | Receive bubble | scale 0.6 → 1 from the tail corner + fade | 0.40 / 0.75 | E |
| 20 | Swipe-left timestamps | bubbles x 0 → −64 with the finger (rubber band beyond); release back | 0.35 / 0.9 | A behaviour, E |
| 21 | Reaction mode | bubble lift 1.03; bar materialises; menu grows | bar 0.35 no bounce; menu 0.40 b .2 | LG §5.2/5.3, E |
| 22 | Tapback badge | scale 0 → 1.15 → 1 | 0.35 / 0.6 | E |
| 23 | Keyboard show / hide; input bar rides it | IR §4 | 0.38 / 1.0 | IR §16 |
| 24 | Interactive keyboard dismiss | keyboard + bar follow the finger 1:1; release → finish or return | 0.38 / 1.0 | E |
| 25 | Input field grows a line | height +21 | 0.25 / 1.0 | E |
| 26 | Send button appear | waveform glyph cross-fades to the green circle (scale 0.6 → 1) | 0.30 / 0.7 | E |
| 27 | "+" menu | grows from the + circle (morph) | 0.40, bounce 0.20 | LG §5.3 |
| 28 | Search focus (Messages / Contacts) | capsule rises above the keyboard; compose → "x" circle morph; title collapses | 0.38 / 1.0; morph 0.40 b .2 | IR §10 |
| 29 | Index scrub | instant jump; no animation | — | E |
| 30 | Card scroll | photo scale 1 → 0.6 and fade; name to the nav row | scroll-driven | E |
| 31 | Edit mode toggle (card) | cross-fade card ↔ form; ⊕/⊖ slide in from the left 20 pt | 0.25 s; 0.35 / 0.9 | E |
| 32 | Favourites jiggle (Edit Favorites) | ±1.5° at ~7 Hz | loop | IR §9.2 |
| 33 | Reduce Motion | 10/13/15/17/19 become 0.25 s cross-fades | — | IR §16 |

---

## 5. Words (our English; iOS casing)

**Phone**
- Tabs: Calls · Contacts · Keypad · (Search circle, accessibility label "Search")
- Nav: Edit · Done · Calls · Recents · Favorites (iOS spelling in UI strings; our docs may say favourites)
- Edit menu: Add Favorite · Edit Favorites · Select
- Filter: Classic · Unified · Calls · Missed · Unknown Callers · Manage Filtering
- Classic footer (ours): "Classic layout isn't available yet."
- Rows: mobile · home · work · iPhone · unknown · Yesterday · (n) · Show All · Mark as Known
- Swipe: Remind Me · Delete. Remind menu: Call Back Reminder (header) · Remind Me in 1 Hour · Remind Me Tonight ·
  Remind Me Tomorrow · Remind Me Later…
- Row menu: Message · Call · Create New Contact · Add to Existing Contact · Delete
- Empty: No Recent Calls · "Calls you make and receive appear here." · "Turn on Cellular to make and receive calls." ·
  Cellular Settings
- Keypad: Paste · Create New Contact · Add to Existing Contact · " and n more" · emergency line (today's text)
- Call screen: Calling… · Ringing… · Call Ended · On Hold · Ending… · Speaker · Audio · Hold · Mute · More · End ·
  Keypad · Hide · Add Call · Send Message · Contact · No audio yet (today's reasons)
- Incoming: mobile · Phone call · Remind Me · Message · Decline · Accept · slide to answer
- Respond with Text: Sorry, I can't talk right now. · I'm on my way. · Can I call you later? · Custom…
- Notification: Missed Call
- Info page: Create New Contact · Add to Existing Contact · Block this Caller · Outgoing Call · Incoming Call ·
  Missed Call · Declined Call · Canceled Call

**Messages**
- List: Messages · Edit · Done · Select Messages · Edit Pins · Search · Pin · Unpin · Mark as Unread · Mark as Read ·
  Hide Alerts · Show Alerts · Delete · Read All · Read · Unread · Draft: · Not Delivered
- Filter: Messages · Unread · Unknown Senders · Recently Deleted · Manage Filtering
- Recently Deleted: Recover · Delete · "Permanently delete n conversations? This can't be undone." · n days
- Empty: No Messages · "Your conversations appear here." · "Turn on Cellular to send and receive text messages." ·
  No Results · Conversations · Messages
- Thread: Text Message • SMS · Today · Yesterday · Sending… · Sent · Delivered · Not Delivered · Try Again ·
  Delete Message · New Message (the "↓" capsule) · Copy · Select · More… · Forward · Call · Send Message ·
  Add to Contacts
- Reactions as text: Loved "…" · Liked "…" · Disliked "…" · Laughed at "…" · Emphasized "…" · Questioned "…" ·
  Reacted 😃 to "…"
- "+" menu: Send Later · Contact
- Compose: New Message · To: · "Group messages aren't supported" · OK
- Settings (Apps > Messages, local): Show in Text Field (Start Dictation / None) · Send Reactions as Text · Screen
  Unknown Senders · Keep Messages

**Contacts**
- Contacts · My Card · Search · No Contacts · "Contacts you add appear here." · Create New Contact · No Results
- Card: Edit · Poster colour · Send Message · Add to Favorites · Remove from Favorites · Block Contact · Unblock
  Contact · Copy · Make this My Card · Delete Contact
- Block sheet: "You will not receive phone calls or text messages from people on the block list." · Block Contact ·
  Cancel
- Edit: Cancel · Done · First name · Last name · Company · add phone · add birthday · add notes · Delete · Discard
  Changes · Keep Editing · Add Photo
- Labels: mobile · home · work · school · iPhone · main · home fax · work fax · pager · other · Custom Label
- New Contact · Add to Existing Contact

---

## 6. Starbite today → gap (trunk 1c166b85: `shell_comms.inc` 2182 lines; Contacts in `shell_lock.c` `page_contacts`)

**What the trunk has**
- **Phone:** a glass tab group Calls / Contacts / Keypad (no Search circle).
  - Calls is a large-title list with a "Favourites" section (number rows) and Recents rows: 64 tall, monogram 40,
    an (i) button that toggles favourite, the time only. A row tap puts the number on the keypad.
  - Keypad: 78 circles at pitch 104 x 96, digits 32 / 500, letters 10 / 700 +1.4, the call button at (195, 605) with
    a **0.6 s hold to dial**, delete at (299, 605), held 0 → "+".
  - Call screen: the avatar over the number (32 / 500) at y 150; the status *under* the number; one row of
    Speaker / Mute / Keypad (78) at y 470; End / Decline / Accept at y 668; slide to answer over the lock; the
    "No audio yet" pill.
  - Incoming always takes the full screen. Minimising goes to the island capsule (B7).
- **Messages:**
  - List: large title; a compose glass circle at the top right; rows 78 with a monogram 44, an unread dot at x 10, a
    2-line preview with a "You: " prefix; swipe left → a red Delete block.
  - Thread: avatar 40 + number 11 / 400 in a capsule; bubbles r 18, line 22, pad 7, max 292, gaps 2 / 10, the older
    side tail; "Sending… / Sent / Delivered / Not Delivered"; the time headers.
  - Input: "+" 36 at x 16, field 36 from x 60, placeholder "Text Message", a green send circle.
  - Long-press: a one-item "Delete" platter. New Message: To: on the 123 layer. A haptic on send.
- **Contacts:** an empty page only ("Contacts can't be added on this phone yet."). There is no contact store, so
  every name shown anywhere is the raw number.

**Ranked changes** (S ≤ ½ day, M ≤ 2 days, L > 2 days)

| # | Change | Size |
|---|---|---|
| 1 | **Contact store + Contacts app** (§2.13-2.15): list with sections and index, card, edit, new, delete, My Card; names resolve in Calls, Messages, the call screen, banners | **L** |
| 2 | **In-call screen to the iOS 27 3 x 2 layout** (§2.5): status above the name 34 / 700, Speaker / Hold / Mute · More / End / Keypad at y 558 / 684, labels 14, the gradient background, the notice card | M |
| 3 | **Incoming call banner when unlocked** (§2.7) + the incoming full screen's Remind Me / Message row (§2.6) | M |
| 4 | **Calls tab to Unified** (§2.1): inline title, Edit + Filter, favourites poster grid, "Recents" 22 / 700, 74 rows with the 50 avatar, the trailing call button (row tap → Info), direction glyphs, run counts, date formats | M |
| 5 | **Thread bubbles to measured values** (§2.10): r 20, h = 19.5 + 21·n, inset 14.4, max 266, gap 4.5, the new hook tail | S |
| 6 | **Input bar to measured values**: + Ø 40 at x 27, field 79..363 h 40, bottom 774, "Text Message • SMS", the audio-button slot | S |
| 7 | **Send animation + sounds**, remove the send haptic (§2.10.2); receive pop + sound | M |
| 8 | **Reaction mode** (bar + menu + badges + SMS text fallback) replacing the Delete platter (§2.10.6); More… select mode with delete | M |
| 9 | **Messages list to iOS 27**: Edit / Filter in the nav, bottom Search + Compose, rows 88 with the 47 avatar at x 27, dot Ø 11 at x 13.5, drop "You: ", swipes (Unread left→right, Delete + Alerts right→left) | M |
| 10 | **Keypad**: tap to dial (hold → a setting), digits 37 / 400, rows at y 234..522, call at 619, delete at (294, 619), T9 capsule, add-to-contacts, redial-on-empty, hold * / #, Paste | M |
| 11 | Thread header: avatar 56 + 17 / 600 name capsule with a chevron, centre y 78 / 121; conversation details page | S |
| 12 | Swipe-left timestamps on the thread | S |
| 13 | Interactive keyboard dismiss | M |
| 14 | Filter menus (Phone, Messages) with Unknown Callers / Senders, Unread, Recently Deleted | M |
| 15 | Search (Phone circle, Messages bottom field) | M |
| 16 | Failed-message auto-retry, then Try Again / Delete sheet | S |
| 17 | Pinned conversations; long-press previews on rows | M |
| 18 | Classic Phone layout option | M (later) |
| 19 | Reply-from-notification inline field | M |
| 20 | `BACK_X` 20 → 16 for these apps' nav buttons (M: 16 on SUP / UGm / TB) — check against the other app specs first | S |

---

## 7. Still estimated (the E items most worth re-checking from public sources)

1. **Incoming call screen, iOS 26/27** (§2.6): whether Remind Me / Message are glass circles in row 1 and Decline /
   Accept in row 2, the "slide to answer" track size, and the status word above the name. Look for User Guide iOS 27
   "Answer or decline incoming calls" art at full size (only icons were found), MacRumors / 9to5Mac iOS 26 Phone
   galleries, Apple Newsroom WWDC25 call-screening renders.
2. **Compact incoming call banner** (§2.7): height, button sizes, layout. iOS 26 review screenshots (MacStories iOS
   26 review, Phone section).
3. **Long-press conversation preview** in Messages (size of the lifted thread card, menu order). The pinned grid is
   now measured.
4. **Long-press menu items** under a bubble (Reply / Copy / Select / Translate / More… order) — the reaction bar is
   measured, the menu is not (the UG27 art shows the bar only).
5. **Messages trailing swipe** (Hide Alerts colour and position next to Delete): Phone's swipe and Messages' leading
   swipe are measured; the Messages trailing pair is not.
6. **Where "Unread" and "Unknown Senders" sit** in the Messages Filter menu (the art shows only Messages / Spam /
   Recently Deleted).
7. **Bubble metrics**: the UG27 art gives line pitch 20.7 with the 17 pt text assumed. Re-measure on a native
   3x screenshot (any review) to confirm 21 vs 22 and the 9.75 padding.
8. **Search results layout** (Messages, Phone): the UG27 search art (`b7eaa123…`, `930c1bb1…`).
9. **New Message sheet** (close circle vs "Cancel", token style): an iOS 26 review screenshot.
10. **Contact card lower section** (action row order, the scroll collapse) and **edit form** (⊕/⊖ style in 26):
    UG27 "Edit contacts" art and TB "Phone-UI-Contacts" (only the list was measured).
11. **Keypad dark mode** fill and **DTMF / sent / received sound lengths**.
12. **Call screen keypad layout** while in a call (Hide placement).

---

## Sources
- Apple Support 126076, "Use Unified layout in the Phone app on iPhone" (Filter items, favourites up to eight, Show
  All, Classic switch; art measured): https://support.apple.com/en-ca/126076
- Apple iPhone User Guide, iOS 27 edition (fetched 2026-11-13): "Make a call", "Answer or decline incoming calls",
  "While on a call", "Screen and block calls", "View and delete the call history", "Use contacts from the Phone app",
  "Send and reply to messages", "React with Tapbacks", "Keep track of messages", "Search in Messages", "Delete
  messages and attachments", "Add backgrounds", "Screen and filter text messages", "Add and use contact information",
  "Edit contacts", "Add or edit your contact info". https://support.apple.com/guide/iphone/welcome/27/ios
- TidBITS, "Comparing the Classic and Unified Views in iOS 26's Phone App" (2025-11-10), screenshots measured:
  https://tidbits.com/2025/11/10/comparing-the-classic-and-unified-views-in-ios-26s-phone-app/
- 9to5Mac, "Here's everything new for Messages in iOS 27" (2026-07-23): auto-retry, Show in Text Field, search by
  number.
- MacRumors, "How to bring back the Phone app's classic design" (iOS 26).
- IR (IOS-REFERENCE.md) §3, §4, §5, §6, §7, §10-§17; LG (LIQUID-GLASS.md) §5.
- Study images: `build/design-ref/apps27/{phone,messages,contacts}/` (git-ignored, never committed).
