# Starbite OS Next: Linux apps and an app store (design, 2026-11-11)

Goal (the user's): Starbite OS Next, the iOS-style bare-metal OS (branch `claude/sbos-ui-p1-921bbf`), runs "the full
Linux apps and kind of an app store too". This document builds on what already runs on the phone
(docs/STARBITE-OS-LINUX-M3.md): Linux 7.1 alone on the four M3 cores next to Starbite OS. It covers the shared
memory contract, starting and stopping the cluster, a surface Starbite composites, touch and keys, and a terminal.
It plans the rest and states what fits. Milestone L1 (section 9) is the first Wayland app whose window is a layer.

## 1. The shape

```
 A55 x4: Starbite OS Next                                 M3 x4: guest Linux 7.1 (7.1.0-sbguest)
 ┌──────────────────────────────────────────┐            ┌──────────────────────────────────────────┐
 │ sbtk shell: home, app pages, keyboard    │            │ sbwl: our Wayland compositor (wlroots)    │
 │   a Linux window = one tk_layer          │◄──frames───┤   one headless output per app window      │
 │ guest service (kern/guest.c, sbghost)    │──input────►│   wl_touch / pointer / xkb keyboard       │
 │ storage, Wi-Fi, power, the store UI      │◄─blocks────┤ apps: foot, GTK, ... (Debian arm64)       │
 └──────────────────────────────────────────┘  shared    └──────────────────────────────────────────┘
                                              memory 64 MiB
```

Starbite OS owns every device: display, touch, storage, radios and power. Linux owns only its cores and its RAM.
Everything an app needs from the hardware crosses the shared region, so a crashing app or guest never takes the
phone with it. The exception is memory: the guest kernel can still write anywhere, because there is no stage 2 to
stop it (docs/STARBITE-OS-LINUX-M3.md 8).

## 2. What already works (phone-proven, 2026-11-08 .. 09)

- The cluster cold-starts on demand (HELLO 98–120 ms after CPU_ON) and powers off after the stop (50–140 ms).
  It restarts in the same boot (P2b–P2d), hosted by Starbite OS itself (P3b, 1.2-alpha2).
- A surface Starbite composites, with only the changed rows copied (the row table), touch → uinput, and keys
  through `sbshm_ascii_key`.
- The guest's own kernel (`linux/guest.config`, 6.7 MB): simpledrm, a terminal (P4, photographed).

## 3. Windows become layers

**In the guest: sbwl** (`starbite/guest/sbwl`), a small compositor on wlroots 0.17 (headless backend, pixman
renderer; no GPU in the guest, the Mali stays Starbite's).

- **One headless output per app window ("window slot").** The output is the size Starbite gives the window: the
  app page's rect, in px. The app's toplevel is maximized into it. The output's frame is then the window as the
  user should see it, with popups, subsurfaces, decorations and scaling done by wlroots' scene renderer. One copy
  per frame, not one per client buffer.
- **Each frame** goes to the slot's buffers in shared memory, in the same way as today's surface: the changed rows
  into the back buffer, `row_seq` for them, the flip.
- **L1 uses one slot: the existing surface.** The contract's next version (v2) has N slots: per slot an id, the
  app id, title, state, size, stride, two buffers, its own row table and seq.

**What L1 does today, and what it found.**

- sbwl publishes the toplevel's own committed buffer: wl_shm, ARGB/XRGB8888, copied with the alpha forced opaque.
  That's exact for single-surface apps like foot, but popups and subsurfaces don't show yet.
- wlroots 0.17's pixman scene renderer, on the headless output here, rendered scene rects but left the client's
  texture out (the texture existed, opacity 1). After the first frames, the output buffer's CPU access also failed.
- Making the per-window output path work (or an own pixman composite of the surface tree) is L2's first item;
  wlroots 0.18 is the other way out.
- The guest also needs, beyond the terminal's init: a tmpfs on `/dev/shm` (wlroots' shm allocator uses `shm_open`),
  `/dev/pts` (the terminal's PTY), and the C.UTF-8 locale files.

**In Starbite OS Next: one `tk_layer` per window slot** (the `gwin` module, section 8):

- Its `bmp` is the slot's front buffer, converted to sbtk's premultiplied ARGB. Apps are opaque (XRGB, alpha
  0xff), so it is a copy with the alpha byte set.
- Only rows newer than the layer's last update are copied (`sbshm_row_newer`), then `tk_layer_dirty`.
- The shell treats it as an app page's content. The app-open spring from the icon, the close into the icon, the
  page's clip and corner radius, and the switcher all work on it like on any other page (shell_lock.c
  `app_open` / `app_close` / `set_app_rect`).

**Sharpness:** the panel is 1440 px wide.

- L1 renders the window at 720 px and sbtk draws it at 2× (cheap, slightly soft).
- L2 makes the output 1440 px with `wl_output.scale = 2`. Apps lay out in 720 logical px and draw 1440 real px:
  text as sharp as Starbite's own, at 4× the pixels per full frame. The row table keeps typing cheap.

## 4. Input

**Touch.** The shell hit-tests first: its gestures win, such as the home swipe from the bottom bar and the
switcher. A touch inside a Linux window layer becomes `SBG_MSG_TOUCH` (window slot, finger, phase, x/y in window px):

- for clients that bound `wl_touch`, sbwl sends real `wl_touch` (multi-finger);
- otherwise, pointer emulation: the first finger becomes motion plus the left button, so every Wayland app is
  usable.

**Keyboard.** Starbite's own iOS-style keyboard (shell P3e, Notes) is the only keyboard. It comes up when the
guest app wants text:

- **L2:** sbwl implements `text-input-v3` and sends `SBG_MSG_TEXT_FOCUS` when the app focuses a text field.
  The shell shows its keyboard, shrinks the window's rect (the app gets a configure with the new size), and
  commits words with `SBG_MSG_TEXT` (`commit_string`).
- **Apps without text-input** (terminals) get key events through the xkb keyboard (`sbshm_ascii_key`, Shift
  around capitals). A key strip adds Esc, Tab, Ctrl and the arrows.

**System gestures.** The home swipe hides the window (`SBG_MSG_FOCUS 0`, the app keeps running). The back swipe
sends `KEY_BACK`, and the app decides.

## 5. Apps on the home screen: lifecycle and icons

**An app is a `.desktop` entry in the guest** (Exec, Name, Icon). The guest's catalog (L3) is a small file in the
shared region or the store: per app an id, name, exec, and a 180×180 RGBA icon rendered from the theme by the
guest at install. The shell shows Linux apps beside its own, with a small badge so the user knows which is which.
Tapping one:

- starts the guest if it is off (the cluster on, HELLO ~0.1 s, sbwl ~0.3 s);
- sends `SBG_MSG_LAUNCH(app id)`; sbwl starts the app and binds its first toplevel to a free window slot;
- the shell shows the icon on the app's tint (the splash), and the open spring finishes on the first frame.
  A cold foot takes ~0.5 s on QEMU, the phone being faster; a GTK app 1–2 s.

**Closing:**

- the home swipe keeps the app running in the background (no frames while hidden, so no cost but RAM);
- quitting it from the switcher sends `xdg_toplevel.close`, then SIGTERM, then SIGKILL after 3 s.

**The cluster:** with no Linux app open or in the background for 60 s, the guest stops and the M3 cluster
powers off, as today. A background app is killed then, with a "the app will be closed" note in the switcher.

**Memory pressure:** the guest's guestd/sbwl watches PSI and MemAvailable. Background apps go first, oldest
first. Their last frame stays as a snapshot in the switcher, and opening one starts it again.

## 6. Storage

Starbite OS owns UFS and the microSD; the guest only gets blocks through the shared region.

- **sbblk, a block ring:** read/write 4 KiB blocks of one store entry or one file on the card, served by Starbite
  OS's storage path. Writes are fenced to that entry, as the fence does for any OS.
- **The root:** a read-only base image (EROFS: compressed, fast random reads, in our kernels) with the
  distribution's user space. Over it, a writable overlay for installed apps and user data: an ext4 image on the
  card, or a volatile store entry.
- **Sizes (Debian arm64, trixie):** a minimal base with Wayland, GTK3/4 and fonts is about 350–500 MB as EROFS.
  foot is 1 MB, a GTK4 text editor 5–10 MB of its own, Firefox ESR about 250 MB installed.
- **The user's files:** `/home` lives on the overlay. The shell's Files app can show it later through the same ring.

## 7. The App Store

**The source: Debian arm64, not Flathub, at first.**

| | Debian arm64 (apt) | Flathub arm64 (flatpak) |
|---|---|---|
| per app on disk | its package + what the base lacks (small for GTK apps) | the app + a runtime (GNOME 47/48 ~1 GB, KDE ~1 GB), shared |
| RAM | shared libraries shared across apps | a runtime's libraries per runtime |
| arm64 coverage | almost everything in Debian | most apps; some x86-only |
| needs in the guest | dpkg/apt, signed Release files (gpgv) | flatpak, ostree, bubblewrap (user namespaces) |
| updates | apt | flatpak update |

Flathub's runtimes alone are about 2× the guest's RAM in size, so they belong to a later "full" mode with more card
space. Debian's packages fit the 1.5 GiB guest and the card now.

**The store UI is Starbite's own** (an sbtk app in the shell's style). It shows a curated, signed index built on
the host from Debian's package lists: name, summary, screenshot and icon, download size, the RAM it needs, a "works
with touch" note, and a category. The rest of Debian is reachable by search.

**Install:** the store asks the guest (`SBG_MSG_PKG`). The guest runs apt in the overlay over network (section
10), verifies Debian's signatures, renders the app's icon into the catalog, and the app appears on the home screen.
Remove and update work the same way. Progress comes back as messages, and the store shows it.

**What fits in RAM** (the guest has 1.5 GiB; kernel + sbwl + base ≈ 80 MB):

| App | RSS, measured on arm64 Debian / estimate | One at a time? |
|---|---|---|
| foot (terminal) | 15–25 MB | many |
| GTK4 text editor, calculator, file manager | 60–150 MB each | 4–6 together |
| LibreOffice Writer | 250–400 MB | 2–3 with small apps |
| Firefox ESR (software rendering) | 350–700 MB | yes, plus small apps |
| Chromium | 400–800 MB | yes, alone |
| Games with 3D | GLES on llvmpipe only (no guest GPU) | not real-time |

There is no GPU in the guest: the Mali is Starbite's. Apps render in software with pixman, cairo, Skia or
llvmpipe on four M3 cores. That is fine for 2D apps and browsers, but WebGL and 3D are slow. A later step could
lend the Mali to the guest through a command ring (virtio-gpu style), which is a large project.

## 8. Starbite OS Next integration (the `gwin` module)

**Built for L2** (`starbite/guest/next/`, INTEGRATION-NEXT.md; a patch on the toolkit's fb9114a): one window,
the Terminal's. The shell calls the platform through `sh_linux_open / _hide / _blit / _touch / _char`, and the
platform calls the shell back through `sh_linux_state / _frame` (shell.h). `gwin.c` is the platform side on
kern/guest.c (a third guest mode, `GUEST_WL`: `guest-wl.img` / `.dtb` / `-rd.img`, surface 720 x 916). Section 12
has the details. The general form below (window slots, several apps) is L3's:

- `gwin_layer(slot)` returns the `tk_layer` (the bmp sized to the page rect);
- `gwin_poll()` runs from the kernel's tick and copies the new rows (`tk_layer_dirty`);
- `gwin_touch(slot, phase, x, y)` from the shell's hit test inside an open Linux page;
- `gwin_key` / `gwin_text` from the shell's keyboard;
- `gwin_launch(app)` / `gwin_close(app)` from the home and the switcher;
- the APPS table gains the catalog's Linux apps. The existing "Terminal" entry ("Linux apps run in Starbite OS")
  becomes the first: foot.

## 9. Milestones

| | What | Proven where |
|---|---|---|
| **L1** | sbwl + one Wayland app (foot) in the guest; its window in the shared surface as a layer; touch and keys reach it | **QEMU PASS 2/2** (`run-term.sh MODE=wayland`): foot's window composited exactly, the typed command ran in foot's shell, the tap reached foot as `wl_touch`, cluster off ~650 ms after the stop; phone: P5 staged (section 11) |
| **L2** | the `gwin` drop-in in Starbite OS Next: the Terminal opens foot's window as its page (720 x 916 at 2x between the status bar and the keyboard), touch and the shell's keyboard reach it; popups and subsurfaces composited by sbwl (pixman, the window's surface tree) | **QEMU PASS** (popups; foot at 720x916), **host PASS** (the real Next shell + gwin.c + a fake guest; a film through the real gestures), every Next image builds; phone: `starbite-next-linuxtest` staged (section 12). Not done: 1440 px at scale 2, text-input-v3 (moved to L3) |
| **L3** | contract v2 (sbshm.h: catalog, icon pool, store table, block rings, text input); the guest catalog from .desktop files; Linux icons on home page 3; launch / activate / close; text-input-v3 with Next's keyboard (viewport 916 / 1344) | **done**: sbwl v2 hostsim 73/73 (trixie, wlroots 0.18); next-linux.patch in Next P3r (toolkit 5fc1067); every image links. Not yet: an app switcher (background apps stop after 60 s hidden) |
| **L4** | sbblk, EROFS base + overlay (Debian trixie arm64 + GTK) | **done on QEMU**: sbblk read / persist / stress / fallback PASS; the full Debian guest (root switch, sbinit, zram, sbwl v2, foot) RESULT PASS. Phone: the three images as contiguous files on the card's exFAT p1 behind a libsb fence grant for overlay.img only (blk/guest-card.patch; card-test 60/60); staged 2026-11-11 |
| L5 | network: a packet ring bridged by Starbite's Wi-Fi core (the radio stays Starbite's) | not started: installs come from the offline repo on the card |
| **L6** | the App Store: curated index, apt installs in the overlay | **done (offline)**: repo.erofs (23 entries, 424 .debs), sbpkg (install / remove / cancel, progress), the store UI (search, detail, honest RAM and space); test-sbpkg ALL PASS, also with the guest's 1970 clock. Not yet: signed index, updates |
| L7 | Firefox ESR; memory pressure handling; the Mali lent to the guest (research) | Firefox ESR installs (800 MB measured peak for one local page; TOO_BIG ~890 MB), zram swap; no GPU |

## 10. Network (L5, needed by the store)

A packet ring over the shared region, `virtio-net` style: the guest sees an ethernet device, `sbnet0`, and Starbite
OS forwards frames through its Wi-Fi core. NAT runs in Starbite, so the guest has a private address and the phone
one Wi-Fi identity. DNS goes through Starbite's resolver. Without it, the store can only install from a package
cache on the card, which is useful offline.

## 11. Milestone L1 on the phone (P5)

An unattended payload, like P4:

- it starts the guest (guest kernel, Wayland initramfs: sbwl + foot + libraries, 9.6 MB gz);
- composites the foot window at 2× between Starbite's bars;
- types `uname -a; ...; echo STARBITE-TERM-OK > /dev/kmsg` into foot as key events, and checks the marker in the
  guest's log;
- taps inside the window (sbwl logs which surface the touch reached);
- stops, and checks that the cluster is off.

Results: see docs/STARBITE-OS-LINUX-M3.md and the request note.

## 12. Milestone L2: the Terminal in Starbite OS Next

**What it is.** Tap Terminal (page 2) and the app opens like any other. Its page is foot's window from y 44 pt to
540 pt (720 x 916 surface px, pixel-doubled to 1440 x 1832), with Next's keyboard below it. The first time it shows
"Starting Linux" until the guest's first frame (~1.5 s on QEMU). Taps and drags in the window go to foot as
`wl_touch`. The keyboard types into it (`guest_type`: the Linux key, Shift around capitals). Closing the app hides the window
(`MSG_FOCUS 0`), and guest.c stops Linux after 60 s hidden. Opening it again within that time shows the running
shell at once.

**Guest side (commit 8e5c8d0).** sbwl now composites the window's whole surface tree with pixman, in the order
`wlr_xdg_surface_for_each_surface` gives: the toplevel, its subsurfaces, its popups, each at its offset from the
window geometry. Popups are constrained to the window's box (`wlr_xdg_popup_unconstrain_from_box`). The wlroots
scene renderer is no longer used for the window. `popuptest` (a blue window with a yellow popup at 100,100) passed
sbsim's check: yellow at the popup, blue beside it.

**Sharpness.** foot at 20 pt DejaVu Sans Mono gives 45 columns x 29 rows in 720 x 916, the 1.2 terminal's width.
At 11 pt it was ~81 columns, under 1 mm a character on the phone. Pixel doubling keeps the edges hard. Real 1440 px
(`wl_output.scale = 2`) is still the better answer, for apps that scale.

**Checks.** See starbite/guest/next/INTEGRATION-NEXT.md: QEMU (foot at 720x916: PASS), the host stills (running /
starting / failed), a film through the real gestures, gwin-host (gwin.c's self test with the real shell: PASSED),
and the full sbos build with the patch.

**Phone (staged in build/store, request note sbos-linuxapps-l2-request.md):** `starbite-next-linuxtest.img` (Next
at fb9114a + the L2 patch, `-DSBOS_SELFTEST_NEXT_LINUX`: own 30 s watchdog, 120 s limit, no storage writes, then
recovery) and the three store files `guest-wl.img` (the guest kernel 44acd54f), `guest-wl.dtb` and `guest-wl-rd.img`
(sbwl + foot, 9.7 MB). The test opens the Terminal through the shell, waits for foot's frame in the window, types
`uname -a; echo STARBITE-TERM-OK > /dev/kmsg` through the keyboard's path, checks the marker and text in the window,
taps the window, closes the Terminal, stops Linux and checks the cluster off.
