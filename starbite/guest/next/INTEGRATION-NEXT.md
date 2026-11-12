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
