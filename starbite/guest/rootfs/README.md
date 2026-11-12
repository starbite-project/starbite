# The guest's Debian root and the App Store repository (L4 + L6)

docs/STARBITE-LINUX-APPS.md sections 6 and 7. The guest Linux (7.1.0-sbguest, the four M3 cores, 1.5 GiB) gets
its root from three block devices Starbite OS serves (sbblk, `sbshm.h`):

| dev | name | image | made by | in the guest |
|---|---|---|---|---|
| 0 | base | `base.erofs` (EROFS lz4hc, label `sb-base`, read-only) | `mk-base.sh` | overlayfs lower |
| 1 | overlay | ext4 (label `sb-overlay`, `/upper` + `/work`) | `mk-overlay.sh` | overlayfs upper: apps, /home, /etc, /var |
| 2 | repo | `repo.erofs` (EROFS, label `sb-repo`, read-only) | `mk-repo.sh` | `/repo`: the store's apt repository |

The initramfs mounts 0 and 1 (and 2 on /repo), makes the overlay and `switch_root`s to **`/sbin/sbinit`** (this
directory's `sbinit`), a shell script that is PID 1: proc/sys/dev/devpts/`/dev/shm`/`/run`, the clock, zram swap
(lz4, half the RAM, swappiness 100), `/repo` if not mounted yet (`sbinit.repo=` or `findfs LABEL=sb-repo`), the
user's home on a fresh overlay, **sbpkg** (`../pkg`, root, in a restart loop) and **sbwl** (`/usr/bin/sbwl`, root, in
a restart loop; sbwl makes the user's session itself: /run/user/1000, the Wayland socket, a D-Bus session bus, and
starts the apps as `user`). Kernel command line: `sbinit.epoch=SECONDS`, `sbinit.repo=/dev/X`, `sbinit.zram=0`,
`sbinit.nopkg`, `sbinit.nosbwl`. sbwl's extra arguments: `/etc/starbite/sbwl.args`.

All builds run in the Lima VM (aarch64, no emulation); the outputs stay in `/mnt/build/sbguest2/rootfs` (`OUT=`).

Images there on 2026-11-11 (each with a `.sha256`, all fsck.erofs clean):

| image | sha256 (start) | what |
|---|---|---|
| base.erofs | 1b8e1b9a | staged for the first phone test: sbwl, the kmsg fix, the apt date fix |
| repo.erofs | 82b53519 | staged: 23 entries, ram_mb 0 (not measured yet) |
| repo-small.erofs | e5086a4c | 14 entries, 169 MB |
| base-v3.erofs | 18ddaa9e | + the clock (sbinit.epoch / the build date), the hidden helper entries |
| repo-v3.erofs | c7dd6783 | + the measured ram_mb, apostrophe + python3-chardet |

`test-sbpkg.sh` passes on every pair (FAKE_1970=1).

## base.erofs (`mk-base.sh`)

Debian trixie arm64 by mmdebstrap (`--variant=apt`, no Recommends), **no systemd** (pinned to -1), dpkg path
excludes kept in the image (docs, man, info, help, translations: also for what the store installs later):

- system: apt/dpkg, util-linux, procps, kmod, iproute2, dbus-daemon (session bus only), dconf, xdg-utils;
- fonts: DejaVu (terminal: DejaVu Sans Mono 20), **Inter** (the UI font; fontconfig `sans-serif` -> Inter);
- GTK 3 + GTK 4 + libadwaita, Adwaita + hicolor icons, librsvg (SVG icons), wlroots 0.18 (for sbwl), foot;
- system apps (SYSTEM in the store): Text Editor (gnome-text-editor), Calculator (gnome-calculator), Files
  (pcmanfm), Image Viewer (gpicview), Document Viewer (Papers, PDF), Terminal (foot). Picked for what they pull in:
  Nautilus brings gvfs-backends (samba), localsearch (tracker, GStreamer) and udisks2 (~150 MB; it is in the store),
  Evince needs ghostscript (~50 MB), eog needs libpeas + Python and iso-codes;
- helper entries hidden from the catalog (`hide-desktop.sh`: dpkg-divert + a NoDisplay copy): foot-server,
  footclient, pcmanfm-desktop-pref;
- user `user` (uid 1000, /home/user with Documents, Downloads, Pictures, Music, Videos, Desktop; XDG user dirs),
  locale C.UTF-8, hostname starbite;
- apt (`apt.conf` -> /etc/apt/apt.conf.d/90starbite): only `deb [trusted=yes] file:/repo ./` (Debian's mirror list
  is kept disabled for L5), no Recommends, no _apt sandbox, and **no date checks** (`Acquire::Check-Date`,
  `Check-Valid-Until` false): the guest has no RTC, its clock can say 1970 and the Release (2026) would be "not valid
  yet" (found in the QEMU end-to-end run; `test-sbpkg.sh FAKE_1970=1` reproduces it with libfaketime);
- every base package is marked manual, so no `--autoremove` of a store app can take a base package;
- `/usr/share/starbite/base-packages.txt`: what sbpkg treats as SYSTEM.

Options: `SBWL=` the compositor (default `/mnt/build/sbguest2/sbwl/out/sbwl`, installed by
`../sbwl/install-rootfs.sh`: /usr/bin/sbwl, /etc/sbwl/system-apps; empty = none), `SBPKG=` a prebuilt sbpkg
(default: `../pkg/build.sh`), `IMG=` the image's name (`$OUT/$IMG.erofs`; write new builds under new names),
`KEEP_ROOT=1` keeps the unpacked tree, `REFRESH=1` reuses it (only sbinit, apt.conf, the hidden entries, sbpkg and
sbwl change: a minute instead of a full bootstrap). Every image is checked with fsck.erofs and gets a `.sha256`.
sbinit also sets the clock: `sbinit.epoch=SECONDS` from the host (hdr.host_time_ms is Starbite's uptime, not a
date), else the base's build date if the clock is before 2025; and `chmod 0622 /dev/kmsg` (the self tests' marker
from the user's shell). Software rendering defaults set by sbinit: `GSK_RENDERER=cairo` (GTK 4 without GL), `NO_AT_BRIDGE=1`,
`MOZ_ENABLE_WAYLAND=1`, `GDK_BACKEND=wayland`, `SAL_USE_VCLPLUGIN=gtk3`; `/usr/games` is in PATH.

**Size (2026-11-11): base.erofs 202 MB** (544 MB unpacked, 334 packages, lz4hc with 64 KiB pclusters, fragments,
dedupe). The biggest: libllvm19 118 MB + libz3 26 MB + mesa-libgallium 33 MB (all three only because
libwlroots-0.18 depends on libegl1 -> mesa; sbwl renders with pixman and never uses GL, so a dummy libegl-mesa0 would
save ~65 MB of the image, but then nothing in the guest has GL at all), libicu76 37 MB, GTK 3 + 4 data 46 MB,
fonts-inter 21 MB. `$OUT/base-sizes.txt` has the list.

## repo.erofs (`mk-repo.sh`)

Input: `store-list.tsv` (key, title, summary, category, featured, apt packages, RAM workload). For each entry, an
`apt-get -s install` in a throw-away overlay of base.erofs gives exactly what it needs beyond the base; those .debs
are downloaded into `pool/`. Then `Packages`(.gz) and `Release` (apt-ftparchive; flat repo), the store index and
the icons, and `mkfs.erofs`.

Image layout:

```
/Packages /Packages.gz /Release      the flat apt repository (sources: deb [trusted=yes] file:/repo ./)
/pool/*.deb                          every package any entry needs beyond the base
/store/index.tsv                     the curated store index (below)
/store/icons/KEY.png                 160x160 RGBA, rendered from the app's .desktop Icon= (SVG via rsvg)
/store/icons/KEY.argb                the same, 160*160 little-endian 0xAARRGGBB words, premultiplied (the pool format)
```

### The store index (`store/index.tsv`)

UTF-8, one entry per line, tab-separated, `#` lines are comments; the line order is the store index (the `IDX` of
`op`/`pkg` lines and `sbshm_store.pkg[]`):

| # | field | example | sbshm_pkg |
|---|---|---|---|
| 1 | key | `firefox-esr` | `pkg` (also the icon file's name) |
| 2 | title | `Firefox` | `name` (cut at 31 bytes on a UTF-8 boundary) |
| 3 | summary | `Mozilla's web browser ...` | `summary` |
| 4 | category | Internet, Office, Graphics, Utilities, Games, Development | `category` |
| 5 | ram_mb | measured, `measure-ram.sh` | `ram_mb` |
| 6 | featured | 0 / 1 | `SBSHM_PKG_FEATURED` |
| 7 | dl_kb | sum of the .deb sizes beyond the base | `dl_kb` |
| 8 | inst_kb | sum of their Installed-Size | `inst_kb` |
| 9 | apt | `libreoffice-writer libreoffice-gtk3` | what `apt-get install` gets |
| 10 | ram method | how ram_mb was measured | (not in the table) |

dl_kb / inst_kb are against the bare base: with other entries installed (shared libraries) the real cost is lower.
`NAME=repo-small LIST=store-list-small.tsv mk-repo.sh` makes a smaller repository from a shorter list (same format).
The list's optional 8th column names a theme icon instead of the .desktop's Icon= (pcmanfm: folder).

**Sizes (2026-11-11): repo.erofs 369 MB** (424 .debs, 23 entries); repo-small.erofs 169 MB (143 .debs, 14 entries).

| entry | download MB | installed MB | packages beyond the base |
|---|---|---|---|
| firefox-esr | 63.1 | 246.3 | 5 |
| libreoffice-writer (+ libreoffice-gtk3) | 84.9 | 323.8 | 70 |
| libreoffice-calc | 87.4 | 329.4 | 77 |
| libreoffice-impress | 79.5 | 303.6 | 75 |
| apostrophe (WebKitGTK) | 95.0 | 484.3 | 107 |
| xournalpp | 7.5 | 23.8 | 19 |
| gimp | 62.4 | 298.6 | 112 |
| inkscape | 33.3 | 159.3 | 35 |
| drawing | 10.0 | 34.2 | 26 |
| gnome-chess (+ gnuchess) | 1.0 | 4.6 | 3 |
| gnome-sudoku / mines / 2048 | 0.7 / 0.5 / 1.7 | 3.9 / 3.1 / 8.1 | 5 / 4 / 17 |
| gnome-clocks / characters / baobab | 7.2 / 15.0 / 0.3 | 54.6 / 71.7 / 2.2 | 17 / 27 / 1 |
| nautilus | 37.0 | 195.9 | 134 |
| foot, text editor, calculator, pcmanfm, gpicview, papers | 0 (base) | 0 | SYSTEM |

## Honest RAM (`measure-ram.sh`, `measure-ram.py`)

A chroot of base.erofs with a tmpfs upper (nothing written to disk), every entry installed **from repo.erofs,
offline** (which also proves the repository resolves), cage from the mirror (wlroots 0.18 like sbwl: headless
backend, pixman renderer, a seat without devices; the output set to 720x1440 with wlr-randr: the app full-window as
on the phone; grim screenshots prove each app drew. Weston's headless backend has no seat, and Firefox then stalls
on its profile dialog: the first numbers were wrong),
the guest's environment (`GSK_RENDERER=cairo`, Wayland, no a11y bus). Each app runs as uid 1000 in its own
session with a real document (`gen-testfiles.py`: a 250 KB article, a 4000x3000 photo, a 20000-row sheet, a
2000-shape SVG, a 10-slide presentation, a PDF made by LibreOffice); every 0.5 s the PSS of all processes of that
session (smaps_rollup) is summed. **ram_mb = the peak PSS, rounded up to 10 MB.** cage, D-Bus and D-Bus-activated
services are not counted. Firefox's content sandbox cannot make user namespaces in the chroot (it can on the guest). The results go to `store-ram.tsv` (checked in), which mk-repo.sh puts into the index.

| entry | ram_mb | peak PSS / last 5 s / RSS (MB) | workload |
|---|---|---|---|
| firefox-esr | 800 | 790 / 579 / 1346, 10 procs | a local 250 KB article with tables, SVG, a canvas animation; real sites with ads and scripts need more |
| libreoffice-writer | 300 | 291 / 291 / 305 | a 25-page document |
| libreoffice-calc | 260 | 258 / 246 / 268 | a 20000 x 8 sheet |
| libreoffice-impress | 210 | 205 / 194 / 216 | a 10-slide presentation |
| gimp | 330 | 328 / 315 / 381 | a 4000x3000 photo |
| inkscape | 710 | 705 / 705 / 713 | a 2000-shape SVG |
| drawing | 90 | 87 / 87 / 95 | a 1600x1200 picture |
| xournalpp | 70 | 60 / 57 / 65 | a new notebook |
| gnome-2048 | 150 | 144 / 144 / 155 | idle main window |
| gnome-characters / chess / clocks / sudoku / mines / baobab | 70 / 60 / 50 / 50 / 50 / 50 | | idle main window |
| papers / text editor / calculator / gpicview / pcmanfm / foot | 90 / 70 / 70 / 70 / 40 / 20 | | a PDF / a Markdown file / idle / a photo / home / a shell |
| nautilus | 70 | 62 / 56 / 74 | the home folder |
| apostrophe | not measured | | Debian's apostrophe imports chardet without depending on it: the entry now installs python3-chardet too |

LibreOffice's headless conversion (`soffice --convert-to`) peaks at 223 MB RSS. gpicview's 70 is its peak while
decoding the 12 MP photo (it exits on its own in the run: to be checked). With the 1.5 GiB guest (MemTotal about
1490 MB) nothing is TOO_BIG; Firefox (800) and Inkscape (710) are close and are the ones to run alone.

**TOO_BIG** is computed by sbpkg at run time: `ram_mb > MemTotal - 600 MB` (about 890 MB for the 1.5 GiB guest;
600 MB = kernel caches, sbwl, the base daemons and room for the page cache), override `SBPKG_RAM_LIMIT_MB`.

## sbpkg (`../pkg`)

`../pkg/build.sh` builds it for trixie arm64 in the dev chroot (`$OUT/devroot`, made with mmdebstrap if missing).
See the head of `../pkg/sbpkg.c` for the behaviour; in short: maps the shared region up to `SBSHM_BLK_OFF`, fills
`sbshm_store` and icons 64.. under the seqlock, computes INSTALLED / SYSTEM / NO_SPACE (inst_kb + 64 MB reserve >
the overlay's free space) / TOO_BIG, is a client of sbwl on `/run/sbwl.sock` (reconnects every second; lines to a sbwl that does not read are dropped,
the table still holds the state), runs one
apt-get at a time from a queue (`APT::Status-Fd` -> `pkg IDX STATE PCT`, at most 4 a second), `rescan` after every
install/remove, `store SEQ` after every table change; `dpkg --configure -a` + `apt-get -f install` on start and
after a failure (apt's E:/W: lines go to the kernel log), a failed install that leaves dpkg unhappy is purged again; a removal is simulated first and
refused if it would take a base package; cancel: queued -> dropped; before dpkg -> apt-get killed; during dpkg ->
the install finishes and is removed again. It also writes `/run/sbpkg/apps.tsv` (`IDX<TAB>key<TAB>desktop-id` for
every installed entry's .desktop files) so sbwl can fill `sbshm_app.pkg`.

## The test (`test-sbpkg.sh`)

In the VM, no phone, no QEMU: a fresh 3 GB overlay image (`mk-overlay.sh`), base.erofs + overlay + repo.erofs on
`/repo` mounted like the initramfs will, sbpkg in a chroot of that against a 32 MiB plain file standing in for the
shared region (`SBPKG_SHM`, `SBPKG_MEM_MB=1490` for the guest's MemTotal), and `fake-sbwl.py` serving
`/run/sbwl.sock`: it reads the table with the seqlock rules, installs firefox-esr, checks the progress lines, the
flags, `firefox-esr.desktop`, `apps.tsv`, queues two installs and cancels the queued one, sends a stale op,
tries to remove a system app, removes firefox-esr and checks the overlay space comes back. Options: `BASE=`, `REPO=`
(images), `SBPKG=` (a fresh sbpkg build), `TEST_EXTRA="libreoffice-writer ..."` (install + remove more entries),
`FAKE_1970=1` (the guest's clock: libfaketime at 1970-01-01 for sbpkg and everything it runs).

Result (2026-11-11, base 1b8e1b9a + repo 82b53519, FAKE_1970=1 TEST_EXTRA=libreoffice-writer): **ALL PASS**.
firefox-esr installed in 5 s (18 progress lines 1..95, IDLE 100, rescan, firefox-esr.desktop, apps.tsv), 2977 ->
2730 MB free, removed in 1 s, 2976 MB free again; the queued second install cancelled to IDLE; a stale op ignored;
`remove papers` -> FAILED "a system app"; libreoffice-writer installed in 23 s, removed in 5 s, space back.
Against repo-small: ALL PASS.
