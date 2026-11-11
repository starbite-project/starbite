# L2: Linux apps in Starbite OS Next: the drop-in for the toolkit (claude/sbos-ui-p1-921bbf)

Design: docs/STARBITE-LINUX-APPS.md (L2). The **Terminal** app on page 2 is now a Linux app's window: foot, a real
Wayland terminal, runs under sbwl (our Wayland compositor) on the guest kernel. That kernel runs on the four M3
cores next to Starbite OS Next. The window is a `tk_layer` of the app's page between the status bar and the
keyboard. Taps and drags in it reach foot as `wl_touch`, and Next's own keyboard types into it. Popups (menus,
tooltips) are composited into the window by sbwl itself (L2's guest side, commit 8e5c8d0).

Base: the toolkit tip **fb9114a (P3m)**. `l2-next.patch` (in this folder) applies to it cleanly
(`git apply l2-next.patch` from the repo root). It touches 8 files, all at fb9114a's state:

| File | What changes |
|------|--------------|
| `starbite/sbos/ui/shell.h` | the `sh_linux_*` API (below), `SH_LINUX_W/H/Y` = 720 x 916 surface px at 2x from y 162 px (44 pt) |
| `starbite/sbos/ui/shell_lock.c` | the Terminal as an own page: the window layer `aplin` + a state line `aplst` (children of `appc` after the strips, so they fade with the page); `page_terminal` (black window area + `kb_draw`); `terminal_tap` (`kb_hit` -> `sh_linux_char`, shift / 123 like Notes); `G_LINUX` (touch down in the window -> `sh_linux_touch` down / move / up); `app_open` -> `sh_linux_open`; `app_finish_close` -> `sh_linux_hide` and frees the 10 MiB bitmap; `sh_set("app", "", 0)` closes the open app (the self test); the Terminal's info-page row removed |
| `starbite/sbos/ui/host/main.c` | the host's fake platform: `SBTK_LINUX=frame.ppm` (720x916 P6) shows that frame, `=starting` / `=fail` those states; `SBTK_NO_LINUX_FAKE` leaves them out (gwin-host links the real gwin.c) |
| `starbite/sbos/ui/kern_next.c` | `next_kdraw`: `gwin_now()` every loop turn (a state change or a new frame -> one frame drawn when not ticking and the screen is on); `next_ready()` |
| `starbite/sbos/build.sh` | `starbite/guest/next/gwin.c` in every Next image; `starbite-next-linuxtest.img` (`-DSBOS_SELFTEST_NEXT_LINUX`) |
| `starbite/sbos/kern/uitest.c` | the `next-linux` self-test variant (phases 97 / 94): own 30 s watchdog, `gwin_selftest_step` every 100 ms once the shell is baked, 120 s at most, then recovery |
| `starbite/guest/sbos/guest.{c,h}` | a third mode `GUEST_WL` beside APP and TERM: store entries `guest-wl.img` / `.dtb` / `-rd.img`, surface 720 x 916; the host fake's marker check for any non-APP mode. **guest.{c,h} belong to the starbite-os session**: the same change should go to them for master (it only adds a mode) |

New files, outside the toolkit's folders: `starbite/guest/next/gwin.c` (the platform side), `wl-bundle.sh` (the three
store files), `host/` (gwin-host: gwin.c + the real shell + a fake guest, on the Mac).

## The API (shell.h)
```c
/* platform (gwin.c on kern/guest.c) */
int  sh_linux_open(void);       /* start the guest (GUEST_WL) if off, show it; < 0 refused (sh_linux_state says why) */
void sh_linux_hide(void);       /* MSG_FOCUS 0; guest.c stops the guest after 60 s hidden */
int  sh_linux_blit(uint32_t *px, int stride_px, int first, int *y0, int *y1);   /* guest_blit at 2x: new rows only */
int  sh_linux_touch(int phase, int x, int y);    /* surface px -> sbg_touch */
int  sh_linux_char(char c);     /* guest_type: the Linux key with Shift around it; '\n' Return, '\b' BackSpace */
/* shell */
void sh_linux_state(int state, const char *why); /* GUEST_* order: the state line redrawn */
int  sh_linux_frame(void);      /* the window takes the new rows; 1 = draw a frame */
```
The window bitmap is the guest's frame at 2x, pixel-doubled by `guest_blit` (crisp text, no bilinear), opaque, so
the compositor draws nothing under it. A new frame dirties the whole layer (1440 x 1832). The row damage that
`sh_linux_blit` returns (`*y0..*y1`) is there for a future partial-damage `tk_layer` call.

## Checked
- `sbtk-host` builds with `-Werror`. Stills: `SBTK_LINUX=foot.ppm sbtk-host PAK still out.ppm app=terminal:1` (foot's
  real frame from QEMU), `SBTK_LINUX=starting` ("Starting Linux"), `SBTK_LINUX=fail` ("Linux Didn't Start" + why).
- A film through the real gestures: unlock, page 2, tap Terminal, tap and drag in the window, type `l` `s` Return,
  shift + `l`, swipe the app closed. The platform got: touch down/up at 360,472 (195,300 pt), the drag's moves,
  `l` `s` `\n` `L`, then the window hidden.
- `starbite/guest/next/host/run.sh TREE PAK OUT` (gwin.c + its self test with the real shell, fake guest): PASSED
  (open, first frame, the command typed through the keyboard path, the marker, 81 window rows with text, the tap's
  2 touch messages, closed, off). With `GWIN_STORE=none` it fails with "the Linux apps' files are not in the store".
- The whole `starbite/sbos/build.sh` at fb9114a + the patch in the Lima VM: every image builds and links, the host
  test passes (680 checks); `starbite-next-linuxtest.img` 195fab72...
- The guest at 720 x 916 on QEMU (run-term.sh MODE=wayland SURF=720x916): PASS 1/1. HELLO, foot mapped at
  720x916, the typed command ran, the panel matched the guest's frame row for row, the tap arrived as wl_touch, and
  the cluster was off 676 ms after the stop. foot's font is 20 pt now (sbwl/mk-initrd.sh): 45 columns, the 1.2
  terminal's width (11 pt gave ~81 columns, under 1 mm a character on the phone).

## Not in L2 (follow-ups)
- The keyboard has no `|`, `>`, `~`, `<`, Tab, Ctrl or arrows (Notes' layout). A terminal needs a third layer
  (iOS's `#+=`) and a key row for Ctrl / Tab / Esc / arrows. The toolkit owns the keyboard.
- Partial damage: one tk call to redraw only rows y0..y1 of a layer would cut each keystroke's composite from
  1440 x 1832 to a few rows.
- Every app beyond the Terminal: icons from .desktop files, the store, storage (L3+ in the design).
