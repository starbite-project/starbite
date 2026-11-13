# Settings (iOS 27) — Starbite build spec

Track C, design-ref. v1 2026-11-13. Builders: implement straight from this file. Shared components are in
[IOS-REFERENCE.md](../IOS-REFERENCE.md) (§3 sheets / alerts / menus, §4 keyboard, §6 Settings list geometry, §10 bars and
search, §12 controls, §14 type, §15 colour, §16 motion, §17 haptics) and [LIQUID-GLASS.md](../LIQUID-GLASS.md) (§2.2 the
Liquid Glass slider, §2.8 accessibility, §5 press / materialise / morph / scroll edge). This file does not repeat them.

**Sources**
- Apple iPhone User Guide, **iOS 27 edition** (fetched 2026-11-13): "Find settings" (the full root list with one line
  per section, and the bottom-search art), "Adjust iPhone display and text settings" (the **Appearance** page art),
  "Adjust the screen brightness and color", "Keep the iPhone display on longer", "Manage Wi-Fi settings", "Connect
  Bluetooth accessories", "View or change cellular data settings", "Show the battery percentage", "Understand your iPhone
  battery usage and health" (Battery art), "Save battery life with Power Modes", "Change sounds and vibrations", "Change
  the wallpaper" (Wallpaper art), "Get information about your iPhone", "Change the date and time", "Add or change
  keyboards", "Update iOS", "Reset iPhone settings", "Customize onscreen motion", "Make text easier to read".
  `https://support.apple.com/guide/iphone/<page-id>/27/ios/27`.
- MacStories, iOS and iPadOS 27 review, p. 2 (Appearance, Liquid Glass slider); 9to5Mac 2026-06-15 (slider positions);
  MacObserver (Appearance takes Text Size, Bold Text, Display Zoom, Liquid Glass).
- Starbite trunk 1c166b85: `shell_settings.inc`, `shell_wifi.inc`, `shell_cell.inc`, `shell_update.inc`,
  `shell_startup.inc`, `shell_passcode.inc`, `shell_lock.c`.
- Study images: `build/design-ref/apps27/settings/` (git-ignored). The guide art is 490 x 1008 px with the screen at
  x 22..467 (445 px = 393 pt), **1.132 px/pt**, screen top y 22. ±1 pt.

**Tags**: **A** Apple stated · **M (from X)** measured by us from image X · **3P** a reviewer · **E** estimate.
iOS 27 wins where 26 and 27 differ. All pt are on the S9 frame (390 x 802) unless they say "iPhone".
**Spelling**: the repo's British iOS strings (Control Centre, Colour, Customise) except where code already ships US
words; one choice per build, set in §5.

---

## 1. Information architecture

Settings is one navigation stack: a root list with a bottom search field, pushed pages with a glass back circle and an
inline title. No tab bar. Pushed pages do **not** use large titles (M, from the Appearance, Battery and Wallpaper art:
inline 17 / 600 titles only).

### 1.1 Root, in iOS 27 order (A: "Find settings" table), with our decision per row

| # | iOS 27 row (A) | Icon tile (E colour) | Starbite |
|---|---|---|---|
| 0 | *Your name* (Apple Account) | avatar | **Local equivalent**: the device row, "Galaxy S9" / "Starbite OS" (opens General > About > Name) |
| — | (suggestion cards: "Finish Setting Up…") | | **omit** |
| **Group 1** | | | |
| 1 | Airplane Mode | orange #FF8D28 | **build**: toggle in the row (Wi-Fi, Bluetooth, modem off) |
| 2 | Wi-Fi | blue #0088FF | build (§2.5) |
| 3 | Bluetooth | blue | build (§2.6) |
| 4 | Cellular | green #34C759 | build (§2.7) |
| 5 | Personal Hotspot | green | **omit** (no tethering); the row is hidden, not greyed |
| 6 | Battery | green | build (§2.8) |
| **Group 2** | | | |
| 7 | General | grey #8E8E93 | build (§2.9) |
| 8 | Accessibility | blue | build (§2.10) |
| 9 | Action Button | — | **omit** (no Action button; the S9's Bixby key stays unassigned in v1) |
| 10 | **Appearance** (new in 27) | dark-blue / half-black-half-white circle (E) | **build** (§2.11): Light/Dark, Liquid Glass, Text Size, Bold Text |
| 11 | Apple Pencil | — | **omit** |
| 12 | Camera | grey | build, small (§2.21) |
| 13 | Control Centre | grey | build, small (§2.21) |
| 14 | Display & Brightness | blue | build (§2.12) |
| 15 | Home Screen & App Library | indigo #6155F5 | build, small (§2.21) |
| 16 | Search | grey | build, small (§2.21) |
| 17 | Siri | — | **omit** |
| 18 | StandBy | — | **omit** in v1 |
| 19 | Wallpaper | cyan #00C0E8 | build (§2.13) |
| **Group 3** | | | |
| 20 | Notifications | red #FF383C | build (§2.15) |
| 21 | Sounds & Haptics | pink-red #FF2D55 | build (§2.14) |
| 22 | Focus | indigo | **local equivalent**: Do Not Disturb only (one schedule, one switch); title stays "Focus" |
| 23 | Screen Time | — | **omit** |
| **Group 4** | | | |
| 24 | Face ID & Passcode | green (Face ID) / red (passcode-only phones) | **"Passcode"**, red tile (the S9 has no Face ID we use) (§2.16) |
| 25 | Emergency SOS | red | build + **our Survival tools** (§2.17) |
| 26 | Privacy & Security | blue | build (§2.18) |
| **Group 5** | | | |
| 27 | Game Center / iCloud / Wallet & Apple Pay | — | **omit** all three |
| 27a | (App Store, as on iOS ≤ 26) | blue | **local equivalent**: our free catalogue ("App Store") |
| 27b | — | — | **Ours: "Linux"** (black tile, Tux-free glyph of our own): the Linux subsystem (§2.20). It sits here because this group is where iOS puts platform services. |
| **Group 6** | | | |
| 28 | Apps | indigo grid | build: one row per app, A-Z (§2.19) |

### 1.2 Our own settings and where they go (iOS-native homes)
- **Startup** (OSes, firmware, recovery; Track J's `shell_startup.inc`) → **General > Startup**, directly under Software
  Update (as macOS puts Startup Disk under General). Searchable as "Startup", "Operating Systems", "Firmware",
  "Recovery", "Boot".
- **Survival / Emergency mode, Last 2 %** → **Emergency SOS > Survival** section; Last 2 % also appears in
  **Battery > Low Power Mode** (as iOS shows Low Power Mode in Battery).
- **Medical Card** → Emergency SOS > Medical Card (iOS keeps Medical ID in Health; we have no Health app).
- **Haptic Strength** (our motor level) → Sounds & Haptics > Haptics section.
- **Linux apps** → root Group 5 row "Linux"; each Linux app also has a row in Apps.
- **Software Update** = our updater (`shell_update.inc`), General > Software Update.

### 1.3 Navigation tree (pages built; leaf pickers in italics)
```
Settings
├─ Galaxy S9 (device row) → General > About
├─ Airplane Mode (toggle)
├─ Wi-Fi → network info (i) · join sheet · Other… sheet · Ask to Join Networks* · Auto-Join Hotspot (omit)
├─ Bluetooth → device info (i)
├─ Cellular → Cellular Data Options → *Voice & Data*, *Data Mode*; Network Selection; SIM PIN
├─ Battery → Battery Health · Low Power Mode (toggle) · Battery Percentage (toggle)
├─ General → About → Name
│          → Software Update → Automatic Updates
│          → Startup (Track J's pages)
│          → Phone Storage · Date & Time → *Time Zone* · Keyboard → Keyboards · Language & Region
│          → Legal & Regulatory · Transfer or Reset Phone → Reset (action sheet) · Shut Down
├─ Accessibility → Display & Text Size → Larger Text · Motion · Touch (Vibration) · Per-App (omit)
├─ Appearance → Liquid Glass · Text Size · Display Zoom (omit) · Automatic options (*Sunset to Sunrise / Custom*)
├─ Camera · Control Centre · Display & Brightness → *Auto-Lock* · Night Shift (optional)
├─ Home Screen & App Library · Search · Wallpaper → Add New Wallpaper (gallery sheet)
├─ Notifications → per app · *Display As* · Show Previews
├─ Sounds & Haptics → *Ringtone* · *Text Tone* · *Default Alerts* · *Keyboard Feedback* · *Haptics*
├─ Focus (Do Not Disturb)
├─ Passcode (behind the passcode) → Change Passcode · *Require Passcode*
├─ Emergency SOS → Medical Card · Survival
├─ Privacy & Security → Location Services · Analytics
├─ App Store · Linux
└─ Apps → Calculator, Camera, Clock, Messages, Music, Notes, Phone, Photos, … (one page each)
```

---

## 2. Screens

### 2.0 Shared page geometry (everything below uses it)

| Item | Value (S9 pt) | Tag |
|---|---|---|
| Background | grouped: light #F2F2F7 (M, from guide art (242,242,247)), dark #000000 | M / A (§15) |
| Card | x **16**, w 358, radius 26 continuous; light #FFFFFF, dark #1C1C1E | M (from Battery, Wallpaper and search art: card edge 15.0 / 377.1 pt on 393; the Appearance art alone shows 19.4) |
| Row height, one line | **52** | M (Appearance art: 51.3-53.0; Notes folder list 51.9) |
| Row, title + subtitle | 61-62 (Battery app rows) / 72 (search results) | M (Battery art row pitch 61; search art 71.6-72.4) |
| Row, title + 2 subtitle lines | 95 | M (search art) |
| Icon tile | **30**, radius 7, glyph white ~18-20 | M (search art: 28.3-29.2 pt; ±1) |
| Icon x / text x | tile at card + 15; text at **card + 57** | M (search art) |
| Text x, no icon | card + 16 | M (Appearance art 14-16 incl. glyph bearing) |
| Label / value | 17 / 400 label; 17 / 400 secondaryLabel value; subtitle 15 / 400 secondaryLabel | A / M |
| Chevron | 13 / 600 tertiaryLabel, right edge **16** from the card's right edge | M (Appearance art 15.5) |
| Toggle | 64 x 28; right edge **12** from the card edge (M 9-13: the art disagrees by 4); off track light **#C4C4C6** in the guide art (IOS-REF §12.4 says #E9E9EA E: keep #E9E9EA for the track, #C4C4C6 is the art's outline tone; verify) ; on #34C759 | M (Appearance art) |
| Separator | 0.5 pt, from the text x to the card's right edge − 16 | M (Notes folder art ends 18 short of the card edge; Settings art 16) |
| Section header | sentence case, 15 / 600 secondaryLabel, baseline **13 above** the card top, x = row text x | M (Appearance art) |
| Header → previous card | 35 between cards when there is no footer | M (Appearance art 35.3) |
| Footer | 13 / 400 secondaryLabel, first baseline 23 below the card bottom (cap top 14 below), max width card − 32; **24** from the footer's last line to the next card (or its header) | M (Appearance art) |
| Pushed page: nav row | y 44..88; inline title 17 / 600 centred on y 66; back = 44 glass circle at **x 16**, y 44 | M (Appearance / Battery art: circle 44, left edge 14-16; title centre 22 below the status band) |
| Pushed page: first header baseline | y **128** (40 below the nav row) ; first card top 141 | M (Appearance art: 39.7 below the nav row) |
| Pushed page without a header | first card top y 104 (E) | E |
| Root large title | "Settings" 34 / 700, baseline 124, x 20 | A (§14) / existing `LARGE_BASE` |
| Root first card | top y 140 (E) | E |
| Bottom search field | glass capsule **48** tall, x 20..370 at rest; bottom edge **28 above the screen bottom** → y **726..774**. While searching: field x 20..310, a 12 gap, and a **48 glass dismiss circle at x 322..370** | M (search art: field h 48.6, bottom 28.6 above the screen bottom, X circle 48.6, gap 12; side insets 26.5 on a 55-corner phone → our 20 on a 32 corner) |
| Scroll edge | content scrolls under the status band and nav row with the soft edge (LIQUID-GLASS §5.4) | A |

Cards and rows are flat (no glass) (A: glass is for controls and bars, not content).

**Row press** (all tappable rows, E): touch-down + 80 ms still in a scroll view → the row fills systemGray4 (light
#D1D1D6, dark #3A3A3C), clipped to the card's corners for the first / last row. Release inside → push; the highlight
stays on the row under the incoming page. On pop back, the highlight fades out over 0.35 s ease-out **after** the pop
lands (the iOS "deselect on return"). Scroll starts → highlight cancels at once. Toggle rows have no row highlight;
only the toggle reacts.

**Toggle** (IOS-REF §12.4): tap anywhere on the toggle (not the row) or drag the thumb. Haptic: impact light at the
state change (E). The value applies immediately (no Save anywhere in Settings: A convention).

**Value + chevron rows** open a checkmark list page: rows 52, the chosen one has a blue checkmark (17 / 600 glyph,
systemBlue) at the trailing inset 16; tap = select + selection haptic + the check moves at once (no animation), and the
page **stays** (iOS does not pop on pick). Back shows the new value on the parent row.

### 2.1 Root (Settings)

**Layout** (S9, top to bottom)
1. Status band 0..44. Nav row 44..88: empty at rest (no buttons; A: search is the only toolbar item).
2. Large title "Settings" baseline 124 (34 / 700). Collapses on scroll (IOS-REF §10): over the first 20 pt of
   scroll the large title fades and an inline "Settings" 17 / 600 fades in at y 66.
3. Card 0, the device row (66 tall, E): our Starbite tile as a 56 circle avatar at card + 12; "Galaxy S9" 20 / 600;
   "Starbite OS" 15 / 400 secondary below; chevron. Tap → General > About.
4. Group 1: Airplane Mode (toggle), Wi-Fi (value: network name, "Not Connected", or "Off"), Bluetooth ("On" / "Off"),
   Cellular (value: none; iOS shows "Off" only with no SIM / airplane, E), Battery (no value on iOS 27, E; we keep none).
5. Group 2: General, Accessibility, Appearance, Camera, Control Centre, Display & Brightness, Home Screen & App
   Library, Search, Wallpaper.
6. Group 3: Notifications, Sounds & Haptics, Focus.
7. Group 4: Passcode, Emergency SOS, Privacy & Security.
8. Group 5: App Store, Linux.
9. Group 6: Apps.
10. 35 pt gap, then the end of the content clears the bottom search field: content inset bottom = 802 − 726 + 16 = 92.
11. The search field floats over the content (glass), magnifier glyph 17 pt at field x + 14, placeholder "Search"
    17 / 400 (#BFBFBF light / #404040 dark, IOS-REF §10), trailing mic glyph (**omit**: no dictation; leave the
    space empty).

**States**: Airplane Mode on → Wi-Fi value "Off" (unless re-enabled), Bluetooth "Off", Cellular row label greys to
tertiary with value "Airplane Mode" (E). A pending software update → a red badge "1" on General's row before the
chevron and on the Settings icon (A behaviour on iOS; E geometry: 20 circle, 13 / 600 white).

**Interactions**: tap a row → push. Long-press: nothing (E). Pull down at the top: rubber band only (no refresh).
Scrolling down does not hide the search field (E: it stays; on iOS 26 it stays visible).

### 2.2 Search (root, field focused)

**Flow** (A: field at the bottom, results above it; M: geometry from the search art)
1. Tap the field → keyboard rises (IOS-REF §4, spring 0.38 / 1.0); the field rides on top of the keyboard with an 8 pt
   gap (E) and shrinks from the right to make room for a **48 glass circle with an "xmark"** (17 / 600) appearing
   12 pt to its right (materialise, LIQUID-GLASS §5.2). The large title and list fade out (0.2 s) leaving the grouped
   background; the caret blinks in the field.
2. Empty query: the background stays empty (E; iOS 27 shows no suggestions).
3. Typing: results appear per keystroke (no debounce needed for our index size) in **one card** at the top (card
   top y 44 + 8 = 52 on S9; M: 91 pt on iPhone = status 54 + 37). Each result row:
   - the page's icon tile (30) — the **section's** tile for a setting inside a page;
   - title 17 / 400 (the setting's own name, e.g. "Auto-Lock");
   - breadcrumb 15 / 400 secondaryLabel: the path **without** "Settings", pages joined by " → " (A art: "Sounds &
     Haptics → Alarms and Timers"), wrapping to a second line when long (row 95);
   - no chevron (M: none in the art).
   - Row heights 72 / 95 (M); separators from the text x.
4. Ranking (E): exact title prefix, then word prefix, then substring; pages before rows inside pages; at most 30.
5. A clear button (a filled grey circle with a white "x", 17 pt) appears inside the field's trailing end once text
   exists (M: present in the art); tap clears the text, keeps focus.
6. Scrolling the results dismisses the keyboard interactively (E); the field drops back to y 726 with the X circle
   still beside it.
7. Tap a result → push that page (from the root, the stack becomes root → … → page, so Back walks up the real
   breadcrumb: iOS does this, E); the target row flashes the press colour once (0.6 s fade) (E).
8. Tap the X circle → query cleared, keyboard down, results gone, root list and title fade back (0.25 s), the circle
   dematerialises and the field grows back to full width (spring 0.4 / 0.85, E).
9. No match: centred in the space above the field: a 48 magnifier glyph (tertiaryLabel), "No Results for “<q>”"
   20 / 600, and "Check the spelling or try a new search." 15 / 400 secondary (E: iOS's standard empty-search view).

**Index**: every page title and every row label in this spec, plus synonyms (E): "dark" → Appearance, "brightness"
→ Display & Brightness, "boot" / "recovery" / "firmware" → Startup, "vibration" → Sounds & Haptics > Haptics,
"glass" / "transparency" → Appearance > Liquid Glass and Accessibility > Reduce Transparency, "battery percentage",
"medical" → Medical Card, "survival" → Emergency SOS > Survival, "linux" / "terminal" → Linux.

### 2.3 Pushed page template
Back circle (chevron.left 17 / 600, label colour) at x 16, y 44, 44 x 44, glass (interactive press, LIQUID-GLASS
§5.1). Inline title centred on y 66, 17 / 600, truncated to 390 − 2 × (16 + 44 + 8) = 254 wide. Right side: nothing
unless the page says. Edge-swipe from x < 20 pops interactively (A behaviour; IOS-REF §16 push spring); the back circle
pops. Haptics: none on push / pop.

### 2.4 Airplane Mode (root toggle)
On: Wi-Fi, Bluetooth and modem radios off; status bar shows the airplane glyph (A). Turning Wi-Fi or Bluetooth on
while in Airplane Mode is allowed (A) and remembered for the next time (A behaviour, iOS 13+). Off: radios return to
their previous states. Haptic light.

### 2.5 Wi-Fi

**Page** (E structure from iOS 18/26; A row names from the guide)
1. **Header card** (E, iOS 18+): a 58 Wi-Fi glyph tile centred, "Wi-Fi" 22 / 700 centred, and one centred 15 / 400
   secondary line in our words: "Join Wi-Fi networks nearby and manage the ones you know." Card padding 24 top /
   20 bottom. (Apple's "Learn more…" link: **omit**.)
2. Card: **Wi-Fi** toggle row. When joined, a second row in the same card with the check **in front of** the name:
   blue checkmark (17 / 600) at card + 16, name at card + 44, then a lock glyph (13 pt,
   secondary) if secured, the 3-bar Wi-Fi glyph (17 pt), and a blue "i" info circle (22 pt, systemBlue) at the
   trailing inset 16. Under the name, while joining: "Joining…"; after a failed internet check: "No Internet
   Connection" 13 / 400 secondary (E).
3. Section **"My Networks"**: known networks in range (name, lock, bars, i). Hidden when empty.
4. Section **"Other Networks"** with a 17 pt spinner at the header's trailing end while scanning; rows: name, lock,
   bars, i; strongest first; last row **"Other…"** (label colour, no glyphs) opens the manual-join sheet.
5. Card: **Ask to Join Networks** (value: "Notify"; picker Off / Notify / Ask) (A) — our implementation: "Off" only is
   real in v1, Notify posts a notification (E: build later). Footer in our words: "Known networks are joined
   automatically. When none is in range, you'll be told about networks you can join."
6. **Auto-Join Hotspot**: **omit** (needs another Apple-account device).

**Interactions**
- Toggle off → the networks sections collapse (rows slide up and fade, 0.3 s, E); on → "Other Networks" header with
  spinner appears and rows fade in as found.
- Tap an open network → joins in place: the row moves to the top card with a spinner where the check will be; success
  → check. Tap a secured one → the **join sheet**.
- Tap "i" → network info page (only for known / joined networks; for unknown, i opens a reduced page with "Join This
  Network").

**Join sheet** (IOS-REF §3.4, large detent, keyboard up)
- Title, centred 17 / 600, two lines max: `Enter the password for “<SSID>”`.
- Leading: 44 glass circle with "xmark" (Cancel). Trailing: **"Join"** text capsule, prominent (systemBlue glass,
  white 17 / 600) — disabled (grey label, no tint) until ≥ 8 characters (A: WPA2 minimum; E UI).
- Card: one row "Password" label + secure field (dots 17 pt) with a trailing eye toggle (our extra: our existing
  Show / Hide; iOS has none, E) — keep it, it helps on a phone without password sharing.
- Footer (omit Apple's password-sharing text). Our footer: "The password stays on this phone."
- Return key reads "Join". Wrong password → alert "Incorrect Password" / message `The password for “<SSID>” is
  incorrect.` / "OK" (E words), the sheet stays with the field cleared. Success → sheet slides down, the row joins.
- **Other… sheet**: title "Other Network"; rows Name, Security (value "WPA2/WPA3 Personal", picker: None, WEP (omit),
  WPA2/WPA3 Personal, WPA3 Personal) and Password; "Join" capsule.

**Network info page** (title = SSID) (A rows from the guide; E order from iOS 18)
1. Card: **Forget This Network** (systemBlue action row). Tap → alert `Forget Wi-Fi Network “<SSID>”?` / "Your phone
   will no longer join this network." / Cancel, **Forget** (destructive). (E words)
2. Card: **Auto-Join** toggle. **Password** row (value "••••••••"; tap → passcode screen, then the password shown in
   plain text with a Copy menu) (E).
3. Card: **Low Data Mode** toggle (A) — footer in our words: "Low Data Mode lets apps hold off on background downloads
   and syncing on this network."
4. Card: **Private Wi-Fi Address** (A) → **omit** in v1 (our driver uses the fixed address); **Limit IP Address
   Tracking** → **omit**. Show instead a value row "Wi-Fi Address" with the MAC, masked to the last 3 octets (local
   privacy policy).
5. Section "IPv4 Address": Configure IP (value "Automatic", picker Automatic / Manual / BootP→omit), IP Address,
   Subnet Mask, Router; then a card with **Renew Lease** (blue action).
6. Section "DNS": Configure DNS ("Automatic"). Section "HTTP Proxy": Configure Proxy ("Off").
7. **Ours**: card "Test the Internet" (blue action) with its result as a value ("Works" / "Failed") — keep from trunk,
   put last.

**Haptics**: toggle light; join success none (E); wrong password notification error.

### 2.6 Bluetooth
1. Header card (E, as Wi-Fi's): Bluetooth glyph tile, "Bluetooth", "Connect keyboards and other accessories."
2. Card: **Bluetooth** toggle. Footer while on: `Now discoverable as “Galaxy S9”.` (E; iOS says the device name).
3. Section **"My Devices"**: paired devices: name, value "Connected" / "Not Connected", blue "i". Tap a row: connect /
   disconnect toggle in place ("Connecting…" value with a spinner).
4. Section **"Other Devices"** with the header spinner while scanning; named devices only; tap → pair; a PIN request
   is an alert with the code (existing pairing alert) (A behaviour).
5. Our honest footer when the driver can't do audio: "Starbite OS can pair keyboards. Audio and file transfer aren't
   available yet."
**Device info page** (A): Name (tap → rename field, Done) (A "Not all accessories support renaming"), Device Type
(value, e.g. "Keyboard"), **Disconnect** (blue, when connected), **Forget This Device** (blue) → action sheet "Forget
Device" (destructive) / Cancel (A: "Forget Device" confirm).

### 2.7 Cellular (A rows from the guide; E order)
1. Card: **Cellular Data** toggle. **Cellular Data Options** (chevron). Footer, our words: "Turn off cellular data to
   keep all data on Wi-Fi, including notifications and updates."
   - iOS asks no confirmation on Cellular Data; our trunk shows "Turn On Cellular Data?" → **drop the alert** (gap).
2. Card (SIM): carrier name as a section header (E: iOS shows the SIM label); rows **Network Selection** (value
   "Automatic"; page: Automatic toggle, operators list when off), **SIM PIN**, **Carrier Services** → omit.
3. Section "Cellular Data": Current Period (value e.g. "1.2 GB"), then per-app toggles (our apps + "System Services"
   row with its value), **Show All** if more than 10; last card **Reset Statistics** (blue) with footer "Last Reset:
   <date>" (A rows).
4. **Cellular Data Options** page: Data Roaming toggle (A), **Voice & Data** (value "4G" / "LTE"; picker: 4G, 3G, 2G →
   S9 modem modes), **Data Mode** (value "Standard"; picker Standard / Low Data Mode) (A).
5. Wi-Fi Calling, Calls on Other Devices, Personal Hotspot row → **omit**.
Our extras kept on this page: "Signal" (value in dBm bars) and "Network" (e.g. "LTE") in the SIM card, read-only.

### 2.8 Battery (A rows; M from the Battery art)
1. **Level card** (M: partly visible in the art): the percentage large (~44 / 700, E), under it 15 / 400 secondary
   "Last Charged to 70%  ·  <relative time>" (E words from art), a 6-pt level bar across the card (green, white
   track) (M: bar visible at the card's bottom). While charging: "Charging · 80% in 25 min" (A behaviour: estimates to
   80 % and 100 %; our estimate from the gauge rate, E).
2. Card: **Battery Percentage** toggle (A) → shows the number inside the status-bar battery.
3. Card: **Low Power Mode** toggle (A: on iPhone 14 and earlier it is a plain toggle here) with footer, our words:
   "Low Power Mode cuts background work and lowers peak performance until you charge to 80%." (A: auto-off at 80 %).
   **Ours below it**: "Last 2 %" (chevron, value On / Off) and "Last 2 % at 3 %" toggle.
4. Card: **Battery Health** (value "Normal" (M: art) or "Service" when < 80 %, E) → page: Maximum Capacity, Cycle
   Count (A: shown on newer models), footer as trunk.
5. Section **"Daily Usage"** (A + M): card: a 17 / 600 sentence comparing today to usual (A semantics: blue when
   similar or lower, orange when higher) — our words: "You're using about as much battery as usual by <time>."; a
   divider; "Average" (13 / 600 secondary) / "Today" (13 / 600 blue) labels over values 28 / 600 with a smaller "%"
   (M); an 8-day bar chart (bars ~18 wide, r 4, grey #E5E5EA "all day" and darker grey "by now", today blue)
   with a horizontal "Average" line and label (M); weekday letters 11 / 500 under the bars; legend "All Day" / "Daily
   by <time>" with dots (M).
   **Local equivalent**: we keep level history in memory since boot (trunk BH). v1: show the "Today" level chart
   (the iOS View All page's lower graph: level line over the day, green fill while charging) in this card instead of
   the 8-day bars until we persist 8 days. (E)
6. Section **"App and System Activity"** (M: art): rows with the app icon (30, its own rounded square), name 17 / 400,
   "On screen: 1h" 15 / 400 secondary, a second line "Background: 16m" when any, percentage value + chevron; last row
   **View All Battery Usage**. Needs per-app CPU accounting → **E / later**; omit the section until we have it.

### 2.9 General (E order from iOS 18/26; A rows from the guide)
1. Card: **About**, **Software Update** (badge when pending), **Startup** (ours).
2. Card: **Phone Storage** (iOS "iPhone Storage": a stacked bar of used space by kind, then apps by size; local
   equivalent on the card + internal flash), **Background App Refresh** → omit.
3. Card: **Date & Time**, **Fonts** → omit, **Keyboard**, **Language & Region**, **Dictionary** → omit.
4. Card: **VPN & Device Management** → omit. **Legal & Regulatory** (our open-source licences, text page).
5. Card: **Transfer or Reset Phone** (iOS "Transfer or Reset iPhone").
6. Card: **Shut Down** (systemBlue action, centred? E: left-aligned blue label) → the power-off slider screen.

**About** (A list, our values; values right-aligned 17 / 400 secondary; long-press a value → "Copy" menu, A):
- Card: Name ("Galaxy S9", chevron → rename page: one text field row with a clear button; Done = keyboard return),
  Starbite OS Version ("1.x (build)"; iOS "iOS Version" opens a page with the build; ours: tap toggles build number,
  E), Model Name "Galaxy S9", Model Number "SM-G960F", Serial Number → **hidden** (local policy: no identifiers on
  screen in v1).
- Card: Notes / Photos / Videos / Applications counts (iOS lists Songs, Videos, Photos, Applications), Capacity,
  Available.
- Card: Wi-Fi Address (masked), Bluetooth (masked), Modem Firmware (value from the modem).
- Card: Network (operator), Carrier (operator + version), IMEI → hidden.
- Footer, ours: "Starbite OS runs from the memory card."

**Software Update** (A rows; our updater's states from `shell_update.inc`):
- Card: **Automatic Updates** (value "Off" / "On"; page: section "Starbite OS Updates": **Automatically Download**
  toggle; footer ours: "Updates are copied to the card when the phone is charging and on Wi-Fi.")
- Status card, centred column: 60 Starbite tile, "Starbite OS 1.4" 17 / 600, "Starbite · 48 MB" 13 / 400 secondary,
  the notes (15 / 400, 3 lines + "More…"), then a full-width prominent capsule **"Update Now"** (50 tall) and a
  plain capsule **"Update Tonight"** (omit until scheduling exists) (E layout from iOS 26).
- Checking: a 20 spinner and "Checking for Update…" centred (E words). Up to date: "Starbite OS 1.3" + "Starbite OS is
  up to date" (E). Staging: a progress bar + "Preparing… 34%" (E). Ready: "Restart to Install". Failure: the reason
  sentence + "Try Again" (trunk words).

**Date & Time** (A rows): Card: **24-Hour Time** toggle. Card: **Set Automatically** toggle (on, disabled while
the modem gives the time — trunk), **Time Zone** (value: city; page: search field at the bottom + city list; A
"search for a city"). When Set Automatically is off (later): inline date row (opens an inline calendar card) and time
row (inline wheel), E.

**Keyboard** (E order from iOS 18): Card: **Keyboards** (value "1"; page: list "English (UK)", "Add New Keyboard…"
greyed, Edit → omit), **Text Replacement** → omit v1. Section "All Keyboards": **Auto-Capitalisation**,
**Auto-Correction** (when we have it, else hide), **Check Spelling** (hide), **Enable Caps Lock**, **Predictive
Text** (hide until the predictive bar works), **Smart Punctuation**, **Character Preview**, **“.” Shortcut** (all
toggles). Keyboard haptics live in **Sounds & Haptics > Keyboard Feedback** (A), not here.

**Language & Region**: Section "Preferred Languages": "English" (one row) + footer ours; Card: Region (value), Calendar
("Gregorian"), Temperature (°C / °F, segmented-like picker page), Measurement System (Metric / US / UK), First Day of
Week, Date Format, Number Format (E list); a preview card at the bottom "Region Format Example" (E) with the formatted
time, date and numbers.

**Transfer or Reset Phone** (A): page with an empty top area (iOS shows "Prepare for New iPhone" → **omit**), then a
card at the bottom with **Reset** (blue) and **Erase All Content and Settings** (blue; **omit** v1). Reset → action
sheet (IOS-REF §3.2, anchored at the row): **Reset All Settings**, **Reset Network Settings**, **Reset Keyboard
Dictionary** (omit until we learn words), **Reset Home Screen Layout**, **Reset Location & Privacy** (A list). Each →
passcode (if set) → an alert with the destructive button named like the item (E).

### 2.10 Accessibility (E structure from iOS 18/26; A rows named in the iOS 27 guide)
Root: section **"Vision"**: VoiceOver (omit), Zoom (omit), **Display & Text Size**, **Motion**, Spoken Content
(omit), Audio Descriptions (omit). Section **"Physical and Motor"**: **Touch** (→ Vibration toggle, Haptic Touch
duration Fast / Default / Slow), the rest omit. Sections Hearing / Speech / General: omit in v1 except **Accessibility
Shortcut** (later).

**Display & Text Size** (A rows: Bold Text, Larger Text, Auto-Brightness, Dim Flashing Lights, Reduce Bright Effects;
E order and the rest from iOS 18):
- Card: **Bold Text** (mirrors Appearance's), **Larger Text** (value "Off" / "On" → page: Larger Accessibility Sizes
  toggle + the text size slider, 12 steps when on), **Button Shapes**, **On/Off Labels**.
- Card: **Reduce Transparency** (glass becomes opaque: LIQUID-GLASS §2.8), **Increase Contrast** (rims + darker
  text), **Differentiate Without Colour**, **Prefer Horizontal Text** (omit).
- Card: **Smart Invert**, **Classic Invert**, **Colour Filters** (omit v1 for all three).
- Card: **Reduce White Point** (slider 25-100 % when on), **Auto-Brightness** (A: lives here, not in Display &
  Brightness) with footer ours: "Turning off Auto-Brightness can use more battery."
- Card: **Dim Flashing Lights** (omit: no video analysis), **Reduce Bright Effects** (A: tones down highlights and
  flashes, e.g. on key presses → our key pop-up flash and glass glints dimmed).
- Footer under Reduce Transparency (A behaviour): with it or Increase Contrast on, the Appearance > Liquid Glass slider
  is disabled (A: you must turn them off to change the look).

**Motion** (A rows from the iOS 27 guide): **Reduce Motion** (A: off = full Liquid Glass morphing; on → IOS-REF §16
cross-fades), **Prefer Cross-Fade Transitions** (E, appears under Reduce Motion), **Auto-Play Message Effects**
(omit: SMS only), **Auto-Play Animated Images**, **Auto-Play Video Previews**, **Prefer Non-Blinking Cursor** (A),
**Limit Frame Rate** (omit: 60 Hz panel). Vehicle Motion Cues: omit.

### 2.11 Appearance (new in iOS 27) — **M from the guide's Appearance art** unless tagged

Content in order (A: guide text + art):
1. Header **"Appearance"** (section header, 15 / 600).
2. Card (M: 267 + 52 = 319 tall on iPhone; ours the same):
   - **Two appearance cards**, centred at screen x 195 ± 66.5 (M ±67 on 393): each a phone thumbnail **67 x 150**,
     radius ~10 (E), top **30** below the card top; it shows our wallpaper with a clock "9:41"-style time and two glass
     notification bars, light and dark versions (our art).
   - Label "Light" / "Dark" 17 / 400 centred, baseline ~ card top + 205 (M: centre +199.6).
   - Selection circle under each, centre at card top + 227 (M): selected = filled systemBlue 20 circle (M ~18-20) with a
     white check; unselected = a 1.5 pt systemGray3 ring.
   - Separator at card top + 267 (M), then the **Automatic** toggle row (52).
   - Tap a thumbnail, its label or circle → switches at once: the whole UI cross-fades 0.3 s (E), selection haptic.
3. Footer (A meaning, our words): "Switches between light and dark at sunrise and sunset."; when Automatic is on, a
   row **Options** (value "Until Sunrise" / "Light Until 22:00") appears in the card under Automatic (A: guide says turn
   on Automatic, then tap Options) → page: **Sunset to Sunrise** / **Custom Schedule** checkmarks; Custom shows
   "Light Appearance" and "Dark Appearance" time rows (inline wheels). Sunset needs location: we have the time zone's
   city coordinates → OK (E).
4. Card: **Liquid Glass** (value "Default" (M), chevron) with footer (M, our words): "Liquid Glass changes with what's
   under it."
5. Card: **Text Size** (chevron) and **Bold Text** (toggle) (M: two rows, 104 tall).
6. Card: **Display Zoom** (value "Default") (M) → **omit** on the S9 v1 (no second layout scale; hide the card).

**Liquid Glass page** (A: drag right = more tinted, left = more clear; 3P: positions "More Clear" / "Default" / "More
Tinted", continuous; MacStories: preview over photos):
- A preview card on top (E, 300 tall): one of our photos with real glass controls on it (a tab bar capsule, a
  button, a notification platter) rendered live by the compositor so the slider changes them as it moves; swipe the
  preview sideways to change the photo (3 photos, page dots) (E).
- Card: the slider (IOS-REF §12.3), full card width − 32, with the end labels "More Clear" and "More Tinted"
  (13 / 400 secondary, under the track's ends) and a tick at the centre "Default" (3P). Continuous; a **selection
  haptic** and a soft snap when crossing the Default tick within ± 3 % (E). The value maps to LIQUID-GLASS §2.2.
- Row value on the parent: "Default" at the tick, else "Custom" (E).
- Disabled (greyed, footer explains) when Reduce Transparency or Increase Contrast is on (A).

**Text Size page** (A: drag the slider): a preview paragraph card (17 pt body at the chosen size, our words: "Apps
that follow your text size will use the size you choose below."), then the slider card with a small "A" (13) and a
large "A" (24) at the ends, **7 steps** (E: Dynamic Type xS…xxxL, "Large" default at step 4), ticks, selection haptic
per step. sbtk needs a global type scale (gap L).

### 2.12 Display & Brightness (iOS 27: what is left after Appearance moved out; A desc: "Screen brightness, Night
Shift, Auto-Lock, Raise to Wake")
1. Header "Brightness"; card: the brightness slider (sun small / large glyphs at the ends) (A: drag the slider); True
   Tone toggle → **omit** (no colour sensor calibration).
2. Card: **Night Shift** (value "Off" / "21:00 to 07:00") → page: Scheduled toggle, From/To row, "Manually Enable
   Until Tomorrow" toggle, Colour Temperature slider "Less Warm" … "More Warm" (A rows). Local: a compositor warm
   tint (gap M; optional).
3. Card: **Auto-Lock** (value "30 Seconds") → checkmark list: 30 Seconds, 1 Minute, 2 Minutes, 3 Minutes, 4 Minutes,
   5 Minutes, Never (A: "choose a length of time"; E list) — footer: "With Low Power Mode on, Auto-Lock is 30
   seconds." (E), **Raise to Wake** toggle (local: the S9 accelerometer, optional), Always On Display → **omit**.
4. Our trunk's "Auto-Brightness" row moves to Accessibility > Display & Text Size (A); search still finds it.

### 2.13 Wallpaper (M from the guide's Wallpaper art; A text)
1. No header. One big card (M: card x 16, top ~ y 140 on S9; 455 pt tall on iPhone → **~420** on the S9, E):
   - "CURRENT" 15 / 400 secondary, uppercase, centred, 30 below the card top (M: caps in the art).
   - A horizontal pager of **pairs**: the lock screen thumbnail (left) and home screen thumbnail (right), each ~131 x
     285 on iPhone → scale to 0.92 → **120 x 262**, radius ~16, 10 apart (M ±2), centred; the next pair peeks at the
     right edge. Each thumbnail has a glass capsule **"Customise"** (13 / 500, white on dark glass, 28 tall, ~100 wide)
     14 above its bottom.
   - Page dots under the pager (7 pt dots, 10 apart, 16 below the thumbnails) (M).
   - A prominent capsule **"+ Add New Wallpaper"** (systemBlue fill, white 15 / 600, 32 tall, ~170 wide) centred,
     16 below the dots, 24 above the card bottom (M).
2. A tip card below (systemGray5 fill, not white; M): title 17 / 600 "Change your Wallpaper from the Lock Screen", body
   15 / 400 secondary (our words: "Touch and hold the Lock Screen to add, edit or switch wallpapers.") and a small lock
   screen drawing on the right (our art).
- **Customise** (lock) → the lock screen editor (lock-screen spec). **Customise** (home) → a sheet with colour /
  photo / Blur options (A). **Add New Wallpaper** → the gallery sheet (large detent): top row of category buttons
  (local: **Photos**, **Colour**, **Starbite** night-sky set; Emoji / Weather / Astronomy → omit), featured sets
  below; pick → preview full screen with "Add" (top-right) → action sheet: **Set as Wallpaper Pair** /
  **Customise Home Screen** (A).
- Trunk today: one fixed wallpaper → gap L.

### 2.14 Sounds & Haptics (A rows from the guide; E order from iOS 18)
1. Card: **Silent Mode** toggle (E: iOS 17+ top row; local: mutes ringer and alerts; the S9 has no switch).
2. Header "Ringtone and Alerts": card: volume slider (speaker glyphs), **Change with Buttons** toggle (A).
3. Card: **Ringtone** (value "Ring"), **Text Tone** ("Note"), **New Voicemail** (omit), **Calendar Alerts** (when we
   have Calendar alerts), **Reminder Alerts** (omit), **Default Alerts** ("Tri-tone"-like: ours "Beep") — each a picker
   page: top card **Haptics** (value "Synchronised"), then section "Ringtones" with checkmark rows; **tap previews the
   sound** (A) and selects it; leaving the page stops it.
4. Header "Alarms and Timers": **Match Ringtone Volume** toggle; when off, a volume slider appears (A: iOS 27 separate
   volumes). Header "Alerts and System Sounds": same pair (A).
5. Card: **Keyboard Feedback** (value "Haptic"; page with two toggles **Sound** and **Haptic**) (A), **Lock Sound**
   toggle (A).
6. Card: **System Haptics** toggle (A) with footer ours: "Play haptics for system controls and interactions."
   **Ours** under it: header "Haptic Strength", segmented Off / Light / Medium / Strong (trunk), footer as trunk.
7. Headphone Safety → omit (no BT audio).

### 2.15 Notifications (E from iOS 18/26)
1. Header "Display As"; card: three phone thumbnails **Count**, **Stack**, **List** with circles under them (same
   component as Appearance's cards, 3 across) (E).
2. Card: **Scheduled Summary** → omit. **Show Previews** (value "Always"; picker Always / When Unlocked / Never).
   **Screen Sharing** → omit.
3. Header "Notification Style": one row per app that posts: app icon (30), name, subtitle 13 / 400 secondary
   ("Banners, Sounds, Badges" / "Deliver Quietly" / "Off").
**App page** (title = app name): Card: **Allow Notifications** toggle. Header "Alerts": card with three
thumbnails **Lock Screen**, **Notification Centre**, **Banners** each with a check circle; **Banner Style** (value
"Temporary"; picker Temporary / Persistent). Card: **Sounds** toggle, **Badges** toggle. Header "Lock Screen
Appearance": **Show Previews** ("Always (Default)"), **Notification Grouping** ("Automatic"; By App / Off).

### 2.16 Passcode (`shell_passcode.inc`; E rows from iOS)
Opening the row asks for the passcode first (full-screen passcode, A behaviour). Card: **Turn Passcode Off** /
**Turn Passcode On**, **Change Passcode** (blue actions). Card: **Require Passcode** ("Immediately"). Header "Allow
Access When Locked": **Today View and Search**, **Notification Centre**, **Control Centre**, **Lock Screen Widgets**,
**Return Missed Calls** (toggles). Erase Data → omit. Footer ours: "Emergency mode and the torch always work without
the passcode." (safety rule from trunk).

### 2.17 Emergency SOS (A: Auto Call, contacts; E rows) + **Survival** (ours)
1. Card: **Call with Hold and Release** (local: hold on the lock screen's SOS; toggle), **Call with 5 Button
   Presses** (local: 5 presses of the power key; toggle). Footer ours: "Calls the local emergency number after a
   countdown."
2. Card: **Countdown Sound** toggle. Crash Detection → omit. Emergency SOS via Satellite → omit.
3. Card: **Medical Card** (heart tile, chevron → trunk's editor). Footer ours: "Shown on the Lock Screen in an
   emergency."
4. Header **"Survival"** (ours): **Emergency Mode** toggle, **Last 2 %** (chevron), **Last 2 % at 3 %** toggle. Footer
   (trunk words, kept).

### 2.18 Privacy & Security (E)
Card: **Location Services** (value "On" / "Off"; page: toggle + per-app list "While Using" / "Never"). Card: per-kind
rows with tiles: Contacts, Photos, Camera, Microphone, Bluetooth, Files (values: count of apps). Card: **Analytics &
Improvements** (value "Off"; page with all toggles off and disabled; footer ours: "Starbite OS collects no analytics."). Tracking, Safety Check, Sensitive Content Warning, Apple Advertising, Lockdown Mode → omit. Our footer from trunk
stays as the last footer.

### 2.19 Apps
Search field at the **bottom** inside this page too (E: iOS 18+ Apps page has search), then sections by first letter
(A-Z headers 15 / 600) with app rows (icon 30 + name). Each app's page: per-app settings (Notes: see spec-notes §2.8;
Messages, Phone, Camera, Clock: their specs). Top card on each app page: **Notifications**, **Cellular Data** toggle
(E: iOS "Allow <App> to Access").

### 2.20 App Store and Linux (ours)
- **App Store**: Automatic Downloads / App Updates toggles, "Use Cellular Data" toggle, "Offload Unused Apps" → omit.
- **Linux**: card: Linux (value "Running" / "Off" / "Could not start"), Apps (count), Running (count), Memory; card:
  App Store, Terminal (chevrons); footer (trunk words).

### 2.21 Small pages (E)
- **Camera**: Formats (High Efficiency / Most Compatible), Record Video (value), Grid toggle, Mirror Front Camera toggle.
- **Control Centre**: **Access Within Apps** toggle; **Reset Control Centre** (blue) (A desc).
- **Home Screen & App Library**: Newly Downloaded Apps (Add to Home Screen / App Library Only), Show in App Library
  toggle, Show on Home Screen ("Search" button) toggle, Notification Badges toggle (A desc).
- **Search**: Show Recent Searches toggle, Show Related Content toggle, Apps list with "Show in Search" (A desc).
- **Focus** (local: Do Not Disturb only): card **Do Not Disturb** (value "Off" / scheduled time) → page: toggle,
  schedule (From / To), Allow Calls From (Everyone / No One / Favourites (omit)).

### Transitions and haptics (all pages)
- Push / pop: IOS-REF §16 (response ~0.45, damping 1.0; old page moves 30 %, dims; the moving page's leading corners
  round to the display corner 32 during the move, 3P). Interactive edge-swipe pop follows the finger; release past
  50 % or with velocity > 500 pt/s completes.
- Large title collapse (root only): 20 pt cross-fade; the scroll edge effect fades in under the status band.
- Sheets (join, Other…, wallpaper gallery): IOS-REF §3.4.
- Haptics: toggle impact light; picker check selection; slider ticks selection; segmented change selection; errors
  notification error. No sounds in Settings except the tone previews (and keyboard clicks if enabled).

---

## 3. Cross-screen flows

**Join a secured Wi-Fi network**
1. Settings → Wi-Fi (push). 2. Toggle on if off; the "Other Networks" spinner shows; rows fade in.
3. Tap a secured network → join sheet slides up with the keyboard. 4. Type ≥ 8 characters → Join enables.
5. Join (or return key) → sheet goes; the network moves into the top card with a spinner in place of the check.
6. Connected → check mark, bars; the root's Wi-Fi row value becomes the SSID. Failure → "Incorrect Password" alert,
   the sheet stays.

**Find a setting by search**
1. Root → tap the bottom field → keyboard + X circle. 2. Type "lock" → results: Auto-Lock (Display & Brightness),
   Lock Sound (Sounds & Haptics), Lock Screen… 3. Tap Auto-Lock → the stack becomes Settings › Display & Brightness ›
   Auto-Lock; the Auto-Lock page shows with the check on the current value. 4. Back twice → root, search still
   active with the query (E: iOS keeps the search state) → X circle to leave search.

**Switch to Light appearance**
1. Settings → Appearance. 2. Tap the Light thumbnail → selection haptic; the whole system cross-fades to light
   (0.3 s); glass presets flip (LIQUID-GLASS §2.5). 3. Back → root now light.

**Make glass more opaque**
1. Settings → Appearance → Liquid Glass. 2. Drag the slider right → the preview's glass tints live; crossing Default
   ticks. 3. Back: the row reads "Custom".

**Reset all settings**
General → Transfer or Reset Phone → Reset → action sheet → Reset All Settings → passcode → alert "Reset All Settings"
(destructive) → every setting back to its default; known Wi-Fi networks are forgotten too (A: Reset All Settings
includes network settings; no notes, photos or other data are touched). Trunk's Reset keeps networks → change.

**Update Starbite OS**
General (badge 1) → Software Update → card with the version → Update Now → progress → "Restart to Install" → the
phone restarts into the new version; first start shows "Updated to <version>" (trunk).

---

## 4. Motion table

| Trigger | What moves | Spring / duration | Tag |
|---|---|---|---|
| Tap a row | new page slides in from the right (full width), old page slides 30 % left and dims 10-20 % | response 0.45, damping 1.0 | E (IOS-REF §16) |
| Back / edge swipe | reverse; interactive follows the finger | same; release spring from the finger's velocity | E |
| Return to a page | the pressed row's highlight fades out | 0.35 s ease-out, starting when the pop lands | E |
| Scroll root | large title fades out, inline title in | linear over 20 pt of scroll | A behaviour / E distance |
| Rubber band | list overscroll | iOS scroll physics (trunk) | — |
| Toggle | thumb slides + glass lens grows on press | response 0.30, damping 0.70 | IOS-REF §12.4 |
| Search focus | keyboard up; field rides up; X circle materialises; list fades | keyboard 0.38 / 1.0; fade 0.2 s; glass materialise LIQUID-GLASS §5.2 | M layout / E timing |
| Search cancel | reverse; field widens | spring 0.40 / 0.85 | E |
| Results update | rows appear / disappear | no animation (instant), E | E |
| Wi-Fi off | network sections collapse | 0.3 s ease-in-out | E |
| Network joins | row moves to the top card | spring 0.45 / 1.0 | E |
| Spinner | 8-spoke rotation | 1 turn / 1 s, stepped | E |
| Appearance switch | whole screen cross-fade | 0.3 s ease-in-out | E |
| Liquid Glass slider | preview glass re-tints | every frame, no lag | E |
| Picker check | check jumps to the new row | instant | E |
| Sheet (join / gallery) | slides up, presenting page scales 0.92 | IOS-REF §3.4 | M/E |
| Action sheet (Reset) | glass popover morphs from the row | LIQUID-GLASS §5.3 | A/E |
| Text Size step | preview text re-flows | instant per step | E |

---

## 5. Words (fixed strings, our English)

Root: Settings · Search · Galaxy S9 · Starbite OS · Airplane Mode · Wi-Fi · Bluetooth · Cellular · Battery · General ·
Accessibility · Appearance · Camera · Control Centre · Display & Brightness · Home Screen & App Library · Wallpaper ·
Notifications · Sounds & Haptics · Focus · Passcode · Emergency SOS · Privacy & Security · App Store · Linux · Apps ·
On · Off · Not Connected.
Search: No Results for “%s” · Check the spelling or try a new search.
Wi-Fi: Join Wi-Fi networks nearby and manage the ones you know. · My Networks · Other Networks · Other… · Ask to Join
Networks · Notify · Ask · Joining… · No Internet Connection · Enter the password for “%s” · Password · Join · Cancel ·
Incorrect Password · The password for “%s” is incorrect. · OK · Other Network · Name · Security · None · WPA2/WPA3
Personal · WPA3 Personal · The password stays on this phone. · Forget This Network · Forget Wi-Fi Network “%s”? · Your
phone will no longer join this network. · Forget · Auto-Join · Low Data Mode · Wi-Fi Address · IPv4 Address ·
Configure IP · Automatic · Manual · IP Address · Subnet Mask · Router · Renew Lease · DNS · Configure DNS · HTTP Proxy
· Configure Proxy · Test the Internet · Works · Failed.
Bluetooth: Connect keyboards and other accessories. · Now discoverable as “%s”. · My Devices · Other Devices ·
Connected · Not Connected · Connecting… · Device Type · Keyboard · Disconnect · Forget This Device · Forget Device.
Cellular: Cellular Data · Cellular Data Options · Network Selection · SIM PIN · Current Period · System Services ·
Show All · Reset Statistics · Last Reset: %s · Data Roaming · Voice & Data · Data Mode · Standard · Low Data Mode ·
Signal · Network.
Battery: Last Charged to %d% · Charging · %d% in %s · Battery Percentage · Low Power Mode · Last 2 % · Last 2 % at 3 %
· Battery Health · Normal · Service · Maximum Capacity · Cycle Count · Daily Usage · Average · Today · All Day · Daily
by %s · You're using about as much battery as usual by %s. · You're using more battery than usual by %s. · App and
System Activity · On screen: %s · Background: %s · View All Battery Usage.
General: About · Software Update · Startup · Phone Storage · Date & Time · Keyboard · Language & Region · Legal &
Regulatory · Transfer or Reset Phone · Reset · Shut Down · Name · Starbite OS Version · Model Name · Model Number ·
Notes · Photos · Videos · Applications · Capacity · Available · Bluetooth · Modem Firmware · Network · Carrier ·
Automatic Updates · Automatically Download · Update Now · Checking for Update… · Starbite OS is up to date · Preparing…
· Restart to Install · Try Again · 24-Hour Time · Set Automatically · Time Zone · Keyboards · All Keyboards ·
Auto-Capitalisation · Auto-Correction · Enable Caps Lock · Predictive Text · Smart Punctuation · Character Preview ·
“.” Shortcut · Preferred Languages · Region · Calendar · Gregorian · Temperature · Measurement System · First Day of
Week · Date Format · Number Format · Region Format Example · Reset All Settings · Reset Network Settings · Reset Home
Screen Layout · Reset Location & Privacy.
Accessibility: Vision · Physical and Motor · Display & Text Size · Motion · Touch · Vibration · Bold Text · Larger Text
· Larger Accessibility Sizes · Button Shapes · On/Off Labels · Reduce Transparency · Increase Contrast · Differentiate
Without Colour · Reduce White Point · Auto-Brightness · Turning off Auto-Brightness can use more battery. · Reduce
Bright Effects · Reduce Motion · Prefer Cross-Fade Transitions · Auto-Play Animated Images · Auto-Play Video Previews ·
Prefer Non-Blinking Cursor.
Appearance: Appearance · Light · Dark · Automatic · Options · Sunset to Sunrise · Custom Schedule · Light Appearance ·
Dark Appearance · Switches between light and dark at sunrise and sunset. · Liquid Glass · Default · Custom · More
Clear · More Tinted · Liquid Glass changes with what's under it. · Text Size · Apps that follow your text size will use
the size you choose below.
Display: Brightness · Night Shift · Scheduled · From · To · Colour Temperature · Less Warm · More Warm · Auto-Lock · 30
Seconds · 1 Minute · 2 Minutes · 3 Minutes · 4 Minutes · 5 Minutes · Never · With Low Power Mode on, Auto-Lock is 30
seconds. · Raise to Wake.
Wallpaper: CURRENT · Customise · Add New Wallpaper · Change your Wallpaper from the Lock Screen · Touch and hold the
Lock Screen to add, edit or switch wallpapers. · Photos · Colour · Add · Set as Wallpaper Pair · Customise Home Screen.
Sounds: Silent Mode · Ringtone and Alerts · Change with Buttons · Ringtone · Text Tone · Default Alerts · Haptics ·
Synchronised · Ringtones · Alarms and Timers · Alerts and System Sounds · Match Ringtone Volume · Keyboard Feedback ·
Sound · Haptic · Lock Sound · System Haptics · Play haptics for system controls and interactions. · Haptic Strength ·
Light · Medium · Strong.
Notifications: Display As · Count · Stack · List · Show Previews · Always · When Unlocked · Never · Notification Style ·
Banners, Sounds, Badges · Deliver Quietly · Allow Notifications · Alerts · Lock Screen · Notification Centre · Banners
· Banner Style · Temporary · Persistent · Sounds · Badges · Lock Screen Appearance · Notification Grouping · By App.
Passcode: Turn Passcode Off · Turn Passcode On · Change Passcode · Require Passcode · Immediately · Allow Access When
Locked · Today View and Search · Lock Screen Widgets · Return Missed Calls · Emergency mode and the torch always work
without the passcode.
Emergency: Call with Hold and Release · Call with 5 Button Presses · Calls the local emergency number after a
countdown. · Countdown Sound · Medical Card · Shown on the Lock Screen in an emergency. · Survival · Emergency Mode.
Privacy: Location Services · While Using · Analytics & Improvements · Starbite OS collects no analytics.
Focus: Do Not Disturb · Allow Calls From · Everyone · No One.
Linux: Running · Could not start · Starting… · Terminal · Memory.

---

## 6. Starbite today → gap

**Trunk 1c166b85** (`shell_settings.inc` + wifi / cell / update / startup / passcode files): root = profile card,
Wi-Fi / Bluetooth / Cellular / Battery; General / Display & Brightness / Wallpaper / Sounds & Haptics / Notifications;
Passcode / Emergency / Privacy / Startup; Linux Apps / App Store. Pages: General (About, Software Update, Date & Time,
Keyboard, Language & Region, Reset), Display (brightness slider, Auto-Brightness with lux footer, Text Size "Default"
(no page), Appearance "Dark" (value only), Auto-Lock), Wallpaper (one fixed), Sounds & Haptics (toggles, Haptic
Strength, volume, tones), Notifications (per app), Battery (level, temperature, health, history graph, Last 2 %),
Privacy (value rows), Emergency, Linux, Bluetooth (scan, keyboard pairing), Battery Health, Search (a page with the
field above the keyboard, ≤ 7 page-name results). Rows 52 (`LG_ROW`), toggles from the kit, back circle at x 20,
search capsule at y 716 (`SE_SB_Y`), a 44 X circle.

Ranked changes:
1. **Root to the iOS 27 order** (§1.1): add Airplane Mode, Accessibility, Appearance, Control Centre, Home Screen &
   App Library, Focus, Apps; rename Emergency → Emergency SOS, Privacy → Privacy & Security, Linux Apps → Linux; move
   Startup into General; Notifications after Wallpaper group. **M**
2. **Appearance page** with the Light / Dark cards, Automatic, Liquid Glass page (slider wired to LIQUID-GLASS §2.2),
   Text Size, Bold Text; remove "Appearance: Dark" from Display. Needs a **system light theme** to be real. **L**
3. **Search to the iOS 27 pattern**: field bottom at y 726..774 (trunk 716..764), 48 X circle (trunk 44), full index of
   rows (not just page names; trunk caps at 7), result rows 72 / 95 with icons + " → " breadcrumbs, stack rebuilt on
   tap, "No Results for" empty view. **M**
4. **Accessibility page** with Reduce Transparency / Increase Contrast / Reduce Motion / Bold Text / Reduce Bright
   Effects actually driving glass, motion and type; Auto-Brightness moved here. **M**
5. **Wi-Fi page structure**: header card, joined network in the switch's card with check + i, My / Other Networks with
   header spinner, Other…, info page (Forget, Auto-Join, Low Data, IPv4, Renew Lease), join as a **sheet** (trunk: page
   with its own keyboard). **M**
6. **Battery page**: Battery Percentage + Low Power Mode toggles, Health value "Normal", Daily Usage card (local:
   today's level chart). **M**
7. **Pushed-page geometry**: back circle x 16 (trunk 20), first header baseline 128, header 13 above card, footer
   spacing 23 / 24, chevron and toggle trailing insets 16 / 12, icon tile 30 at card + 15 with text at card + 57
   (trunk card + 59). **S**
8. **Row press + deselect-on-return fade**; picker pages that stay open on pick. **S**
9. **Sounds & Haptics** reorder (Silent Mode, Ringtone and Alerts, separate alarm / alert volumes, Keyboard Feedback
   page Sound / Haptic, Lock Sound, System Haptics); Calls / Messages vibrate rows → per-tone Haptics. **S**
10. **General**: About rows (counts, capacity, masked addresses), Legal & Regulatory, Transfer or Reset Phone →
    Reset action sheet with the five items, Shut Down. **S**
11. **Notifications** Display As cards + per-app Alerts thumbnails, Banner Style, Sounds, Badges. **M**
12. **Wallpaper** gallery (current pair pager, Customise, Add New Wallpaper sheet) — depends on lock-screen editor. **L**
13. **Text Size** (Dynamic Type scale through sbtk). **L**
14. Drop the "Turn On Cellular Data?" alert; add Cellular Data Options page. **S**
15. Emergency SOS page with Survival section; Medical Card row moved there. **S**

---

## 7. Still estimated

Measured from public art: the root list and its order (A), Appearance page layout, search results and bottom field,
Wallpaper and Battery pages, row / card / header / footer geometry. Still **E** (no public iOS 27 image found):
- Root page: first card top, device-row height and avatar size, whether Battery / Cellular show values, root icon
  tile colours and the Appearance icon's look.
- Search: field position while the keyboard is up (gap to the keyboard), the empty-query state, ranking, the
  target-row flash, search state kept after Back.
- Wi-Fi: header card text sizes, the joined-row layout inside the toggle card, join sheet layout, network info row
  order, alert words.
- Bluetooth, Cellular, General, About, Date & Time, Keyboard, Language & Region, Notifications, Passcode, Privacy,
  Emergency SOS, Accessibility pages: row **order** (rows named by Apple; order from iOS 18/26 memory).
- Liquid Glass page: preview layout, labels placement, detent snapping.
- Text Size slider step count; Display Zoom on S9.
- All motion timings (push 0.45 / 1.0, highlight fade 0.35 s, appearance cross-fade 0.3 s, list collapses).
- Toggle off-track colour (#C4C4C6 in the guide art vs #E9E9EA E) and the toggle's trailing inset (9-13 across art).
- Dark-mode values for everything measured (all public iOS 27 Settings art found is light).
