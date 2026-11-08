# Starbite OS 1.0-rc3: release note (2026-11-08)

Starbite OS is the Galaxy S9's (SM-G960F, Exynos 9810) own small system: an emergency phone that boots in a
fraction of a second after the loader, with a light, a heater, an alarm, a medical card, Wi-Fi, a clock and the
battery, in a normal-mode phone shell with an Emergency Mode one switch away. It runs bare on libsb (no Linux),
from RAM; it writes only its own save slot. Guide: `docs/STARBITE-OS-GUIDE.md`; design and tests:
`docs/STARBITE-OS.md` (12.13 .. 12.19 for 0.6 .. 1.0); the plan after 1.0: `docs/STARBITE-OS-PLAN-0.6-0.9.md`.

## rc3: from rc2's first run on the phone (2026-11-08, 17:31-17:40)

Everything worked except two things; rc3 changes only these (and the self-test images' names).

1. **Settings > Alarm > LED at rated power** was refused ("The LED chip did not answer"); the log went with the
   hardware power cut. From the code: S-Boot never sets up the LED chip's bus (USI10 at 0x10920000), unlike the touch
   and charger buses libsb already drives, so its IP clock is in its reset state. Linux sets it with
   `clk_set_rate(200 MHz)` (CMU_PERIC1's IP_USER mux and the USI10 divider); fled.c only reads the CMU (it checked
   only the four gates' manual-closed state) and used timing for 200 MHz. No power-up or enable step is missing on
   the chip's side (its supply is always on and it has no enable pin); the address (0x59) and the bus (hsi2c_24) are
   the vendor DT's. Also, every refusal showed the same text, and a refusal stuck until a restart. rc3:
   - the log has everything that decides it: the CMU_PERIC1 gates, IP_USER mux, divider and Q-channel, CMU_TOP's
     PERIC1_IP gate / divider / mux, the controller's registers before and after the bring-up, the SCL / SDA levels
     before the pins are switched, and after a failed read the controller's INT_STAT / FIFO / TRANS_STATUS (no ACK =
     the bus ran and the chip did not answer; nothing at all = the controller never clocked);
   - Settings says which: the clock closed, the bus ran but no ACK, the bus never ran, another chip, an unexpected
     mode; each switch-on tries again;
   - `starbite-os-1.0-rc3-ledprobe.img`: a one-shot image that runs only that probe (the bus bring-up and the chip's
     reads; nothing written to the chip, no heat, no screens, no Wi-Fi), logs to pmsg and restarts to the menu after
     about 8 s (`selftest: passed: LED probe` or `selftest: FAILED: LED probe: <why>`).
   Host-tested (`starbite/host/sbos-led-test.sh`: the chip answering, no ACK, no clock, the retry).
2. **The minutes-left estimates** used 1.9 A and 30 mAh per percent, which promised about twice the time. The run's
   record (the sbos-state slot): Manual flat out, 7 cores, 2.8 A at 3.67 V (about 10 W, peak 2.86 A); the gauge fell
   100 -> 91 % in 183 s and to 90 % at the hardware cut: about 3 % a minute. In coulombs that is about 5 % of
   3000 mAh, so under this load the gauge's percent is worth about 16 mAh. Both estimates (the confirmation and the
   live "min to the reserve") now use 2.8 A and 16 mAh per percent (`sbapp.h` SBOS_HEAT_FULL_MA /
   SBOS_HEAT_MAH_PER_PCT): from 100 % with the 15 % reserve, about 29 minutes instead of 80. One run from a full
   charge; more runs refine it. The "about 7 W" line now says 10 W.

## rc2: fixes from an independent safety review of rc1

1. A fuel gauge that stops answering mid-heat stopped none of the charge / current / voltage stops (they were
   skipped without a reading): heat now stops after 10 s without a gauge reading (every level; host-tested).
2. A stop saved the session record (UFS, FUA + cache flush, retries) before turning the hardware off: now the
   cores, the GPU, its voltage, the clocks, the panel boost and the LED go off first; the record is saved after,
   deferred (`sbos-heat-test.sh` checks the order). The power menu's switch-off saves it explicitly.
3. The power-off waited without a limit for every key to be released: a stuck key blocked it (also the 3 %
   switch-off). Now at most 5 s, then it goes ahead (logged; unit-tested).
4. Manual's GPU step (598 MHz) carried over into High's GPU share: every non-Manual start resets it to 572 MHz.
5. The "go on below the reserve" hold turned the reserve off for every later session: it now overrides the next
   session only, and that session's confirmation and screen say the reserve is off.

## What works

- **Normal mode**: lock screen (holds against pocket touches), home with a clock widget and glass icons, wallpapers,
  the control center (tap the status bar): Emergency Mode, flashlight, Wi-Fi, brightness, wallpaper.
- **Emergency Mode**: the big black emergency screens; saves and switches the phone off 5 minutes after the
  screen goes off; at most Mid brightness and frames only on change.
- **Light**: the LED, or the screen as a light (white, warm, red).
- **Alarm (SOS)**: the screen flashes white and red at the panel's top normal brightness; the LED strobes with it.
- **Heat**: Low and High (regulated, 30 minutes, stops at 42 °C battery / 46 °C back), **Extreme** (every core at
  its top table step, the GPU, the screen, the LED: flat out until Off), **Manual** (pick each cluster's table step,
  A55 up to 1950, GPU up to 598 MHz, with warnings), **Keep the battery warm** (10 / 20 / 30 °C target). The screen
  stays on while heating with a pocket lock (Power unlocks). Live mA / W / minutes left. A record of each session
  survives a forced power-off.
- **Medical card** (Info): written on the phone, shown read only from the lock screen.
- **Wi-Fi**: scan, open and WPA2 networks (passphrase typed, kept in RAM), DHCP, the router's ping, the clock from
  the internet (NTP). The join never blocks the screen.
- **Clock** (world clock, stopwatch, timer), **Battery** (charge, estimate, a chart), **Settings** (saved on the phone).
- **Self test** (the `-selftest` image): every screen and app, Wi-Fi (and a join of the saved network), the save
  slot, the audio codec's state (read only); one line per step in the log, then back to the boot menu.

## Safety stops (heat)

| Mode | Stops |
|---|---|
| Low, High | battery 42 °C (band 40), back 46 °C (band 43), SoC 85 °C (band 75), 15 % charge, 3.45 V, 3.5 A, 30 min, a charger |
| Extreme, Manual, Keep warm | battery **55 °C** (its fire safety), the call / SOS reserve (5 / 15 / 30 %, hold to override), 3.5 A (Manual: a hold turns it off), a lost or stuck battery / SoC reading, a core fault, the SoC's hardware trip reading unarmed, a charger. Manual: an optional SoC stop (off by default) |
| always | the SoC's own hardware power cut (S-Boot's TMU trip; seen acting at 128 °C on our reading), the PMIC's and fuel gauge's protections |

Clocks: only steps in the chip's own tables (ACPM / ECT) with their own voltages; Extreme at Samsung's caps
(A55 1794, M3 1794 MHz without the OCP handler, GPU 572); Manual may go to A55 1950 / GPU 598 MHz (above Samsung's
policy caps, within the table voltages), never M3 above 1794.

## New, behind their switches (off by default; first runs with the user present)

- Settings > Alarm > **LED at rated power**: the S2MPB02 over I2C: torch 240 mA, the alarm's LED as 1250 mA camera
  flash pulses (the chip's 125 ms timer, at most one a second).
- Emergency Mode's control center > **Dim clock when off**: the panel's own low-power mode (ALPM 10 nits).
- Settings > About > **Read the panel's registers**: read only, for the brighter-screen (HBM) work.

## Known limits

- **Calls and messages**: coming soon (the modem driver is stage 0.8; emergency calls will be implemented but are
  never dialled in tests).
- **Sound and vibration**: coming soon (0.7: the codec is found; the speakers are on two MAX98512 amplifiers).
- **WPA2 on a router's 2.4 GHz side** does not join yet (the shared Wi-Fi core's 2.4 GHz fix is in test); open
  networks and 5 GHz work on the host, 2.4 GHz WPA2 fails on the phone (SET_SSID status 2).
- Heat and Wi-Fi do not run at the same time. No real RAM sleep (the screen-off state lowers the clocks). No lock
  code. The SoC reading vs the hardware trip (128 vs 115 °C) is not explained yet.

## Where the rest of the full OS plugs in (kept clean in 1.0)

Starbite OS is meant to grow into a full phone OS: normal mode and Emergency Mode, our own apps, heavier apps
through a Linux guest (stage 0.9) and cellular from the modem core (stage 0.8, an OS-independent library like the
Wi-Fi core, being built by the iOS 16 cellular session). The 1.0 seams for that:

- **Apps** (`kern/app.c`, `sdk/sbapp.h`): apps are `.sba` files at EL0 with a manifest of permissions and a numbered
  system-call table (SYS_*); UI is declarative (ui_* elements built by the kernel). A new service adds system calls
  and a permission bit; no app changes. `SBOS_P_PHONE` and `SYS_PHONE_STATE` (today `SBOS_ENOSYS`) are the reserved
  telephony slot: calls (`SYS_CALL_*`) and SMS (`SYS_SMS_*`) go there, with the emergency dial refused in the kernel
  in test images.
- **Services next to Wi-Fi** (`kern/wifi.c` over `starbite/wifi`): the pattern for the modem: a core library with an
  OS shim (`bcmw_os`: register access, DMA blocks, time, logging, events), driven by a kernel timer tick, never
  blocking the loop (start + poll), its own state machine and UI tile. The modem core plugs in the same way.
- **Display** (`kern/ui.c`, libsb `sbui`): one back buffer, dirty rectangles, `ui_present` + `panel_kick`, the
  brightness arbiter (`panel.c`). A guest-app surface is one more element kind (a rectangle the kernel copies
  from a shared framebuffer region on each present) plus a full-screen app mode; touch inside it is forwarded.
- **Input** (`kern/input.c`): one event queue (touch, keys) with the pocket lock in front; a guest gets the events
  of its surface through a ring.
- **Power** (`kern/services.c`): one screen state machine (on / off / the dim clock), the A55 clock policy, the
  switch-off path with the apps' save events; a guest and the modem get the same "screen off / save / power off"
  events.
- **EL2 (the hypervisor question), answered:** no. The loader hands Starbite OS EL1 (libsb's crt0 records
  CurrentEL; the self test now logs "entered at EL1"); Samsung's hypervisor (uH / RKP) owns EL2: `hvc` is
  undefined and ID-register reads trap to it (NOTES.md 2026-10-27; docs/STARBITE-MASTER-PLAN.md 46, 475). So no
  hypervisor and no isolated Linux guest on this phone. Stage 0.9 takes the other route in its plan: Linux alone on
  the M3 cluster as a second kernel next to Starbite OS (both at EL1, separate memory, a shared message ring and a
  framebuffer region for its surface, the GIC partitioned by interrupt), started through the heater's worker-core
  path. No isolation between the two: a research build first.

## Install (one step: the store payload)

Files (`build/store/`): `starbite-os-1.0-rc3.img` (the OS), `starbite-os-1.0-rc3-selftest.img` (the self test) and
`starbite-os-1.0-rc3-ledprobe.img` (the one-shot LED probe, installed like the self test), each with a `.sha256`. The build is reproducible (`starbite/sbos/build.sh --repro` gives the same sha256).

1. The OS store (UFS) entry `starbite-os.img` = `starbite-os-1.0-rc3.img`, keeping the volatile `sbos-state`
   entry (1 MiB, the save slot; made once by `sbstore2.py build --volatile sbos-state=1048576`). For example, from a
   pulled store: `tools/store/sbstore2.py build --import store.img --volatile sbos-state=1048576 ... starbite-os.img=build/store/starbite-os-1.0-rc3.img`
   (as for 0.6.3, T14), then write the store as usual.
2. The catalog entry (catalog-live.toml), unchanged except the version line:
   ```toml
   [[entry]]
   key = "starbite-os"
   name = "Starbite OS"
   version = "1.0-rc3 · heat, light, alarm, card, Wi-Fi"
   category = "starbite"
   method = "bare"
   tag = "beta"
   files = ["ufs:starbite-os.img|sd:starbite-os.img"]
   root = "none (RAM only)"
   wdt = 30
   wdt_feed = "own"
   ```
3. For the installer (tools/installer), the same two lines go into the bundle's `store/catalog.toml` and the payload
   into the `ufs-store` image it builds (`sbstore2.py build --medium ufs --catalog store/catalog.toml ...`); nothing
   else in the manifest changes.

Undo: the previous payload (`starbite-os-1.0-rc2.img`) in the same entry; the save slot is kept either way.
