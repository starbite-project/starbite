# Linux apps + the App Store in Starbite OS Next (L2 + L3): the drop-in for the toolkit (claude/sbos-ui-p1-921bbf)

Design: docs/STARBITE-LINUX-APPS.md (sections 5, 7, 8, 12). Linux runs on the four M3 cores next to Starbite OS
Next (the guest kernel of its own, sbwl, our Wayland compositor, contract v2 in `starbite/guest/sbshm.h`).

- **Home page 3 ("Linux")**: the App Store, then the catalog's apps. Each icon is the guest's 160 px icon on a dark
  rounded tile with a small Tux badge, so Linux apps read as Linux beside Starbite's own. The grid is page 2's
  (top 62, 4 x 103 pt), with up to 19 apps (5 rows). The Search pill shows three page dots while the pages move.
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
- **The App Store** (a Starbite app on `sbshm_store`): the title with the free space on the memory card, a search
  field with the shell's keyboard (name, package, summary, category), up to three FEATURED cards, and the list
  (icon, name, category and size or the state, GET / OPEN / RETRY, or a progress ring with a stop square). Rows are
  drawn in 2-row chunks that are reused as the list scrolls, so the whole index never sits in the heap.
- **A package's page**: icon, name, package, GET / OPEN / CANCEL / RETRY with a progress bar and its state. It warns
  for TOO_BIG (amber: the honest RAM need against Linux's total; installing is allowed), refuses for NO_SPACE (red:
  needed vs free, GET dimmed) and shows FAILED with the guest's error. Then the summary, Category, Download,
  Installed size, Memory in use and Free space. **Remove App** (tap twice within 3 s) appears only for installed
  packages that are not SYSTEM: a SYSTEM package says it can't be removed.

## The patch

`next-linux.patch` applies to the toolkit tip **d8a69de** (`git apply --check` passes on a clean
`git archive d8a69de starbite/sbos starbite/guest/sbos starbite/libsb`). It contains L2 (rebased from fb9114a, same
behaviour, the `next-linuxtest` self test) and L3. It supersedes `l2-next.patch` (kept for history, against fb9114a).

| File | What changes |
|------|--------------|
| `starbite/sbos/ui/shell.h` | the `sh_linux_*` API below (replaces L2's), `SH_LINUX_H` 1344, `SH_LINUX_HKB` 916 |
| `starbite/sbos/ui/shell_lock.c` | page 3 (`hparts[5]`, `build_home_apps3`, `lh_update`), three page dots (`hsearch[4]`), page swipes over three pages, `app_hit` / `icon_rect` for page 3. APPS gains `linux` (the open Linux app, `LX`) and `store`. The window is four strip layers `aplin[4]` (3.9 MiB each) filled through a row table, and the last strip is freed while the keyboard is up. The keyboard is one layer, `apkb`, shared with the store's search. Linux pages and the store's list draw no page strips. The App Store (`SS`, `page_store`, `build_store`, chunks `apstl[5]`, `apsth`, `apstb`). `G_STORE` (the list's scroll). `sh_set` gains `lapp=ID:v`, `store=PKG:v`, `storeq=TEXT:v` |
| `starbite/sbos/ui/host/main.c`, `host/linux_fake.inc` (new) | the host's pretend Linux: 6 apps, 14 packages in every state (QUEUED, INSTALLING 40 %, REMOVING, FAILED, TOO_BIG, NO_SPACE, SYSTEM, FEATURED). Icons are drawn and windows rendered with the pack's fonts (foot from `SBTK_LINUX=foot.ppm`); every call is logged as `linux: ...`. `SBTK_LINUX=cold` starts with Linux off, `=starting` / `=fail` as in L2. Still-only states: `lx=focus:1`, `lx=type:TEXT`. `SBTK_NO_LINUX_FAKE` drops it (gwin-host) |
| `starbite/sbos/ui/kern_next.c` | `next_kdraw`: `gwin_now()` every loop turn; `next_ready()` (as L2) |
| `starbite/sbos/build.sh`, `starbite/sbos/kern/uitest.c` | gwin.c in every Next image; `starbite-next-linuxtest.img` (as L2) |
| `starbite/guest/sbos/guest.{c,h}` | `GUEST_WL` mode (L2), the surface 720 x **1344**, `cfg.v2 = 1` for GUEST_WL (the tables cleared at load), `guest_blit_rows()` (the 2x copy into a row table), `guest_keep(on)` (no idle stop while the App Store is open or a package operation runs). **guest.{c,h} belong to the starbite-os session**: the same change should go to master (it only adds things) |

New files outside the toolkit: `starbite/guest/next/gwin.c` (the platform), `host/` (gwin-host), `wl-bundle.sh`.

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
- A film through the real gestures (with `SBTK_HEAP_MB=160`, the phone's heap: no refusal, peak 148 MB): unlock, two
  swipes to page 3, App Store, the field, `f i r e`, return, GET on Firefox ESR, the progress to OPEN, OPEN (it
  springs from page 3's new icon, "Opening Firefox ESR"), tap the address bar, `h i`, return, home, the icon again
  (raised at once), home.
- `host/run.sh TREE PAK OUT` (gwin.c with the real shell and a v2 fake guest): the self test PASSED (foot from the
  catalog, LAUNCH with layout_seq, foot's TEXT_FOCUS still typed as keys), then 12 L3 checks PASSED: page 3, a
  Linux app's launch / activate / viewport, the field's focus (viewport 916, text as SBG_MSG_TEXT, Return a key),
  home (ACTIVATE none), the store's GET (SBG_MSG_PKG with the store's seq), the install to the end, no Remove for
  SYSTEM, the hold released. `GWIN_L2=1` (an L2 guest, no catalog): PASSED. `GWIN_STORE=none`: fails with the
  store's reason.
- The whole `starbite/sbos/build.sh` at d8a69de + the patch + gwin.c + the current sbghost / sbshm.h in the Lima VM:
  every image builds and links (2026-11-11, /mnt/build/l3tree), host tests 320 + 680 checks passed;
  `starbite-next-linuxtest.img` 19a473be..., `starbite-next.img` d69b72d9....

## Needed from the guest side (sbwl / sbpkg)
- sbwl: send TEXT_FOCUS after each ACTIVATE (the shell assumes no focus for a newly activated app), and WINDOW
  (index, 1) only once that app's window is in the surface. gwin.c shows no rows before then.
- The catalog should list foot as `org.codeberg.dnkl.foot` (or `foot`). Its pool icons go under the catalog's seq,
  the store's under the store's seq.

## Not done (follow-ups)
- An app that quits by itself while shown leaves its last frame (WINDOW has_window 0 is not shown yet): the page
  could close or show "<name> Closed".
- No switcher: background apps live until guest.c's 60 s idle stop (they are killed then). There is no "quit".
- The catalog is not stored: after a phone restart, page 3 shows only the App Store until Linux has run once.
- Search (the home's pill) does not list Linux apps. Page 3 holds 19 apps; a fourth page is not built.
- The keyboard still has no Ctrl / Tab / Esc / arrows or `| > ~ <` (the toolkit's keyboard), as in L2.
