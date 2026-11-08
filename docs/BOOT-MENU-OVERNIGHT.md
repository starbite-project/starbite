# Boot menu overnight (2026-11-07 → 08): speed, labels, icons

Session: boot-menu speed (a side session of the main session). Nothing was flashed. BOOT is unchanged.
Phone tests went through the main session only.

## 1. What was slow (measured, not guessed)

The phone's own logs already had the numbers (pmsg `STARBITE v7:` lines, log slots 128-138, from the
2026-11-05 22:17 Android test boot, renderer `MV4` = today's):

| class (M7_PH_*) | frames | mean ms on the A55 | VM ms (same code) | factor |
|---|---|---|---|---|
| intro (wallpaper) | 11 | 147 | 7.7 | ×19 |
| intro (glass) | 4 | 331 | 11.1 | ×30 |
| idle (countdown ring) | 15 | 203 | - | - |
| lens (focus moves) | 10 | 165 | 1.6 | ×100 |
| list (scroll) | 25 | 235 | 6.8 | ×35 |
| expand (section opens) | 15 | 248 | 6.2 | ×40 |
| bake (before the intro) | 1 | 435 | 16.7 | ×26 |

So 3–6 fps everywhere. Of the frame time, ~31 % was the present (2x + text and icons at panel
resolution, 85 ms a frame) and ~54 % the glass rows. All of it ran on **one** Cortex-A55 at S-Boot's
~1.05 GHz (bench: 20 M `subs/b.ne` in 38 ms), while the other three A55s sat off. The framebuffer
mapping is fine (normal non-cacheable, write-combined; 2 MiB copy 0.8 ms, same as cached RAM).

## 2. What changed (commits 6b04f3d, fee8929, 9db5fd5, c956c62, f97aae8, 01deb43, 036feca)

**All four A55 cores render** (`shim/starlte/glass_smp.c`). The menu starts A55 cores 1–3 with PSCI
CPU_ON (the targets em.c's core test measured) as row workers and powers them off (CPU_OFF, checked)
before `menu7_main` returns and before Lab. `g7_par(fn, ctx, y0, y1, grain)` hands out row chunks
with an atomic counter; every core has its own scratch (band buffers, half/quarter rows, frost-plane
rows, shift caches, blur rows: the top 3 MiB of `G7_CANVAS`, which was free). Parallel now: the
present (2x, items, edge), base glass, the list's row shift (two phases; the rows each band reads
across a cut are copied first), over glass, the frozen crossfade, the freeze (soft glass, box blurs,
2x), the intro wallpaper, and the bake (field, wallpaper, blur planes).

Safety: a worker that faults records ESR/ELR/FAR and powers itself off; core 0 then stops using
workers and redoes the job alone. A core whose CPU_OFF returned is reported (log slot 157) and no
cores are started again. Only core 0 polls keys/USB. Boot-arg `starcores=1` gives the old
single-core renderer.

Checks (host, `h7`): every frame of `scen.sh`'s four scenarios (1363 frames) is **byte-identical**
on 1, 2, 3 and 4 cores, and identical to the old renderer except the intro wallpaper (max 1/255 in
a few pixels: the compiler groups the moved loop's float maths differently). Every `--state`
`--check-full` 0.

Host timings, 1 → 4 threads (VM shared with other sessions, so noisy):
list 12.5 → 5.7 ms, expand 11.5 → 5.2, lens 3.1 → 1.5, intro 16–23 → 6–10.
**The A55 numbers come from the phone bench (section 4).**

**Labels and icons** (fee8929):
- A section's line under its name kept getting cut mid-word ("iOS 16, macOS Ventura, iOS 26 · 3 Syst").
  The names now give way with an ellipsis, so "· 3 Systems" always shows in full.
- One UI 8 (Android 16) showed the **Android 17** release badge. It now uses the Android logo tile.
- Utility OS and Starbite OS both fell back to the same Starbite star tile. Utility OS now has its own
  tile (sliders on teal), and Starbite OS keeps the star.
- The loader's built-in fallback catalog said "Android 17 / LineageOS 24.0". It now says One UI 8 /
  Android 16, as the store does.

## 3. Catalog text: a uniform pattern (main session's file; proposed, not applied)

Every row's second line becomes `version · base · kernel`, version first, at most three parts, with
no prose:

| key | name now → proposed | version now → proposed |
|---|---|---|
| ios16 | iOS 16 | `16.5 20F66 · Starbite (an earlier port kernel)` → `16.5 (20F66) · an earlier port XNU` |
| mac13 | macOS Ventura | `13.5 22G74 · XNU 12377 · root on microSD` → `13.5 (22G74) · XNU 12377 · microSD` |
| ios26 | iOS 26 | `26.6.2 · XNU 12377` (already uniform) |
| win11 | Windows 11 | `23H2 · NT 22631` → `23H2 · build 22631` |
| android | One UI 8 | `UN1CA 9810 · Android 16 · Linux 4.9` → `Android 16 · UN1CA 9810 · Linux 4.9` |
| chromeos | ChromiumOS | `R158 · Google arm64-generic · Linux 7.1` → `R158 · arm64-generic · Linux 7.1` |
| linux-ubuntu | Ubuntu | `26.04 LTS · Phosh · Linux 7.1` (uniform) |
| linux-pmos | postmarketOS | `edge · Phosh · Linux 7.1` (uniform) |
| engineer | Engineer OS | `Starbite Lab 2.1 · tiers 1-4 · CPU/GPU` → `2.1 · Lab tiers 1–4 · CPU/GPU` |
| utility | Utility → **Utility OS** | `Utility OS 2.0 · keys, gamepad, sensors` → `2.0 · keys, gamepad, sensors` |
| starbite-os | Starbite OS | `0.3: new UI, emergency, light, heat, SOS` → `0.3 · emergency, light, heat, SOS` |

With "Utility OS", the Starbite section reads "Engineer OS, Utility OS, Starbite OS". All of these fit
47 bytes.

## 4. Phone tests (unattended, no BOOT write)

`tools/menu/chain/` builds a **menuchain** bare store image: a 12 KiB stub that copies a test loader
(and optionally a BOOT ramdisk) where S-Boot puts them and enters it as S-Boot does. BOOT is untouched.
`pack.py --wdt S` makes the stub arm cluster 1's watchdog first (same registers as `wdt_arm`), as a
dead-man for unattended runs. The stub was checked in QEMU (copy, FDT initrd-end patch, jump).

### 4.1 MB1 hung the phone (2026-11-07 ~23:50)
The main session ran `menubench-1` (4-core present only) as a bare entry with `wdt = 60, no_launch = true`.
The phone dropped off USB and never came back: **it needs Power + Volume Down in the morning.**
- **Why there was no automatic reset (my mistake):** the loader arms the watchdog in `launch_go()`,
  which returns early when there is no launch record. `no_launch = true` therefore means **no watchdog**,
  whatever `wdt` says. Fixed on our side: the stub arms its own dead-man (commit c956c62). Saved as a memory note.
- **What hung:** not known yet. Found and fixed since:
  1. Core 0 waited for the workers with WFE and checked its timeout only between wake-ups. With interrupts
     masked, a lost worker could hold it forever. Now a bounded 400 ms spin, then core 0 alone (c956c62).
  2. A real race (ThreadSanitizer on the host preview): a worker leaving one job could claim a chunk of the
     next while core 0 was still writing its fields (half-written pointers, completed chunks miscounted).
     Fixed with one generation-tagged claim word (f97aae8); TSan is now clean over 837 frames on 4 threads.
  3. The other candidate, the loader restarting after another loader (not S-Boot), is untested. UFS start
     is re-entrant by reading; MB3a below tests it alone.
- **QEMU test of the phone's SMP code** (`tools/menu/smptest/run.sh`, commit 036feca): virt, 4 vCPUs, PSCI
  over HVC, the real worker entry and MMU setup, 9000 jobs, 3 start/stop cycles (CPU_ON works again, so
  CPU_OFF completed): PASS. The pre-fix code also passes there (the race window is tiny), so QEMU doesn't decide
  which candidate it was.

### 4.1a Root cause (2026-11-08 09:15, pmsg saved after the user's reset: `build/menu/mb1-hang-pmsg.txt`)
It wasn't a hang. The test loader ran POST, started cores 1-3 (all checked in: `smp 222004`), then declined v7:
`too slow (bake over 400 ms)` (436 ms; MB1 didn't parallelize the bake yet). BOOT's command line carries `starmenu7=2`
(skip the check), but a chained loader doesn't get BOOT's command line. So menu v6 ran, and v6 doesn't serve
background fastboot: the phone sat in the v6 menu with no USB and no watchdog. Fixed in e1e4d5d: test builds default
to `starmenu7=2` (`M7_MODE_DEFAULT`); MB3 and the trial images were rebuilt. (Follow-up for the loader owner: v6
could poll fbbg too, so a v6 fallback is never unreachable.)

### 4.2 MB3 (replaces MB1/MB2; `side-session-notes/…/menu-bench-3-request.md`)
Both have the stub's 75 s dead-man and pmsg breadcrumbs (`STARBITE v7 bench: menu7 up / cores … / bake
done / intro done`, per-core `CPU_ON` / `checked in` lines):
| image | sha256 | |
|---|---|---|
| `menubench-3a.img` | ed7dcabd24a9281be55ebe82eb379daf959ae27c1f358aa696d4a035f50e49cd | no extra cores: tests chain + loader restart only |
| `menubench-3b.img` | d0cfe0b676885dcf8ad443ecd4b42133fb8f702b5c2c97b3a4d17cb32d756f35 | 4 cores, alternating 1/4-core frames |
Run 3a first, and 3b only if 3a came back by itself. Before either, save pmsg from the first boot after the hang
(`build/menu/mb1-hang-pmsg.txt`).

## 5. For the morning (user present)

1. Power + Volume Down to bring the phone back; save pmsg (above).
2. MB3a, then MB3b (unattended, ≤ 75 s each). Their pmsg lines give the real A55 frame times, 1 core vs 4.
3. Then the trial: `menutrial.img` (sha256 5f7b6e4fcaa0a5056f871cb7289ee2f40ef4b6ccb8f453e9f8046fd35775260b;
   `side-session-notes/…/menu-trial-request.md`). New loader + new ramdisk (icons) from the store, user
   driving: scroll, open sections, options menu, then start Linux (check that all 4 CPUs are online),
   One UI, Windows, iOS. If it misbehaves, `starcores=1` gives the single-core renderer.
   Fallback with the same labels and icons but the old single-core renderer: `menutrial-1core.img`
   (sha256 6369d911ec346f35cd6ce20be7f4a2ab1758b90daf702aa1b236b0b24a46d852; loader
   `build/menu/loader-1core/xnu_boot.Image`, built with `M7_DEF_CORES=1`; `starcores=4` turns the cores on).
4. Only then flash BOOT: `build/menu/loader-new/xnu_boot.Image` + `build/menu/menu-v7.sbm`, with the
   usual verified backup. If SMP ever hung at boot from BOOT, the way back is download mode (the old BOOT
   backup), so do it with the user present.

## 6. Results from the phone

**MB3a and MB3b passed (2026-11-08 morning; both came back by themselves; pmsg in `build/menu/mb3a-pmsg.txt`,
`mb3b-pmsg.txt`).** 3b: CPU_ON 0 for cores 1-3, all checked in within 0-1 ms, 616 jobs, 18869 chunks run by
workers, 0 faults, all cores off again (no CPU_OFF returned). Mean frame time on the A55, the same run alternating
1-core and 4-core frames (MB3b):

| class | 1 core | 4 cores | speed-up | 4-core fps |
|---|---|---|---|---|
| intro (wallpaper) | 160 ms | 49 ms | 3.3× | 20 |
| intro (glass) | 324 ms | 99 ms | 3.3× | 10 |
| lens (focus moves) | 47 ms | 14 ms | 3.3× | 69 |
| list (scroll) | 234 ms | 74 ms | 3.2× | 13.5 |
| expand (section opens) | 298 ms | 129 ms | 2.3× | 8 |
| bake (before the intro) | 499 ms (3a) | 129 ms | 3.9× | — |

So the focus/lens is now smooth. Scrolling and sections are about 3× better but still well under 30 fps: the
remaining work is per-pixel (2x upscale and glass) at S-Boot's 1.05 GHz on four in-order cores. Next levers, in
order: the expand path (only 2.3×: its serial part, the opening section's own glass and item drawing), then fewer
presented pixels while moving, then the GPU (section 7).

Screens:
`build/menu/screens/{before,after,real-before,real-after}/*.png` and the contact sheet
`build/menu/screens/contact.png` (26 before/after pairs: 16 concept states, 10 with the real catalog;
`tools/menu/contact.py`).

### 6.1 One UI regression on the 4-core BOOT (2026-11-08 ~10:56)
BOOT-new.img (1eaf6e21…) passed the menu trial and Ubuntu, but under One UI (Samsung 4.9) cpus 1-3 stayed offline and
Android reset after ~2 min. With the old loader, cores 1-3 were never powered before the kernel. With the new loader they
are powered on and off once (PSCI CPU_ON/CPU_OFF), and Samsung's kernel does not bring them back. The old BOOT was restored.
Fix staged: **BOOT-1core.img** (sha256 5c32e2c0…5950), the same UI fixes with cores 1-3 never touched (M7_DEF_CORES=1).
Details and the data needed to keep 4 cores: side-session-notes/…/boot-menu-latest.md.

### 6.2 The fix: Samsung's FlexPMU hotplug sequences on the workers (commit 24e65da), MH1 passed
Samsung's kernel (ref/starlte-kernel, exynos-powermode.c and cal-if/exynos9810/flexpmu_cal_cpu_exynos9810.h)
runs a per-core FlexPMU sequence on the core itself, under one lock: `cal_cpu_enable` when the core starts, and the
CPU_INFORM C2 hint plus `cal_cpu_disable` right before PSCI CPU_OFF. Our workers skipped both. The workers now run
the same sequences. MH1 (`menuhandoff-1.img`: 4-core menu, then One UI started by itself): One UI ran 433 s+,
`cpu/online` = 0-7, cpu1-3 clocked at 455-598 MHz, no reset. BOOT-smp2.img (d3a31942…) = the 4-core BOOT with the fix.
PMU registers (GRP2_BID_EN, GRP2 pending, GRP1 pending, EVT_EN_GRP2, CPU_INFORM 1-3, CPU1-3 status):
- before the first CPU_ON (cold, from S-Boot): `0 ef 0 ffffffff 10000 10000 0 880000 880000 880000`
- cores up (after the "on" sequence): `0 0f 0 ffffff1f 0 0 0 100f0f 100f0f 100f0f`
The "after off" line was overwritten in that run's pmsg. After "off" the bits for cores 1-3 match Linux's own hotplug-out
state (BID_EN and EVT_EN bits set, CPU_INFORM = C2), which is where Samsung's kernel CPU_ONs from at runtime.

## 7. Not done / next

- GPU (Mali-G72) composition: the right long-term answer for 60 fps (MV5). The win-mali-gpu track can
  run jobs (fragment clears, triangles) but has no shader toolchain for this glass yet. Too big and
  too risky for one night.
- The four M3 big cores could double the workers. They need XNU's PMU prep and ARMv8.0-only code, and
  their CPU_ON has hung before (win-cores notes). Not done.
- A CPU clock boost was declined by the user (decision 16), so it's not used.
