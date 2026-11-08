# P3: wiring the guest service into Starbite OS (for the starbite-os session)

`guest.c` / `guest.h` here are a drop-in for `starbite/sbos/kern/`, built on `starbite/guest/host/sbghost.c`, the
core the phone payloads P2..P2d ran. That is Linux 7.1 on all four M3 cores with the app, touch, the stop and the
cluster off (P2c, 2026-11-08 22:48). They compile clean (`-Wall -Wextra -Werror`, Starbite's flags) against
starbite-os 1.1-beta7b (1393334), with `-DKT_GUEST=11` until step 1 exists. Nothing in your tree was edited; these
are the edits, in order.

1. **kern.h**: `KT_GUEST` in the timer owners, before `KT_APP0` (11 is free: `..., KT_WIFI, KT_GUEST, KT_APP0 = 16`),
   and `#include "guest.h"` (or copy its declarations).
2. **build.sh**: the kernel sources get `$root/starbite/guest/sbos/guest.c` and `$root/starbite/guest/host/sbghost.c`
   (normal flags: both run on cpu0), and `-I$root/starbite/guest/sbos`. `guest.c` includes
   `../../guest/host/sbghost.h`, which resolves from either directory.
3. **main.c**: `dispatch_timer`: `case KT_GUEST: guest_timer(id); break;`. `sb_main`: `guest_init();` right after
   `hw_irq_init()` (it routes the host doorbell, INTID 510, edge, to cpu0, enabled).
4. **hw.c**:
   - `irq_drain`: before the stray branch (`/* nothing else is enabled ... disable it again */`),
     `else if (id == guest_db_host()) { guest_irq(); }`. Otherwise the first doorbell disables line 510.
   - `hw_spi`: the ITARGETSR update must be a **byte store**
     (`*(volatile uint8_t *)(GICD + 0x800 + intid) = me;`), not the word read-modify-write. The guest kernel
     routes its own lines with byte writes in the same words. Change ICFGR (a 16-line word) only while
     `guest_state() == GUEST_OFF`.
5. **heater**: refuse a start while `guest_owns_m3()` (both own the M3 cluster); `guest_start` already refuses
   while `heater_busy()`.
6. **services.c**:
   - next to `wifi_power(0)` in the switch-off / restart paths: `guest_power_off();` (MSG_STOP, waits up to 1 s:
     the guest powers its own cores and the cluster off);
   - where the screen state changes (`kui_screen_changed`): `guest_screen(on);`.
7. **ui.c**: the Linux view, a full-screen mode like SOS's:
   - a home tile "Linux", shown when `guest_available()`: `guest_start()` then `guest_show(1)`. While
     `guest_state() == GUEST_STARTING`, "Starting Linux…" (HELLO came after 119 ms on the phone, plus the store
     load, about 27 MiB from UFS); `GUEST_FAILED` shows `guest_why()`;
   - shown (`guest_shown()`): your status bar at y 0..39, then
     `guest_blit(ui_back(), UI_W, 0, 40)` (720×1440 at 2× = 1440×2880), your navigation bar at 2920..2959;
     present when `guest_frame_new()`. The service calls `ui_mark_dirty()` when the guest publishes a frame;
   - input while shown: touches go to `guest_input(type, x, y)` first (it returns 0 for presses on the bars),
     Volume to `guest_key(115/114, down)`, short Bixby to `guest_key(158 /* BACK */, ...)`. Long Bixby
     (`KIN_BACK`) is `guest_show(0)` and home: the guest keeps running in the background and is stopped after 60 s
     hidden or 60 s with the screen off (`guest_timer`). The pocket lock stays in front, unchanged.
8. **Memory**: Starbite OS must leave alone 0x8_8000_0000..0x8_E3FF_FFFF (guest RAM + the shared region) and
   0xa010_0000..0xa010_007f: the two CPU_ON trampolines, inside libsb's non-cacheable RAM-test window, which
   Starbite OS does not use. They must sit below 4 GiB; above that, the S9's PSCI never starts the core (P1/P1b).
9. **FlexPMU**: `guest.c`'s `m3_prep` = `hw_m3_fp_reset()` + `hw_m3_prep()` + `hw_m3_core_on(4, 1)` (your 1.1
   helpers). The guest does the rest itself. Its kernel (linux/0002) does core on/off around its own CPU_ON /
   CPU_OFF. sbguest.ko's last core asks for the cluster-off **itself, right before its own CPU_OFF**: CPD +
   APM mailbox bit 24. That powered the cluster off in P2c (and the firmware cleared bit 24), and P2d (23:24)
   restarted it twice in one boot (3/3 cycles, cluster off 65..141 ms each time). The same order would let
   the heater's last worker power the cluster down too.
10. **Clock**: the guest has no ACPM; set the M3 clock for it (`dvfs_request(DVFS_M3, ...)`) when the view is shown,
   lower it when hidden (P2d ran at the boot step, 1066 MHz).
11. **Store**: three entries from `starbite/guest/probe/build.sh bundle`. Today: `guest-linux.img` 898bb78a…
    (Linux 7.1 with linux/0001 + 0002), `guest-linux.dtb` c6544670… (S9 profile, doorbell 511), `guest-linux-rd.img`
    63f982f8… (busybox, guestd, sbguest.ko; the DTB carries this initrd's size, so rebuild them together).

First phone step for P3 (P3a, unattended): the self test starts the guest after its screens, waits for HELLO
(log `guest: HELLO after N ms, 4 CPUs`), shows the view for 5 s, sends a tap through `guest_input`, stops and
checks `guest_state() == GUEST_OFF` with `hw_m3_on() == 0`. The sbos-linux session can write that `selftest.c` hunk
against your current tree on request.
