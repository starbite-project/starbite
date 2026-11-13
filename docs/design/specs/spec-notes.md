# Notes (iOS 27) — Starbite build spec

Track C, design-ref. v1 2026-11-13. Shared components: [IOS-REFERENCE.md](../IOS-REFERENCE.md) (§3 alerts / action
sheets / menus / sheets, §4 keyboard, §6 list geometry, §10 bars and bottom search, §12 controls, §14 type, §15
colour, §16 motion, §17 haptics), [LIQUID-GLASS.md](../LIQUID-GLASS.md) §5, and [spec-settings.md](spec-settings.md)
§2.0 (row / card / header geometry used here too).

> **Platform: storage (in the trunk, 9910ebf9).** Each note is one document in the shell's document store
> (shell.h: `sh_doc_put / get / count / id_at / new_id / usage`, collection **`SH_DOC_NOTES`**).
> - A document is the app's own bytes, **up to 32 KiB**, under a stable id. It is saved with the settings and
>   survives restarts.
> - The whole Notes collection has a **96 KiB budget**; Recently Deleted notes count towards it.
> - `sh_doc_usage` gives used / budget. Build for "plenty, with a full state":
>   - no fixed note count;
>   - a note's text stops growing at 32 KiB, with a footer "This note is full." (13 / 400, secondaryLabel);
>   - when the collection is full, a **"Storage Full"** alert: "Delete some notes or empty Recently Deleted to make
>     room." (OK);
>   - an edit that can't be saved keeps its text on screen with a footer "Not saved: storage full" (13 / 400,
>     systemRed);
>   - Recently Deleted notes are purged after 30 days.
> - Our own format inside a document: the title line, the body with style runs and checklist states, flags (pinned,
>   deleted + time), the folder id, and created / modified times.

**Sources**
- Apple iPhone User Guide, **iOS 27 edition** (fetched 2026-11-13): "Get started with Notes", "Create and format notes"
  (two editor screens of art), "Search notes", "Organize in folders", "Delete and recover notes", "Change the Notes
  view", "Change Notes settings". `https://support.apple.com/guide/iphone/<page-id>/27/ios/27`. Art 490 x 1008 px,
  screen x 22..467 = 393 pt, **1.132 px/pt**.
- MacRumors iOS 26 Notes guide, two iOS 26 screenshots (2000 x 1125, an iPhone Pro Max, screen 481 px = 440 pt,
  **1.093 px/pt**): the Folders screen and an empty note with the keyboard and the format bar.
- 9to5Mac / MacRumors / Tom's Guide on iOS 27 Notes: section links, divider lines, Copy as Markdown (3P).
- Starbite trunk 1c166b85: `shell_lock.c` (Notes: `page_notes`, `build_note`, `notes_tap`, `notes_hold`), `shell_kb.inc`.
- Study images: `build/design-ref/apps27/notes/` (git-ignored).

**Tags**: **A** Apple stated · **M (from X)** measured by us · **3P** reviewer · **E** estimate. iOS 27 wins.
pt are S9 (390 x 802) unless marked "iPhone".

**Scope decisions**: no iCloud, no Apple Account, no sharing / collaboration, no Quick Notes, no Math Notes, no
handwriting / Markup, no Apple Intelligence (Writing Tools, Image Playground), no locked notes, no audio recording or
document scanning. One account: **"On My Phone"** (local equivalent of iOS's "On My iPhone" account).

**Notes accent**: the yellow of Notes' tint, **#E9AD12** (M (from MacRumors editor shot: the Done circle fill
(234,171,18) / (225,174,3), JPEG ±6)); dark mode the same hue a step brighter #F5B915 (E). Used for: back chevron,
toolbar glyphs, folder glyphs, checklist checks, the Done circle, links, the caret, search hit highlight.

---

## 1. Information architecture

One navigation stack, three levels, no tab bar:
```
Folders (root)
├─ [smart] Recently Deleted (only when it has notes — A)
├─ On My Phone (section, collapsible)
│   ├─ Notes (the default folder; can't be deleted or renamed — E)
│   └─ <user folders>, nested by drag (subfolders, A)
└─ Notes list (one folder)  ── Folder Actions menu, Sort By, Group By Date, View as Gallery
    └─ Note editor  ── Aa format panel, checklist, table, attachments, Find in Note, Move, Delete
Settings > Apps > Notes (per-app settings, §2.8)
```
iOS opens Notes **where you left it** (last note or list) (A behaviour, long-standing; E for 27). Cold first start:
the Notes list of the "Notes" folder (E: iOS opens the default folder when only one account exists), with Back
showing Folders.

Omitted rows on the folder list: Quick Notes, Shared, Math Notes (smart folders), iCloud section, Tags section
(**omit v1**; tags come with "Auto Convert to Tag", later).

---

## 2. Screens

### 2.1 Folders (root) — M (from MacRumors Folders shot) unless tagged

**Layout** (S9)
1. Nav row y 44..88. Trailing, two separate glass shapes (M: not one capsule): **New Folder** 44 glass circle
   (folder-with-plus glyph, Notes yellow) at x 263..307, and **"Edit"** 44-tall glass capsule (17 / 500 label colour
   text, 14 h padding → ~52 wide) at x 318..370. Gap 11, right inset 20 (M: 21 / 11 on a 440-pt phone).
2. Large title **"Folders"** 34 / 700 at x 20, baseline 124.
3. First card top **141** (M: 17 below the title's baseline).
4. **"On My Phone"** section header: **20 / 700 label colour** (not secondary, not 15 / 600 — M: cap height 16.5 pt on
   the shot), x 20 (aligned with the large title, M), baseline 17 above its card (M), with a Notes-yellow chevron.down
   (15 / 600) at the trailing edge x 370 − 15: tap the header collapses / expands the card (chevron rotates −90°,
   spring 0.35 / 0.85, E). 34 from the previous card's bottom to the header's cap top (M).
   - With only one section and no smart folders, iOS still shows the account header (E). Keep it.
5. Card x 16 w 358 (settings §2.0). Folder rows **52** (M 51.9): folder glyph (outline folder, Notes yellow, ~24 x 20)
   centred at card + 30; name 17 / 400 at **card + 57** (M: card + 59 on 440, E scaled); note count 17 / 400
   secondaryLabel + chevron 13 / 600 tertiary at the trailing inset 16; separators from the text x to card right − 18
   (M: they stop short of the card edge).
6. Recently Deleted row (trash glyph, yellow): the **last row inside the account's card** (E).
7. Bottom bar (M, same as Settings search): **Search** glass capsule x 20..310, y 726..774 (48 tall), magnifier 17 pt
   at +14, placeholder "Search" 17 / 400, trailing mic glyph → **omit** (no dictation); **Compose** 48 glass circle
   x 322..370 (square.and.pencil glyph 22 pt, Notes yellow) (M: capsule 48, circle 48, 27.4 above the screen bottom,
   13 gap; side insets 28 on a 55-corner phone → 20 on ours).

**Content and states**
- Empty (no user folders): the card shows "Notes 0". Never an empty screen (E).
- **Edit** mode: "Edit" becomes a prominent yellow **"Done"** capsule (white 17 / 600); folder rows show a red
  minus circle (22) at the leading edge (delete) and a grab handle (≡, tertiary) at the trailing edge (reorder); the
  "Notes" folder has neither (E). Compose and Search dim to 40 % and don't respond (E).
- Folder count updates live.

**Interactions**
- Tap a folder → push its Notes list.
- **New Folder** → a sheet (IOS-REF §3.4, medium detent, keyboard up): leading 44 "xmark" circle (Cancel), title
  "New Folder" 17 / 600, trailing prominent yellow capsule **"Done"** (disabled while the name is empty); one card with
  a text field, placeholder "Name", autofocus. Duplicate name → alert "Name Taken" / "Please choose a different name."
  / OK (E words). Smart folders → omit. (A: tap New Folder, enter a name; E: sheet form.)
- **Swipe left** on a folder (E): **Delete** (red #FF383C, trash glyph) and **Move** (purple #CB30E0, folder glyph)
  capsules; full swipe = Delete. Delete → action sheet / alert "Delete Folder?" with message "Its notes go to Recently
  Deleted." (ours) and **Delete Folder** (destructive) / Cancel (E). A: notes are recoverable from Recently Deleted.
- **Long-press** a folder (A: Rename, Delete): context menu (IOS-REF §3.3, medium impact at lift): **Rename**
  (pencil), **Move** (folder), **Delete** (trash, red) (E order). Rename → the New Folder sheet titled "Rename Folder"
  with the name selected.
- **Drag** a folder (A): long-press then move → drop onto another folder = subfolder (it indents 20 under it with a
  disclosure chevron); drop between rows = reorder. Lift haptic medium; drop haptic light (E).
- Pull down: rubber band only.
- Compose → creates a note in the "Notes" folder (or the last used folder, E) and pushes the editor directly (the list
  is inserted under it in the stack).

### 2.2 Notes list (one folder)

No public iOS 27 art of this screen was found; structure is **A** (guide behaviour), geometry **E** from iOS 18/26
with the measured Settings / Folders values.

**Layout** (S9)
1. Nav row: back 44 glass circle (Notes-yellow chevron) at x 16; trailing **Folder Actions** 44 glass circle
   ("ellipsis", Notes yellow) at x 326..370 (A: the Folder Actions button).
2. Large title = folder name, 34 / 700, x 20, baseline 124; under it a subtitle **"12 Notes"** 15 / 400 secondary,
   baseline 146 (E: iOS 26 navigation subtitle; iOS ≤ 18 put the count in the bottom bar — trunk does that).
3. Sections (A: grouped by date by default; Pinned on top):
   - **"Pinned"** (header with a pin glyph? E: text only), then **Today**, **Yesterday**, **Previous 7 Days**,
     **Previous 30 Days**, then month names of this year ("October"), then years ("2025") (E: iOS 14+ groups).
   - Section headers 20 / 700 label colour at x 20 (as Folders' "On My Phone", E), collapsible (chevron) — Pinned is
     collapsible (E).
   - Each section is a card x 16 w 358, r 26.
4. **Note row** (E, ~70 tall): title 17 / 600 label, one line, at card + 16, baseline row top + 26; second line 15 /
   400: the **time** (today: "09:41"; this week: weekday "Tuesday"; older: "12/10/2026") in label-secondary **then**
   two spaces and the preview text (the first body line after the title) in secondaryLabel (A semantic, E style);
   "No additional text" when there is none (iOS's words, E). With an image attachment: a 44 x 44 r 8 thumbnail at the
   trailing inset 16, text width shrinks (E). Separator from card + 16 to card right − 16.
   - When the list shows notes from several folders (search, Recently Deleted from several): a third line 13 / 400
     tertiary with a folder glyph + folder name (E).
5. Bottom bar: Search capsule + Compose circle exactly as Folders §2.1.7.

**Gallery view** (A: View as Gallery) (E geometry): 2 columns, 16 side inset, 12 gaps; each tile ~173 x 190: a
white (dark #1C1C1E) rounded card r 12 with the note's first ~10 lines rendered at 40 % scale, then under the card
title 13 / 600 (2 lines max) and date 12 / 400 secondary, centred. Date sections still apply.

**States**
- Empty folder: centred in the free space, "No Notes" 20 / 600 tertiaryLabel (E words). Compose stays.
- Loading: none (local).
- **Select** mode (Folder Actions > Select Notes, E): checkbox circles (22) at the leading edge; bottom bar becomes
  **Move** (left text button) · **Delete** (right, red) with "Move All" / "Delete All" when nothing is selected (E);
  top: "Done" prominent capsule.

**Interactions**
- Tap a note → push the editor (keyboard **down**, caret not placed; iOS opens notes read-ready, E).
- **Swipe right** (A: then tap the pin): **Pin** / **Unpin** (orange #FF8D28 capsule, pin glyph); full swipe pins.
  The row animates into / out of the Pinned section (spring 0.45 / 0.9, E). Haptic selection at the threshold.
- **Swipe left** (A: Move Folder, Delete): **Delete** (red, trash) leading-most visible on the far right, **Move**
  (purple, folder) left of it; full swipe = Delete, no confirmation (A: goes to Recently Deleted). Share / Lock → omit.
- **Long-press** (A: Pin Note, Move, Delete): the row lifts into a preview of the note (a 300 x 340 rounded card
  with the note's start) and a menu below (IOS-REF §3.3): **Pin Note** (pin), **Move** (folder), **Delete** (trash,
  red, last) (A items; E order). Share Note / Send a Copy / Lock Note → omit. Tap the preview → open the note.
- **Folder Actions** menu (A items; E order), glass menu morphing from the ellipsis circle:
  1. **Select Notes** (checkmark.circle) (E)
  2. **View as Gallery** / **View as List** (toggles its title) (A)
  3. **Sort By** ▸ submenu: **Default (Date Edited)**, Date Edited, Date Created, Title — then a divider — **Newest
     First / Oldest First** (date) or **Ascending / Descending** (title) (A: oldest first, descending)
  4. **Group By Date** ▸: **On** / **Off** (E: iOS's submenu offers "Default (On)", On, Off)
  5. **View Attachments** → omit
  (Folder-level Rename / Move / Delete stay on the Folders screen.)
- Search: tap the field → as §2.4.

### 2.3 Note editor — M (from guide art + MacRumors editor shot) unless tagged

**Layout, reading (keyboard down)** (M from the two guide images)
1. Nav row: back 44 glass circle at x 16 (M: 16 on iPhone). Trailing **one glass capsule** grouping the note's
   actions, right edge at x 374 (M: 16 inset), 44 tall, ~50 per button (M: 100 wide with two, 157 with three):
   **Undo** (arrow.uturn.backward; only present when there is something to undo — M: one image has it, one not),
   **Share** (square.and.arrow.up), **More** (ellipsis). Glyphs label colour (M: dark glyphs, not yellow, in the art).
   - Local: Share → our share sheet with "Copy" and "Send as Message" (SMS) only (local equivalent).
2. Note body (no background card; white / black page, M: pure white in the art):
   - text x **26**, right margin 26 (M: 26.5 pt on 393 — the left gutter leaves room for collapse chevrons at x 17).
   - **First line** is the title (A): **Title** style 28 / 700 (E size), line pitch **37** (M 37.5), baseline of the
     first line **136** (M: 48 below the nav row's bottom).
   - Body 17 / 400 with a **26 pt line pitch** (M: 26.2-26.5, notably looser than the 22 of UIKit Body).
   - Heading ~22 / 700, pitch 29 (M); Subheading 17 / 600 (M); paragraph gap = one half line (E).
   - Colours: text label colour; highlight runs as rounded rects r 4, 20 % tint of the colour behind the text (M:
     purple / pink / orange / mint highlights in the art).
3. Bottom bar (M): a **glass capsule** x 20..191 (M: 171 wide, 48 tall) with three glyphs on a 56 pitch at x 48, 104,
   160: **Checklist** (checklist), **Attachments** (paperclip), **Markup** (pencil.tip.crop.circle) — Markup →
   **omit**: put the **Table** glyph in its place (local); and the **Compose** 48 circle at x 322..370 (new note).
   Bars at y 726..774 (M: 27-29 above the bottom).

**Layout, editing (keyboard up)** — M (from MacRumors editor shot; 440-pt phone, fixed chrome kept in pt)
1. Nav row: back circle; trailing capsule **Share · More** (~100 wide, share greyed when the note is empty — M);
   then **Done**: a **filled Notes-yellow 44 circle with a white checkmark** (17 / 600) at x 326..370 (M: 44.6, solid
   tint — iOS 27's prominent Done; A: tap Done to save). Gap capsule → Done 12 (E).
2. Keyboard (IOS-REF §4) top at y 496 on S9.
3. **Format bar**: a glass capsule floating **27 above the keyboard's top** (M), 48 tall (M 49), x 20..370, y
   421..469. Items on a 56 pitch from x 48 (M: ~60 on the 440 phone; we use the bottom bar's 56): **Aa** (format),
   **Checklist**, **Table**, **Attachments**, **Markup** (omit), **Writing Tools / Image Playground** (omit), then a
   trailing **›** "Expand" item (A: Insert Divider Line lives behind it in iOS 27). Our set: Aa · Checklist · Table ·
   Attachments · ›. The bar **scrolls horizontally** when items overflow (M: the shot's last glyph is cut by the
   capsule edge; A: "swipe over the format bar options").
4. The text area ends 12 above the format bar; the caret line is kept visible (auto-scroll with 24 pt margin, E).

**Content and states**
- New note: empty, caret on line 1 in Title style, keyboard up, shift on (auto-capitalise). Placeholder: none (iOS
  shows nothing, E).
- Empty note on leave (back or app switch) → discarded silently (E; trunk does this).
- Title for lists: the first line's text; if the first line is empty, the first non-empty line; if none, "New Note"
  (E; trunk uses "New Note").
- Read-only (in Recently Deleted): tap anywhere → alert **"Recover Note?"** / "Notes in Recently Deleted can't be
  edited." (ours) / Cancel · **Recover** (E).
- Very long note: up to 32 KiB per note ("This note is full."); past the 96 KiB collection see the "Storage Full" state in the box at the top.

**Interactions**
- Tap text → caret there + keyboard up. Double-tap word → select + the edit menu (IOS-REF §3.3 compact horizontal
  glass menu): **Cut · Copy · Paste · ▸** (more: **Copy as Markdown** (A iOS 27), **Format** ▸ Bold / Italic /
  Underline / Strikethrough, **Add Link** (→ "Link to Section" in 27, A — **omit v1**), Look Up (omit), Translate
  (omit), Share (omit)).
- **Done** → keyboard down, Done circle dematerialises, Undo appears in the capsule if there's history (M), the
  capsule slides to its 3-button width (morph, LIQUID-GLASS §5.3, E).
- **Undo** tap → undo one step; long-press Undo → menu **Redo** (E). Three-finger swipe → omit.
- **More** (ellipsis) menu (A items: Find in Note, Lines & Grids; E order and the rest from iOS 18/26):
  - top row of 4 small square buttons (E): Scan (omit) · Lock (omit) · **Pin** · **Delete** → ours: **Pin**, **Delete**
    (two-button row)
  - **Find in Note** (magnifyingglass) (A)
  - **Move Note** (folder) (E)
  - **Lines & Grids** ▸ None, Small Lines, Medium Lines, Large Lines, Small Grid, Medium Grid, Large Grid (A item, E
    list) — draws the paper pattern behind the text
  - **Export as Markdown** (iOS 26, 3P) → local: writes `<title>.md` into the card's Notes export folder, then a
    toast "Exported to the memory card" (ours)
- **Find in Note**: a find field rides above the keyboard in place of the format bar (glass capsule, "Find in Note"
  placeholder), with **‹ ›** arrows and "2 of 5" count at its trailing end; matches highlighted Notes-yellow at 40 %,
  the current one at 100 % (E).
- **Aa (Format panel)** (A: Heading, bold, italic, highlight; E layout from iOS 18/26): the keyboard is replaced by a
  glass panel of the keyboard's height:
  - header row "Format" 17 / 600 + a 30 "xmark" circle (closes back to the keyboard);
  - row 1: style chips, horizontally scrolling, each showing its own style: **Title**, **Heading**, **Subheading**,
    **Body**, **Monostyled**; the current one has a filled yellow 15 %-tint capsule;
  - row 2: segmented buttons **B**, *I*, U̲, S̶ and a **highlight** button (marker glyph; tap applies the last colour,
    long-press opens 5 swatches: purple, pink, orange, mint, blue — M colours from the guide art);
  - row 3: **Bulleted List**, **Dashed List**, **Numbered List**, **Block Quote** (toggles), **Decrease Indent**,
    **Increase Indent**.
  - Selection haptic on each toggle. Applies to the selection or the current paragraph.
- **Checklist button** (A): turns the current paragraph (or each selected one) into a checklist item / back to text.
  - Item: a 22 ring (1.5 pt systemGray3) whose centre is at text x + 11, text indented 34 (E).
  - Tap the ring → it fills Notes yellow with a white check (spring scale 0.8 → 1.0, response 0.3 / 0.6), **impact
    light** (E); the text stays in the label colour, not struck through (E).
  - Return on a non-empty item → a new item; return on an empty item → ends the list (E).
  - Swipe an item right / left → indent / outdent (E). Long-press the ring → drag to reorder (E).
  - Settings > Notes > **Sort Checked Items**: Manually / Automatically (checked move to the bottom with a 0.35 s
    slide) (E).
- **Table** (A: Add Table): inserts a **2 x 2** table (E) at the caret: cells with 0.5 pt separator lines, cell
  padding 8, text 17 / 400; the table scrolls horizontally inside the note when wider than the text column. Tap a cell
  → edit it; a column handle (ellipsis pill, 20 x 8) appears above the column and a row handle left of the row;
  tapping a handle → menu: **Add Column Before**, **Add Column After**, **Delete Column** / **Add Row Above**, **Add
  Row Below**, **Delete Row** (E). Return in the last cell → a new row (E). The Table button while inside a table →
  menu **Convert to Text**, **Delete Table** (E).
- **› Expand** → menu (A, iOS 27): **Insert Divider Line** — a 1 pt separator-colour rule across the text width,
  14 above and below (E); deletable like a character.
- **Headings collapse** (A): a Heading or Subheading shows a **chevron.down** (13 / 600 tertiary) in the left gutter
  at x 17 (M) when the caret is in it or after a tap on the heading; tap → the section folds (content up to the next
  heading of the same or higher level hides, chevron becomes chevron.right) (A: collapse / expand). Spring 0.35 / 1.0.
- **Attachments** (A list: photo/video, scan, take photo, record audio, file): menu → local: **Choose Photo**
  (opens our Photos picker sheet), **Take Photo** (our Camera); others omit. An image attaches inline full text width,
  r 10, tap → full screen viewer (E).
- Links: URLs typed are detected and shown in Notes yellow, tap opens the browser (when we have one; else copy) (E).
- **Compose** (bottom-right, keyboard down) → saves this note and pushes a new empty one (A: New Note button).

**Transitions**
- List → editor: push (IOS-REF §16, 0.45 / 1.0). New note from Compose: push of a fresh editor with the keyboard
  rising together (keyboard spring 0.38 / 1.0) (E).
- Keyboard up/down: bottom bar fades out (0.15 s) as the format bar fades/slides in above the keyboard (E).
- Done circle: materialise / dematerialise (LIQUID-GLASS §5.2).

**Haptics and sounds**: key clicks / light key haptic per Settings (IOS-REF §4); checklist check impact light;
format toggles selection; menu lift medium; delete none. No sounds besides keys.

### 2.4 Search (Folders or a Notes list)

A: search across all notes (typed text; scanned / handwriting / image search → omit); 3P: field at the bottom.
- Tap the field → keyboard up, field rides above the keyboard, a 48 "xmark" glass circle appears at its right (same
  component as Settings §2.2), compose hides (E).
- Empty query: **Suggested** section (A mentions suggested searches) → local: "Notes with Checklists", "Notes with
  Tables", "Notes with Attachments", "Pinned Notes" as rows with glyphs (E).
- Typing: results per keystroke in **"Top Hits"** (up to 3, E) then **"Notes"** (all matches, by date edited) (E),
  rows as §2.2 with the third folder line; the matched substring in the title and preview gets a Notes-yellow 30 %
  background (E). Searching from inside a folder searches **all** notes (iOS default, E).
- No results: "No Results" 20 / 600 + "Check the spelling or try a new search." (E, as Settings).
- Tap a result → push the editor with the first match highlighted (Find in Note opens with the query, E).
- X circle → leave search, back to the list.

### 2.5 Recently Deleted (A)
- Appears in the Folders card only when not empty (A). Notes are kept **30 days** then removed (A).
- Header text under the large title (ours): "Notes are kept here for 30 days, then removed." 15 / 400 secondary (E
  placement).
- Rows as §2.2; each preview line ends with "· 12 days" remaining in tertiary (E).
- **Swipe left** (A): **Delete** (red) → confirm action sheet **"Delete Note"** (destructive) / Cancel (A: tap Delete
  Note); **Move** (purple) → folder picker (A: recovering = moving to a folder).
- Top trailing: **Edit** capsule → select mode with bottom **Move All** / **Delete All** (E).
- Folder picker (Move): a sheet (large detent), title "Move Note" (or "Move 3 Notes"), "Cancel" leading circle, the
  folder list as on Folders (current folder greyed), a "New Folder…" row (E).

### 2.6 Delete flows
- Note (list swipe / context menu / editor More > Delete): goes to Recently Deleted at once (A), no alert; the row
  collapses (height → 0 and fades, 0.3 s, E); from the editor the page pops after the delete (E).
- Folder: alert or action sheet as §2.1, its notes → Recently Deleted (A).
- Permanent delete only inside Recently Deleted (A).

### 2.7 Empty states (words, E)
- Folder with no notes: "No Notes".
- Recently Deleted empty: the row is hidden (A).
- Search: "No Results" / "Check the spelling or try a new search."

### 2.8 Settings > Apps > Notes (A list from the guide; our decisions)
- **Default Account** → omit (one account). **Password** → omit (no locked notes). **"On My Phone" Account** → always
  on, hidden. **Mention Notifications** → omit. **Siri** / **Search** → Search only ("Show in Search" toggle).
- Section **"Viewing"**: **Sort Notes By** (Date Edited / Date Created / Title), **New Notes Start With** (Title /
  Heading / Subheading / Body, E: iOS option), **Sort Checked Items** (Manually / Automatically), **Lines & Grids**
  (picker with 7 drawn previews), **Group Notes by Date** toggle (A rows).
- **Auto Convert to Tag** → later (tags omitted v1). **Save to Photos** toggle (photos taken in Notes → our Photos).
- **Access Notes from Lock Screen** (Off / Always Create New Note / Resume Last Note) (A) → **omit v1** (no lock
  screen Notes control yet).

---

## 3. Cross-screen flows

**Write a new note**
1. Folders or a list → tap Compose (bottom-right glass circle). 2. Editor pushes; keyboard rises; the format bar
   floats above it; the caret is in the title line (Title style). 3. Type the title, Return → the next line is Body
   style. 4. Tap Done (yellow circle) → keyboard down; the bottom bar returns. 5. Back → the list, the note at the top
   of "Today" (or under Pinned if pinned).

**Make a checklist**
1. In the editor, tap the Checklist glyph → the line becomes an item (ring). 2. Type, Return → next item. 3. Return
   on an empty item → back to body text. 4. Tap rings to check (yellow fill, light haptic).

**Pin a note**: list → swipe right → Pin (or long-press → Pin Note) → it slides into "Pinned".

**Move a note**: swipe left → Move → folder sheet → tap a folder → sheet closes, the row leaves the list (it stays if
viewing all).

**Recover a note**: Folders → Recently Deleted → swipe left → Move → pick "Notes" → gone from Recently Deleted.

**Search**: list → tap Search → type → tap a result → editor with the match highlighted.

**Change the view**: list → Folder Actions → View as Gallery → the list cross-fades to the grid (0.3 s, E).

---

## 4. Motion table

| Trigger | What moves | Spring / duration | Tag |
|---|---|---|---|
| Open folder / note | push | 0.45 / 1.0 | IOS-REF §16 (E) |
| Compose | push + keyboard up together | 0.45 / 1.0; keyboard 0.38 / 1.0 | E |
| Keyboard up | bottom bar fades out, format bar fades + rises 8 pt into place above the keyboard | 0.15 s fade; with the keyboard spring | E |
| Done | Done circle dematerialises; capsule morphs to 3 buttons | LIQUID-GLASS §5.2 / §5.3 | E |
| Swipe actions | row follows the finger; capsules reveal; past the full-swipe threshold the first action expands | finger 1:1; snap 0.35 / 0.85 | E |
| Pin / unpin | row moves between sections | 0.45 / 0.9 | E |
| Delete row | row height → 0 with fade | 0.3 s ease-in-out | E |
| Context menu | row lifts to the preview; menu grows from it; rest blurs | IOS-REF §3.3; medium haptic | E |
| Folder Actions / More menu | glass morph from the circle | LIQUID-GLASS §5.3 | A/E |
| Section collapse (Folders header, headings) | content folds; chevron rotates | 0.35 / 0.85 (header) / 0.35 / 1.0 (heading) | E |
| Checklist check | ring fills, check scales 0.8 → 1 | 0.30 / 0.6 | E |
| Checked item auto-sort | item slides to the list end | 0.35 s | E |
| Format panel | replaces the keyboard (cross-fade, same height) | 0.25 s | E |
| List ↔ Gallery | cross-fade | 0.3 s | E |
| Search focus | field rides up; X circle materialises; list fades | as Settings §2.2 | M/E |
| Sheets (New Folder, Move) | IOS-REF §3.4 | — | M/E |

---

## 5. Words (fixed strings)

Folders · Edit · Done · New Folder · On My Phone · Notes · Recently Deleted · Search · Name · Rename Folder · Name
Taken · Please choose a different name. · OK · Rename · Move · Delete · Delete Folder? · Its notes go to Recently
Deleted. · Delete Folder · Cancel · Pinned · Today · Yesterday · Previous 7 Days · Previous 30 Days · %d Notes · 1 Note
· No additional text · New Note · No Notes · Select Notes · View as Gallery · View as List · Sort By · Default (Date
Edited) · Date Edited · Date Created · Title · Newest First · Oldest First · Ascending · Descending · Group By Date · On
· Off · Move All · Delete All · Pin · Unpin · Pin Note · Unpin Note · Move Note · Move %d Notes · New Folder… · Undo ·
Redo · Share · Copy · Send as Message · Find in Note · %d of %d · Lines & Grids · None · Small Lines · Medium Lines ·
Large Lines · Small Grid · Medium Grid · Large Grid · Export as Markdown · Exported to the memory card · Format ·
Heading · Subheading · Body · Monostyled · Bulleted List · Dashed List · Numbered List · Block Quote · Decrease Indent
· Increase Indent · Insert Divider Line · Add Column Before · Add Column After · Delete Column · Add Row Above · Add
Row Below · Delete Row · Convert to Text · Delete Table · Choose Photo · Take Photo · Cut · Paste · Copy as Markdown ·
Bold · Italic · Underline · Strikethrough · Top Hits · Suggested · Notes with Checklists · Notes with Tables · Notes
with Attachments · Pinned Notes · No Results · Check the spelling or try a new search. · Notes are kept here for 30
days, then removed. · %d days · Delete Note · Recover Note? · Notes in Recently Deleted can't be edited. · Recover ·
Viewing · Sort Notes By · New Notes Start With · Sort Checked Items · Manually · Automatically · Group Notes by Date ·
Save to Photos · Show in Search.

---

## 6. Starbite today → gap

**Trunk 1c166b85** (`shell_lock.c` P3e / B3 / B5 / P3p, `shell_kb.inc`): one flat list, **8 notes max x 600 bytes**
(`NNOTES`, `NOTELEN`), kept in the "notes" blob; large title "Notes"; one card at `CONTENT_TOP` with 64-pt rows
(title 17 / 600, "hh:mm  preview" 15 / 400); a glass compose circle at bottom-right (y 718, d 48, yellow pencil) with
the count "N Notes" centred beside it (iOS 18 style); "No Notes" / "Tap the pencil to write one." empty state;
long-press 0.5 s → alert "Delete Note?" with Cancel / Delete (permanent). Editor: yellow back chevron on a glass
circle, Done = a glass circle with a yellow check, our keyboard always up, first line 28 / 700 with 38-pt pitch,
body 17 / 400 with **24-pt** lines at x 20, yellow caret; an empty note is dropped on leave.

Ranked changes:
1. **Storage model**: unlimited notes (bounded by space), 64 KB per note, folders, pinned flag, created / edited
   timestamps (full date, not hh:mm), deleted-at for Recently Deleted, styled runs (title / heading / body, B I U S,
   highlight, checklist state, tables, dividers). A versioned blob or one file per note on the card. **L**
2. **Folders screen + Notes list hierarchy** (On My Phone, Notes, user folders, Recently Deleted, New Folder sheet,
   Edit). **M**
3. **Bottom bar to iOS 26/27**: Search capsule (x 20..310) + Compose circle (x 322..370) at y 726..774 (trunk: compose
   at y 718 + count label); move the count to the subtitle. **S**
4. **Editor chrome**: trailing glass capsule Undo · Share · More, filled yellow **Done** circle with a white check
   (trunk: glass circle, yellow check), keyboard **down** when opening an existing note, the bottom checklist /
   attachments / table capsule when reading. **M**
5. **Format bar above the keyboard** (Aa · Checklist · Table · Attachments · ›) and the **Format panel**; styled
   text rendering in sbtk (runs, highlights). **L**
6. **Checklists** (rings, check haptic, return rules, auto-sort). **M**
7. **List interactions**: swipe right Pin; swipe left Move / Delete; long-press context menu (Pin Note / Move /
   Delete) instead of the delete alert; date sections; Recently Deleted (30 days) instead of permanent delete. **M**
8. **Search** across notes with highlights; Find in Note. **M**
9. **Body typography**: text x 26 (trunk 20), body line pitch 26 (trunk 24), title pitch 37 (trunk 38), first
   baseline 136. **S**
10. **Tables**, **divider lines**, **collapsible headings**, **Lines & Grids**, **Gallery view**. **M-L** (later)
11. Edit menu: Copy as Markdown; Export as Markdown to the card. **S**
12. Settings > Apps > Notes page (Viewing section). **S**

---

## 7. Still estimated

Measured: the Folders screen (top buttons, large title → card spacing, account header style and spacing, 52 rows,
glyph / text x, separators, bottom search + compose), the editor's chrome both reading (back, action capsule widths,
bottom capsule with three tools, compose) and editing (Done filled-yellow 44 circle, format bar 48 tall floating 27
above the keyboard, item pitch, horizontal scroll), text margins (26), gutter chevrons (x 17), title / heading /
body line pitches, the Notes yellow. Still **E**:
- The **Notes list inside a folder**: row height, the time / preview format per age, thumbnail size, the note-count
  placement (subtitle vs bar), section header style, the Folder Actions menu order — no public iOS 27 image found.
- Gallery tile geometry; select mode bar.
- Format panel layout and its controls; table handles and default size (2 x 2); checklist ring size and indent; the
  divider's spacing; Find in Note bar.
- All swipe-action colours and orders (Pin orange, Move purple, Delete red), full-swipe rules, context-menu order.
- New Folder sheet vs alert; Recently Deleted header text and the "days left" label; the read-only alert words.
- Title font size (28 assumed; the 37-pt pitch fits 28-30).
- Every timing in §4; dark-mode colours (all public art found is light).
