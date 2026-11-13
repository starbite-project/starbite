# Music, Files, Maps (iOS 27) — Starbite build spec

Track C, design-ref. v1 2026-11-13. Builders: implement straight from this file; shared components are in
[IOS-REFERENCE.md](../IOS-REFERENCE.md) (§ numbers below) and [LIQUID-GLASS.md](../LIQUID-GLASS.md) (§5 behaviour).

> **Platform decisions (Track A, 2026-11-13):**
> - **Music decoders:** **MP3 (minimp3) and FLAC (dr_flac) first**. AAC / M4A comes later (licensing): show such
>   files greyed, with "This format isn't supported yet."
> - **Later, not in this wave:**
>   - **exFAT write** on the card (card-write safety first). Build Files read-only, with write actions hidden.
>   - **Live GPS** (drivers / modem-core). Maps uses the saved fix and the states below.

**Sources** (study images in `build/design-ref/apps27/{music,files,maps}/`, git-ignored, never shipped):
- **UG27**: Apple iPhone User Guide, iOS 27 edition (support.apple.com/guide/iphone): Music "Use the music player
  controls" (iph676daac9b), "Play music" (iph0138fb328), "Queue up your music" (ipha4521ef7d), "Shuffle or repeat"
  (iphe22408524), "Show song credits and lyrics" (iphb9bf483aa); Files "Find and view files and folders" (iphe4bff8827),
  "Modify files and folders" (iphc61044c11); Maps "Set up Maps" (iph10d7bdf26), "Use Maps" (iph02f94fc1c),
  "Download offline maps" (iphcfb5f5bc6). Apple-drawn screen art from these pages, 490 x 1008 px, iPhone 16 class,
  screen x 22..467, y 20..985 = 393 x 852 pt at **1.1323 px/pt** (calibrated from the screen width and height).
  - Art used: `UG27-LIB` (Library tab + MiniPlayer + tab bar), `UG27-NP` (Now Playing), `UG27-Q` (queue),
    `UG27-FB` (Files Browse), `UG27-FI` (Files icon view), `UG27-MAP` (Maps 3D map with controls), `UG27-MS` (Maps
    search sheet with keyboard).
  - The art is clean vector drawing: ±1 pt on edges, ±2 pt on soft glass edges.
- **9TM-264**: 9to5Mac, "Apple Music in iOS 26.4 has new design for albums, playlists and more" (2026-02-16): flat
  playlist screenshot, iPhone 16 screen 412 x 895 px = 393 x 852 pt at **1.0483 px/pt**. It shows the album/playlist
  page and the minimised tab bar with the inline MiniPlayer. Its hero image (two phones, perspective) is used only for
  structure.
- **9TM-27**: 9to5Mac "Apple Music in iOS 27: five new features" (2026-09-18) with queue / landscape screenshots; 9to5Mac
  2026-06-19 (artist pages); MacStories "iOS and iPadOS 27: The MacStories Review", Music section (2026-09).
- Apple SwiftUI API (A): `tabViewBottomAccessory`, `tabBarMinimizeBehavior(.onScrollDown)`, the accessory placement
  `.expanded` / `.inline`, and the `.search` tab role (iOS 26 SDK).
- Starbite trunk 1c166b85: `starbite/sbos/ui/shell_media.inc` (Music, Files), `shell_nc.inc` `page_maps()`.

**Tags:**
- **A**: Apple stated it.
- **M (src)**: we measured it from the named image.
- **3P**: a reviewer said it.
- **E**: an estimate.

**Units:** pt on the S9 canvas 390 x 802.
- Conversion from a 393 x 852 iPhone: **top-anchored chrome y − 10** (status band 54 → 44); **bottom-anchored chrome
  y − 50**; fixed chrome keeps its size; full-width layouts scale by 390/393.
- Fonts are given by §14 style name or size / weight. Colours are §15 names.

**Policy (brief):**
- Our own art always: every glyph and the placeholder artwork are drawn by us.
- No iCloud, no Apple Account, no Apple Music catalogue, no AirPlay, no SharePlay.
- Each omission is marked **omit** or **local equivalent: …**.

---

# PART A — MUSIC

## A1. Information architecture

iOS 27 has the tabs Home, New, Radio, Library and a Search tab (A, UG27; 9TM-264 shows Search as a separate glass circle
in iOS 26.4+). Home, New and Radio are catalogue services.

**Starbite Music:** two tabs in the capsule plus the Search circle.
- **Home**: a **local equivalent** of Apple's Home.
  - Shelves: "Recently Played", "Recently Added", "Most Played" and "Shuffle All" (a pill button).
  - All from the play history we keep on the phone.
- **Library**: Apple's Library, 1:1.
- **Search** (role `.search`): searches the library only.

**New, Radio: omit.** The catalogue, Apple Music profile, Friends playlist, AutoMix (DJ transitions needs Apple
analysis), Sing, Lyrics translation, AirPlay, SharePlay and the Apple Music Classical links are all **omit**.

Navigation tree:
- Home
  - Recently Played (grid) → Album / Playlist page
  - Recently Added (grid) → Album page
  - Most Played (song list)
- Library
  - Pinned grid (top) → Album / Playlist / Artist page
  - **Playlists** → playlist list → Playlist page
    - New Playlist sheet
    - Playlist Folder
  - **Artists** → artist list → Artist page → Album page
  - **Albums** → 2-column album grid → Album page
  - **Songs** → song list
  - **Downloaded** → **local equivalent: "On This Phone"**: songs copied to the phone's own store (future), hidden while
    there is no internal store
  - Recently Added (2-column grid, at the bottom of Library)
- Search: the field, then results in sections (Top Results, Songs, Albums, Artists, Playlists); recent searches.
- Overlays, present from anywhere:
  - MiniPlayer (the tab bar's bottom accessory)
  - **Now Playing** (full screen), with the Lyrics and Queue modes inside it
  - Context menus
  - Add to Playlist sheet
  - Song info sheet (local)

## A2. Screens

### A2.1 Tab bar + MiniPlayer (bottom accessory) — every root screen

**Layout, expanded** (M UG27-LIB; S9 = iPhone y − 50):

| Element | iPhone 16 (M) | **S9** |
|---|---|---|
| Tab bar | glass capsule, x 19.4..372.6 (353 wide), y 768..828, **60 tall**, bottom 24 above the screen bottom | x 20..306 (our 2 tabs), y **718..778**; Search circle **60** at x 314..374 (8 gap, §10). §10's 62 is within ±2: **use 60** |
| MiniPlayer (accessory) | glass capsule **353 x 47**, y 713..760, **10.6 gap** above the tab bar, same width as the bar | x 20..370 (350 x 47), y **661..708** |
| MiniPlayer art | **30 x 30**, radius ~6 (E), x inset 17 from the capsule | 30 x 30 at x 37, centred |
| MiniPlayer words | 2 lines: title **15 / 600** label, artist **13 / 400** secondaryLabel (M size ±1), x art + 10 | same, ellipsized at the pause glyph − 12 |
| MiniPlayer buttons | pause/play glyph centre x 298, next centre x 343 (on 393), glyphs ~17 x 21 and 24 x 17, label colour | centres x **296 / 340**; hit 44 x 47 each |
| Selected tab | lighter glass pill ~84 x 56 behind the item; icon + label systemBlue (A 3P: iOS 27.2 uses blue for the active tab in Music) | as §10 |
| Tab labels | 10-11 / 500 (§10) | "Home", "Library" |

**Layout, minimised** (iOS 26 behaviour, A: `tabBarMinimizeBehavior(.onScrollDown)`; M 9TM-264):
- The tab bar shrinks to **one glass circle of the selected tab** (~46). The MiniPlayer slides **inline** between it and
  the Search circle (~46). All three sit on one row.
- iPhone: circle x 27..73, MiniPlayer x 82..319 (**237 x 46**), search x 323..368; row bottom 25 above the screen
  bottom (M 9TM-264, ±2).
- **S9:** row y **731..777**: tab circle 46 at x 25; MiniPlayer x 79..311 (232 x 46); Search circle x 319..365.
- Inline MiniPlayer content: art 28 (E), one line "Title" 13 / 600 + "Artist" 13 / 400 secondary stacked (M 9TM-264
  shows two lines at 13 / 11: use title 13 / 600, artist 12 / 400), and **play/pause only** (no next) (M 9TM-264).

**Content and states:**
- Nothing has played since boot and there is no last song: **no MiniPlayer**; the tab bar alone (A behaviour: the
  accessory appears once something is queued).
- Paused: the play glyph. Playing: the pause glyph.
- A file that can't be decoded: the MiniPlayer shows the title with the play glyph dimmed 30 %.
- The MiniPlayer persists across all Music screens. It hides only under Now Playing and full-screen sheets.

**Interactions:**
- Tap the MiniPlayer (outside the buttons) → Now Playing (A UG27).
- Tap play/pause → toggles, with a light haptic.
- Tap next → the next track.
- **Swipe left / right across the MiniPlayer's title** → next / previous track (A UG27 tip). The text slides with the
  finger. Past 35 % of the width (E) it commits: the old title slides out and the new one in, spring .35 / .85 (E).
- Long-press the MiniPlayer → its context menu (E, as iOS 26):
  1. Play Next
  2. Add to a Playlist…
  3. Favorite
  4. Go to Album
  5. Go to Artist
  6. Get Info (local)
  7. Stop (clears the MiniPlayer; local, matches iOS 27's "clear Now Playing", 3P IOS-REFERENCE §9.1)
- Scroll the content down → the bar minimises; scroll up or tap the minimised circle → it expands.
  - The trigger is a downward scroll of > 20 pt from rest (E).
  - The morph is LIQUID-GLASS §5.3 "tab bar minimise": spring .40 / 0.
  - The MiniPlayer's geometry and the tab capsule morph together. The capsule's other items fade over the first
    40 %.
- Tap a tab → that tab's root. Tap the current tab again → pop to the root, then scroll to the top (A HIG long-standing
  convention).
- Tap Search → the Search screen; its field focuses (§10 search).

**Haptics:** light impact on play/pause and next; selection on a tab change.

### A2.2 Library (root)

**Layout** (M UG27-LIB; S9 = iPhone y − 10):

| Element | iPhone (M) | **S9** |
|---|---|---|
| Header row | **no separate nav row**: the large title shares the row with the trailing buttons | |
| Title "Library" | Large Title **34 / 700**, x 20, baseline y 91 | x 20, **baseline 81** |
| Trailing group | one glass capsule **104 x 44** holding two glyphs, x 216..320, centre y 79.5: "add" (list-plus glyph → New Playlist) and "…" (More) | x 213..317, centre y **69.5** |
| Account circle | 46 avatar at the right edge | **omit** (no Apple Account). The capsule moves to x 266..370 |
| Pinned grid | 3 columns, tile **108.6** square, x 20 / 141 / 262 (gap 12.4), top y 122; label 13 / 400 label colour, centred, 1 line ellipsized, centre 12.5 below the tile; row pitch **148** | tile **108.7**, x 20 / 140.7 / 261.3, top **112**; at most 2 rows shown (E) |
| Category list | 5 rows **52** tall (separator pitch M), glyph centre x 31, text x **61** 20 / 400 (M ±1), chevron 13 / 600 tertiaryLabel at x 373 − 7; separators 0.5 separator colour from x 61 to 373 | the same at x 20..370; first row's centre y = pinned grid bottom + 26 |
| Rows (in order, A UG27) | Playlists, Artists, Albums, Songs, Downloaded | Playlists, Artists, Albums, Songs, (On This Phone) |
| "Recently Added" | Title 2 22 / 700 at x 20, 24 above, then a **2-column grid**: tile **170.5** (x 20 and 202.9, gap 12.4) | tile **169** (x 20 / 201, gap 12) |
| Album tile | square art, radius **8** (E), title 15 / 400 label + artist 15 / 400 secondaryLabel below (E sizes), 1 line each; tile pitch 169 + 54 | |
| Bottom inset | content scrolls under the tab bar; last item ends 16 above the MiniPlayer's top | |

**Edit pins / the list (E, as iOS 26):**
- "…" → menu: "Edit" (show/hide and reorder the categories with toggles; local), "Sort Pins".
- Edit mode: a list with drag handles and "–" / "+".

**States:**
- **Empty library** (no audio on the card and none on the phone):
  - Centred content-unavailable view: glyph (our music-note outline, 56) at y 300.
  - Title 22 / 700 **"Add Music to Your Library"**.
  - Body 17 / 400 secondary: **"Copy MP3, AAC, M4A, FLAC or WAV files to the microSD card, and they will appear
    here."**
  - The tab bar stays. The pinned grid and category list are hidden.
- **No card:** title **"No microSD Card"**, body **"Put a card with music in the phone."**
- **Card error:** **"Can't Read the Card"** + the platform's reason.
- **Scanning** (first index): the category list shows; a small spinner + **"Updating Library…"** 13 / 400 secondary
  under the title row. Rows fill in as found. Never block the screen.
- Permission denied: n/a.

**Interactions:**
- Tap a row → push the category (§16 push).
- Tap a pinned tile → its page.
- Long-press a pinned tile → menu: Play, Shuffle, Play Next, Add to Queue, Unpin, Go to Artist.
- Pull down: rubber band only (no refresh; local scan is automatic, E).

**Transitions:**
- Push / pop per §16 (0.45 / 1.0). The large title collapses into the inline 17 / 600 title on scroll (§10).
- Under the floating header buttons, use the scroll-edge effect (LIQUID-GLASS §5.4).

### A2.3 Playlists, Artists, Songs lists

**Playlists:**
- Inline nav title "Playlists"; large title on load (§10).
- Top: two glass capsules **"New Playlist"** and **"New Folder"** side by side (A UG27-LIB art alt text: "buttons to
  create a new playlist or create a new folder").
- Rows **64** (E): 56 art (radius 6), title 17 / 400, "n songs" 13 / 400 secondary, chevron.

**Artists:**
- Rows 64 (E): 48 round portrait.
  - We have no artist photos. Use a **monogram circle**: the initials, 20 / 600 white, on a colour hashed from the
    name (systemGray2..our palette).
- Name 17 / 400. Index scrubber on the right (A-Z, 11 / 600 systemBlue, E).

**Songs:**
- Header: a **Play** capsule and a **Shuffle** capsule, each 50% − 6 wide, 44 tall, glass, systemBlue glyph + label
  17 / 600 (E, as iOS 26 Songs).
- Then rows **52.5** (M 9TM-264):
  - Art **44** (radius 5, E) at x 18 → on S9 x 18.
  - Title **16 / 400** at x 75 (M ±1), artist **13 / 400** secondary below. Title centre −8, artist +10 from the
    row centre (M).
  - "…" glyph centre x 361 (M) → S9 358.
  - Separator from x 75 to 372.
- Sort ("…" in the nav bar): Title, Artist, Album, Recently Added (E).
- The A-Z index scrubber appears with > 40 songs (E).

**Now playing marker (all lists):**
- The playing row shows **animated level bars** (3 bars, 10 pt tall, systemBlue / the page's tint) in place of the
  track number or over the art (M 9TM-264 hero: bars before "Life Goes On").
- Paused: the bars freeze.

**Swipe actions (songs, E as iOS 26 §12.5):**
- Leading full swipe → **Play Next** (systemIndigo #6155F5 capsule).
- Trailing → **Add to Queue** (systemOrange), **Delete** (systemRed, local, needs card write: hidden while the card is
  read only).

### A2.4 Album / Playlist page (iOS 26.4+ design, kept in 27)

Structure: A 3P (9to5Mac 26.4, bzamayo: full-screen page whose background colour comes from the artwork; text only black
or white).

**Layout:**
- **Background:**
  - The whole page, including behind the track list, is one colour picked from the artwork.
  - Pick a colour that "pairs well": the dominant hue of the art at saturation clamp 0.25..0.75 and lightness
    0.20..0.85 (E).
  - Text is **white if L < 0.6, else black** (A 3P: only ever white or black).
  - Increase Contrast keeps the list on systemBackground (A 3P).
- **Top bar** (M 9TM-264):
  - Back **glass circle 44** at x 18..62, centre y 84 → **S9 y 74**.
  - Trailing glass capsule **95 x 44** at x 279..374 holding **share** + **"…"** → S9 x 276..371.
  - Local: share = the share sheet with our targets (SMS, Files). If none, show only "…" as a 44 circle.
  - The glass picks up the page colour.
- **Artwork:**
  - Square, centred, **~280** (E: 9TM-264 hero in perspective gives 0.71-0.73 of the width; a flat measurement is in
    §7). Radius 10 (E), soft shadow σ 20, y + 10, black 25 % (E).
  - S9: x 55..335, y **96..376**.
  - Playlists made by the user without art: a 2 x 2 mosaic of the first four album arts (E, iOS convention). If fewer,
    our gradient tile with a large music glyph.
- **Text block** (M 9TM-264; S9 = iPhone − 10 + art offset, laid out from the art's bottom):
  - Title **20 / 700**, centred, art bottom + 37 (centre); 2 lines max.
  - Subtitle (artist; for a playlist the owner, local: "My Playlist") **20 / 400**, centre 27 below the title.
    Tappable for an artist → Artist page.
  - Caption **12 / 600** at 60 % of the text colour: "Rock · 2025" for an album, "Updated 55m ago" for a playlist.
    From ID3 genre + year (local). Centre 22 below the subtitle.
- **Button row** (M 9TM-264), centre 43 below the caption:
  - **Shuffle** glass circle 44.
  - **Play** capsule **150 x 44**: filled in the text colour (black on light pages, white on dark), label "▶ Play"
    17 / 600 in the page colour.
  - Then a trailing 44 glass circle.
  - Gaps **13**; the row is 264 wide, centred.
  - Trailing circle per A 3P: "+" (add to library) on catalogue items, a checkmark when in the library, "↓" download.
    **Local equivalent:** the trailing circle is "…" (the page menu). Library items are always "in library".
- **Description** (playlists only): 15 / 400 at 70 % text colour, x 20, 2 lines + "MORE" 13 / 700 (M 9TM-264: x 20,
  y 560).
- **Track list:**
  - Rows **52.5** (M), separators 0.5 at the text colour 15 %, starting at x 20 (album) or 75 (playlist).
  - **Album rows:** the number 17 / 400 at 60 % at x 20 (right-aligned to x 34), title **16 / 400** at x 48,
    "…" at x 358.
  - **Playlist rows:** art 44 + title / artist (as Songs).
  - Explicit marker: omit (no data). Lossless / Dolby badges: omit (catalogue).
- **Footer:**
  - "12 songs, 47 minutes" 13 / 400 at 60 % (E words iOS-style), then the release date "3 March 2025" from tags
    (local).
  - Then "More by <Artist>" (a horizontal shelf of this artist's other local albums, E).
- **Scroll:**
  - The art and text scroll with the content (no parallax, E).
  - The title appears **inline** 17 / 600 centred in the nav row when the text block passes under it (cross-fade over
    20 pt, §10).
  - The scroll-edge effect at the top uses the page colour.

**States:**
- Loading the art from the card: the square shows our placeholder (gradient + glyph) and cross-fades to the art over
  0.2 s.
- A file that can't be played: the row's text at 40 %. A tap toasts **"This song can't be played."**

**Interactions:**
- Tap a row → plays from that track. The rest of the album becomes the queue.
- Tap Play → from track 1. Shuffle → shuffled, with shuffle on.
- Long-press a row → **song context menu** (below).
- "…" (top) → album menu: Play Next, Add to Queue, Add to a Playlist…, Shuffle, Pin, Go to Artist, Show in Files
  (local), Get Info (local).

**Song context menu** (A UG27 lists Play Next, Play After / Add to Queue, Add to a Playlist, Favorite; order E):
- Group 1: Play Next · Add to Queue
- Group 2: Add to a Playlist… · Favorite (star; "Undo Favorite" when set)
- Group 3: Go to Album · Go to Artist
- Group 4 (local): Show in Files · Get Info
- Group 5: Delete from Library (red; hidden while the card is read only)
- Omit: Share Song (no service), Create Station, Suggest Less, Download, Lyrics.
- The menu is IOS-REFERENCE §3.3: medium impact at the lift; the row lifts (scale 1.03) into a preview card (art 56 +
  title / artist).

**Haptics:** medium impact on a context-menu lift; light on Play / Shuffle.

### A2.5 Artist page (iOS 27 redesign, A 3P 9to5Mac / MacStories)
- **Header:**
  - The artist image extends and blends into a full-page colour.
  - **Local equivalent:** we have no artist images. Use the newest album's art, blurred σ 30, as the header, with the
    page colour taken from it.
- Under the name (Title 1 28 / 700 white, x 20): the buttons **Play** (capsule) and **Favourite** (star circle) "placed
  prominently" (A 3P). Artist info / bio: **omit** (no data).
- **Sections:** Top Songs (local: by play count, 4 rows), Albums (2-column grid), Singles & EPs (grid), Appears On.
- Featured release box: the newest local album as a 2-line card (E).

### A2.6 Search
- The field is the §10 bottom search.
  - Tapping the Search circle morphs the tab bar into a **full-width glass search capsule** at the bottom, which rises
    above the keyboard with a 44 "x" circle (A behaviour iOS 26; §10).
  - Placeholder **"Your Library"** (local; Apple's is "Artists, Songs, Lyrics, and More").
- Before typing:
  - "Recent Searches" (Title 3 20 / 600) with a "Clear" text button.
  - Rows 52 (E), up to 10.
  - No browse categories (catalogue: **omit**).
- Typing: live results with **no submit** needed, grouped: **Top Results** (1-3), Songs, Albums, Artists, Playlists.
  Each group's header is 20 / 600 with "See All" 17 / 400 systemBlue.
- Matching: case- and accent-insensitive prefix on words in the title / artist / album; within a group, ordered by
  play count, then name (E).
- **No results:** a centred glass-less view: magnifier glyph 48, **"No Results"** 22 / 700, **"Check the spelling or
  try a new search."** 17 / 400 secondary (iOS standard words).
- Scrolling the results dismisses the keyboard (interactive, §4).

### A2.7 Now Playing (full screen)

Opens from the MiniPlayer (A UG27). Measurements: M UG27-NP; S9 positions bottom-anchored (iPhone y − 50) for the
controls; the art band is laid out top-down.

**Background:**
- A full-screen field coloured from the artwork, A UG27 (Settings > Music > Animated Art). iOS 26 animates the
  colours slowly.
- **Ours:** take 4 colours from the art (k-means on a 32 x 32 copy).
- Draw a mesh gradient (4 blobs, σ 120) darkened so white text has ≥ 4.5:1 contrast.
- Animate the blob centres on 20-30 s loops (E). Pause when Reduce Motion is on, or after 10 min with the screen
  still, to save power (E).
- **Animated (video) album art: omit** (no motion art for local files). UG27-NP's art shows the full-bleed variant used
  for motion art. We use the square-art layout below.
- Text and glyphs are **white**; secondary text is white 60 % or the "vibrant" lighter tint of the art colour (M:
  UG27-NP's artist line is light blue #78A0C5 over the blue art).

| Element | iPhone (M UG27-NP) | **S9** |
|---|---|---|
| Grabber | capsule ~59 x 4 (drawing; E: §3.4's 36 x 5 is the standard), white 35 %, centre y 59 | **36 x 5**, centre y **51** |
| Artwork (square art, E) | — (UG27 shows motion art full-bleed) | **326 x 326** at x 32, y **66..392**; radius **12** (E); shadow σ 30, y + 16, black 35 % |
| Artwork while paused | — | scales to **0.86** about its centre (iOS long-standing, E) on spring .5 / .8; 1.0 while playing |
| Title | **20 / 600** white, x **32.7**, cap top y 462 | x 32, centre y **420** (art bottom + 28); 1 line, **marquee** when too long (scrolls after 2 s, 30 pt/s, E) |
| Artist (subtitle) | 20 / 400, art-tinted vibrant / white 60 %, below the title (pitch ~26) | centre y **446** |
| Favourite star | glyph ~21 x 20 pt, centre x 296 | centre x **294**, y 433; 44 hit |
| More "…" | centre x 344.5 | centre x **342**, y 433; a 30 x 30 glass circle at rest (E; UG27 shows a bare glyph) |
| Scrubber | track x **31.8..361**, **7** thick (M ±1.5), centre y 540.5; elapsed fill white 85 %, rest white 25 % | x 32..358, centre y **490.5** |
| Times | "1:21" left, "−2:06" right, **12 / 600** tabular, white 55 %, centre y 562.6 | centre y **512.6** |
| Transport | previous **38 x 21** at x 88.8; pause **28 x 39** at x 196.5; next 38 x 21 at x 303.4; centre y **628** | centres x **88 / 195 / 302**, y **578**; hit targets 88 x 72 (E) |
| Volume | speaker-low glyph x 33.6..40.6, track x 56.5..327 (**7** thick), speaker-high glyph at x 341, centre y 715.4 | track x 56..324, centre y **665.4** |
| Bottom row | Lyrics x 81.3, AirPlay x 196.5, Queue x ~311; glyphs ~20-22; centre y **774.5** (77.5 above the bottom) | Lyrics x **81**, centre **output** slot, Queue x **309**; y **724.5** |

**Bottom-row centre:**
- AirPlay is **omit**.
- **Local equivalent: an "Output" button** (speaker / wired headset / Bluetooth glyph) only when more than one output
  exists. Otherwise the slot is empty and Lyrics / Queue keep their x.

**Lyrics:**
- Shown only if the file has lyrics: synced LRC beside the file, or ID3 USLT / SYLT (local).
- When there are none, the button is **hidden**. iOS hides it too: "lyrics not available for all songs", A UG27.

**Scrubber detail (E, iOS 18-26 behaviour):**
- Touch-down on the track grows it **7 → 12** and widens it 4 % (spring .3 / .7). The times brighten to 85 %.
- Dragging moves the playhead. The time labels track it, and a label is pushed aside when the playhead nears it.
- Vertical finger distance slows the scrub: below 100 pt, ½ speed (A, long-standing iOS "hi-speed / half / fine"
  scrubbing; Apple no longer labels it, E).
- Release seeks. **Selection haptic** on the drag start; **rigid** at 0 and at the end.

**Volume slider:**
- Same component as the scrubber.
- The glyphs bounce (scale 1.15) when the track hits 0 / max, with **rigid** haptic (E).
- The hardware volume keys move it live (A UG27).

**Transport behaviour (A UG27):**
- **Previous:** goes to the start of the song; a tap again (within 3 s of the start, E) plays the previous song.
  Touch-and-hold rewinds.
- **Next:** skips. Touch-and-hold fast-forwards (2x, then 4x after 2 s, E).
- **Play / pause** glyphs morph (the bars split into the triangle) over 0.25 s (E).
- **Press feedback:** a white 12 % circle 64 grows behind the glyph and the glyph scales to 0.9, then springs back
  (E, iOS 16+).
- **Swipe left / right on the title** → next / previous (A UG27 tip).
- **Tap the artist name** → a menu: Go to Artist, Go to Album (A UG27: "choose an option").
- **Favourite star:** fills on tap, with a light haptic (A UG27). Local: stored in our library DB.

**More "…" menu** (A UG27 says "more options"; the items are E, iOS 26):
- Add to a Playlist…
- Play Next / Add to Queue (n/a for the current song: shows "Play Again", E)
- Go to Album
- Go to Artist
- Get Info (local: format, bitrate, sample rate, file path, size)
- Show in Files (local)
- Not in this menu: a sleep timer (iOS has it in Clock) and the equaliser (in Settings > Music on iOS)

**Dismiss:**
- Swipe down anywhere outside the sliders (A UG27's dismiss control is the grabber).
- The page follows the finger 1:1 with its corners rounding to 32 and scale toward 0.94.
- Commit at 25 % of the height or a velocity > 800 pt/s (E). It then **shrinks into the MiniPlayer** (the reverse
  zoom, below).
- Otherwise it springs back (.4 / .85).

**Open (matched-geometry zoom, A behaviour iOS 26; numbers E):**
- One spring (response **0.45**, damping **0.86**) drives:
  - the MiniPlayer capsule's rect → the full screen (corner 23.5 → 32)
  - the MiniPlayer art 30 → the 326 art (x, y, size, radius 6 → 12)
  - the background colour field's alpha 0 → 1
  - the MiniPlayer text cross-fading into the title / artist
- The controls below the art fade + rise 24 pt over the second half (delay 0.08 s).
- The page underneath scales to **0.94** and dims black 30 % (sheet-like, E).
- The tab bar fades with the MiniPlayer it leaves behind.

**Queue mode** (tap the Queue button; M UG27-Q; S9 = iPhone − 10 for the header):
- The art shrinks into a **header row**:
  - art **72 x 72** at x 32, y **74** (radius 8)
  - title 20 / 600 + artist 17 / 400 at x 116 (2 lines)
  - star at x 294, "…" at x 342
  - Morph: the 326 art flies to the 72 slot on spring .45 / .86, and the title / artist move with it.
- **Mode capsules row:** 4 glass capsules **72.5 x 39**, gap 12.3, x 32..358, y **160..199**:
  1. Shuffle
  2. Repeat (tap cycles Off → All → One; A UG27 "double-tap to repeat a single song" → repeat one)
  3. **AutoPlay ∞: omit** (catalogue)
  4. **AutoMix / Crossfade: local equivalent "Crossfade"** (we can fade locally)
  - So **our row is 3 capsules: Shuffle, Repeat, Crossfade**: each (326 − 2 x 12) / 3 = **100.7** wide (E).
  - States: inactive = glass, white 12 % (#41546B over the blue field, M); active = **white 55 %** fill
    (#A2B5CC, M) with the glyph in the background colour.
- **"Queue"** section header **17 / 700** white at x 32, centre y **230**; "Clear" 17 / 400 white 60 % right-aligned to
  x 358 (A UG27 "Tap Clear at the top of the queue").
  - iOS 27 also shows "Continue Playing" with a subtitle in place of "Queue" when the queue was built by AutoPlay
    (M 9TM-27). Ours: "Continue Playing" + subtitle "From <Album / Playlist>" when the queue is the rest of an album
    (E use).
- **Rows:**
  - Art **48** (radius 5), pitch **55.6** (M).
  - Title 17 / 400 white, artist 15 / 400 white 60 %.
  - **Reorder handle** (3 lines, 22 x 10, white 45 %) at x 358 − 11.
  - Drag the handle: the row lifts (scale 1.03, shadow), the others make room with a spring (.35 / .85), selection
    haptic at each slot (E).
  - Swipe left → red **Remove** (A UG27).
- **"Add Songs to Queue"** row:
  - A 48 glass square with "+" and the label 17 / 400 (A UG27, M UG27-Q).
  - It opens a Library picker sheet (§3.4 large) with checkmarks.
- At the bottom: "n songs" 13 / 400 white 50 % (A UG27: "see how many songs are in the queue").
- **History:** pull down in the queue to reveal **"History"** (recently played, newest last) with its own "Clear"
  (A UG27).
- The list scrolls between the capsules row and the scrubber. Its bottom fades out over 40 pt above the scrubber (M:
  UG27-Q shows the last row dissolving into the scrubber).
- The **Queue button** becomes a filled **white 25 % circle ~48** behind the glyph while in Queue mode (M UG27-Q,
  ±3). Tap it again → back to the art (the reverse morph).

**Lyrics mode** (E: same structure as Queue):
- The header row as in Queue mode. Lines are **28 / 700** white at x 32, wrapping at 326.
- The current line is 100 %; past lines 35 %; next lines 50 %.
- The view auto-scrolls the current line to y ~300 on spring .6 / 1.0. Each line lifts as it becomes current (scale
  1.0 → 1.03 to the left, E).
- Tap a line → seek to it.
- **Unsynced** lyrics: plain 22 / 600 scrolling text, no highlighting.
- After 3 s idle in Lyrics mode, the transport fades out (iOS behaviour, E). Any touch brings it back.

**Landscape** (A 3P iOS 27 9to5Mac, MacStories):
- Art left (height − 2 x 24 square) and the controls / lyrics at the right, side by side.
- **Starbite:** phase 2 (our shell has orientation only in the photo viewer). Listed in the gap.

**Haptics:**
- light impact: play / pause / next / previous / star
- selection: scrub start, reorder slots
- rigid: scrub / volume at the ends
- soft: dismiss commit (E)

**Sounds:** none (the song is the sound).

### A2.8 Lock screen Now Playing and Control Centre (cross-app)
- **Control Centre:** the Now Playing 2x2 module is IOS-REFERENCE §1 (M). It shows our art, title, artist, and
  previous / play / next.
  - The AirPlay circle is **omit**. Show the output glyph only with more than one output.
  - Tap the module (not a button) → expands to the large media view (E, iOS 18+: art 64, title, scrubber, transport,
    output).
  - Tap the art → opens Music Now Playing.
- **Lock screen** (E, iOS 26; IOS-REFERENCE §9.1):
  - A glass **media platter** replaces the notifications area's top, full width − 2 x 14.5 (= 361 on S9), radius ~23,
    height ~176:
    - art 52 (radius 8) + title 15 / 600 + artist 15 / 400 at 60 %
    - scrubber with times
    - transport (prev / play / next 32 glyphs, centres 90 apart)
    - an output glyph if more than one output
  - With **art that has a wide colour field**, iOS 26 can also fill the lock wallpaper with the art (enlarged, blurred)
    when the setting is on (3P). **Starbite: phase 2.**
  - iOS 27 lets the platter be **cleared**: swipe it left → "Clear" (3P IOS-REFERENCE §9.1). That ends the session
    (stops and clears the MiniPlayer).
  - The platter reacts to taps exactly like CC's.
  - Tap the art → unlock request → Music Now Playing.
- **Dynamic Island equivalent:** IOS-REFERENCE §11 proposal (art left, level bars right) while Music is in the
  background.

## A3. Cross-screen flows (Music)
1. **Play an album:** Library → Albums → tap a tile (push) → tap Play. The MiniPlayer materialises above the tab bar
   (LIQUID-GLASS §5.2, .35 / 0) → playback starts within 300 ms (E budget) → the row shows level bars.
2. **Open and dismiss Now Playing:** tap the MiniPlayer → zoom open (A2.7) → swipe down → shrink back into the
   MiniPlayer.
3. **Queue something next:** long-press a song → Play Next → a 1.5 s **HUD toast** "Playing Next" with the art (E, the
   iOS 26 banner style, top) → the queue shows it first.
4. **Make a playlist:** Library → Playlists → New Playlist.
   - Sheet (§3.4 large): title field "Playlist Title", art placeholder, Description field.
   - "Add Music" button → the picker. Done (blue glass) → the playlist page.
   - A UG27: playlists are created from Library > Playlists > the "+" button. Apple's AI "Playlist Playground" is
     **omit**.
5. **Search and play:** Search circle → type → tap a song → plays; the MiniPlayer updates; the keyboard stays until a
   scroll.
6. **Shuffle all:** Home → "Shuffle All", or Songs → Shuffle.

## A4. Motion table (Music)

| Trigger | What moves | Spring / duration |
|---|---|---|
| Scroll down > 20 pt | tab bar → selected-tab circle; MiniPlayer → inline | LIQUID-GLASS §5.3 tab-bar minimise .40 / 0 (E) |
| Scroll up / tap the circle | the reverse | .40 / 0 |
| First play | MiniPlayer materialises above the bar | .35 / 0 (§5.2) |
| Swipe on the MiniPlayer title | title slides; commit swaps | follow 1:1; .35 / .85 (E) |
| Tap the MiniPlayer | matched zoom into Now Playing | .45 / .86 (E); controls fade-rise 24 pt with 0.08 s delay |
| Swipe down on Now Playing | page follows, corners → 32, scale → 0.94; commit shrinks into the MiniPlayer | 1:1; .40 / .90 (E) |
| Play ↔ pause | art scale 1.0 ↔ 0.86 | .50 / .80 (E) |
| Play / pause glyph | morph | 0.25 s ease-in-out (E) |
| Transport press | 64 circle grows, glyph 0.9 | .25 / .6 (E) |
| Scrubber / volume touch | track 7 → 12 | .30 / .70 (E) |
| Queue / Lyrics toggle | art 326 ↔ 72 header; list fades in | .45 / .86 (E) |
| Queue reorder | rows make room | .35 / .85 (E) |
| Background mesh | blob drift | 20-30 s linear loops (E) |
| Push / pop | page slide | §16 .45 / 1.0 |
| Context menu | lift + menu morph | §3.3 / LIQUID-GLASS §5.3 .40 / .20 |
| Reduce Motion | all morphs → 0.25 s cross-fade; mesh still | A §16 |

## A5. Words (Music)
- Tabs: **Home, Library**; Search circle (accessibility "Search").
- Library rows: **Playlists, Artists, Albums, Songs, On This Phone**.
- Sections: **Recently Added, Recently Played, Most Played, Top Songs, Albums, Singles & EPs, Appears On, More by %s,
  Recent Searches, Top Results, Songs, Artists, Queue, Continue Playing, History**.
- Buttons and links: **Play, Shuffle, Shuffle All, Clear, See All, MORE, New Playlist, New Folder, Add Songs to Queue,
  Done, Cancel**.
- Menu items: **Play Next, Add to Queue, Add to a Playlist…, Favorite, Undo Favorite, Go to Album, Go to Artist, Show
  in Files, Get Info, Pin, Unpin, Delete from Library, Remove, Stop, Play Again, Repeat, Repeat One, Crossfade, Sort,
  Title, Artist, Album, Recently Added**.
- Toasts: **Playing Next, Added to Queue, Added to "%s"**.
- States:
  - **Add Music to Your Library** / **Copy MP3, AAC, M4A, FLAC or WAV files to the microSD card, and they will appear
    here.**
  - **No microSD Card** / **Put a card with music in the phone.**
  - **Can't Read the Card**
  - **Updating Library…**
  - **No Results** / **Check the spelling or try a new search.**
  - **This song can't be played.**
  - **Not Playing** (CC / lock when idle)
- Format strings:
  - **%d songs, %d minutes**
  - **%d song** / **%d songs**
  - **Updated %s ago**
  - **My Playlist**
  - **Unknown Artist**, **Unknown Album**
  - Placeholders: **Your Library** (search), **Playlist Title**, **Description**
- Get Info labels: **Format, Bit Rate, Sample Rate, Duration, Size, Location**.

---

# PART B — FILES

## B1. Information architecture

iOS 27 tabs are **Recents, Shared, Browse** (A UG27 art alt text, M UG27-FB: a 3-item glass tab bar, centred).
- **Shared: omit** (iCloud sharing).
- **Starbite tabs: Recents, Browse**, plus the search field that lives on each root (UG27-FB: the field is under the
  large title, not a tab).

Navigation tree:
- **Browse**
  - **Locations:**
    - **On My Phone** (local equivalent of "On My iPhone"; the phone's own user store, future, hidden until it exists)
    - **SD Card** (local equivalent of a removable location; label is the card's volume name if set, else "SD Card")
    - **Recently Deleted** (local, only for writable locations)
    - iCloud Drive: **omit**
  - **Favourites:** folders the user marked (local DB).
  - **Tags:** Red, Orange, Yellow, Green, Blue, Purple, Grey (A UG27-FB order) → a tag's file list.
  - → Folder → sub-folder … → File preview (Quick Look)
- **Recents:** recently opened files, newest first, with the search field and a filter row.
- Sheets:
  - More menu
  - New Folder (inline rename)
  - Get Info
  - Move / Copy picker
  - Share sheet (our targets)
  - Rename (inline)
  - Customise Folder (colour + icon, A UG27; phase 2)

**The card today is read only** (trunk shell_media.inc, kernel exFAT reader). Every write action below is specified
fully, but appears only when the location is writable (see B6).

## B2. Screens

### B2.1 Browse (root)

**Layout** (M UG27-FB; S9 = iPhone − 10):

| Element | iPhone (M) | **S9** |
|---|---|---|
| Nav row | **back / forward glass capsule 98 x 44** at x 14..112 (a split capsule: ‹ › glyphs, forward dimmed when unavailable), centre y 79.5; **"…" glass circle 46** at x 331..377 | capsule x 16..114, circle x 328..372 (44), centre y **69.5** |
| Large title "Browse" | 34 / 700, x 16, baseline y 148 | baseline **138** |
| Search field | glass-fill capsule **361 x 44**, x 16, top y 164; magnifier, "Search" 17 / 400 placeholder (#7C7C82), mic glyph at the right | x 16..374 (358), top **154** |
| Section header | **"Locations"** 20 / 600 label colour, x 32, centre y 240; a **collapse chevron** (13 / 600, tertiaryLabel) at x 377 − 16 | x 32, centre **230** |
| Section card | white card (secondarySystemGroupedBackground), x 16..377 (**361** wide), radius **26** (E: from the drawing ~24-28), rows **52** (M separator pitch), card top 12 below the header centre | x 16..374, radius 26 |
| Row | glyph 22-24 tinted systemBlue (cloud / phone / bin), centre x 42; title 17 / 400 at x **72**; chevron at the right; separator from x 72 to the card's right − 16 | same |
| Tags rows | coloured **17 pt dot** at x 42 (systemRed … systemGray), names | same |
| Background | systemGroupedBackground (#F2F2F7 light / #000 dark) | |
| Tab bar | glass capsule **275 x 61**, centred (x 59..334), 3 items; selected pill ~97 wide | **2 items: capsule ~190 x 61** centred (x 100..290, E), bottom **780** |

**Sections collapse** (A UG27-FB chevron): tap the header → its card folds (height → 0, rows fade). The chevron
rotates −90°. Spring .35 / .9 (E).

**"…" menu on Browse** (A UG27: "Tap …, tap Edit"): **Edit** (only on Browse).
- In Edit mode, rows show toggles (hide a location), "–" for tags and favourites, and reorder grips. "Done" replaces
  "…" (blue glass).

**States:**
- **No card:** the SD Card row shows a 13 / 400 secondary "Not Inserted" and is not tappable (E local).
- **Card error:** the row shows "Can't Read" + an alert on tap with the platform's reason.

### B2.2 Folder (location or folder)

**Layout:**
- **Nav row:** the same back / forward capsule and "…" circle. The **inline title** is the folder name, 17 / 600,
  centred, y 69.5 (M UG27-FI: "iCloud Drive" inline; no large title in a folder).
- The **search field** sits under the nav row: x 16..374, top **102** (M UG27-FI: iPhone top 112 → S9 102). It
  searches this folder and below.

**Icons view** (M UG27-FI; the default for a location on iOS):
- **3 columns**, cell centres x 69 / 197 / 324 on 393 → S9 **68 / 195 / 322** (pitch 127.4, scaled).
- Folder icon **~80 x 72**. File thumbnail: the file's preview, fit into an 80 x 80 box with a 0.5 pt border and
  radius 4 (E).
- Name 15 / 400 label, centred, 2 lines max, middle-ellipsized.
  - A **tag dot** (8 pt) before the name (M UG27-FI).
  - The cloud glyph after the name in the art: **omit**.
- Under the name: "6 items" (folder) or "1.2 MB" / date (file), 13 / 400 secondary.
- Row pitch: **~199 in the drawing** (icon top to icon top). That is generous art spacing: **use 168** (icon 80 +
  12 + name 2 x 18 + 4 + caption 16 + gap 20) until measured (§7).
- **Folder icon design:** our own drawing of the iOS 26 folder: a light-blue (systemCyan-ish #5AC8FA → #34AADC
  gradient) tabbed folder with a lighter top sheet.
  - A coloured folder takes its tag's colour.
  - A custom icon or emoji is centred on the front at 32 pt (A UG27 "Customize Folder & Tags": colour and icon or
    emoji).

**List view:**
- Rows **60** (E):
  - icon 40 (folders: our folder drawing; files: the thumbnail or a kind glyph) at x 16
  - name 17 / 400 at x 68
  - second line 13 / 400 secondary: "12 Nov 2026 at 14:03 · 1.2 MB" (date first, then size; for a folder,
    "6 items")
  - chevron for folders
- Folders show a **disclosure triangle** that expands them inline, indented 20 per level (A UG27: "tap > next to a
  folder to expand it").

**"…" menu in a folder** (A UG27-FI alt text; order as Apple lists it):
- Group 1: **Select** · **New Folder** · Scan Documents (**omit**: no scanner app yet) · Connect to Server (**omit**)
- Group 2 (inline row of two): **Icons** | **List** (the current one checked)
- Group 3: **Name** · **Kind** · **Date** · **Size** · **Tags** (the sort, one checked)
  - Tapping the checked one again flips the direction; a ↑ / ↓ glyph shows it (A behaviour long-standing iOS, E).
- Group 4: **View Options**: a submenu with "Group By" (None, Kind, Date, Size, Shared By (**omit**), Tags) and
  "Show All Extensions" (A long-standing; E for 27).
- The menu morphs out of the "…" circle (LIQUID-GLASS §5.3, .40 / .20).

**States (words):**
- **Empty folder:** **"No Items"** 17 / 600 secondary, centred in the content area (E: the iOS string; trunk says
  "Folder is Empty", so change it).
- **Loading** (card listing): the cells appear page by page. Before the first page, a centred spinner after 300 ms
  (E).
- **Can't read:** **"Can't Open This Folder"** + "The card did not answer." + a **Try Again** button (glass capsule).
- **Read-only location:** no "New Folder" or "Select → Delete / Move". The footer line 13 / 400 secondary centred
  under the last cell: **"%d items · Read Only"** (trunk wording kept).
  - iOS shows "%d items, %s available" on a writable location (A long-standing). Ours: **"%d items, %s available"** when
    writable.

**Interactions:**
- **Tap a folder** → push (§16).
- **Tap a file** → Quick Look preview (B2.4). A file kind we can't preview opens Quick Look's info card.
- **Long-press an item** → context menu (B2.3).
- **Swipe** (list view only, E iOS): trailing **Delete** (red); leading **Favourite** / tag (E).
- **Back / forward capsule:** back pops; forward re-pushes the last popped folder (A UG27-FB shows both). Long-press
  back → a menu of the parent path (E, iOS 26 folder history).
- **Pull down** in a folder → reveals the search field if it scrolled away (iOS long-standing, E).
- **Drag (E):**
  - Long-press and move > 10 pt → the item lifts into a drag preview (scale 1.05, shadow, count badge when several).
  - Drop on a folder → Move. The folder highlights with a systemBlue 2 pt outline and springs open after hovering
    1 s.
  - Writable locations only. Phase 2.

### B2.3 Context menu on a file / folder (A UG27 items; order as iOS 26, E)

Preview card at the top: a large thumbnail (Quick Look) for files, the folder icon for folders.

| Group | Items (file) | Folder differences |
|---|---|---|
| 1 | **Open** (local: our viewer) | — |
| 2 | **Copy** · **Duplicate** · **Move** · **Delete** (red) | same |
| 3 | **Rename** · **Compress** · **Get Info** | + **Customise Folder & Tags** (A UG27) |
| 4 | **Share…** · **Tags** · **Favourite** (folders only) | |
| 5 | **Show in Enclosing Folder** (in Recents / search results only) | |

- **Open With** (A UG27): **omit** (one viewer per kind on our phone).
- **Quick Look**: always (A UG27, "Preview with Quick Look").
- On a read-only location: Copy, Get Info, Share…, Tags, Favourite and Show in Enclosing Folder remain. Duplicate,
  Move, Delete, Rename and Compress are hidden (E local).
  - Tags and Favourite live in our DB, not on the card, so they work read-only.

**Actions:**
- **Rename:** the name becomes an inline text field with the name selected *without* the extension; the keyboard
  opens. Return commits. An invalid or duplicate name → an alert **"The name "%s" is already taken."** with OK (E iOS
  wording).
- **Duplicate:** creates "Name 2.ext" next to it. The new cell flashes (selection fill 0.4 s).
- **Compress:** makes "Name.zip" (a folder → "Name.zip"; several items → "Archive.zip"). A tap on a .zip expands it to
  a folder (A long-standing iOS).
- **Move:** a §3.4 large sheet: the Browse tree, "Copy" / "Move" in the trailing capsule, Cancel at the top left.
- **Delete:** no confirmation on a location with Recently Deleted (the item animates out: scale 0.8 + fade 0.25 s).
  Where there is no Recently Deleted, an alert **"Delete "%s"?" / "This item will be deleted immediately. You can't
  undo this action." / Delete (red) / Cancel** (E iOS wording).
- **Get Info:** a §3.4 sheet:
  - Preview at the top (160).
  - Name 20 / 600; kind and size 15 / 400.
  - A grouped card with Kind, Size, Where, Created, Modified, Last Opened.
  - Tags row.
  - "Show More" for image EXIF (dimensions, camera) and audio tags (duration, bit rate).
  - Words: **Information, Kind, Size, Where, Created, Modified, Last Opened, Dimensions, Duration, Tags, Show More,
    Show Less**.

### B2.4 Quick Look preview
- Full screen.
  - **Images:** black (dark) / white (light) ground. Pinch-zoom, double-tap zoom, swipe between the folder's images.
    This is our Photos viewer (IOS-REFERENCE §8).
  - **PDF / text / audio / video:** the page, with the content scrolling.
  - **Audio:** a centred art card + a play capsule (plays inline through Music's engine; not added to Music's queue).
- **Top bar:**
  - "Done" glass capsule at the left (iOS Quick Look uses Done, A long-standing).
  - The name 17 / 600 + "1 of 12" 13 / 400 secondary, centred, when several items.
  - Trailing: a "…" circle.
- **Bottom toolbar:** glass capsule(s): **Share**, **Markup (omit)**, **Info (i)**, **Delete** (writable only).
- Tap the content → toggles the chrome (fade 0.2 s).
- **Open:** a zoom from the tapped cell (matched geometry, .40 / .90, IOS-REFERENCE §8).
- **Close:** swipe down shrinks it back into the cell (follows the finger; background alpha by distance), or Done.
- **Unsupported kind:** a card with the kind's large icon (96), the name, "Kind — size", and **"No Preview
  Available"** (E words) + a Share button.

### B2.5 Recents
- Large title **"Recents"** (S9 baseline 138), the search field, then a **filter row** of glass chips (E, iOS 18+):
  "All", "Images", "Documents", "Audio", "Video".
- Grouped by **Today, Yesterday, Previous 7 Days, Previous 30 Days, <Month>** (E iOS grouping).
- Default view: Icons (3 columns), the same cells as a folder.
- Empty: **"No Recents"** 22 / 700 + **"Files you open will appear here."** 17 / 400 secondary, centred (E).

### B2.6 Select mode
- "…" → **Select**, or a two-finger swipe down a list (E).
- **Cells:**
  - A **24 circle** appears at each cell's bottom-right (icons) or leading edge (list, which slides content right
    40). Selected = systemBlue fill + white check.
  - Dragging a finger across cells selects them in sequence (A long-standing).
- **Nav:**
  - The title becomes **"%d Selected"** (or the folder name when 0).
  - The leading capsule shows **"Select All"**; the trailing button is **"Done"** (prominent blue glass).
- **Bottom toolbar** (replaces the tab bar, the same glass capsule height 50, E): **Share** · **Duplicate** · **Move** ·
  **Delete** (red) · **"…"** (Compress, Tags, Copy).
  - Disabled (35 %) when nothing is selected.
  - On a read-only location: Share and "…" (Copy, Tags) only.
- **Haptics:** selection on each toggle.

### B2.7 New Folder
- "…" → New Folder: a folder named **"untitled folder"** appears in the sort position, with its name in an inline
  field, selected; the keyboard is up.
- Return → commit and re-sort (the cell moves on spring .35 / .85, E).
- Name collision → "untitled folder 2". Writable locations only.

### B2.8 Search
- Tap the field:
  - The field rises into the nav position (the large title collapses), and **"Cancel"** appears at the right as a
    text button.
  - iOS 26 bottom-placed search is used by apps with tabs. In Files the field stays at the **top** (M UG27-FB/FI).
- **Scope chips** under the field (A UG27: "Recents" or a location or tag name): "Recents", "This Folder", "SD Card",
  "On My Phone", tags.
- Live results as the user types (name match, case- and accent-insensitive; kind words "pdf", "image", "audio" filter
  by kind, A UG27 "or document type"). Results are list view, with the path as the second line.
- "Search" on the keyboard hides it to show more results (A UG27). The "x" in the field clears (A UG27).
- **No results:** **"No Results for "%s""** 22 / 700 + **"Check the spelling or try a new search."** (E).

## B3. Cross-screen flows (Files)
1. **Open a photo from the card:** Browse → SD Card (push) → DCIM (push) → tap a photo → zoom into Quick Look → swipe
   to the next → swipe down to close.
2. **Find a file:** Browse → tap the search field → type → choose the "SD Card" scope → tap a result → Quick Look →
   "…" → Show in Enclosing Folder.
3. **Tidy a folder (writable):** "…" → Select → tap 3 files → Move → choose a folder in the sheet → Move. The cells
   fly out (scale 0.8 + fade) and the rest close the gaps (.35 / .85).
4. **Rename:** long-press → Rename → type → Return.
5. **Tag a folder:** long-press → Tags → Red. The red dot appears before the name. The folder appears under Browse >
   Tags > Red.
6. **Change the view:** "…" → List → the cells cross-fade into rows (0.25 s, E). The choice is remembered per folder
   (A long-standing iOS).

## B4. Motion table (Files)

| Trigger | What moves | Spring / duration |
|---|---|---|
| Folder push / pop | page slide | §16 .45 / 1.0 |
| "…" menu | morph out of the circle | LIQUID-GLASS §5.3 .40 / .20 |
| Section collapse | card height → 0, rows fade, chevron −90° | .35 / .90 (E) |
| Icons ↔ List | cross-fade | 0.25 s (E) |
| Select mode on | circles scale 0 → 1 (staggered 10 ms), list content slides 40, tab bar → toolbar morph | .35 / .85 (E) |
| Delete / move out | cell scale 0.8 + fade; others reflow | 0.25 s; .35 / .85 (E) |
| New folder / rename re-sort | cell moves | .35 / .85 (E) |
| Quick Look open / close | zoom from the cell; swipe-down shrink | .40 / .90 (IOS-REFERENCE §8) |
| Drag lift | scale 1.05 + shadow | .30 / .70 (E) |
| Search focus | title collapses, field rises, Cancel slides in | .40 / 1.0 (E) |

**Haptics:**
- medium impact: context-menu lift, drag lift
- selection: select toggles, sort change
- light impact: a drop
- notification .warning: a refused write

## B5. Words (Files)
- Tabs: **Recents, Browse**.
- Locations and sections: **Locations, Favourites, Tags, On My Phone, SD Card, Recently Deleted**.
- Tag names: **Red, Orange, Yellow, Green, Blue, Purple, Grey**.
- More menu: **Select, New Folder, Icons, List, Name, Kind, Date, Size, Tags, View Options, Group By, None, Show All
  Extensions, Edit, Done, Select All, Cancel**.
- Item menu: **Open, Copy, Duplicate, Move, Delete, Rename, Compress, Get Info, Share…, Favourite, Show in Enclosing
  Folder, Customise Folder & Tags, Quick Look**.
- Filters and groups: **All, Images, Documents, Audio, Video, Today, Yesterday, Previous 7 Days, Previous 30 Days**.
- Counts and names: **%d Selected**, **%d items**, **%d item**, **%d items, %s available**, **%d items · Read Only**,
  **untitled folder**, **Archive.zip**, **%d of %d**.
- States: **No Items**, **No Recents**, **Files you open will appear here.**, **No Results for "%s"**, **Check the
  spelling or try a new search.**, **No Preview Available**, **Not Inserted**, **Can't Open This Folder**, **The card
  did not answer.**, **Try Again**.
- Alerts: **The name "%s" is already taken.**, **Delete "%s"?**, **This item will be deleted immediately. You can't undo
  this action.**
- Get Info: **Information, Where, Created, Modified, Last Opened, Dimensions, Duration, Show More, Show Less**.
- Placeholder: **Search**.

---

# PART C — MAPS (shell + honest offline state)

## C1. Information architecture

iOS 27 Maps is one full-bleed map with floating glass controls and one **search sheet** with detents (A UG27, M
UG27-MAP / UG27-MS).
- Everything that draws streets, searches places, routes or shows place cards needs **map tiles and a place
  database**, which we don't have.
- **Future** (tile-dependent; no build now): map rendering, place search, Guides, place cards with hours / ratings,
  directions / navigation, Look Around, Visited Places, Offline Maps download, Map Modes other than our grid.

Starbite Maps now:
- **Map surface:** the **"Map Not Available"** surface (C2.1). A coordinate grid with our GPS position.
- **Sheet:** search field (coordinates only); **Your Location** card; **Pinned Places** (local equivalent of
  Favourites / Recents: coordinates the user saved); **Status** (what Maps needs, live: today's trunk content).
- **Controls:** Map Modes button (one mode now: "Grid"); Location button (centre / follow / heading); compass when
  rotated.
- Profile circle next to the search field: **omit** (no account). Its slot holds nothing; the field is full width.

## C2. Screens

### C2.1 Map surface (full screen, under everything)

**iOS (structure A, geometry M UG27-MAP):**
- The map fills the screen edge to edge, under the status bar. The status band has no background.

**Starbite "Map Not Available" surface (local, E):**
- Ground: a neutral map paper (light #F2EFE9, dark #1C1C1E).
- A **graticule**: lat / lon lines every 0.01° to 1° chosen to keep 60-120 pt spacing. Lines 0.5 pt at label colour
  10 %. Every 5th line is 1 pt with an 11 / 500 label at the top / left edge ("60.17° N", "24.94° E") in secondary.
- A **scale bar** at the top-left under the status band (44 + 8): "0 — 200 m", 11 / 600, a 2-pt bar. Visible while
  zooming, then it fades after 2 s (iOS behaviour, A long-standing).
- **The location puck:** a 22 systemBlue (#0088FF) dot with a 3 pt white ring and a soft shadow, centred on the fix.
  - An **accuracy circle** (systemBlue 12 % fill, 1 pt 30 % stroke) with the fix's radius in metres at the current
    scale.
  - With a heading: a 60° light-blue cone (iOS long-standing).
  - **Stale fix** (> 5 min) → a grey puck.
- **Gestures (all work on the grid):**
  - one-finger pan with momentum
  - pinch zoom (with rotation)
  - double-tap zoom in; two-finger tap zoom out
  - one-finger zoom (double-tap-hold then drag)
  - Zoom range: 1° … 0.0005° per 100 pt.
- **Long-press on the grid** → drops a red pin. The sheet goes to its medium detent with a **"Dropped Pin"** card
  (coordinates; Save to Pinned Places; Copy Coordinates; Share; Remove).
  - A UG27: touch and hold drops a pin and offers actions. "Download" there is **future**.
- **Watermark:** a centred caption under the puck only when no map is drawn, 13 / 400 secondary: **"Map not
  available — showing coordinates"**.
- **No fix:** the grid centres on the last fix (or 0°, 0° with no history) and the empty-state card shows (C2.3).

### C2.2 Floating controls (M UG27-MAP; S9 = iPhone − 50 for bottom-anchored)

| Control | iPhone (M) | **S9** |
|---|---|---|
| Map / location capsule | one vertical glass capsule **48.6 x 103** at the right edge, right inset **21**, bottom 17 above the sheet's top; top half = **Map Modes** button (folded-map glyph), bottom half = **Location** button (arrow glyph, systemBlue when following) | x 321..370, y **607..710** (sheet top 727) |
| Compass | glass circle **48** at left inset 22, shows the heading letter ("W") with a red north tick; visible only when the map is rotated; tap → north up (A long-standing) | x 22..70, y 607..655 |
| 2D / 3D | glass circle 48 under the compass (UG27-MAP "2D"); A UG27 says 3D needs the map | **omit** (no 3D: future) |
| Weather / AQI chip | iOS shows it at the bottom-left of the map (E) | **omit** (service) |

**Location button cycle** (A UG27): not following (outlined arrow) → tap → **follow** (filled arrow, the map centres
on the puck) → tap → **heading** (arrow with a cone; the map rotates with the compass) → tap → follow.
- Any pan exits follow (the glyph goes back to outlined).
- **Light impact** on each change (E).

**Map Modes** (A UG27: Explore, Driving, Transit, Satellite with a confirm):
- Tap → a §3.4 medium sheet **"Choose Map"**, 4 tiles 2 x 2 (160 x 100 each, radius 16).
- **Ours:** "Grid" (selected, checkmark) + Explore / Driving / Transit / Satellite drawn **disabled** with the caption
  **"Needs maps on this phone"**.
- The modes are listed so the shell matches iOS. They are **future**.

### C2.3 The search sheet (detents)

**Collapsed (rest; M UG27-MAP):**
- A floating glass card **337 x 64.5** at side insets **28**, bottom **28** above the screen bottom, radius ~32
  (capsule-like, E: concentric with the display).
- Grabber at its top: 36 x 5 (§3.4; the drawing shows ~49, E).
- Inside:
  - Search field (glass-fill capsule ~**36** tall).
  - Placeholder: iOS says "Apple Maps" (M UG27-MAP). **Ours: "Search Maps"**.
  - Magnifier glyph and mic glyph.
- **S9:** x 26..364 (338), y **710..774** (bottom 28 above 802). The field x 38..352, centre y 746.
  - The UG27 avatar circle (38) at the right: **omit**; the field takes its width.

**Medium (~52 % of the height, §3.4):**
- The sheet widens to inset 6 and rises to y 379 (§3.4 S9 numbers).
- Content (local):
  1. **Your Location** card:
     - a white grouped card, radius 26, rows 52
     - the coordinates **"60.1699° N, 24.9384° E"** 17 / 600
     - "± 8 m · 11 satellites · 2 min ago" 13 / 400 secondary
     - altitude "Altitude 24 m"
     - buttons in a row of 3 glass capsules 44 tall: **Copy**, **Share**, **Pin**
  2. **Pinned Places** (Title 3 20 / 600 header with "See All ›"):
     - rows 52: red pin glyph in a 32 circle, the name (or the coordinates), the distance from you "1.2 km"
       13 / 400 secondary
  3. **Status** card (trunk content, kept): Wi-Fi, Cellular Data, Location rows with their states.
- **Large (S9 y 52):** the same content scrolls; the map behind dims ≈ black 25 % and the map stays visible at the top
  edge (A UG27-MS: the sheet top at y 66 on iPhone → 56 on S9 under the island-less status band; use §3.4's 52).

**Search focused** (M UG27-MS):
- The sheet goes **large**.
- The field: **306 x 43** at x 16 (S9: x 16..322); a **44 "x" glass circle** at x 336 closes the search.
- Under the field, a horizontal row of **category chips** (glass capsules 44 tall, emoji-style icon + 17 / 600 name;
  "Coffee Shops", "Grocery Stores", …) → **future**. In our shell the chip row is **hidden**.
- **Our search** accepts coordinates in any common form:
  - "60.17, 24.94"
  - "60°10′12″N 24°56′18″E"
  - "N60.17 E24.94"
  - and a pinned place's name
  - A valid coordinate shows one result row: "Go to 60.1700° N, 24.9400° E" → the map flies there and drops a pin.
- Anything else:
  - Centred **"No Results"** 22 / 700.
  - **"Maps can only find coordinates and your pinned places on this phone."** 15 / 400 secondary.
- Recents: the last 10 searches (A UG27-MS "Recents" header with "›", card rows with a magnifier glyph and "…"):
  rows 52, the "…" menu with Remove.

**Detent behaviour:**
- Drag the grabber or the content at its top to move between detents (follow the finger). Release springs to the
  nearest (.40 / .85, §3.4).
- Tapping the field from collapsed goes straight to large with the keyboard. Cancel / "x" returns to collapsed.
- The floating controls (C2.2) ride above the sheet's top edge until it passes y 400, then they fade (iOS behaviour,
  E).
- **Soft** haptic on landing at a detent (§3.4 E).

### C2.4 Location permission and states

| State | What shows | Words |
|---|---|---|
| Location Services off (Settings) | the sheet's top card is a warning card with a "Turn On in Settings" glass capsule; no puck | **"Location Services Are Off"** / **"Turn on Location Services in Settings to show where you are."** |
| No fix yet (GPS on, searching) | the puck replaced by a pulsing grey ring at the last fix | **"Finding Your Location…"** |
| GPS not available on Starbite (trunk: only Android's saved fix) | the Your Location card shows the saved fix with its age | **"Last Known Location"** + "From Android, 12 Nov 2026 at 18:04 UTC" (trunk wording kept) |
| No fix ever | the grid at 0°, 0°, no puck | **"Location Not Available"** / **"Maps will show your coordinates when the GPS has a fix."** |
| Offline | nothing changes (we never need the network today); the Status card shows Wi-Fi / Cellular | — |

**First open:** iOS asks for permission with a system alert ("Allow "Maps" to use your location?" with **Allow Once /
Allow While Using App / Don't Allow**, A long-standing). Ours: the same alert (§3.1) the first time, stored in
Settings > Privacy.

## C3. Cross-screen flows (Maps)
1. **Where am I:** open Maps → the map flies to the puck (zoom-in 0.8 s, E) → the sheet shows Your Location → Copy →
   toast **"Copied"**.
2. **Save a spot:** long-press the grid → pin drops (light impact) → sheet medium → "Pin" → name field (alert with a
   text field, §3.1) → Save → it appears under Pinned Places.
3. **Go to coordinates:** tap the field → type "60.17, 24.94" → tap the result → the sheet collapses, the map flies
   there (.6 / 1.0 for the camera, E), and a pin drops.
4. **Share:** Your Location → Share → our share sheet → SMS with the text **"I'm at 60.1699° N, 24.9384° E (± 8 m)"**
   (local; the user sends it).

## C4. Motion table (Maps)

| Trigger | What moves | Spring / duration |
|---|---|---|
| Open | the map camera flies from the last view to the puck | 0.8 s ease-in-out (E) |
| Pan / pinch | the camera follows 1:1, then momentum decays (deceleration 0.998 / ms, iOS scroll) | E |
| Sheet detents | sheet y + width (inset 28 → 6) + radius | .40 / .85 (§3.4) |
| Field focus | sheet → large, keyboard up | .45 / 1.0; keyboard §4 |
| Pin drop | the pin falls 40 pt with a bounce, then the shadow appears | .35 / .55 (E) |
| Location button state | glyph cross-morph | 0.2 s (E) |
| Compass show / hide | materialise (LIQUID-GLASS §5.2) | .35 / 0 |
| Puck | the accuracy circle eases to new radii; pulse when searching (1.2 s loop) | 0.3 s ease; loop E |
| Controls under a rising sheet | fade | 0.2 s |

**Haptics:**
- light impact: pin drop, location-mode change
- soft: detents
- selection: Map Modes choice

## C5. Words (Maps)
- **Search Maps**
- **Your Location, Pinned Places, Status, See All, Recents**
- **Copy, Share, Pin, Remove, Copied, Dropped Pin, Save**
- **Choose Map, Grid, Explore, Driving, Transit, Satellite, Needs maps on this phone**
- **Map not available — showing coordinates**
- **No Results**, **Maps can only find coordinates and your pinned places on this phone.**
- **Go to %s**
- **Location Services Are Off**, **Turn on Location Services in Settings to show where you are.**, **Turn On in
  Settings**
- **Finding Your Location…**
- **Last Known Location**, **From Android, %s UTC**
- **Location Not Available**, **Maps will show your coordinates when the GPS has a fix.**
- **± %d m · %d satellites · %s ago**, **Altitude %d m**
- **Wi-Fi, Cellular Data, Location, Connected, Not Connected, Off, Connecting…**
- **I'm at %s (± %d m)**
- Permission alert: **Allow "Maps" to use your location?**, **Allow Once, Allow While Using App, Don't Allow**

---

## 6. Starbite today → gap

**Trunk 1c166b85 has:**
- `shell_media.inc`: Music and Files in one page engine. A large-title header; 61-pt rows drawn into tiles; the
  microSD card's exFAT partition, **read only**.
  - Music: a flat song list (ID3 title / artist, a grey square with a pink note as art) and a basic Now Playing.
    - The Now Playing has a back link "Library", 135-pt art at half resolution, 22 / 700 title, a 6-pt scrubber, and
      triangle-drawn transport.
    - Playback is WAV only (`wp_*`). MP3 / AAC / FLAC say "can't be played yet".
    - No MiniPlayer, no queue, no tabs, no album / artist grouping, no colour from art.
  - Files: one folder list (40-pt icons, name, size · date, "Read only" footer), push into folders, photos open in the
    viewer, other files toast. No Browse, Recents, tags, icon view, menus, Quick Look for non-images, or writes.
- `shell_nc.inc` `page_maps()`: an honest static page ("Maps Needs the Internet and Location"), a status card (Wi-Fi /
  Cellular / Location) and the fix from Android's saved record, refreshed once a minute. No map surface, sheet or
  controls.

**Ranked changes** (S < 1 day, M 1-3 days, L > 3 days):

| # | Change | Size |
|---|---|---|
| 1 | **Music decoders: MP3 + AAC/M4A + FLAC** (blocks everything that "plays"; everything else is chrome without it) | L |
| 2 | **Music tab bar + MiniPlayer accessory** with the minimise-on-scroll morph (A2.1); a shared `tk` component reused by Files / Podcasts later | M |
| 3 | **Now Playing rebuild** (A2.7): art-colour mesh background, 326 art with play/pause scaling, scrubber / volume components with the grow-on-touch, the zoom open / swipe-down dismiss from the MiniPlayer | L |
| 4 | **Library model:** group by album / artist from tags + embedded art (ID3 APIC, MP4 covr, FLAC PICTURE) decoded once to a 326 / 170 / 44 thumbnail cache on the card's side; play counts + favourites DB | L |
| 5 | **Library / Albums grid / Album page** (A2.2-A2.4) incl. the page colour from art and the 150 x 44 Play capsule | M |
| 6 | **Queue mode** + Shuffle / Repeat / Crossfade + History (A2.7) | M |
| 7 | **CC Now Playing module and lock-screen platter** wired to our player (A2.8; IOS-REFERENCE §1) | M |
| 8 | **Files Browse + Recents tabs**, grouped cards, collapsible sections, back / forward capsule, the "…" menu with view / sort (B2.1-B2.2) | M |
| 9 | **Files icons view** (3 columns, our folder drawing, thumbnails) + list view with disclosure | M |
| 10 | **Quick Look** for images (reuse the viewer), text, PDF (phase 2), audio inline; unsupported card (B2.4) | M (PDF L) |
| 11 | **Files context menu + Get Info + select mode + search** (read-only subset now) | M |
| 12 | **Card write support** (exFAT create / rename / delete / move, with power-loss safety) → enables New Folder, Rename, Duplicate, Move, Delete, Compress, Recently Deleted, Music "Delete from Library" | L (kernel) |
| 13 | **Maps shell:** the grid surface with gestures, the puck, the floating controls, the detent search sheet with coordinate search, Pinned Places, permission states (C2) | M |
| 14 | **Starbite-side GNSS** (live fixes instead of Android's saved one): the shell is ready for it via `sh_gnss` | L (driver) |
| 15 | Search in Music (A2.6) | S |
| 16 | Lyrics mode (LRC / USLT) | S |
| 17 | Landscape Now Playing; drag-and-drop in Files; folder customisation; Output button | M each, phase 2 |

## 7. Still estimated

No attended iOS 27 capture session will happen (brief change 2026-11-13). These values are still **E**, with the
public source most likely to turn each into M. Look for a flat (non-perspective) screenshot in the next review pass.
- **Music Now Playing, square-art layout:** art size / top / radius, art scale when paused. UG27 only shows motion
  art. Source: MacStories / 9to5Mac Now Playing screenshots of a non-animated album; Apple Newsroom iOS 27 gallery.
- **Album page art size** (~280) and radius: 9TM-264's album phone is in perspective. Source: a flat iOS 26.4 / 27
  album screenshot (9to5Mac's 26.4 feature article, the 2026-03-25 version).
- **All springs** in the motion tables (MiniPlayer zoom, minimise, scrubber grow, queue morph). Source: WWDC25/26
  session videos frame-stepped (public); Apple's SwiftUI defaults (A) where the API is known.
- **Lyrics mode** typography and scroll position. Source: 9TM-27 landscape-lyrics image (landscape only) + an iOS 27.3
  "improved lyrics view" article (9to5Mac 2026-11-04).
- **Context-menu item orders** (songs, albums, files). Source: UG27 text gives the items, not the order; review
  screenshots of a long-press.
- **Lock-screen media platter** geometry. Source: the Newsroom WWDC25/26 lock-screen renders with a playing song;
  UG27 "Control audio from the Lock Screen" page art.
- **Files list-view row height** (60), icons-view row pitch (168 vs the drawing's 199), Get Info sheet layout,
  Recents grouping and filter chips, Quick Look toolbar. Source: UG27 Files pages' other art (Recents, Get Info,
  Quick Look); MacStories iOS 26 Files section.
- **Files and Maps glass radii** (cards ~26, Maps collapsed card ~32). Source: HIG iOS 26 template art.
- **Maps:** the grabber width (36 vs the drawing's 49), the medium detent content (local), the controls' fade point.
  The local "grid" surface is our own design (no iOS equivalent to measure).
- **Inline (minimised) MiniPlayer type sizes** (13 / 12) and art size (28): 9TM-264 is small at 1.05 px/pt.
  Source: a higher-resolution flat iOS 26/27 screenshot.
