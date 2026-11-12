# Linux apps + the App Store in Starbite OS Next (L2 + L3): the drop-in for the toolkit (claude/sbos-ui-p1-921bbf)

Design: docs/STARBITE-LINUX-APPS.md (sections 5, 7, 8, 12). Linux runs on the four M3 cores next to Starbite OS
Next (the guest kernel of its own, sbwl, our Wayland compositor, contract v2 in `starbite/guest/sbshm.h`).

- **Home page 3 ("Linux")**: the App Store, then the catalog's apps. Each icon is the guest's 160 px icon on a dark
  rounded tile with a small Tux badge, so Linux apps read as Linux beside Starbite's own. The grid is E's lattice
  (`COL_CX`, `HOME_TOP2`, `ROW_PITCH`, 84 pt `d_text_box` labels), with up to 19 apps (5 rows). Icons are drawn once
  into the page; a press or an open copies its icon back out. The Search pill shows three page dots while the pages
  move: one pill plus a small dots layer redrawn when the page changes (the toolkit had a pill per page).
- **A Linux app opens like any app**: it springs from its icon. Its page is the app's window from 44 pt to 772 pt
  (720 x 1344 surface px at 2x, pixel-doubled). The page shows "Starting Linux" / "Opening <name>" until the
  window arrives. The keyboard comes up when the app's text field has the focus (text-input-v3) or the app has the
  KEYBOARD flag (terminals). The viewport is then 916 rows (44 to 540 pt). Typed characters go as committed text
  to a focused field, or as keys for KEYBOARD apps (foot reports TEXT_FOCUS too and still gets keys). Return and
  BackSpace are always keys. The home swipe is ACTIVATE none: the app keeps running until guest.c's 60 s idle stop.
- **The Terminal** stays on page 2 as Starbite's own entry. It is the catalog's foot (`org.codeberg.dnkl.foot`, also
  `foot`), and page 3 leaves foot out. Why: its place and icon are in the approved concept, the L2 self test opens it
  by key, and there is no second icon for the same app. It also works before any catalog exists: an L2 guest (no
  catalog 2 s after HELLO) shows foot by itself, so the staged L2 bundle still passes the phone test.
- **The App Store** (a Starbite app on `sbshm_store`), in P3q's bands: the large title (`d_h1` on `LARGE_BASE`) with
  the free space on the memory card, a search field at `CONTENT_TOP` (its glass baked once per opening, not in a tap)
  with the shell's keyboard (name, package, summary, category), up to three FEATURED cards, and the list
  (icon, name, category and size or the state, GET / OPEN / RETRY, or a progress ring with a stop square). Rows are
  drawn in 2-row chunks that are reused as the list scrolls, so the whole index never sits in the heap.
- **A package's page** (a pushed page: the glass back circle, content from `PUSHED_TOP`): icon, name, package, GET / OPEN / CANCEL / RETRY with a progress bar and its state. It warns
  for TOO_BIG (amber: the honest RAM need against Linux's total; installing is allowed), refuses for NO_SPACE (red:
  needed vs free, GET dimmed) and shows FAILED with the guest's error. Then the summary, Category, Download,
  Installed size, Memory in use and Free space. **Remove App** (tap twice within 3 s) appears only for installed
  packages that are not SYSTEM: a SYSTEM package says it can't be removed.

## The patch

`next-linux.patch` applies to the toolkit tip **c29ca0d** (`git apply --check` passes on a clean
`git archive c29ca0d starbite/sbos starbite/guest/sbos starbite/libsb`). It contains L2 (the `next-linuxtest` self
test) and L3, and supersedes `l2-next.patch` (kept for history, against fb9114a).

The UI is in its own file, `starbite/sbos/ui/shell_linux.inc` (like shell_media.inc), included after
shell_media.inc. shell_lock.c only gets one or two lines at each hook:

| shell_lock.c place | hook |
|---|---|
| after `hparts[]` | the `lx_*` prototypes; `home_dots()` (the pill's three dots) |
| `app_icon` | `lx_icon()`: the App Store's icon, a Linux app's (Search lists the App Store, never "linux") |
| `build_home` | `lx_home()` (page 3, `hparts[5]`); the dots pill built once, `home_dots(0)` |
| `set_page` | the dots follow the page (three pages) |
| APPS | `linux` (the open Linux app) and `store` |
| `app_own` / `own_strip` / `own_extras` | `lx_is()`; no strips unless `lx_strips()` (a package's page), `lx_page()`, `lx_build()` |
| `icon_rect` / `app_hit` | `lx_icon_rect()` / `lx_hit()` for page 3; `app_hit` picks one of three pages |
| `app_open` / `app_finish_close` / `app_tap` | `lx_open()`, `lx_close()`, `lx_tap()` |
| includes | `#include "shell_linux.inc"` |
| `relink` | `lx_layers()` under `appc`; `hparts[5]` under `pages` |
| `sh_set` | `lx_set()` (`lapp=ID:v`, `store=PKG:v`, `storeq=TEXT:v`); `app=` closes the open app (L2) |
| gestures | `G_LINUX` (`lx_touch()`: a Linux window, the store's list); page swipes over three pages |
| `sh_tick` / `sh_moving` | `lx_moving()` |

Other files:

| File | What changes |
|------|--------------|
| `starbite/sbos/ui/shell.h` | the `sh_linux_*` API below, `SH_LINUX_H` 1344, `SH_LINUX_HKB` 916 |
| `starbite/sbos/ui/host/main.c`, `host/linux_fake.inc` (new) | the host's pretend Linux: 6 apps, 14 packages in every state (QUEUED, INSTALLING 40 %, REMOVING, FAILED, TOO_BIG, NO_SPACE, SYSTEM, FEATURED). Icons are drawn and windows rendered with the pack's fonts (foot from `SBTK_LINUX=foot.ppm`); every call is logged as `linux: ...`. `SBTK_LINUX=cold` starts with Linux off, `=starting` / `=fail` as in L2. Still-only states: `lx=focus:1`, `lx=type:TEXT`. `SBTK_NO_LINUX_FAKE` drops it (gwin-host) |
| `starbite/sbos/ui/kern_next.c` | `next_kdraw`: `gwin_now()` every loop turn; `next_ready()` |
| `starbite/sbos/build.sh`, `starbite/sbos/kern/uitest.c` | gwin.c in every Next image; `starbite-next-linuxtest.img` |
| `starbite/guest/sbos/guest.{c,h}` | see below. **guest.{c,h} belong to the starbite-os session**: the same change should go to master (it only adds things) |

guest.c, the functions it touches (for the card-backed block devices patch on top): the `names[3][3]` table and
`surf_w` / `surf_h` (GUEST_WL); `guest_available_mode` and `guest_start_mode` (real and host fake: GUEST_WL); a new
`static int kept` beside `sb_up` and a new `guest_keep()` after `guest_shown()`; `cfg_init` (one line,
`.v2 = mode == GUEST_WL`); `guest_timer` (real and fake: the idle-stop condition gets `!kept &&`); `guest_blit`
(real and fake), which becomes a wrapper of a new `blit_any()` with the new `guest_blit_rows()` beside it (the row
addresses through a `TROW()` macro); the host fake's marker check (any non-APP mode). guest.h: `GUEST_WL_*`
(H 1344), `guest_blit_rows`, `guest_keep`.

### Memory (the host with SBTK_HEAP_MB=160, the phone's UI heap)
- The window is four 3.9 MiB strip layers, freed whenever the app leaves the screen (closed, home, another app).
  The last strip is freed while the keyboard is up. No page strips sit under the window.
- The store's list is five chunk layers of two rows (2.5 MiB each), reused as it scrolls. A package's page uses the
  usual strips.
- Page 3 is one layer sized to its rows. No icon bitmaps are kept besides the open app's.
- Our film: peak **146 MB**, no refusal. The toolkit's own films (tools/films.txt via `sbtk-host PAK selftest`):
  peak **156 MiB** with the patch, the same as without it. Heap refusals: 16 with the patch (9 x 8.4 MB and
  3 x 9.6 MB, which c29ca0d has too) against 392 without it (c29ca0d alone refuses a 1.3 MB block 347 times). The
  single dots pill saves more than page 3 costs at those moments.

## The API (shell.h)
```c
/* platform (gwin.c on kern/guest.c + sbghost v2) */
int  sh_linux_open(const char *id);     /* start Linux if off; LAUNCH + ACTIVATE the app once the catalog has it */
void sh_linux_hide(void);               /* ACTIVATE none, MSG_FOCUS 0: the app keeps running */
int  sh_linux_blit(uint32_t *const *rows, int first, int *y0, int *y1);  /* the active window's new rows at 2x */
int  sh_linux_touch(int phase, int x, int y);   /* surface px */
int  sh_linux_char(char c);             /* SBG_MSG_TEXT to a focused field, keys for KEYBOARD apps; \n \b keys */
int  sh_linux_viewport(int rows);       /* 1344, or 916 with the keyboard: SBG_MSG_VIEWPORT (also after ACTIVATE) */
int  sh_linux_icon(int icon, uint32_t *px);     /* a 160 x 160 pool icon, copied under its table's seqlock */
int  sh_linux_hold(int on);             /* the App Store open: Linux started if off, kept from the idle stop */
int  sh_linux_pkg(int pkg, int op);     /* SBG_MSG_PKG install / remove / cancel with the store's seq */
/* shell, called by the platform */
void sh_linux_state(int state, const char *why);
int  sh_linux_frame(void);              /* the active window has new rows */
void sh_linux_catalog(const sh_lcat_t *c);      /* copied; page 3 is rebuilt only when ids, names or icons change */
void sh_linux_store(const sh_lstore_t *s);      /* the platform's copy, kept until the next call */
void sh_linux_textfocus(int on);
```
`sh_lcat_t` / `sh_lstore_t` mirror `sbshm_cat` / `sbshm_store` in the shell's own types. shell.h does not include
sbshm.h, and gwin.c `_Static_assert`s that the flag values match. gwin.c reads the tables only under their
seqlocks. LAUNCH carries the catalog's `layout_seq` (seq moves every ~2 s with rss), and indices are used only from
this run's catalog. The last catalog is kept while Linux is off, so the icons stay on page 3.

## Checked (on the Mac, the toolkit's host tool; and the Lima VM)
- `sbtk-host` builds with `-Werror`. Stills (`SBTK_LINUX=foot.ppm sbtk-host PAK still OUT.ppm STATES`): page 3
  (`unlock=:1 page=:2`), the store (`store=:1`), the search (`storeq=fire:1`), the package pages
  (`store=gimp|chromium|gnome-text-editor|libreoffice-writer|inkscape:1`), the Text Editor
  (`lapp=org.gnome.TextEditor:1`, plus `lx=focus:1 "lx=type:Hello"` for the keyboard), the Terminal (`app=terminal:1`).
- A film through the real gestures (with `SBTK_HEAP_MB=160`, the phone's heap: no refusal, peak 146 MB): unlock, two
  swipes to page 3, App Store, the field, `f i r e`, return, GET on Firefox ESR, the progress to OPEN, OPEN (it
  springs from page 3's new icon, "Opening Firefox ESR"), tap the address bar, `h i`, return, home, the icon again
  (raised at once), home.
- `host/run.sh TREE PAK OUT` (gwin.c with the real shell and a v2 fake guest): the self test PASSED (foot from the
  catalog, LAUNCH with layout_seq, foot's TEXT_FOCUS still typed as keys), then 12 L3 checks PASSED: page 3, a
  Linux app's launch / activate / viewport, the field's focus (viewport 916, text as SBG_MSG_TEXT, Return a key),
  home (ACTIVATE none), the store's GET (SBG_MSG_PKG with the store's seq), the install to the end, no Remove for
  SYSTEM, the hold released. `GWIN_L2=1` (an L2 guest, no catalog): PASSED. `GWIN_STORE=none`: fails with the
  store's reason.
- The whole `starbite/sbos/build.sh` at c29ca0d + the patch + gwin.c + the current sbghost / sbshm.h in the Lima VM:
  every image builds and links (2026-11-11, /mnt/build/l3tree-c29); host tests 320 + 704 checks and shub's 83
  passed. The Next images are starbite-next, -selftest, -wifitest and -linuxtest.
- The same with `starbite/guest/blk/guest-card.patch` (e2f6642) applied after this patch and the current
  sbghost.{c,h} (with `sbg_set_blk`), at /mnt/build/l3tree-c29card: both apply, every image links, and the host
  tests pass (320 + 704, shub 83 + 64).

## The guest side (sbwl v2, committed)
- sbwl sends WINDOW (index, 1) only after that app's first published frame, and sends TEXT_FOCUS again after each
  ACTIVATE. gwin.c shows no rows before the WINDOW message.
- LAUNCH carries the catalog's `layout_seq`. foot is `org.codeberg.dnkl.foot` (or `foot`) and is KEYBOARD, so it
  gets keys even though it reports text focus (purpose terminal).
- Pool icons are read under their table's seq. sbblk wants 4 KiB alignment (sbshm.h), which needs nothing here.

## Not done (follow-ups)
- An app that quits by itself while shown leaves its last frame (WINDOW has_window 0 is not shown yet): the page
  could close or show "<name> Closed".
- No switcher: background apps live until guest.c's 60 s idle stop (they are killed then). There is no "quit".
- The catalog is not stored: after a phone restart, page 3 shows only the App Store until Linux has run once.
- Search (the home's pill) does not list Linux apps. Page 3 holds 19 apps; a fourth page is not built.
- The keyboard still has no Ctrl / Tab / Esc / arrows or `| > ~ <` (the toolkit's keyboard), as in L2.

## v3 hooks: Linux apps as first-class apps (Track E, 2026-11-12, "apps work fully")

Contract v3 (`sbshm.h` "contract v3", `sbghost.h` v3) in Next. Each running Linux app is its own switcher card. A key
bar sits over the keyboard. Linux notifications go to the Notification Centre. The clipboard works both ways. Track A's
zero-copy window is supported. The logic is in Track E's files (`shell_linux.inc`, `gwin.c`, `guest.c/h`). The shell's
files get **one hook only**: `next-linux-v3-hooks.patch` is `git diff HEAD` of the toolkit's / OS core's files. It
touches `shell.h` alone (the declarations below). Everything else targets the toolkit's coming API (Track B), which
is off until it defines `SH_EXT_API`.

### The hook: `starbite/sbos/ui/shell.h` (after `sh_linux_textfocus`)
| Declaration | Side | What |
|---|---|---|
| `SH_LMOD_CTRL`, `SH_LMOD_ALT` | both | the key bar's modifiers (= `SBSHM_MOD_*`; gwin.c `_Static_assert`s it) |
| `sh_linux_close(id)` | platform | a card swiped away: `SBG_MSG_APP_CLOSE` with the app's index in this run's catalog |
| `sh_linux_key(code, mods)` | platform | KEYMOD mods, the Linux key down + up, KEYMOD 0 (Esc 1, Tab 15, arrows 103/105/106/108) |
| `sh_linux_char_mods(c, mods)` | platform | a typed character as keys with Ctrl / Alt held (Ctrl+C is a key, never committed text) |
| `sh_linux_cards(n)` | platform | how many Linux apps have a card: gwin.c -> `guest_apps(n)` |
| `sh_linux_notif_act(id, action)` | platform | `SBG_MSG_NOTIF_ACT`: 0 the tap, -1 dismissed (`SBSHM_NONE`) |
| `sh_linux_clip(text)` | platform | Starbite's clipboard offered to Linux (`sbg_clip_set`, text/plain;charset=utf-8) |
| `sh_linux_front(&stride)` | platform | the zero-copy path: the guest's front buffer, marked with `sbg_surf_reading` |
| `sh_linux_notify(...)`, `sh_linux_notify_close(id)` | shell | a Linux notification posted / withdrawn (gwin.c's `v3_poll` on `SBG_CH_NOTIF`) |
| `sh_linux_clip_in(text)` | shell | a Linux copy (`SBG_CH_CLIP`) into the shell's clipboard (not offered back) |
| `sh_clip_set(text)`, `sh_clip_get()` | shell | **the shell-wide clipboard** (UTF-8 text, 8 KiB, RAM): Starbite's text fields call these |

The platform's v3 calls also have **weak no-op definitions in shell_linux.inc**. So a platform without them still
links: the host preview's `host/linux_fake.inc`. gwin.c's strong ones win in every Next image. Nothing in
`shell_switcher.inc`, `shell_lock.c`, `shell_nc.inc` or `kern_next.c` changes.

### What the toolkit (Track B) provides; shell_linux.inc calls it when `SH_EXT_API` is defined
Define `SH_EXT_API` in the commit that lands these (in `shell.h`, or in `shell_lock.c` before
`#include "shell_linux.inc"`). shell_linux.inc forward-declares them as **static**, in this file's form:
```c
static void sw_ext_add(int subid);     /* add a kind-1 card, or move it first if it is there (called at every open) */
static void sw_ext_remove(int subid);  /* drop it (the app quit by itself, Linux stopped); no-op if unknown */
static void kb_accessory(const char *const *keys, int n, void (*tap)(int i));   /* (0, 0, 0) clears; keys stay valid */
static int kb_accessory_tap(float x, float y);   /* a tap on the bar: 1 taken (tap(i) called); called from lx_kb_tap */
static void nc_withdraw(const char *who);        /* remove the Centre's items of who, WITHOUT the dismiss callback */
#define KB_ACC_H 40.0f                 /* optional: the bar's height in pt (default 40), from 540 - KB_ACC_H to 540 */
```
The window's rows with the keyboard up shrink by the bar: `LX_KBROWS` = 842 surface rows (44 .. 500 pt). The
viewport, the window's clip and its touch area follow that. A tap in the bar is not the window's (`lx_at`): it goes
through `app_tap` -> `lx_tap` -> `lx_kb_tap` -> `kb_accessory_tap`. If the toolkit routes bar taps itself first,
`kb_accessory_tap` can return 0.

### What shell_linux.inc provides to the toolkit (all `static`, after the `lx_*` helpers, before the hooks section)
```c
/* the switcher's provider, for kind-1 entries; subid = a slot of shell_linux.inc's card table (keyed by the desktop
 * id: stable across catalog re-layouts; up to 8 cards, a 9th closes the oldest card's app) */
static const char *sw_ext_title(int subid);           /* the app's name ("Terminal" for foot); "" unknown */
static const tk_bmp *sw_ext_icon(int subid);          /* ICON_PT (62 pt) premultiplied, pad 0 (as app_icon's); NULL none */
static int sw_ext_snapshot(int subid, tk_bmp *out);   /* 1: *out = a VIEW (TK_W/4 x TK_H/4, owned by shell_linux.inc,
                                                       * never freed by the caller, valid until sw_ext_close/remove); 0: none */
static void sw_ext_open(int subid);                   /* lx_pick + app_open (closes an open app first; the open Linux app: nothing) */
static void sw_ext_close(int subid);                  /* thrown away: APP_CLOSE; the switcher drops its own entry */
static int lx_sw_current(void);                       /* the open Linux app's subid (the live first card), -1 none */
/* the Notification Centre, for items whose who is "linux:<id>" (id: the guest's notification id in lowercase hex,
 * at most 8 digits, so who <= 14 characters). Each argument may be the whole who or the part after "linux:". */
static int lx_open_id(const char *id);                /* a tap: NOTIF_ACT(id, 0) and the app opened; 1 opened */
static void lx_nc_dismissed(const char *id);          /* a swipe away / Clear / clear all: NOTIF_ACT(id, none) */
static const char *lx_nc_name(const char *id);        /* the app's name ("Linux" unknown) */
static const tk_bmp *lx_nc_icon(const char *id);      /* the app's icon (ICON_PT, pad 0), NULL none */
```
The Centre should: call `lx_open_id` instead of `app_open` for a `linux:` item, after its lock handling. Call
`lx_nc_dismissed` when the user removes one (not when it is launched or withdrawn). Show `lx_nc_name` / `lx_nc_icon`
for it (`nc_name`, `nc_draw_item`). The switcher should: show kind-1 cards with `sw_ext_title` / `sw_ext_icon` /
`sw_ext_snapshot`. Open them with `sw_ext_open` in place of `app_open`. Call `sw_ext_close` when one is thrown away.
Treat the first card as live when `lx_sw_current() == subid`. Let the home bar's slow swipe work from a Linux app
(`lx_sw_current() >= 0`). An entry that falls off the end (SW_MAX) is only hidden: its app keeps running.
`host/sw-ext-compat.patch` is exactly this, as a test-only stand-in. Use it as the reference.

The key bar's labels: `Esc Tab Ctrl Alt ← ↑ ↓ →`. A latched modifier is passed as `CTRL` / `ALT` (capitals). The
stand-in lights a capitalised label as the keyboard lights shift. **Font**: the pack's subset (tools/sbtk-fonts.py)
has ← → (0x2190 / 0x2192) but not ↑ ↓. Add 0x2191 and 0x2193 to `CHARS`, or draw the arrow keys as icons. The
stand-in draws ↑ ↓ with `d_seg`.

### Track A (OS core): the zero-copy window (16925a8)
shell_linux.inc uses `sh_linux_zero_copy()` and `tk_layer_external()` under `#ifdef TK_HAS_LAYER_EXTERNAL`. Define it
in sbtk.h with the commit that has `tk_layer_external`. How it switches:
- **At each build of the window** (`lx_build_win`: an open, the keyboard up or down), `LX.zc = sh_linux_zero_copy()`.
  - 1: the strips are freed. One layer, `apext`, sits at (0, 81) scaled 2x. Every new frame (`gwin_now` ->
    `sh_linux_frame`) hands it the front buffer: `sh_linux_front` -> `guest_front()` (guest.c: the header's
    `surf_off[front]`) + `sbg_surf_reading(front)`, then `tk_layer_external(&apext, px, 720, rows, stride)`. Rows is
    1344, or 842 with the keyboard.
  - 0: L3's strips exactly as before.
  - A GPU that goes away mid-show switches at the next build.
- Detached (`tk_layer_external(&apext, 0, ...)`) **before any relayer**: in `lx_close`, and when Linux leaves RUNNING.
  `sh_linux_hide` (and the strips' blit) then call `sbg_surf_done()`. The compose is synchronous, so the mark stays
  on the shown buffer until the next front replaces it. No call after `tk_comp_frame` is needed.
- A card's snapshot reads the external buffer one pixel in four (it is uncached), only when the app closes.

### guest.c / guest.h (Track E's; for master too)
- `cfg.services = CLIP | NOTIF | CLOCK | NET | P9` for GUEST_WL. sbghost drops what has no `sbg_os` part.
  - `unix_s` comes from `clock_now` (UTC, the clock settings' `off_min`).
  - `net_rx = sbos_nat_from_guest`: a weak declaration. It stays NULL (no SVC_NET) until Track G's NAT defines
    `void sbos_nat_from_guest(const uint8_t *pkt, unsigned len)`.
  - `fs = GUEST_FS_OPS` from `guest-fs.h` when it is there (`__has_include`: it is now, read-only on the card).
  - GUEST_QEMU (kern-shim) offers neither the clock nor the files.
- `guest_apps(n)`: with Linux cards, the "hidden 60 s" stop is off. The screen-off stop waits 30 min
  (`GUEST_APPS_SCREEN_OFF_MS`), not 60 s. When the last card goes, the 60 s count starts again. `guest_keep` (the
  App Store, a package) still blocks both.
- `guest_front(&idx, &stride)`: the zero-copy front buffer (the host fake: none).

### Checked
- `host/run.sh . build/sbtk/sbtk-phone.pak OUT` with `GWIN_STILLS=build/trackE-stills` passes. It runs the self test,
  the 13 L3 checks and **20 v3 checks**, through the real shell with sw-ext-compat.patch and zc-compat.patch:
  - two Linux cards and a Starbite card (`S:notes,L:Calculator,L:Text Editor`);
  - a card's tap re-opens its app (ACTIVATE 1); a thrown card sends APP_CLOSE 2;
  - the key bar sends `KEYMOD 1;TYPE c;KEYMOD 0;KEY 105 1;KEY 105 0;KEYMOD 2;KEY 106 1;KEY 106 0;KEYMOD 0;KEY 1 1;KEY 1 0;`;
  - the clipboard works both ways;
  - notifications: `linux:1a` is in the Centre, a withdrawal leaves, a Clear sends NOTIF_ACT none, the banner's tap
    sends NOTIF_ACT 0 and opens the app;
  - zero-copy samples the guest's buffer in place, and home sends `sbg_surf_done`;
  - Linux's stop drops the cards (guest_apps 0).
- With `SBTK_HEAP_MB=160` the UI heap peaks at 143.5 MB. `GWIN_L2=1` passes. `GWIN_STORE=none` fails with the
  store's reason.
- Stills: `build/trackE-stills/trackE-{switcher,switcher-scrolled,keybar,keybar-ctrl-latched,banner,nc-list,zerocopy}.png`.

### Still open
- Notifications are not grouped per app (one `who` per notification, so that the tap can act on that one). Actions
  1..3 (buttons) are not shown.
- An app that quits while shown keeps its last frame (as in L3). Its card goes when the catalog's flags clear.
- The Terminal (foot) is a Linux card too (its page-2 icon, title "Terminal").
