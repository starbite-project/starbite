# Windows 11 on the Mali-G72: a GPU driver for the Galaxy S9

Track D of `docs/PLAN-2026-11-07.md`, extending `docs/STARBITE-MASTER-PLAN.md` §3.8 (stages WG1-WG4).
Written 2026-11-07. Status: design reviewed; stage 2 drivers written and host-tested; no phone test yet.

Goal: Windows 11 23H2 on the S9 (SM-G960F, Exynos 9810) draws its desktop on the Mali-G72 MP18 instead of
in software (WARP), through a Windows display driver (WDDM) of our own with Mesa as the user-mode part.
Nobody has done this on this GPU. The nearest precedent drew one Direct3D 11 triangle on a Mali-G52.

Tags used below: **[proven]** = seen working on this phone; **[claim]** = someone else's result, not
checked by us; **[unverified]** = our reading of a document or source, not yet tested.

## 0. Summary

| Stage | What | State |
|---|---|---|
| 1 | This document | done, adversarially reviewed (§13) |
| 2a | `sbthermal.sys`: temperature watchdog (TMU), the gate every GPU driver must pass | **done: T1 passed on the phone 2026-11-07** (§10.1); stays installed (Start 2) |
| 2b | `sbdisplay.sys`: display-only driver for DECON (one mode, presents, tear-free flips at TE, brightness) | written (`starbite/win/sbdisplay`), host tests pass, builds and is test-signed; ACPI table `starbite/win/acpi` compiles with iasl; needs the SSDT in the firmware (T2) before T3 |
| 3 | `sbmali.sys`: KMDF bring-up (power, MMU, a job) ported from StarbiteMali | **done: T5 passed on the phone 2026-11-07**: the first Mali jobs under Windows, 20/20 in both page-table formats, 0 faults (§10.1) |
| 4 | `sbwddm.sys` + Mesa 26.2 UMD: Direct3D 11 → DWM on the Mali | designed (§6), research |

The plan in one paragraph: Windows already runs on the S9 with Microsoft's Basic Display driver copying
WARP's frames into the firmware framebuffer. We first add a temperature watchdog (nothing may load the GPU
without it), then replace Basic Display with our own display-only driver for DECON (tear-free, brightness,
the display half of the final driver), then port the GPU bring-up that already runs on iOS 26 and macOS
(StarbiteMali) to a Windows kernel driver, and finally merge both into one WDDM 1.3 driver in the shape
that wddm-mali-g52 proved on a Mali-G52, with Mesa 26.2's D3D10 frontend and Panfrost as the user-mode
driver. The last stage is research: whether DWM accepts a D3D10-only user-mode driver is still open.

## 1. What we start from

| Source | What it gives us | Status |
|---|---|---|
| `xnu/src/starlte_gpu.c`, `xnu/src/StarbiteMali.cpp` (our XNU Mali driver) | The whole G72 bring-up on this phone: BUCK6 to 950 mV over ACPM, soft reset, L2/shader/tiler power-up, AS0/AS1 page tables, WRITE_VALUE / fragment / tiler jobs, recovery after a fault, ACPM DVFS to 572 MHz, a Panfrost-compatible kernel interface (CREATE_BO, SUBMIT, ...) that Mesa runs on. Jobs are polled; no interrupts. | **[proven]** iOS 26 and macOS (Metal on the Mali, `docs/MACOS-METAL.md`) |
| `starbite/libsb/mali.c`, `mali_desc.c` | The same recipe, OS-free and gated, with hand-packed v6 job descriptors checked word for word against Mesa (host test) | **[proven]** in Starbite Lab |
| `starbite/libsb/tmu.c`, `acpm.c` | TMU temperature reads with glitch filtering; the ACPM IPC protocol (PMIC registers, DVFS requests) | **[proven]** |
| Linux 7.1: Panfrost + `tools/linux/port/src/gpu/drm/s9p/s9p-decon.c` | Panfrost on this G72 at 572 MHz with Mesa 26.2 (Plasma on the GPU); DECON flips by IDMA base address with the TE trigger and the frame-start interrupt | **[proven]** (NOTES 2026-10-27) |
| `shim/starlte/em.c` `panel_set_aor` + `build/em-research-brightness.md` | Panel brightness: S-Boot's AOR-only DSI sequence | in Engineer mode; S-Boot uses the same sequence |
| [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) (GPL-2.0, Mali-G52 on RK3576) | KMDF bring-up (`malikm`), display-only driver (`malidod`), WDDM 1.3 driver (`maliwddm`), 20 Mesa patches for Panfrost + d3d10umd on Windows ARM64, a libdrm shim with a WDDM backend | **[claim]** M1-M4 (2026-10-01..02) and malidod on installed 23H2. M5.2 (b), (c) — a job and the D3D11 triangle through the WDDM driver, Mesa loaded by the D3D runtime as the UMD — passed **in WinPE 22621**, not on a desktop with DWM; inside the UMD, Panfrost still reaches the kernel through the shim's own D3DKMT device, not the runtime callbacks, and presents by CPU readback + copy. maliwddm also loaded on their installed Windows (black screen until primaries were made section-backed, `adapter.c:280-288`). Their eMMC Windows install stopped booting during the work. DWM on the GPU not done |
| Mesa 26.2 `src/gallium/frontends/d3d10umd` | The user-mode D3D10/10.1 DDI over any Gallium driver. Removed in Mesa 26.3 (it emits TGSI, Mesa is moving to NIR only) | 26.2 is the last release with it |
| viogpu3d (virtio-win PR 943, BSD-3) + Mesa MR 24223 | A full WDDM 1.3 driver whose UMD is d3d10umd, with WinUI 3 apps running (with glitches) | **[claim]**, the strongest evidence that DWM runs on a d3d10umd-based UMD |

## 2. Hardware facts

| Block | Facts | Source |
|---|---|---|
| Mali-G72 MP18 | MMIO 0x17500000, 0x5000; GPU_ID 0x62210010 (r0p1); shader_present 0x5771777 (18 cores); 1 tiler, 1 L2; AS_PRESENT 0xff (8 address spaces); JS_PRESENT 0x7 (3 job slots); COHERENCY_FEATURES 0 (**not IO-coherent**) | NOTES 2026-10-28 **[proven]** |
| GPU interrupts | job SPI 282, MMU SPI 281, GPU SPI 283, level high → ACPI GSIV **314, 313, 315** (GSIV = SPI + 32, as the DSDT's UFS entry confirms: SPI 227 = GSIV 259) | `ref/starlte-kernel/include/dt-bindings/interrupt-controller/exynos9810.h`, mainline DTS |
| GPU power | domain `pd_embedded_g3d`: PMU 0x14064040 (config) / 0x14064044 (status, [3:0] = 0xf on). S-Boot leaves it **on**. After a power cycle of the domain the TZPC must be restored by an EL3 SMC `0x82000410(1, 0x17410204, 0)`, or the first GPU access is an SError | `xnu/src/starlte_gpu.c` header **[proven]** |
| GPU supply | vdd_g3d = S2MPS18 BUCK6, PMIC register 0x2b, 300 mV + n × 6.25 mV. S-Boot leaves 0x33 (618.75 mV); jobs need **0x68 = 950 mV** (lower gave DATA_INVALID_FAULTs on Linux) | NOTES 2026-10-28 **[proven]** |
| GPU clock | PLL_G3D (CMU_G3D 0x17400000, CON0 +0x140), S-Boot's 260 MHz. Changed **only** by an ACPM DVFS request for domain **4** (`dvfs_g3d`, cal id 0xb040004). Levels 572, 546, 455, 338, 299, 260 MHz. The vendor table also raises the MIF minimum at high GPU clocks; **we never do** (rule) | `0303-starlte-gpu-present.patch`, vendor DT `gpu_dvfs_table` |
| ACPM | IPC queues in APM SRAM 0x02039000 (0x30000), mailbox 0x14100000. PMIC = channel 2, DVFS = the first channel of the AP table. On Windows nothing else talks to ACPM: the firmware's RTC reads the PMIC once at boot and never at runtime (`tools/woa/patches/0023`) | `starbite/libsb/acpm.c` |
| TMU | TOP 0x10070000 (big cores: probes 1,2,3,4,6; little: 5), SUB 0x10074000 (**GPU: probe 1**, ISP: probe 2). 9-bit codes, per-probe trim. S-Boot arms the hardware trip at **115 °C** (CONTROL bit 12) | `starbite/libsb/tmu.c`, `build/em-research-thermal-clocks.md` |
| Vendor GPU thermal zone | switch-on 78 °C, control 88 °C (IPA), alert0 105, alert1 110, hot 115 °C; vendor throttle steps 572 → 546 → 455 → 338 → 260 MHz | vendor DT `thermal-zones/G3D` |
| DECON0 | 0x16030000 (0x10000) + DPU_DMA 0x16070000 (IDMA channel n at 0x16071000 + 0x1000 n). S-Boot's live path: **IDMA channel 0** → window 5 → DSIM0. Command-mode panel, hardware (TE) trigger. Interrupts: frame start SPI 182 (GSIV 214), frame done 183 (215), extra 188 (220). IDMA base addresses are 32 bits | `s9p-decon.c` **[proven]** |
| DSIM0 | 0x16080000 (0x100). Commands: payload FIFO 0x5c, then header 0x58, wait INTSRC 0x50 bit 28. Never reset, never touch FIFOCTRL/PLL/CLK_CTRL | `build/em-research-brightness.md` |
| Panel | Samsung S6E3HA8, 1440 × 2960, 60 Hz, 4 lanes, DSC. Brightness by AOR only (`F0 5A 5A`, `B1 hi lo`, `F7 03`, `F0 A5 A5`), AOR clamped 0x000C..0x0BA3; **never** C0/C1 (POC flash), MTP, gamma | same |
| Framebuffer | 0xCC000000, XRGB8888 (B,G,R,X in memory), stride 5760, 17,049,600 bytes; the UEFI GOP hands it to Windows | `ref/exynos9810-woa/.../Exynos9810.h` |

## 3. G72 versus G52: what differs from wddm-mali-g52

| | Mali-G52 (RK3576, wddm-mali-g52) | Mali-G72 (Exynos 9810, ours) | Consequence |
|---|---|---|---|
| Architecture | Bifrost v7 | **Bifrost v6** | Mesa compiles v6 descriptors (genxml `v6.xml`) and the v6 Bifrost ISA; both live in Panfrost and run with Mesa 26.2 on this phone under Linux **[proven]** |
| GPU_ID | 0x7002.... | **0x6221** (r0p1) | ID checks |
| Panfrost features | IDVS_GROUP_SIZE (writes JM_CONFIG) | **no** IDVS_GROUP_SIZE; no hardware issues listed in Linux `panfrost_issues.h` | skip the JM_CONFIG write; no SHADER_CONFIG quirks |
| Page-table format | Mali legacy LPAE (`TRANSCFG` 0), leaves `pa | 0x2C5` | Legacy LPAE is what runs on this phone today (XNU **[proven]**; mainline Panfrost has no exynos9810 quirk, so Plasma ran legacy). The exynos-linux port reports legacy tables failing 5-6 runs in 10 on this exact G72 r0p1 and the **AArch64 4K format** (`TRANSCFG` ADRMODE 6) passing 230 of 230 **[claim]**; Linux has the switch (`GPU_QUIRK_FORCE_AARCH64_PGTABLE`) | the Windows driver implements both, **defaults to legacy** until the stage 3 phone test (20 runs each, §10) decides |
| Cores | 3? (MC3) | **18 shader cores**, 3 job slots | affinity 0x5771777 |
| Supply | RK806 DCDC5, set by firmware | **BUCK6 must be raised to 950 mV by the driver** (ACPM) | the driver owns one PMIC write |
| Clock | SCMI via TF-A, firmware-set | **ACPM DVFS domain 4** | the driver asks ACPM; never MIF/INT |
| Power domain | firmware powers it | S-Boot leaves it on; cycling it needs an EL3 SMC for the TZPC | the driver never powers the domain off (§5.4) |
| Display | VOP2 + HDMI, video mode | **DECON + DSI command-mode panel** with a TE trigger, DSC | flips are "write base, request shadow update, unmask trigger" (§7) |
| Scanout addresses | 32-bit | 32-bit (IDMA base registers) | primaries below 4 GiB, contiguous |
| Interrupts | GSIV 379/380/381 | **GSIV 314/313/315** (job/MMU/GPU) | SSDT |
| Coherency | — | none | CPU mappings of GPU buffers are uncached or write-combined; jobs flush caches at start and end (CONFIG_NEXT 3 << 8 \| 3 << 12) **[proven]** |

## 4. Architecture

```
 D3D11 app / DWM                      user mode
   D3D11 runtime (feature level 10_0) ─► UMD: Mesa 26.2 d3d10umd + Panfrost (v6) + pan_kmod WDDM backend
                                          (DXBC → TGSI → NIR → Bifrost; job chains in WDDM allocations)
 ─────────────────────────────── dxgkrnl ───────────────────────────────  kernel mode
   sbwddm.sys (WDDM 1.3, one device "\_SB.GPU0")
     display half  = sbdisplay (DECON, DSIM brightness)           stage 2
     render half   = sbmali    (power, MMU, job slots, IRQs)      stage 3
       │ ExRegisterCallback("\Callback\StarbiteThermal")
   sbthermal.sys (non-PnP service): TMU every 250 ms, levels     stage 2
       │ owns nothing else; read-only on hardware
   ACPM (PMIC BUCK6, DVFS domain 4) — used only by the GPU driver, under one lock
```

### 4.1 Driver set

| Driver | Kind | Binds | Writes to hardware |
|---|---|---|---|
| `sbthermal.sys` | WDM, non-PnP, service (Start = 2, auto: loaded by the SCM once C: exists, so its kill file can be checked; ErrorControl = 0), cannot be unloaded | nothing (maps TMU MMIO itself) | **nothing** (read-only TMU) |
| `sbdisplay.sys` | WDDM 1.2 display-only (KMDOD) | `ACPI\STRB0072` | DECON: IDMA ch0 base (+0x40, +0x44 as s9p-decon), SHADOW_UPDATE_REQ (window bit), HW_SW_TRIG_CONTROL (RMW, two bits, only if not already enabled/unmasked). DSIM: payload/header FIFO, INTSRC W1C. Nothing else (the interrupt registers only once interrupts are added) |
| `sbmali.sys` | KMDF, stage 3 test driver | `ACPI\STRB0072` instead of sbdisplay | Mali registers; PMIC BUCK6 (0x2b) and DVFS domain 4 through ACPM |
| `sbwddm.sys` + `sbumd.dll` | WDDM 1.3 full driver + Mesa UMD | `ACPI\STRB0072` | both of the above |

One ACPI device holds everything (DECON, DSIM, Mali; all six interrupts), as wddm-mali-g52 does: DWM
composites on the adapter that owns the display, so the render half must be in the same device. Only one of
`sbdisplay`, `sbmali`, `sbwddm` is installed at a time.

### 4.2 The ACPI device

Windows only starts a PnP driver for a device that ACPI describes. Our firmware's tables come from the
release DXE's "P" module; there is no GPU or display device today. The device (file
`starbite/win/acpi/StarbiteGpu.asl`):

```
Device (GPU0) { Name (_HID, "STRB0072")  Name (_UID, 0)  Name (_CCA, 0)
  _CRS: Memory32Fixed 0x16030000/0x10000 (DECON), 0x16070000/0x7000 (DPU_DMA),
        0x16080000/0x100 (DSIM0), 0x17500000/0x5000 (Mali);
        Interrupt Level ActiveHigh Exclusive {214} {215} {220}, {314} {313} {315} }
```

**Stage 2 declares no interrupts** (`starbite/win/acpi/StarbiteGpu.asl`): sbdisplay polls, and a
level-triggered line connected by a driver that does not own its block (the Mali's three, DECON's EXTRA
whose status register 0x50 sbdisplay does not clear) could storm. Each stage adds its own lines, and the
driver that connects a line clears its block's status (DECON EXTRA: 0x50 added to the write list then).

Two ways to publish it, both offline-preparable:

1. **Firmware (chosen):** the shared `Star2LteSsdtDxe` of the win-usb-host track (commit 6e4c398 on
   `claude/win-usb-host-7efe60`, its `docs/WIN11-USB.md` §4.1) installs every SSDT blob it carries at
   ReadyToBoot, TPL_CALLBACK − 1 (after the release's GPIO/touch handoff, which needs to see exactly its
   own 4 tables). Ours is a **plain** blob (memory-only device, nothing to power first):
   `build/win-gpu/StarbiteGpu.aml` packed by `tools/woa/ssdt-ffs.py` into FREEFORM FFS
   `53534454-5342-5445-5342-475055000000` (Table ID `SBGPU`), added with `tools/woa/dxe-replace.py --add`
   (`EXTRA_AML=build/win-gpu/StarbiteGpu.aml tools/woa/build-win-usbhost.sh` adds it to every USB image).
   One BOOT flash for all tracks; a BOOT image without the blob is kill switch 1. The pmsg ring
   (`pmsg-ramoops-0`, `grep -a '=SSDT'`) shows `=SSDT installed SBGPU` or `=SSDT skip SBGPU: <reason>`.
2. **Registry override (fast test path):** Windows can replace the DSDT from the registry when test signing
   is on (`asl.exe /loadtable`; it can override an existing table, it cannot add an SSDT). We would dump the
   release DSDT (`tools/woa/winpe/acpi-dump.ps1`), append the device, and write the key offline with
   `hivexsh` in lima. The override table needs a higher OEM revision than the firmware's DSDT. Microsoft
   warns this can leave Windows unbootable; undo = delete the key offline. **[unverified]** on ARM64.

`_CCA 0` declares the device non-coherent, which is true.

### 4.3 Install and undo on the phone

The main session's L0 (`tools/win/`) establishes how a test driver gets onto the phone's Windows and is
signed (clang-cl, lld-link, the "Starbite Test Signing" certificate, test signing is on). This track reuses
it. What this track adds:

- `sbthermal` is a plain service: copy the `.sys` to `Windows\System32\drivers`, add
  `Services\sbthermal` (Type 1, Start 2, ErrorControl 0, ImagePath) to the current ControlSet with the
  shared `tools/win/hive-service.py ... add sbthermal --oneshot 0` (hivexsh in lima, refuses a dirty hive); the SYSTEM hive and
  the file go through TWRP's ntfs-3g as for the UFS driver (NOTES 2026-10-27). ErrorControl 0: a load
  failure never stops boot.
- `sbdisplay` is a PnP driver: it needs a driver package (INF + catalog) installed into the driver store,
  which `pnputil` does from inside Windows (the catalog is made and signed on the Mac by `starbite/win/build.sh`
  with `tools/win/makecat.py`; the certificate must be in Root + TrustedPublisher first; L0's planned
  one-shot install service does both).
There is no network under Windows on this phone (no Wi-Fi, no RDP), so a black screen cannot be fixed
from inside Windows. Undo paths, in the order to use them:

1. **Firmware:** the previous BOOT image (without the Starbite SSDT module): no device, so no driver loads;
   Basic Display keeps the screen. No NTFS or registry write needed.
2. **Kill file:** `\Windows\Starbite\<driver>.off` created from TWRP (ntfs-3g); every driver here checks it in
   `DriverEntry` and does not load. ntfs-3g may refuse a volume left dirty by a forced power-off; then 1.
3. **Registry:** `Services\<driver>\Start` = 4 (`tools/win/hive-service.py IN OUT disable <driver>`), **only on a clean hive**: after a forced
   power-off the SYSTEM hive is often dirty, and Windows replays SYSTEM.LOG1/LOG2 over offline edits.
   Check the base block's two sequence numbers are equal first (`tools/woa/hive.py` reads the primary
   file only).

**Boot guard** in every driver here: a `GuardBoots` counter in the service key, incremented at start and
cleared only after a healthy run (sbthermal: 120 s; sbdisplay: **10 minutes** with presents arriving and no
flip failure). At 2 the driver refuses to start (Basic Display stays). A driver that runs but shows a wrong
screen cannot know it; that is the person's call: **force a restart (hold Power + Volume down) within 10
minutes, twice; the third boot has Basic Display.** Hangs and resets early in boot disable the driver the
same way without anyone touching the phone. The first display test also runs in copy mode (no DECON or DSIM
writes at all, §10 T3a) before flips are switched on.
- Each driver logs to its service key (`...\Services\<name>\State`: values, `ZwFlushKey` after each
  milestone) so the main session reads results offline with `tools/woa/hive.py`.

## 5. Stage 3: the Mali under Windows (`sbmali.sys`, KMDF)

A port of StarbiteMali/starlte_gpu.c to KMDF, with interrupts instead of polling, in the shape of
wddm-mali-g52's `malikm`. Done when a WRITE_VALUE job and a fragment clear run from Windows.

### 5.1 Bring-up sequence

1. Refuse to start unless `\Callback\StarbiteThermal` exists and reports level Normal, and the G3D domain
   status reads 0xf (**never** power the domain: §5.4). Read GPU_ID; expect 0x6221.
2. ACPM: read BUCK6; only if it is in 0.5..1.0 V, write 0x68 (950 mV) and read back (as XNU stage 2).
   **ACPM sets the voltage itself from its own table on every DVFS request** (`acpm.c`), so after every DVFS
   request (raise, throttle, idle) the driver writes 0x68 again and reads it back, exactly the order XNU's
   proven present path uses (`0303-starlte-gpu-present.patch`). The rail is read back before each submit
   batch; a value outside 900..1000 mV stops the GPU.
3. Soft reset (GPU_COMMAND 1, wait RESET_COMPLETED); IRQ masks set; L2, shader cores, tiler on (bounded polls).
4. Address space: legacy Mali LPAE by default (`TRANSCFG` 0, `MEMATTR` 0x888d88, tables `| 3`, leaves
   `| 0x2c1`), what XNU proved. AArch64 4K behind a registry switch until T5 decides (§3): `TRANSCFG` =
   ADRMODE 6 | INA_BITS(55 − 48) (MMU_FEATURES 0x2830: 48 VA bits, 40 PA bits) | PTW_MEMATTR_WB |
   PTW_RA as Linux, `MEMATTR` from Linux's `mair_to_memattr`, leaves with AF set; tables in uncached memory
   with PTW_MEMATTR_NC is our idea, **[unverified]**.
   Every unmap or change of a live mapping: AS LOCK over the range, FLUSH_PT (or FLUSH_MEM), wait for
   AS_STATUS idle, before the pages are given back (uncached tables do not invalidate the GPU's TLB).
5. Jobs: JS0 fragment, JS1 vertex/tiler/compute (Panfrost's split), affinity = shader_present,
   CONFIG_NEXT = AS n | flush clean+invalidate at start and end | priority 8. Completion by the job
   interrupt (GSIV 314) → DPC; MMU faults (313) and GPU faults (315) reported with the fault registers
   (decoded by `sb_gpu_exc_name`'s table).
6. Recovery after a fault or a 2 s timeout: hard-stop the slots, soft reset, cores on, address spaces
   reloaded (`sgu_recover`).
7. Clock: only after (1)-(6) pass and with the watchdog Normal: ACPM DVFS domain 4 to 338 MHz, later
   572 MHz, each followed by the BUCK6 rewrite of step 2. Idle (no job for 100 ms): cores off (PWROFF),
   DVFS 260 MHz.

The watchdog may not be running yet when a PnP driver starts (start order is not guaranteed): the GPU
driver waits up to 30 s for `\Callback\StarbiteThermal` (opened with `Create = FALSE`), then stays off.
It treats a state whose `seq` stops moving or whose `time_100ns` is older than 1 s as Blind.

### 5.1a What the T5 driver actually does (as built)

`starbite/win/sbmali/`: `sbm_core.c` (portable: probe, ACPM, reset/power, both table formats, address
spaces with Linux's lock-region encoding, polled jobs, libsb's job descriptors, the self-test) and
`sbmali.c` (KMDF, binds `ACPI\STRB0072`). Narrower than §5.1 on purpose for the first phone test:

- **Polled, no interrupts** (the stage 2/3 SSDT has none; interrupts come with stage 4), exactly as XNU runs it.
- **No DVFS**: the GPU stays at S-Boot's 260 MHz. `sbm_gpu_dvfs` exists (domain 4 only, table frequencies
  only, BUCK6 rewritten after) but T5 does not call it.
- Thermal gate before anything and before every run: the callback object is **opened, never created**; the
  answer must carry `version` 2, a fresh reading (< 1 s) and a `seq` that moves; level Normal. Up to
  **10 minutes** wait for the watchdog at boot (T5 run 1: PnP started sbmali early in boot, the SCM loaded
  sbthermal about 75 s later, and the first build gave up after 30 s). `State\ThermalGate` codes: 1 object
  missing (watchdog not loaded yet), 2 no answer, 3 stale reading, 4 counter not moving, 10 + level when
  not Normal. `State\Step` (flushed) says where the thread is: 1 gate, 2 probe (first GPU read), 3 BUCK6,
  4/5 legacy/AArch64 runs (`*_RunsDone` flushed per run), 6 putting back, 7 finished. The boot guard is
  cleared whenever the thread ends cleanly.
- Per run (Parameters\Runs, default 20, per format of Parameters\Formats, default both): soft reset, L2 /
  18 shader cores / tiler on, AS0, WRITE_VALUE "STARBITE" on JS1 checked, a 64 × 64 clear-only fragment job
  on JS0 checked pixel by pixel, cores off; 50 ms (in practice ~650 ms, the watchdog's seq check) between runs.
- Afterwards: soft reset, cores off, BUCK6 back to the value found (read back), State\Done 1, GuardBoots 0.
- Memory: one 128 KiB contiguous uncached block (< 2^40): 16 tables + 64 KiB of data at GPU VA 0x02000000.
  Nothing maps the panel.
- Writes only Mali registers, the ACPM queues/mailbox (BUCK6 and nothing else; the core refuses any other
  PMIC register) and its own memory. The PMU and CMU_G3D are mapped but writes to them are dropped.

Host test (`starbite/win/tests/mali_test.c`): a fake G72 that walks the page tables in the format AS0 is set
to (legacy: table 3 / leaf 1 + RW; AArch64: page 3 + AF + AttrIdx 1 + AP_UNPRIV, exact TRANSCFG and MEMATTR),
runs WRITE_VALUE and fragment-clear jobs from the descriptors, and a fake ACPM/PMIC that fails on any write
but BUCK6 and any DVFS domain but 4. Mutation check: dropping AF from the AArch64 leaf makes all 20 fake
runs fault.

### 5.1b T8: the GPU clock and a real draw (as built)

- **Clock:** `Parameters\GpuKhz` (default 572000; 0 = stay at S-Boot's 260 MHz) through ACPM DVFS **domain 4 =
  `dvfs_g3d`**. Evidence (accepted 2026-11-07): Samsung's `cal-if/exynos9810/acpm_dvfs_exynos9810.h` orders
  `enum acpm_dvfs_id` as mif 0, int 1, cpucl0 2, cpucl1 3, g3d 4; `cal-if.c` `cal_dfs_set_rate()` sends ACPM
  `GET_IDX(id)`; the vendor DT gives the GPU `g3d_cmu_cal_id = <0xb040004>`; on the phone, domain 2 moved the A55
  PLL (T6). **Code-level refusal:** `sbw_acpm_dvfs()`, the only DVFS path of every Starbite Windows driver, returns
  without sending anything for a domain other than 2, 3 or 4 (host-tested: 0 = MIF, 1 = INT and 5 never reach
  the fake ACPM). After each request BUCK6 goes back to 950 mV, and **PLL_G3D is read back** (CMU_G3D +0x140): if
  it is not within 1 % of the request, 260 MHz is requested back and the test stops (`GpuClockMismatch`).
- **Throttle**, per run from the watchdog: Warm (70 °C) 338 MHz, Hot (80 °C) 260 MHz, Stop (90 °C) ends the runs.
  Afterwards 260 MHz first, then BUCK6 to the value found.
- **Draw:** XNU's Mesa-built triangle blob (`xnu/src/starlte_gpu_tri.h`: tiler job on JS1, fragment job on JS0,
  512 × 512, magenta on dark grey), copied fresh each run to its fixed VA 0x2008000; its 16 MiB tiler heap
  (0x10000000) and its render target (the panel's VA window from 0x46AE740, stride 5760) are **the driver's own
  pages** (`MmAllocatePagesForMdlEx`, uncached, mapped no-execute), never the panel. Every pixel of the square must
  be the clear or the triangle colour, and the triangle's pixel count must be the same in every run.
- **Runs:** `Parameters\Runs` default 200 per table format, each: WRITE_VALUE + 64 × 64 clear + triangle.
- The watchdog gate now only needs a fresh reading (< 1 s; the watchdog stamps every reading), no 600 ms wait.
- New State values: GpuKhzReq, GpuPllKhz, GpuDvfs, Throttles, GpuMaxMilliC, `*_TriOk`, `*_TriPx`,
  `*_TriMismatch`, `*_TriBadPx`, TriAlloc, Triangle, GpuClockRestore.

### 5.2 Memory

The GPU reads physical memory directly (no SysMMU in front of it on this SoC as configured by S-Boot;
XNU's tables at 0x8f0000000 worked **[proven]**). Under Windows:

- Page tables and the kernel's own job memory: `MmAllocateContiguousNodeMemory` with `PAGE_NOCACHE`
  (or `MmAllocatePagesForMdlEx` + `MmMapLockedPagesSpecifyCache(MmNonCached)` for non-contiguous pages);
  physical addresses can be above 4 GiB (bank 2 at 0x880000000); Mali supports 40-bit output addresses.
- User buffers in stage 3: an IOCTL interface like StarbiteMali's (CREATE_BO, MMAP_BO, SUBMIT, WAIT_BO,
  GET_PARAM; Panfrost's `panfrost_drm.h`) so wddm-mali-g52's libdrm shim and our Mesa can run on it before
  WDDM exists. Buffers mapped write-combined into the process.

### 5.3 Fences

Stage 3: one sequence number per job slot, completed in the DPC; WAIT_BO waits on a KEVENT per BO's
last job (StarbiteMali completes inside SUBMIT; Windows must not, a job can take seconds).
Stage 4: WDDM fences (§6).

### 5.4 Power and clocks: what we never do

- Never switch the G3D power domain off. Turning it back on needs the TZPC restore SMC, which Windows'
  kernel does not issue and which we have not tested outside XNU. Idle = cores off + lowest DVFS level.
- Never write CMU_G3D, PLL_G3D, or any MIF/INT/CPU clock. The only clock action is an ACPM DVFS request for
  domain 4 with one of the six table frequencies.
- We never write BUCK6 above 950 mV (ACPM's own table may, on a DVFS request: read back and logged);
  S-Boot's value is restored when the driver unloads.
- No sustained load without the watchdog: the driver checks the watchdog callback before each submit batch.

## 6. Stage 4: WDDM and Mesa

### 6.1 Kernel half: `sbwddm.sys`, WDDM 1.3

Follow maliwddm / viogpu3d (wddm-mali-g52 `docs/M5.2-PLAN.md`), not WDDM 2.0's GpuMmu: Panfrost needs a
buffer's GPU address when it creates it, and WDDM 1.x lets the driver keep its own GPU address space.

| WDDM concept | Our implementation |
|---|---|
| Memory segments | One **aperture segment** (system memory; VidMm hands us each allocation's pages in `BuildPagingBuffer(MAP_APERTURE_SEGMENT)`). **Primaries: open.** dxgkrnl insists the segments of a shared primary be CPU-visible, which an aperture segment never is: maliwddm got a black screen and no `SetVidPnSourceAddress` until it made primaries section-backed (`adapter.c:280-288`), i.e. ordinary system memory with no contiguity or 4 GiB guarantee, flipped by CPU copy (`command.c:437-470`). For real DECON flips we need a CPU-visible memory segment on a firmware-reserved contiguous region below 4 GiB (DECON's base registers are 32 bits). Until that works: copy-flips, as maliwddm |
| GPU MMU | Driver-owned VA per allocation, reserved in `DxgkDdiCreateAllocation` and returned in private data; `MAP/UNMAP_APERTURE_SEGMENT` writes/clears PTEs at that fixed VA (the reservation survives eviction, so addresses never change). `Render` still emits **one patch location per allocation**: VidMm pages in what the patch list references (wddm-mali-g52 PE runs 13-14). Unmap ends with LOCK + FLUSH_PT before returning (VidMm reuses the pages). First one address space for all processes (as wddm-mali-g52); then one of the 8 hardware address spaces per D3D device (isolation) |
| Nodes and engines | One 3D node (the job manager). Paging: map/unmap only, others `STATUS_NOT_SUPPORTED` (viogpu3d does the same **[claim]**) |
| Submission | `DxgkDdiRender` copies the UMD's command (job chain heads + slot) into the DMA buffer; `DxgkDdiSubmitCommand` writes HEAD_NEXT/AFFINITY/CONFIG/START; the job IRQ → `DxgkCbNotifyInterrupt(DXGK_INTERRUPT_DMA_COMPLETED, fence id)` + DPC |
| Fences | `SubmissionFenceId` per DMA buffer, completed in order (one node, chains run in order per slot; the fragment slot waits on the tiler chain by Panfrost's own job dependencies inside one submit) |
| TDR | `DxgkDdiResetFromTimeout` = §5.1 step 6; `DxgkDdiRestartFromTimeout` reloads the address spaces |
| Preemption | none (WDDM 1.3 allows it); long jobs risk TDR (2 s default) |
| Display | sbdisplay's code, with one required difference: a full driver must pin target-mode frequencies, a display-only driver must leave them unspecified (wddm-mali-g52 run 4: Windows stopped at boot otherwise). `DxgkDdiSetVidPnSourceAddress` → DECON flip (§7.3) once primaries allow it |
| Power | `DxgkDdiSetPowerState`: D3 = cores off + 260 MHz + BUCK6 back; D0 = §5.1 steps 2-4 |
| Thermal | watchdog callback: Warm → cap 338 MHz; Hot → 260 MHz; **Stop → keep executing at 260 MHz with idle gaps between DMA buffers** (dxgkrnl's timeout runs from `SubmitCommand` and there is no preemption, so holding work back would trigger repeated TDRs and finally bugcheck 0x116/0x117); **Critical → complete fences without executing (dropped frames) and power the cores off**; the desktop freezes until it cools. Whether a long Critical should end in a deliberate shutdown is open |

### 6.2 User half: Mesa 26.2

- Base: Mesa 26.2.x with wddm-mali-g52's patches 0001-0020 (MSVC ABI fixes for Panfrost bitfields, Windows
  portability, d3d10umd + Panfrost glue, present through DXGI). They target the G52 but most are
  architecture-neutral; Panfrost builds all Bifrost/Valhall architectures including v6.
- **Our deltas:** the G72 is `PAN_ARCH 6`: check what Mesa 26.2 needs on v6 that the G52 path did not
  exercise (v6 tiler heap/context descriptors, no IDVS group size, the "G72 r0p1" quirks in Mesa's
  `panfrost_model` table) **[unverified]**; `pan_kmod` WDDM backend talks to sbwddm's private-data ABI
  (allocation VA, command format) instead of maliwddm's.
- Shaders: d3d10umd translates DXBC to TGSI, Panfrost converts TGSI to NIR (`tgsi_to_nir`) and compiles
  Bifrost **[unverified for 26.2's Panfrost; wddm-mali-g52 M4 shows it works on v7]**.
- Feature level: d3d10umd implements the D3D10 and D3D10.1 DDIs only; the D3D11 runtime creates a device at
  feature level 10_0 on it **[claim, wddm-mali-g52 CI]**.
- Build: in the lima VM `starbite` or on iCloud overflow (never the Mac SSD): meson cross build for
  `aarch64-pc-windows-msvc` with clang-cl, lld-link and the SDK/WDK headers from `tools/win/`. wddm-mali-g52
  built on GitHub's Windows runners; we cannot push, so the cross build is ours to make work **[unverified]**.
  Fallback: llvm-mingw `aarch64-w64-mingw32` if its DDI headers suffice **[unverified]**.

### 6.3 The open question that can stop stage 4: answered (research 2026-11-08): go

**Does DWM composite on an adapter whose UMD has only the D3D10/10.1 DDI?** On Windows 10, yes **[shown]**:
viogpu3d (virtio-win PR 943, unmerged; WDDM 1.3 KMD + Mesa d3d10umd on virgl) has DWM composing through
`dwmcore → d2d1 → d3d11 → viogpu_d3d10`. The author's verdict: DWM is "mostly fine with just D3D10" (WinUI 3
glitches, Electron windows black). The D3D11 runtime drives D3D10.0/10.1 DDI UMDs directly at feature level
10_x (`OpenAdapter10_2` version negotiation **[MS]**; Mesa's own d3d10umd test creates a D3D11 device at 10_0,
and wddm-mali-g52 did so on Panfrost **[shown]**). Not shown on **Windows 11**: viogpu3d's 22H2 reports are mixed
(black screens, one very slow desktop). The G72 under Panfrost has no geometry shaders, so "10_0" is nominal.
DWM does not use them **[inferred]**.

What this changes in the plan:
- **One full driver.** A render-only Mali driver next to sbdisplay would serve apps that choose it. DWM
  composes on the display adapter and would stay on WARP. Microsoft documents render-only + display-only
  pairing only for paravirtualised VMs (WDDM 2.4+). wddm-mali-g52's render-only attempt was refused with
  `STATUS_GRAPHICS_INVALID_DRIVER_MODEL` **[shown]**. So sbwddm owns both the Mali and the scanout: sbdisplay's
  display half moves into it.
- **No WARP fallback when the UMD fails.** A KMD that fails to start falls back to Basic Display + Basic
  Render **[MS]**. A KMD that starts with a broken UMD gives a black desktop (wddm-mali-g52 run 6: "desktop
  and RDP black") **[shown]**. Every stage-4 boot needs the kill file, the boot guard and a dead-man way back.
- **WDDM 1.2's mandatory features** (Windows refuses the adapter otherwise **[MS]**): preemption (DMA-buffer
  granularity, the minimum, fits sbg's in-order queue; viogpu3d disabled preemption system-wide), offer/
  reclaim, TDR (sbg_tick + ResetFromTimeout), Direct Flip (full drivers). Windows 11 does not appear to
  enforce WDDM 2.0 for a driver to load. "WDDM 2.0" is a hardware requirement in the spec **[inferred]**,
  and our WDDM 1.2 display-only driver already loads on 23H2.
- **The UMD:** d3d10umd frozen at **Mesa 26.2.x** (removed upstream by commit 1f9af627, MR 44717,
  2026-09-30; absent from 26.3). Start from the viogpu3d lineage (arehnman/virtio-win-mesa: WDDM
  allocations, flip presents, swap-effect fixes, `SUPPORT_D3D10_1` on) and replace its virgl winsys with a
  Panfrost one talking to sbwddm through private data (sbg underneath). We keep a TGSI frontend that
  upstream deleted.
- **The real blocker** is the plumbing DWM needs and test programs do not: shared redirection surfaces
  opened across processes, primary flips, residency, preemption, TDR. Any of it wrong = black desktop.
- Other routes, ruled out: D3D9 via Gallium Nine (VirtualBox's route; Nine removed in Mesa 25.2), D3D12 /
  11On12 (needs 11_0 + WDDM 2.0), D3D11-DDI → DXVK → PanVK (a far larger stack; Dozen goes the wrong way).

Sources: github.com/virtio-win/kvm-guest-drivers-windows/pull/943; github.com/arehnman/virtio-win-mesa;
github.com/gahingwoo/wddm-mali-g52; learn.microsoft.com (version discovery, D3D11 DDI init, WDDM 1.2 features
and enforcement, Basic Display, GPU paravirtualization, hybrid cross-adapter resources, IddCx);
gitlab.freedesktop.org/mesa/mesa MR 44717; phoronix.com (d3d10umd and Nine removals).

Other stage 4 risks, in order: primaries (§6.1, open); the TDR without preemption on 18 cores at
260-572 MHz; a WDDM 1.3 full driver on installed 23H2 (it loaded on wddm-mali-g52's installed Windows, but
with a black screen first; their run 6 under a full driver gave "desktop and RDP black", which on our
phone is the no-network black-screen case of §4.3, so stage 4a runs only behind the kill switches and the
10-minute guard); TGSI leaving Mesa before we are done (we pin 26.2); DECON flips at 60 Hz (the Linux
DECON driver hit a 30 fps flip cap; the `early_event` fix in K2 is built but not yet booted).

### 6.4 The scheduler core: `sbg_core` (built 2026-11-08, host-tested; T12 on the phone)

The kernel half stage 4 needs whichever user half wins §6.3. It is portable C on top of sbm_core
(`starbite/win/sbmali/sbg_core.{h,c}`), shaped like Linux Panfrost so Mesa's Panfrost sees the hardware model
it knows:
- **Buffers:** the caller's 4 KiB pages (WDDM: VidMm's aperture pages; now MDL pages) mapped at a GPU
  address the core picks in [32 MiB, 4 GiB) (Panfrost's window; > 64 KiB buffers 64 KiB aligned), or at a
  fixed one. LOCK + FLUSH_PT after every map and unmap. **Deferred release:** a buffer is unmapped only once
  the fence it was last used under has completed (VidMm reuses pages as soon as we return).
- **Submits:** one job chain per submit on slot 0 (fragment), 1 (vertex/tiler) or 2 (compute), queued
  (64 deep), one chain on the GPU at a time, fences completed strictly in order (WDDM 1.x: one node). The
  dependency Mesa expresses between a tiler and a fragment submit is therefore always met.
- **Completion:** from the job/MMU/GPU interrupt (`sbg_irq`) or polled. **Faults** (MMU, job, GPU): HARD_STOP,
  soft reset, cores and AS0 back (tables kept), the submit completes as FAULT and its **context is guilty**:
  its later submits complete unrun (D3D "device removed"), other contexts carry on. **Timeouts** (`sbg_tick`,
  default 2 s = WDDM's TDR delay) take the same path.
- Not yet: slot overlap (vertex of frame n+1 during fragment of frame n), one address space per context
  (isolation), growable tiler heaps (Panfrost's fault-driven HEAP buffers; for now heaps are mapped whole),
  power gating between frames.

Host test (`tests/mali_test.c`, deferred-execution fake): 90 buffers placed without overlap, with reuse after
release; fixed addresses refused when taken or outside the window; 10 chains across two slots, in-order fences,
never two on the GPU; a fault from context 2 (reset, context 2's next submit skipped, context 3's ran,
interrupts re-armed; context 2 back after `sbg_ctx_reset`); a hang (nothing before 2 s, then hard stop + reset,
the next chain runs); deferred release; XNU's triangle through the scheduler. Five deliberate bugs in the core
(no guilt, no deferred release, no timeout, one page mapped for all, an off-by-one fence) each fail it.

**T12 passed on the phone (2026-11-08 18:54, sbmali-e3fdf550, `build/win/results/t10-t12-hive`):**
- 100/100 rounds through sbg at 572 MHz, on scattered Windows pages, each round 8 WRITE_VALUE chains plus
  the triangle.
- 86,528 triangle pixels in every round, 0 mismatches, 0 foreign pixels.
- The slowest round (10 chains, queued at once, polled) took 1,008 µs.
- The deliberate fault: status FAULT on context 2, AS fault 0x7C1002C1 (TRANSLATION_FAULT level 1) at
  0x7FF00000, JS_STATUS 8 (still active when the MMU faulted). One soft reset, no reset failure, and the
  next context's chain ran.
- 1,002 chains started (1,000 + the two of the fault test), none skipped.
- T8's long run in the same boot: legacy and AArch64 200/200 each, 0 faults.

So the stage-4 kernel half queues, completes, faults and recovers on the real GPU.

**T12's first build (sbmali-0b1e6520, 2026-11-08) never ran: the main session's pre-flight found it would
hang.** A text replacement had turned sbg_core's `barrier()` helper into a call to itself (`if (io->barrier)
barrier(g);`), which clang compiled to `b .` (6 places, the map/unmap/start paths). The host test missed it
because its fake had no barrier callback, so the branch was never taken. Fixed, and the scheduler test now
runs with a counting barrier: the old code hangs under it. `tools/win/check-selfloop.sh` (any branch to its
own address) now fails `starbite/win/build.sh`. A source scan for self-calling functions finds no other.

**T12 (phone):** sbmali runs, after T8's runs, `SchedRuns` rounds (default 100) of 8 WRITE_VALUE chains plus
the triangle, all queued at once through sbg, on buffers made of scattered Windows pages (MDLs), polled; then
one deliberate MMU fault (a chain at 0x7FF00000, nothing mapped there) followed by another context's chain,
which must run after the reset. State: `Sched_Init` 0, `Sched_BoFail` 0, `Sched_Rounds` = `Sched_Ok` = 100,
`Sched_TriPx` 86528, `Sched_TriMismatch` 0, `Sched_TriBadPx` 0, `Sched_FaultTest` 1, `Sched_Faults` 1,
`Sched_Resets` 1, `Sched_ResetFail` 0, `Sched_Timeouts` 0, `Sched_MaxRoundUs`. Off with `Parameters\Sched` = 0;
`SchedFault` = 0 skips the fault; `SchedFmt` 0/1 = legacy/AArch64 tables (default AArch64).

### 6.5 The UMD ↔ KMD ABI: `sbx_abi.h` (built 2026-11-08, host-fuzzed)

`starbite/win/sbwddm/sbx_abi.h` (MIT, so the Mesa side can include it) and its kernel-side validator
`sbx_cmd.{h,c}`:
- **Allocations:** `DxgkDdiCreateAllocation`'s per-allocation private data is `struct sbx_alloc` (version,
  kind: buffer / texture / render target / tiler heap / primary / shadow, flags: no-exec, CPU-mapped, shared,
  size). The KMD writes the GPU address back: fixed for the allocation's life (sbg_core places it), as
  Panfrost needs it at creation.
- **Submission:** the command buffer the UMD hands `DxgkDdiRender` is a header plus up to 16 jobs `{slot,
  allocation-list index, offset}`. A chain is named by an entry of this DMA buffer's allocation list, never a
  raw GPU address. The KMD resolves it, so a chain can only start inside a buffer allocation that this
  submission references and VidMm made resident. Never in a primary, shadow or heap. Always 64-byte aligned,
  with a whole descriptor inside.
- The parser trusts nothing beyond the length it is given and reads byte by byte. Host test: every rule,
  plus 200,000 random and mutated buffers under AddressSanitizer at exact length, where any accepted job
  must lie inside a buffer allocation.
- **Not yet:** the descriptors' own pointers (which the GPU follows inside its address space) are not
  checked. Per-context address spaces (§6.4 "not yet") are what will contain a hostile chain; until then,
  one shared AS, as wddm-mali-g52.

### 6.6 Next, in order (2026-11-08)

1. **T12 on the phone:** the scheduler on the real Mali (staged).
2. **sbwddm skeleton** (WDDM 1.3 full driver, compile-checked): sbdisplay's display half, sbg as the render
   half, sbx for Render/Patch/SubmitCommand. One aperture segment, `BuildPagingBuffer` map/unmap through
   sbg, fences from sbg_irq via `DxgkCbNotifyInterrupt`, `ResetFromTimeout` = sbg's recovery, preemption at
   DMA-buffer granularity, the WDDM 1.2 mandatory DDIs (offer/reclaim, Direct Flip) present.
3. **The UMD build:** done for upstream 26.2.6 + wddm-mali-g52's patches (§6.7). Still to do: port the
   viogpu3d lineage's WDDM present/allocation changes and replace g52's malikm shim with a winsys on sbx.
4. **4a on the phone:** sbwddm + the UMD with softpipe first (proves the WDDM plumbing DWM needs without the
   GPU), then Panfrost: behind the kill file, the guard and a dead-man way back (no WARP fallback).

### 6.7 The UMD build: Mesa 26.2.6 for Windows ARM64 (built 2026-11-08)

`tools/win/mesa/build-mesa.sh` (README alongside) cross-builds Mesa 26.2.6 in the lima VM:
- **Toolchain:** clang 18 in `cl` mode, lld-link, and the MSVC/SDK kit from `~/sbwin` (MSVC ABI, as the D3D
  runtime needs). Hybrid CRT. The tarball is checked against the SHA-256 in Mesa's release notes.
- **Patches:** wddm-mali-g52's 20 Mesa patches (GPL-2.0, vendored in `tools/win/mesa/g52/`) apply unchanged
  to 26.2.6.
- **Panfrost's precompiled kernels (libpan)** and the three build-time tools (`mesa_clc`, `vtn_bindgen2`,
  `panfrost_compile`) come from a native Linux build of the same patched tree with LLVM 18. The Windows
  cross build uses them through `-Dmesa-clc=system -Dprecomp-compiler=system`. That is what makes a cross
  build possible: wddm-mali-g52 built natively on a Windows ARM64 runner.

Results, with zero failed steps in each:
- `libgallium_d3d10-softpipe.dll` (10 MB)
- `libgallium_d3d10-panfrost.dll` (13 MB, softpipe + Panfrost including the Mali-G72 model)

Both are ARM64, export `OpenAdapter10` / `OpenAdapter10_2`, and import only GDI32, KERNEL32, USER32 and the
in-box UCRT. The Panfrost build looks for the GPU as wddm-mali-g52's `\\.\MaliG52` device or a maliwddm
adapter. When it finds neither it says "Panfrost found no Mali device, using softpipe". Our device side
(sbwddm / sbx) will replace patches 0014/0015/0019 and `malikm_drm.c`.

**T11a (phone, user mode only, nothing installed, GPU untouched):** `triangle.exe` (wddm-mali-g52's test,
built with tools/win/build.sh) creates a D3D11 device at feature level 10_0 on each DLL as a *software
adapter*, draws a red triangle on blue, and checks two pixels. Exit 0 = right. Proves the cross-built UMD
loads in the phone's D3D runtime and renders (softpipe), and that the Panfrost build falls back cleanly.
Staged in `build/win/stage/mesa-t11a/` with SHA256SUMS. `starbite/win/autotest/d3dtest.ps1` runs both with
a 180 s limit each (autotest `--robust` hook, or by hand).

Disk: the VM work tree is about 1.3 GB (src 504 MB, build-panfrost 161 MB, LLVM packages); the build dirs
were cleaned after copying the outputs out. The pristine extract `~/sbmesa/mesa-26.2.6` (392 MB) is
redundant and can go.

### 6.8 sbwddm: maliwddm ported to the S9 (built 2026-11-08, compile-checked; T13 staged)

**Base:** wddm-mali-g52's `maliwddm`, a WDDM 1.3 full driver (render + display) with which Mesa's Panfrost,
loaded by the D3D runtime as the hardware UMD, drew a D3D11 triangle on their Mali-G52 (their M5.2 c2). DWM
was not reached there. Copied verbatim (commit ff38591, `starbite/win/sbwddm/maliwddm`, `common/`, GPL-2.0),
then ported. Its user↔kernel ABI (`maliwddm_abi.h`: allocation private data, `MW_COMMAND` job chains,
escapes for GPU info and allocation GPU addresses) is kept as it is. **So the Mesa DLL of §6.7 is the UMD
unchanged**: g52's shim finds the adapter by its GPU-info escape. sbx (§6.5) remains our stricter ABI for later;
its rule is applied now on the kernel side (below).

**The S9 port:**
- **Hardware layer** (`common/maligpu.c`): G3D domain check (PMU +0x44), GPU_ID 0x6221, power-up as sbm_core
  (no G52 quirks: no IDVS group size on the G72), the leaf 0x2C1 that ran on this G72 in T5/T8 and under
  XNU (not their 0x2C5 + XN). Legacy table format, as T8 run 1. A stray level interrupt before bring-up is
  masked and cleared.
- **Platform** (`s9.c`): sbmali's accessors, sbthermal's gate and ACPM lock. vdd_g3d to 950 mV before the
  GPU, back on stop. The clock stays at 260 MHz (no DVFS yet).
- **Start order:** the GPU first (watchdog alive within `ThermalWaitMs`, domain on, rail, MMU, GPU), the
  display after. **The GPU is not optional** (upstream allowed it): a failed bring-up refuses the start
  before the firmware framebuffer is taken, so Basic Display keeps the desktop. **No interrupt lines** (the
  stage-2 SSDT has none) refuse the start after a full, logged bring-up: without them fences never complete
  (TDRs, then bugcheck 0x116).
- **Jobs** (`command.c`): every `MW_COMMAND` chain must start inside a non-primary allocation of that
  render's allocation list (`sbx_head_in`: 64-byte aligned, a whole descriptor inside, no wrap;
  host-tested). Upstream accepted any address. While the watchdog says Stop/Critical, or is silent, fences
  complete without running (dropped frames).
- **Modes and guard** (`guard.c`, `sd_embed.c`): one binary with **sbdisplay built in**. Full mode only with
  `Parameters\FullMode` = 1, no kill file `\SystemRoot\Starbite\sbwddm.full.off`, and `FullGuardBoots` = 0.
  The boot is counted at once, and cleared only when healthy: 60 s after start, with ≥ 30 flips/blts and
  ≥ 30 GPU jobs done. Otherwise the next boot is display-only (= sbdisplay, today's desktop). That is the
  answer to "no WARP fallback" (§6.3).
  **Crash bounds:** full-mode crash → next boot display-only; display-only crash twice → the third boot
  refuses (sbdisplay's GuardBoots) → Basic Display. **Kill files** (`C:\Windows\Starbite\`, droppable from
  TWRP): `sbwddm.off` turns the whole driver off and is checked before anything else (also covers a fault
  in the entry code, which neither guard counts); `sbwddm.full.off` turns off full mode; `sbdisplay.off`
  turns off display-only mode.
- **Interrupts for full mode:** `starbite/win/acpi/StarbiteGpu4.asl` (table revision 4) adds the Mali's lines
  (vendor DT: JOB SPI 282 = GSIV 314, MMU 281 = 313, GPU 283 = 315, level). It is installed by the shared
  SSDT firmware DXE (another track's BOOT flash).
- **Packaging:** `starbite/win/build.sh` builds sbwddm.sys (89 KB, /W4 clean, no self-branches), signs it with
  sbumd.dll (= §6.7's Panfrost DLL) and makes sbwddm.cat. `tools/win/hive-service.py display-bind SERVICE
  [--umd DLL] [--preemption 0|1]` switches STRB0072 between sbwddm and sbdisplay offline (Enum Service,
  InstalledDisplayDrivers, UserModeDriverName). `param NAME --root` resets FullGuardBoots. Install and
  rollback were tested on a copy of the latest hive.

**T13a passed (2026-11-08 19:13, user present, sbwddm-60fe1ef8; `build/win/results/t13a`).** Boot 1, full mode:
- dxgkrnl ran the full driver's DriverEntry, AddDevice and StartDevice.
- The G72 came up under WDDM (Step 6 READY, GpuId 0x62210010, GpuUp 1), with the watchdog alive (S9ThermalGate
  0: sbthermal as a system-start service was up in time) and the rail raised.
- Then the designed refusal: S9Interrupts 0, S9NoInterrupt 1, Fail_StartGpu 0xC00000A3. Basic Display took the
  desktop.
Boot 2: display-only by the guard (WddmMode 0, FullModeWanted 1, FullGuardBootsAtStart 1, FullRefused 1), and
the user saw the normal desktop.
**T11a passed in the same boot (by hand):** the cross-built d3d10umd drew the triangle through the phone's
D3D11 runtime (softpipe PASS). The Panfrost build fell back cleanly with no Mali adapter (PASS). When Windows
loads it as the hardware UMD, the shim binds through the runtime's callbacks and finds sbwddm by its GPU-info
escape (`malikm_bind_d3dddi`). The `\\.\MaliG52` / maliwddm names in its messages belong only to the
software-adapter path.
**T13b's firmware:** `build/win/stage/windows-t13b.uefi` (sha256 7f915969…). This is combo3b (the store's
windows.uefi, CRC-32 c0d50c84) with only the SBGPU blob swapped for StarbiteGpu4 (`ssdt-ffs.py` +
`dxe-replace.py --kill-blank/--add`). Checked:
- `dxe-diff` shows the outer FV identical and only SBGPU changed.
- The boot shim and everything before the DXE volume are byte-identical, and the size is the same.
- There is one live SBGPU blob, holding StarbiteGpu4 byte for byte.

**T13a (no firmware change, safe): full mode's bring-up under WDDM.** sbwddm bound with FullMode = 1 on the
stage-2 SSDT: the whole GPU bring-up runs under dxgkrnl and is logged (Enum\ACPI\STRB0072\0\Device
Parameters\sbwddm). Then the start is refused for want of interrupts, and that boot runs Basic Display.
The next boot is display-only (State: WddmMode 0, FullRefused 1) = today's desktop. Proves: dxgkrnl accepts
the full DDI table (DriverEntry, AddDevice, StartDevice), the G72 comes up under WDDM with the watchdog and
the rail, the guard and the refusal paths. **T13b (needs StarbiteGpu4 in firmware):** full mode for real,
DWM on the Mali through Mesa. Expected first result per wddm-mali-g52: black desktop, then the guard. Logs
say how far DWM got. Steps for both, with rollback: `build/win/stage/sbwddm-t13/INSTALL.md`.

### 6.9 T13b: DWM on the Mali got to its first Render (2026-11-08 19:34)

`windows-t13b.uefi` (Mali interrupts) + sbwddm full mode + sbumd.dll, with the user present. The Windows logo
and spinner showed, then a black screen with the spinner for minutes (the login screen normally comes within
seconds). Forced restart at about 19:38, no bugcheck (`build/win/results/t13b`).

**What the trace shows:**
- The start: GpuUp 1, S9Interrupts 3, Started 1.
- Then the full WDDM bring-up, in order: QueryChildRelations, QueryAdapterInfo, CreateDevice, CreateContext,
  BuildPagingBuffer, QueryDeviceDescriptor (no EDID, as always), RecommendMonitorModes, CofuncModality,
  GetStandardAllocationDriverData, CreateAllocation, OpenAllocation, DescribeAllocation, CommitVidPn,
  SetVidPnSourceAddress, SetVidPnSourceVisibility, ControlInterrupt (vsync on), Present, Patch, **Escape,
  Render**, then DestroyContext and DestroyDevice.
- No DDI failed after QueryInterface (LastFailedDdi unchanged).
- The missing Seen_Render / Seen_Escape are **trace artifacts**: upstream's first-call bits were a hash of 64
  slots, and Fail_Render collides with CommitVidPn, Fail_Escape with QueryChildRelations, SubmitCommand with
  CreateContext. Nothing after each DDI's first call was logged.
- The pull also still held T13a's Fail_StartGpu / S9NoInterrupt (the trace key persists across boots).

So it is not known whether the GPU jobs, fences, flips or DWM itself failed.

**T13c (staged, `build/win/stage/sbwddm-t13c/`):**
- sbwddm: an exact first-call table instead of the hash, the trace key emptied at each AddDevice, and `Live_*`
  counters (renders, submits, fences submitted/completed, jobs done/failed/timed out, the last JS status and MMU
  fault with its address, interrupts, resets, flips and flip copies/skips, vsyncs, presents, maps, rejected
  chains, thermal holds) written each second when they change.
- sbumd.dll: `tools/win/mesa/sb-patches/0101`. With `C:\ProgramData\Starbite\umdlog\` present, every process
  loading it logs its stderr there, d3d10umd's debug messages included. With `umdlog\trace`, every driver call
  is logged too.
- Before T13c: T13b's own Application/System event logs and WER reports may already name DWM's exit code or a
  faulting module.

### 6.10 Why T13b's DWM died: tgsi_to_nir has no D3D10 texture opcodes (fixed for T13c)

**The evidence:** T13b's WER reports (`build/win/results/t13b/WER`) show 18 dwm.exe and 11 LogonUI.exe crashes, all
`ucrtbase.dll+0x76ecc`, exception 0xC0000409 with data 7 (FAST_FAIL_FATAL_APP_EXIT, i.e. `abort()`), with
sbumd.dll loaded. The process was gone right after its first Render: dxgkrnl's DestroyContext/DestroyDevice were
the teardown.

**The cause:** d3d10umd translates DXBC to TGSI with the D3D10 texture opcodes (`SAMPLE`, `SAMPLE_B`, `_C`,
`_C_LZ`, `_D`, `_I`, `_L`, and `SVIEWINFO` for RESINFO), separate texture and sampler. Panfrost accepts NIR
only (`supported_irs` = NIR), so every shader goes through Mesa's `tgsi_to_nir`. That implements none of
these opcodes: it prints "unknown TGSI opcode" and calls `abort()`. This explains three observations:
- wddm-mali-g52's D3D11 triangle had no texture, so it never met them;
- softpipe (T11a) takes TGSI directly;
- so does virgl (viogpu3d), where DWM ran.
Every DWM shader samples a texture.

**The fix in sbumd** (`tools/win/mesa/sb-patches/0102`): d3d10umd already has a legacy texture path (`TEX`,
`TXB`, `TXL`, `TXD`, `TXF`, `TXQ`), which tgsi_to_nir handles and which gallium's own blitter shaders exercise
on Panfrost every day. It was compiled into debug builds only (`ST_DEBUG_OLD_TEX_OPS`). Now `st_debug` exists
in release builds too, and device creation turns the legacy path on when the fragment stage cannot take TGSI.
softpipe keeps the D3D10 opcodes.

Known limits, each logged by d3d10umd:
- a texture is read through its sampler's index (fine when tN pairs with sN);
- SAMPLE_C's compare value is not applied (shadow comparisons read the raw value);
- SAMPLE_C targets other than 2D are flagged.

The proper fix is to implement the D3D10 opcodes in tgsi_to_nir: separate texture and bare-sampler variables,
which `nir_lower_samplers` already resolves independently. That needs a native test harness (NIR validation +
the Bifrost compiler for 0x6221) and comes next.

**Diagnostics:** with the umdlog active, a SIGABRT handler writes the stack (offsets in sbumd.dll) before the
process ends. An abort() cannot be turned into a returned error: it happens deep in shader translation, which
has no error path back to the D3D runtime.

### 6.11 T13c: DWM runs on the Mali; LogonUI and every present do not (2026-11-08 20:02)

`build/win/results/t13c`. The hive had to be replayed from SYSTEM.LOG1/LOG2 (tools/woa/hive-replay.py): the
Live_ values are lazily flushed.

**The GPU side works** (Live_, 113 s):
- 1,912 GPU jobs done, 0 failed, 0 timed out, no MMU fault, no reset.
- 2,124 fences submitted and 2,124 completed, 1,910 interrupts: the StarbiteGpu4 lines work.
- 352 maps, 374 allocations, 7,597 emulated vsyncs.

The texture fix of §6.10 held: no DWM crash. DWM's umdlog shows it submitting work the whole time.

**Nothing reached the panel:**
- DWM's log has **241 × "pfnPresentCb failed"**. g52's present path (patch 0019, `malikm_present_d3dddi`)
  copies the frame on the CPU into a private allocation and calls `pfnPresentCb` with only a source: no
  destination allocation and no tie to the presented resource. That does for a test window in WinPE, not
  for the compositor.
- Only 2 flips and 1 flip copy happened.
- Upstream d3d10umd was written for software rasterizers:
  - `OpenResource` is `LOG_UNSUPPORTED` + `E_OUTOFMEMORY`;
  - `CreateResource` ignores MISC_SHARED;
  - no allocation is ever associated with the runtime's resource handle.
- **LogonUI crash-looped** (12 new WER reports: combase.dll, E_INVALIDARG, fail-fast) right after creating its
  1024×1024 shared BGRA surface (bind SRV+RT+PRESENT, misc SHARED). A shared resource with no
  resource-associated allocation has nothing the runtime could share.

**The guard was wrong:** it counted flips + any present-blt (212) and marked the boot healthy, so
FullGuardBoots went back to 0. It now requires frames that reached the panel (flip copies + blts into the
primary, `Live_PrimaryBlts`).

**T13d, from the working design of arehnman/virtio-win-mesa** (branch yttrium-experimental, read
2026-11-08):
- **CreateResource** hands the runtime's resource handle (and primary/VidPn) to the winsys before
  `resource_create`, so the allocation is made with `hResource = hRTResource` (+ `Flags.Primary`,
  `VidPnSourceId`). Shared, present and primary resources get dedicated linear BOs.
- **Present** passes the source and destination resources' own allocations (no CPU readback) with
  `pDXGIContext` and the context.
- **OpenResource** imports the shared allocation (its handle and private data; GPU address via
  MW_ESCAPE_ALLOC_INFO).
- **SetDisplayMode** goes through `pfnSetDisplayModeCb`.
- **KMD:** UMD primaries are treated as primaries. A present's CPU copy waits for its turn in the queue (it ran
  at SubmitCommand, possibly before the GPU had finished the source).

### 6.12 T13d: runtime allocations, presents, shared surfaces (built 2026-11-08; staged)

`tools/win/mesa/sb-patches/0103` (after arehnman/virtio-win-mesa's yttrium design) and sbwddm.

**sbumd.dll:**
- **Resource allocations:** for a 2D resource that is shared, has BIND_PRESENT or is a primary,
  d3d10umd arms the shim with the runtime's hRTResource, the surface's size, a 64-byte-aligned pitch and
  the D3DDDI format (+ Flags.Primary and VidPnSourceId for a scanout primary; an optional primary gets
  NO_SCANOUT). Then it creates the pipe resource with SHARED|LINEAR|DISPLAY_TARGET. The shim's next
  CREATE_BO in that thread becomes `pfnAllocateCb` with `hResource`.
- **Panfrost:** never recycles a shareable BO (cache fetch and put), so that allocation is fresh. A PRIME
  export (an in-process fake FD = tag | GEM handle) marks the BO shared, so Panfrost never swaps it, and
  names its allocation.
- **Logging:** Panfrost's actual stride is logged next to the pitch told to the KMD.
- **Lifetime:** a resource's allocation is released by `hResource` after an idle wait. An opened allocation is
  never destroyed by us.
- **Present** passes `hSrcAllocation` / `hDstAllocation` of the resources themselves, plus `pDXGIContext`,
  to `pfnPresentCb`. No CPU readback, and the HRESULT is logged.
- **OpenResource** imports another process's allocation, using its handle and our private data (size,
  pitch, format): GPU address via MW_ESCAPE_ALLOC_INFO, then `resource_from_handle` (FD, linear, the
  pitch). pan_kmod's import gets the size from the shim, since `lseek` on the fake FD would fast-fail.
- **SetDisplayMode** goes through `pfnSetDisplayModeCb`.
- **Reproducibility:** the three sb-patches apply in order to the g52-patched tree and reproduce the built tree
  exactly.

**sbwddm:** a present's CPU copy waits (up to 200 ms, `Live_PresentWaitTimeouts`) until the in-order queue
has drained, so it can't copy a frame the GPU is still drawing. The guard counts only flip copies and blts into
the primary.

### 6.13 T13d: frames reach the panel, black; T13e reads them with the right caching (2026-11-08)

**T13d (20:26), hive replayed from SYSTEM.LOG1/LOG2 (the primary hive was stale):**
- **GPU:** 4,224 renders, 4,680 submits, every fence completed, 4,221 jobs done, 4,220 IRQs. One job failed
  with JS status 0x51 (INSTR_INVALID_ENC: one shader binary) and was recovered by one GPU reset.
- **Presents:** 458 accepted, with no pfnPresentCb failures. 1,033 blts, of which 223 went into the framebuffer;
  237 present skips, 2 flip copies, no present-wait timeouts.
- **The panel stayed black.**
- **The guard:** declared the boot healthy (FullHealthy 1, FullGuardBoots 0), because black copies counted as
  frames. So the boot after it would have been full mode again; the peer put `sbwddm.full.off` in place.
- **umdlog:** only `trace`, although the UMD ran. Most likely the forced restart left NTFS dirty and TWRP read a
  stale MFT, the same way the hive was stale. A failed file open cannot be ruled out.

**Cause:**
- **The view:** the KMD's blts and flip copies read GPU output through `MmGetSystemAddressForMdlSafe`, a cached
  view.
- **The segment:** it is not cache-coherent, so VidMm gives its pages write-combined caching and the Mali writes
  DRAM directly.
- **The result:** a cached alias of those pages (mismatched attributes, which ARMv8 does not keep coherent)
  returns whatever lines the cache holds. Here that was zeros.

**T13e (sbwddm + sb-patches/0104):**
- **CPU view:** each allocation gets one, with the caching VidMm gave its pages (MAP_APERTURE_SEGMENT
  `Flags.CacheCoherent`: write-combined for our segment), mapped on first use by
  `MmMapLockedPagesSpecifyCache`. A racing vsync DPC and present keep one view between them. The view is
  unmapped on UNMAP_APERTURE_SEGMENT, on a remap, and on destroy. Destination writes to non-primary
  allocations use the same view.
- **Guard:** counts only frames with pixels: 16 samples of each flip copy's or primary blt's source, RGB not all
  zero. Healthy = 60 s, at least 30 content frames, at least 30 jobs. New `Live_ContentFrames`,
  `Live_CoherentMaps` and `Live_CpuMapFails`.
- **Notes:** escape `MW_ESCAPE_SB_NOTE` (0x5B01, {Code, Id < 64, Value}) is written as `UmdNote_<id>` in the
  trace key. This channel needs no file. sbumd sends:
  - the log status;
  - binds;
  - resource backing (fresh, missing, stride mismatches, the last format and size);
  - present OK and failure counts and the last HRESULT;
  - SetDisplayMode's HRESULT;
  - OpenResource results;
  - fallback presents;
  - aborts.
  The id table is in the stage's INSTALL.md.
- **Log fallback:** if a umdlog file cannot be opened, the log goes to `C:\Windows\Temp\sbumd-*.log`.
- **Staged:** `build/win/stage/sbwddm-t13e/` (INSTALL.md, SHA256SUMS), with firmware windows-t13b.uefi
  unchanged. After a forced restart, one clean display-only boot comes before TWRP reads the disk.

### 6.14 T13e: pixels into the framebuffer, panel still black; T13f looks at DECON (2026-11-08)

**T13e (20:46).** The hive was pulled right after the forced restart and replayed; the combo3b boot after it
had already reset the trace key.
- **Logon:** the session went past logon. DWM, Explorer, TabTip, TextInputHost and sblinkd all ran on the Mali.
- **GPU:** 4,133 jobs, every fence completed. One job failed with status 0x51 and one reset recovered it.
- **Copies:** 517 presents and 1,324 blts. 264 of the blts went into the framebuffer, and **257 of those had
  pixels** (ContentFrames), so the caching fix worked. There were only 4 flips (SetDisplayMode) and 2 flip
  copies: DWM presents by blt.
- **The panel stayed black.**
- **UMD notes:** logging worked (UmdNote_1 = 0). DWM made 3 primaries, 1440x2960, pitch 5760, A8R8G8B8.
  SetDisplayMode returned S_OK. 4 OpenResource calls succeeded.
- **The guard:** declared the boot healthy a third time (S9Healthy).

**Resolution.** Full mode offers only the POST framebuffer's own mode (1440x2960, the native panel).
Display-only mode's 1080x2220 is a scaled mode of sbdisplay's own and plays no part here.

**T13f** separates the two remaining explanations: the copy is not landing, or DECON is not scanning that buffer.
- **Readback:** each copy into the framebuffer is read back from it (FbContentFrames, FbKPixels, the last
  rectangle).
- **DECON:** its registers are read, never written, once a second: IDMA0 base live and shadow, image size,
  trigger control, window enables, shadow request. They are compared with the POST framebuffer's address,
  pitch and format.
- **Probe:** a magenta band is written straight into the framebuffer at 20 s and 40 s, for the person to see or
  not see.
- **Fix:** a blt into a primary also goes into that primary's own pages, so a later flip cannot copy stale
  black pages over the screen.
- **Guard:** it no longer clears itself while we are in bring-up (Parameters\GuardAutoClear = 0). Health also
  needs framebuffer readback.

### 6.15 T13f: DECON scans our framebuffer, DWM sends only dirty rectangles; T13g refreshes from the primary (2026-11-08)

**T13f (21:07):**
- **The probe:** the user saw the magenta band.
- **DECON's registers:** IDMA0 base, live and shadow, = the POST framebuffer (0xCC000000), 1440x2960,
  X8R8G8B8, pitch 5760, hardware trigger on (0x3061), no pending shadow update. So the path from the
  framebuffer to the panel works.
- **DWM's copies:** 195 of 202 copies into the framebuffer read back with pixels. Together they wrote only
  ~714K pixels, a sixth of one frame. The last one was 58x61 near the bottom centre. DWM sends dirty
  rectangles only.
- **The flips:** the only primary-to-panel copies of whole frames came from the 4 mode-set flips, before DWM
  had drawn anything.
- **Conclusion:** the panel was black because nothing ever put a whole desktop frame there.

**T13g emulates a scanout:**
- **The refresh:** at most every 200 ms (`RefreshMs`), and only when GPU work or a present completed since
  the last time, the whole frame is copied to the framebuffer. It is copied from the primary the last panel blt
  went into, or from the scanned primary after a flip. An all-black source is skipped, so the refresh can never
  blank the panel.
- **The cost:** one 17 MB copy from write-combined pages per refresh, measured as `RefreshUsMax`.
- **The census:** every present's flags are counted, the first 8 are logged in detail, and blts into the
  scanned primary are counted apart from blts into other primaries. This shows whether DWM's whole frame
  is in a primary at all (`ScanContent`). If it is not, the composed frame must be elsewhere, for example
  rendered only into a back buffer that is never presented whole.

### 6.16 T13g: a green stripe; DWM flips and we ignored it; T13h carries flips out (2026-11-08)

**T13g (21:32).** The panel went logo, then spinner, then black, then a bright green stripe down the right
~25%, full height.

The census:
- **Presents:** 94 flips and 215 blts. The blts were the spinner's rectangle, from a 1440x2960 surface into
  the scanned primary.
- **Flips:** only the 4 mode-set SetVidPnSourceAddress calls. dxgkrnl does not follow a WDDM 1.3 Flip present
  with SetVidPnSourceAddress here, and `MwPresent` (upstream: "flips are not handled yet") did nothing with it.
- **The panel:** it showed DWM's SetDisplayMode primary throughout, and that primary held the stripe
  (ScanContent 1). DWM's composed frames went into its other two primaries.
- **The cost:** a whole-frame copy from write-combined pages took up to 0.7 s. That is far too slow for the
  vsync DPC's flip copy, which T13h moves to the guard thread.

**T13h:**
- **Flips:** a Flip present is recorded (with a patch entry, so its source is resident) and carried out when
  it reaches the queue. `MwShow` makes its source the allocation the panel shows.
- **The shown allocation:** a blt into a primary, or SetVidPnSourceAddress, also makes that allocation the
  one shown; whichever came last wins.
- **The copy:** the guard thread copies the shown allocation whole when it changes or a flip arrives, and
  otherwise at most every 2 s.
- **The green:** 8 pixels of each copy's middle row are logged to identify it.

**Next:** take the copy off the CPU. Two candidates:
- the Mali copies into the framebuffer (map its pages into the GPU VA space);
- primaries rendered straight into the framebuffer.

### 6.17 T13h; the stale-present bug; a dead-man for unattended cycles (2026-11-08)

**T13h (21:46): black after the logo.**
- **Stale presents:** `MwRender` (from upstream) never cleared `MW_DMA_PRIVATE.Present`, and the runtime reuses
  DMA private-data buffers. So render submissions replayed old presents: 853 flips carried out for 92 flip
  presents, 1,137 blts for 458, through allocation pointers that might since have been freed. Fixed: the
  present op is zeroed in every render.
- **Black sources:** every allocation the panel was told to show read back all black (RefPx 0, 217 refreshes
  skipped as black). Whether DWM's real flips carry pixels is the next question.

**The dead-man (for unattended cycles).**
- **The loader:** it already arms cluster 1's watchdog for uefi rows with `wdt` > 0 and
  `wdt_feed = "own"` (`launch_prepare`: CW_OWN fits any method). The win11 rows had no `wdt`.
- **sbwdog.sys** (`starbite/win/sbwdog`), a boot-start service:
  - **Feeding:** a real-time thread writes WTCNT = WTDAT every 5 s.
  - **Unattended:** a clean `NtShutdownSystem(ShutdownReboot)` at 300 s in full mode (120 s display-only),
    called from a second thread so feeding continues. Feeding stops at 360 s (180 s).
  - **Never arms on its own:** unless `ArmSec` is set, the loader stays the only owner. Cluster 0 belongs to
    the woa UEFI and the UFS driver (RWD1).
- **`tools/win/gpu-cycle.sh`:** install (refused without catalog watchdogs or with a dirty hive), the test
  boot, the pull, a self-restarting display-only boot, a hive check, and disarm. Also `--deadman-test` and
  `--clean-only`.
- **Timing.**
  - The loader's watchdog runs up to 82.6 s: WTCON 0xFF39, 793.457 Hz, 16-bit.
  - Only hidden automation rows carry it: `win11-gpu` and `win11-auto`. The user's `win11` stays unarmed, so a
    normal boot can never be reset.
  - sbwdog records the gap it must cover: `GapMsAtStart`, from the watchdog's own count, and
    `UptimeMsAtStart`, from the generic timer since reset.
  - `gpu-cycle.sh --measure` takes that measurement as the first run. Over 82 s, the watchdog resets the phone
    before Windows and it returns to the menu anyway.
  - A UEFI-side feeder (a DXE timer event that only feeds an already-armed watchdog, stops after ~120 s of boot
    services, and does a last feed at ExitBootServices) is designed but not built. It is needed only if the
    margin is under ~20 s.
- **--measure (22:10).**
  - sbwdog's clean restart works: 126 s back to the menu, hive clean.
  - The loader's arming did not survive to Windows: WTCON 0, WTDAT 0xFE27 (its 82 s count), WTCNT held. Most
    likely the woa UEFI stopped it.
  - CNTFRQ_EL0 is 0, as S-Boot leaves it; the counter runs at 26 MHz.
- **T13j:** in Unattended boots sbwdog arms the watchdog itself when it finds it stopped, and re-arms it if
  anything stops it later. The user's own boots stay unarmed. A hang before sbwdog starts is a known gap until
  there is a UEFI-side feeder.
- **deadman-1 (22:19) proved nothing.** The loader recorded a CLEAN RESTART: T13j checked the kill file in
  DriverEntry, where a boot-start driver cannot see files yet, so sbwdog ran its normal restart.
  - Arming did take: WTCON read back enabled.
  - UptimeMsAtStart was 26.8 s, so the boot gap is well inside 82 s.
- **T13k:**
  - **Dead-man mode:** a one-shot registry switch, `DeadmanTest`. The watchdog is armed and never fed, with no
    restart and no cap. WTCNT is sampled at 10 s and 20 s to show the counter runs.
  - **Verdict:** `--deadman-test` passes only on the loader's `HANG (WATCHDOG)` record, and pulls the test
    boot's hive first.
- **deadman-2 (22:31) PASSED.** The loader recorded `HANG (WATCHDOG) rst 0x00800000`.
  - The counter runs: ArmWtCon 0xFF39; WTCNT 65,063 at arming, then 57,116 at 10 s and 49,180 at 20 s
    (7,935 per 10 s, as computed).
  - Windows lived 80 s. sbwdog started 27.3 s after the SoC reset.
  - The display-only boot after it restarted itself and left the hive clean.
  - Proven on the phone: boot-start loading, self-arming, the watchdog reset, and the clean restart.
  - **Remaining gap:** a hang before sbwdog starts (the first ~27 s: firmware, boot manager, winload).
- **Not yet shown on the phone:** that winload loads a Start 0 service from `System32\drivers`, that the UEFI
  leaves cluster 1 armed (sbwdog's ArmedAtStart and WtDat say), and that `NtShutdownSystem` from a driver
  leaves the hive and NTFS clean.

### 6.18 cycle-t13k-1: DWM's own frames are black; T13l tests texturing on the Mali (2026-11-08)

**The first unattended GPU cycle (T13k):**
- **The cycle itself:** a clean restart at 300 s, the display-only boot after it, and a clean hive.
- **Flips:** EmuFlips = PresentFlipFlag = 194, so the stale replays are gone. Every flip source is black
  (211 refreshes skipped, RefPx 0).
- **DWM:** it rendered (5,243 jobs, all fences), created its 3 primaries and set the display mode, and imported
  8 window surfaces. Its log has no failures.
- **Rotation is not the cause:** `RotateResourceIdentities` copies contents between the rotated buffers.
- **The spinner** comes from a non-primary surface (dxgkrnl's boot animation), not from DWM.

**Suspect: texture sampling.** DWM composes by sampling, and sb-patches/0102 routes D3D10 sampling through a
legacy TEX path that has not been checked on the Mali.

**T13l: render tests at logon in the full-mode boot.** `starbite/win/autotest/sb-gputest.cmd`, one-shot and
armed by gpu-cycle.sh, runs:
- `triangle.exe hw`;
- `sbtex.exe hw`: `tools/win/mesa/tests/sbtex.cpp`, 9 tests: clears, a solid shader, Sample, SampleLevel,
  Load, GetDimensions, a BGRA texture, bilinear;
- the same 9 on softpipe, as the reference.

Results land in `pre/gputest/`.

**cycle-t13l-1 (23:00):**
- **The hardware tests never started:** the phone's adapter 0 is ours (vendor 0x42525453), but its description
  is the device's old DeviceDesc, "Starbite Display (Galaxy S9 DECON)", so "Mali" matched nothing.
- **The softpipe reference passed all 9 sbtex tests.**
- **The camera at +70 s showed black.**
- **T13m:** the tests pick the adapter by vendor (or "Mali"/"Starbite"), and are otherwise unchanged.

**cycle-t13m-1 (23:25):**
- **`triangle.exe hw` PASSED:** the first correct hardware D3D11 triangle on the Mali under Windows (FL 10_0,
  through sbwddm and sbumd).
- **sbtex hw crashed at its first shader pass.** Panfrost's Bifrost backend reports "Unhandled intrinsic
  load_vertex_id_zero_base" for SV_VertexID (tgsi_to_nir path) and leaves it undefined.
- **T13n:** sbtex feeds positions from a vertex buffer, and the SV_VertexID variant runs last as a known gap.
- **Unexplained, in every process:** one early job fails with JS 0x51.

**cycle-t13n-1 (23:48):** sbtex hw passed all 9 tests on the Mali, so texturing works. The suspect is now the
runtime-backed surfaces (sb-patches/0103): shared window surfaces and presented primaries. Every one of them
read black in the KMD.

**T13o:** `tools/win/mesa/tests/sbshare.cpp`, a shared B8G8R8A8 texture tested two ways:
- **In one process:** cleared, drawn into, read back, against a plain texture as the control.
- **Across two processes, as DWM:** the second process opens it, copies it, samples it, and sees a later clear
  by the producer.

**cycle-t13o-1 (00:15):**
- **Shared runtime-backed surfaces work in one process,** and a second process samples the opened surface
  correctly on the GPU.
- **CopyResource from the opened surface is wrong:** it read zeros, then stale contents after the producer's
  update.
- **Cause:** Panfrost copies textures on the CPU (`util_resource_copy_region`) through both BOs' mappings. The
  consumer's CPU view (a persistent pfnLockCb) doesn't track GPU writes to the shared pages.
- **The same CPU path carries all of DWM's copies,** including d3d10umd's `RotateResourceIdentities`: four
  full-frame copies per flip, through write-combined memory.

**T13p (sb-patches/0105):** same-format 2D colour copies go through `pctx->blit` (GPU); everything else keeps
the CPU path. sbshare adds a GPU re-sample after the producer's update.

**cycle-t13p-1 (00:38):**
- **All tests pass:** sbshare (open-copy, open-update and open-samp-red included), sbtex and the triangle.
- **DWM runs far more** (736 flips, 17,760 renders), but every flipped primary still reads black in the KMD,
  and the panel is black.
- **T13q samples the presented frame from both sides:**
  - the UMD GPU-copies one row of each sampled present's source and logs it (sb-patches/0106);
  - the KMD samples the flip source at the flip itself, and logs its GPU address.

**cycle-t13q-1 (00:52):**
- **The apps' frames have content:** LogonUI's 1440x2976 surface holds the wallpaper (32/32 non-black), and
  Explorer shows content too.
- **Every one of DWM's 51 presents (flips of a primary) is black.**
- **Cause:** d3d10umd's `_Blt` (DXGI `pfnBlt`) was an upstream stub returning S_OK. DWM composes into
  non-primary back buffers, and DXGI moves each frame into the primary with pfnBlt before the flip, so the
  primaries were never written.

**T13r (sb-patches/0107):** `_Blt` as a gallium blit, covering resolve, format conversion and stretch.

**cycle-t13r-1 (01:13):**
- **DXGI never called pfnBlt.**
- **DWM's only UMD presents are flips of a primary that nothing ever draws** (all zeros, alpha included).
- **DWM's frames reach the KMD as present blts** from its full-screen back buffer into the scanned primary,
  but only as dirty rectangles. Our emulated flips then showed the empty primary.

**T13s:**
- **A blt into a primary from a full-screen source** makes that source (DWM's back buffer) what the emulated
  scanout shows, whole.
- **Flips to an all-black allocation are ignored.**

## 7. Stage 2: the display-only driver (`sbdisplay.sys`)

A WDDM 1.2 display-only driver in the shape of Microsoft's KMDOD sample and wddm-mali-g52's `malidod`
(GPL-2.0, which proved the DDI details on 23H2; its five failed runs are our checklist: DRIVERCAPS sized
for the declared interface version, WDDMv1_2 + SupportNonVGA + SupportSmoothRotation, target modes without
fixed frequencies, two commits before presents).

### 7.1 Mode and takeover

- `DxgkCbAcquirePostDisplayOwnership` gives the firmware framebuffer (expected 1440 × 2960, X8R8G8B8,
  pitch 5760, 0xCC000000). The driver checks it against DECON's live state exactly as s9p-decon does:
  an enabled window covering the panel (DATA_PATH_CONTROL_0, WIN_START/END), IDMA **channel 0** active
  bank == the framebuffer, IMG_SIZE == 2960 × 1440. **No other IDMA channel is ever read** (a read of a
  clock-gated channel wedged the SoC on Linux). Any mismatch → **copy mode**: Basic Display's behaviour,
  with no DECON or DSIM access at all.
- One source mode = that one; one target (internal panel, `D3DKMDT_VOT_INTERNAL`, always connected).
  Identity rotation only in this version (as malidod); landscape by rotating copies comes later.
- `DxgkDdiStopDeviceAndReleasePostDisplayOwnership` points DECON back at 0xCC000000 with the current image
  copied there, so Basic Display can take over seamlessly. Our buffer is freed only after DECON is seen
  to scan 0xCC000000 again; if that cannot be confirmed it is deliberately leaked (never free memory a
  display engine may still read).

### 7.2 Presents (copy)

`DxgkDdiPresentDisplayOnly` gives a system-memory source and dirty/move rectangles. Basic Display writes them
straight into the live scanout; with the TE trigger the panel can show half-updated frames (tearing).

### 7.3 Flips (tear-free) and vsync

- Two scanout buffers: the firmware's (0xCC000000) and a second one of the same size, physically
  contiguous **below 4 GiB** (`MmAllocateContiguousNodeMemory`, write-combined). If the allocation fails,
  the driver presents by copying into the live buffer (Basic Display's behaviour) and says so in its state.
- Present = copy the dirty rectangles of this present **and of the previous one** into the back buffer, then
  flip: wait until `SHADOW_UPDATE_REQ` window bit is clear, write `IDMA_IN_BASE_ADDR_Y` (ch0), `wmb`,
  request the window's shadow update, unmask the HW trigger (RMW of bits 0 and 4 only; the inherited bits,
  uniLoader's 0x1281 pattern, survive). The next TE edge latches it **[proven by s9p-decon on Linux]**.
- Latching is detected by polling the window's shadow-request bit (it clears at the TE edge that takes
  the new address). First version: **no interrupts** (as malidod; `DxgkDdiControlInterrupt`,
  `GetScanLine`, the ISR and DPC stay NULL). The frame-start interrupt (GSIV 214) and
  `DXGK_INTERRUPT_DISPLAYONLY_VSYNC` reporting are a later step **[unverified on 23H2]**.
- Before writing into a buffer, the present waits (≤ 50 ms) until the previous flip has latched: the buffer
  it writes is then not the one DECON scans. Normally the previous flip is ≥ 16 ms old and latched, so
  there is no wait. A latch timeout switches to copy mode for good, writing every later present into
  **both** buffers (one full redraw first), so the screen stays right whichever buffer DECON ends on.
- The second buffer is `MmAllocateContiguousNodeMemory` below 4 GiB, write-combined (normal non-cacheable
  on ARM64, so DECON, which is not coherent, sees the writes after the barrier before the flip). If the
  allocation fails or lands above 4 GiB: copy mode.

Bugcheck screen (`DxgkDdiSystemDisplayEnable/Write`): no waits and no register writes at bugcheck time;
the text is written into **both** buffers, so it shows whichever one DECON scans.

### 7.4 Brightness

`DXGK_BRIGHTNESS_INTERFACE` (version 1: possible levels, set, get) on the internal panel, so Windows shows the
brightness slider **[unverified: Windows offers the slider for a DOD's internal target]**. Level 0..100 →
AOR by the table below (interpolated; S-Boot's own points marked), sent with S-Boot's sequence
(`F0 5A 5A`, `B1 hi lo`, `F7 03`, `F0 A5 A5`, DCS long writes, 50 ms bound per packet, one retry of the
whole sequence 20 ms later, the key always locked again). Clamp 0x000C..0x0BA3. Never anything else.

| Level | AOR | Note |
|---|---|---|
| 100 | 0x000C | brightest the vendor uses |
| 64 | 0x0440 | S-Boot "normal" |
| 31 | 0x0800 | |
| 4 | 0x0B7C | S-Boot level 10-19 |
| 0 | 0x0BA3 | S-Boot level 1-9 (dim, never black) |

The panel's power-on gamma stays (S-Boot never sends gamma); AOR alone moves luminance. Level 0 is dim,
not off. The default at driver start is to send nothing until Windows sets a level.

### 7.5 Blanking

Windows' "turn off display" → `DxgkDdiSetPowerState` on the monitor child. First version: present a black
frame only (no DCS). Panel display-off/sleep (DCS 0x28 / 0x10, proven on Linux with a 120 ms wake delay)
comes later behind a registry switch, since a panel that does not wake stays dark until reboot.

### 7.5b T9: resolutions and rotation (as built, 2026-11-08; not yet run on the phone)

- **Modes** (Settings → Display → Display resolution): 1440 × 2960 (native), **1080 × 2220 (preferred = the
  default)** and 720 × 1480 (T7's mode). Each fills the whole panel: the present scales Windows' image up
  (1080: 4/3, nearest neighbour; 720: 2 × 2 blocks, T7's copy unchanged; native: 1:1). For Windows the path
  stays identity: source, target and monitor modes have the same sizes, as in T7. `Parameters\Modes` (bit 0
  native, bit 1 = 1080, bit 2 = 720; default 7), `Parameters\DefaultMode` (0/1/2, default 1). `Scale` is no
  longer read.
- **Rotation** (Display orientation: landscape, portrait, and both flipped): the path offers Identity, 90, 180,
  270 (`DriverRotation` = 1), and the present rotates in software. Semantics from Microsoft's KMDOD sample:
  source modes stay in the panel's orientation; with `Flags.Rotate` the presented image is rotated (90/270:
  lh × lw) and ROTATE90 maps presented (sx, sy) to mode (sy, H−1−sx), ROTATE270 to (W−1−sy, sx). Orientation
  changes arrive in `UpdateActiveVidPnPresentPath` (or a commit). Host-tested: all 3 scales × 4 rotations,
  whole frame and partial rectangles, against a reference built from those maps. `Parameters\Rotation` = 0
  turns it off. If "landscape" and "landscape (flipped)" come out swapped on the phone, the KMDOD direction is
  the thing to check.
- **Fallbacks:** a mode outside the list is refused at commit, so Windows keeps the one it had; Windows' own
  15 s "Keep these display settings?" revert; **mode trial:** committing any mode other than 720 × 1480 writes
  `Services\sbdisplay\ModePending` (flushed), cleared after a minute of presents in it; still set at the next
  start (that boot ended early) → that boot offers 720 × 1480 only (`State\ModeFallback`); the boot guard
  (two short boots → Basic Display) and the kill file as before.
- **Default:** Windows keeps its saved layout per monitor (`Control\GraphicsDrivers\Configuration`), so after
  T7 it would keep 720 × 1480; `tools/win/hive-service.py IN OUT display-reset 1` removes the saved layouts
  offline, and Windows then starts in the preferred 1080 × 2220.
- New State: ModeMask, DefaultMode, RotAllowed, ModeWidth/ModeHeight (per commit), Rotation,
  PathRotationAsked, ModeFallback, ModeConfirmed.
- Desktop Duplication (Starbite Link's sblinkd): `DXGI_OUTDUPL_DESC.Rotation` reports the orientation and
  `ModeDesc` the size. Whether the duplicated frame then arrives already turned or in the panel's orientation
  is **not checked** (Microsoft's DesktopDuplication sample handles `Rotation` itself, which suggests the
  latter): sblinkd should log both after a rotation and turn the frame when `Rotation` is not IDENTITY.
- Cost: rotated 90/270 presents read Windows' image down columns: slower than T7's copy for full frames.

### 7.5c T10: where the desktop's time goes, and a faster copy (as built, 2026-11-08)

**What makes this Windows feel slow.** Every frame is drawn twice on the CPU: WARP (Microsoft's software
renderer) draws the windows and DWM composes them, then sbdisplay copies the changed rectangles to the panel
(scaled and, since T9, rotated). Only the second part is ours. A Mali-rendered desktop is stage 4 (§6): a
full WDDM driver plus Mesa's D3D10 driver cross-built for Windows, with an open question that can stop it
(§6.3). It is weeks of work, not one step. The order:
1. **Measure (T10).** How much of a core the copy costs (`CopyBusyPermille*`, `CopyUs*`), how often Windows
   presents and how much it changes (`PresentsPerSec*`, `PixelsPerPresentAvg`), and every copy path timed
   on the phone (`Bench_*`). If the copy is a small share, the slowness is WARP. The levers are then fewer
   pixels (the 720 × 1480 mode), the CPU (sbthermal's boost; the big cores are another track's work) and,
   in the end, stage 4.
2. **The copy itself, now:** NEON fast paths: 1:1 rows and 2x doubling with 128-bit loads and stores, the
   4/3 scaler as a byte-table lookup (6 source → 8 panel pixels per two `tbl` instructions), rotated copies
   in 16 × 64 cache tiles. They are pixel-for-pixel identical to T9's plain C (host test: 3,612 transform
   cases at real sizes with odd pitch and unaligned rectangles, plus 400 copies). On the Mac they run 3–6×
   faster upright and about 1.5× rotated; the phone's figures come from `Bench_*`. `Parameters\PlainC` = 1
   switches back to T9's copies, for an A/B comparison.
3. Then, by what T10 shows: DECON page flips at scale 1 (no copy at all; built as T3b, off) or a smoother
   1080 scaler; and the GPU track (sbmali T8 → Mesa without WDDM → stage 4).

The benchmark runs once, 20 s after sbdisplay starts (`Parameters\Bench` = 0 turns it off). It uses a
private write-combined buffer of 256 panel rows and a 17 MB pool copy of a test image, both freed right
after, so it never touches the screen. Results are in microseconds per full 1440 × 2960 frame, best of 3:
`Bench_{1440,1080,720}_R{0,90}_{C,Fast}`.

**T10 on the phone (2026-11-08 18:54, sbdisplay-40548a5b, unattended; `build/win/results/t10-t12-hive`):**
the desktop ran at 1080 × 2220 **rotated 270** (landscape from the T9 session). Presents up to 56/s, on
average 32,079 changed pixels per present. The copy took on average 268 µs and at most 12.9 ms per present,
and **at most 16 % of one core** over a 1 s window. Boot benchmark, µs per full 1440 × 2960 frame:

| | upright C | upright NEON | rotated C | rotated tiled |
|---|---|---|---|---|
| 1440 | 5,480 | 6,070 | 687,170 | 125,383 |
| 1080 | 75,190 | 10,255 | 185,439 | 115,937 |
| 720 | 3,688 | 5,989 | 103,264 | 150,220 |

Readings:
- (a) On the phone's cores, 128-bit stores to the write-combined panel are slower than 64-bit ones. The 720
  C path, with one 64-bit store per pixel pair, is the fastest copy measured: about 4.6 GB/s.
- (b) The 4/3 scaler is the real win: NEON's table lookup is 7× faster.
- (c) Rotated copies are expensive everywhere: a full landscape frame costs 116 ms at 1080. This is what
  made landscape slow.
- (d) The Mac predicts none of this (there, plain C wins rotated).

**T10b (built, not yet on the phone):**
- Every store to the panel is 64-bit, including the NEON 4/3 path.
- Rotated copies gain a third variant: 8 panel rows gathered in a 46 KB cached scratch buffer (reads stay in
  a few source lines), then each row written once, sequentially.
- **Adaptive choice:** `Pick[mode][upright|rotated]` starts at T10's winners. The boot benchmark now times
  C, NEON/tiled and scratch, and switches each path to its winner (State `Pick_<mode>_R<0|90>`: 0 C, 1 NEON or
  tiled, 2 scratch). So a path is never slower than its best measured variant on this phone.
- All variants are pixel-identical (host test).

### 7.6a Refresh rate: what Windows shows, and overclocking (research, 2026-11-07)

**"64 Hz" in Windows is a label.** sbdisplay copies Basic Display: the target mode leaves its frequencies
unspecified (malidod run 4 failed with a timing that had no blanking), and Windows then shows 64 Hz, the figure
Basic Display is known for **[believed, not looked up]**. The panel refreshes at its own fixed rate whatever Windows says: it is
a MIPI command-mode panel, and DECON sends a frame on each of the panel's TE pulses. `Parameters\Timing60` = 1
reports the panel's real timing instead (vendor DT `s6e3ha8_ddi`: 60 Hz, h-porch 2/2/2, v-porch 15/3/1 → 1446 ×
2979 total, scaled with the mode), so Windows shows 60 Hz; untested, so off by default (risk: a mode Windows
rejects → black screen → guard/kill file). sbdisplay now also measures how often Windows really presents
(`State\PresentsPerSecMax`, `PresentsPerSecLast`, over 1 s windows): if that stays under 60, a faster panel
gives nothing.

**Overclocking the panel (70 Hz+): facts.**
- In command mode the refresh is the **panel's internal scan rate** (its oscillator and porches); DECON only
  answers TE. The DSI link is not the limit: 4 lanes × 898 Mbps with 3:1 DSC move a 1440 × 2960 frame in about
  10 ms (about 95 frames/s at most).
- The S6E3HA8 in the S9 is a **60 Hz-only DDI**: the vendor DT says `timing,refresh = <60>`, and the vendor panel
  driver (`ref/starlte-kernel/.../panel/s6e3ha8/`) has no frame-rate control, no VRR or high-refresh tables
  (its only frame constant is "wait 1 frame in 60 Hz"). Samsung's 120 Hz panels of later phones change the rate
  with oscillator/porch sequences behind the level-2/3 keys; for the HA8 those registers are not in any source
  we have.
- Risks on this OLED: gamma, AOR and ELVSS are calibrated (MTP) at 60 Hz, so another rate can shift brightness
  and colour or flicker; a wrong register behind the level-3 key (`FC 5A 5A`) can reach the panel's OTP/MTP
  and destroy its calibration (the brightness research's "never send" list). No safe overclock for the S9's
  panel is known to us.
- A setting would not persist: S-Boot re-initialises the panel at every boot, so a reboot always restores 60 Hz.

**Plan if pursued (later, reviewed, user present, no DECON/DSIM writes before):** (1) measure first: Windows'
real present rate (above) and the panel's TE period from DECON's frame-start interrupt timestamps (read-only);
(2) read-only DCS reads of candidate DDI registers next to S-Boot's init sequence, to find the oscillator/porch
setting; (3) only if a documented or clearly observable setting turns up: one runtime panel command from
sbdisplay, `Parameters\RefreshHz` (default 60, so deleting the value or a reboot is the fallback), with the boot
guard and the kill file. Until (1) shows Windows presenting at 60/s, the bigger gain is rendering on the GPU
(stage 4).

**Panel-side scaling (alternative to 2x copies):** the panel's DDI has its own scaler for the S9's
WQHD+/FHD+/HD+ settings (vendor DT `mres`: 1440 × 2960, 1080 × 2220, 720 × 1480; panel command `BA ...`, CASET/
PASET). Using it means reprogramming DECON, DSC and the panel: a later stage with the same care.

### 7.6 What it never does

Reset or reconfigure DECON, DPP/IDMA formats or sizes, DSIM PLL/clock/lanes, DSC, the panel's init, gamma,
ELVSS, POC/MTP; send TE off; touch power domains or clocks.

## 8. The temperature watchdog (`sbthermal.sys`)

Required before any sustained GPU load (rule). It is first so that every later driver can depend on it.

### 8.1 What it reads

Every 250 ms (a system thread at PASSIVE_LEVEL): TMU_SUB probe 1 (GPU), TMU_SUB probe 2 (ISP),
TMU_TOP probes 1-6 (big and little cores), only while each block's CONTROL.CORE_EN is set. Each probe: the
median of 3 samples 10 µs apart, codes 0 and 0x1FF and temperatures outside -30..150 °C rejected (libsb's
filter, which fixed a real glitch). Conversion with the probe's trim (one- or two-point). Once at start: is
the hardware trip armed (TOP and SUB CONTROL bit 12) and at what temperature (THD_TEMP0 rise code).

The registers are only read. MMIO 0x10070000 and 0x10074000 are mapped uncached (`MmMapIoSpaceEx`,
`PAGE_READWRITE | PAGE_NOCACHE`, 4 KiB each). The TMU's APB clock is left on by S-Boot (the kernel and S-Boot
never touch its gate).

### 8.2 Levels

Driven by the GPU probe for GPU actions and by the hottest probe for the overall state; hysteresis 5 °C on
the way down; a level rises after 2 consecutive readings above it (500 ms), falls after 8 below it (2 s).

| Level | GPU probe | Meaning for the GPU driver (stage 3/4) |
|---|---|---|
| Normal | < 70 °C | any DVFS level |
| Warm | ≥ 70 °C | cap at 338 MHz |
| Hot | ≥ 80 °C | 260 MHz |
| Stop | ≥ 90 °C | start no new jobs; cores off when idle |
| Critical | ≥ 100 °C, or any probe ≥ 105 °C | GPU off now (hard-stop, cores off, BUCK6 back) |
| Blind | 4 readings in a row without a GPU temperature (the GPU probe rejected, or TMU_SUB off or in emulation mode) | treated as Stop |

These are well under the vendor's (control 88 °C, alert0 105 °C, hot 115 °C) because we have no power
allocator. The driver starts at Stop and needs 2 s of cool readings before the GPU may run. The hardware
trip S-Boot arms at 115 °C is decoded from registers but has **never been seen firing**, and which probes
it watches is unknown, so it is not counted on; the watchdog reports whether it is armed and never arms or
disarms it.

Battery temperature is not covered yet (the fuel gauge sits behind MAX77705 on I2C; a later step shares
that with the Windows USB track). Until then the GPU driver also limits a continuous high-clock run to
10 minutes, then drops to 338 MHz.

### 8.3 Interface

- `\Callback\StarbiteThermal` (`ExCreateCallback`, a documented kernel callback object): every level change
  calls the registered functions with (`SBTH_CB_LEVEL`, state). Consumers open it with `Create = FALSE` (a
  consumer that created it would get an empty object that proves nothing). Queries: `ExNotifyCallback(obj,
  SBTH_CB_QUERY, &state)` on a zeroed state; `version` 0 afterwards = no watchdog. Every registered
  callback sees queries too, so consumers ignore other Argument1 values. **No answer, or `seq` not moving /
  `time_100ns` older than 1 s → treated as Blind (Stop)**. The watchdog has no unload routine.
- `\\.\StarbiteThermal`: `IOCTL_SBTH_GET_STATE` returns the same struct (all probes, level, trip state,
  counters) for a user-mode tool.
- Service key `State` values every 60 s and on each level change: level, GPU/hottest °C, max seen,
  rejected samples, trip armed and its °C. `ZwFlushKey` on level changes.

The thermal core (`sbth_core.c`: conversion, filter, level machine) is plain C, shared with a host test.

### 8.4 CPU boost and the ACPM owner (smoothness, 2026-11-07)

Windows runs only on the four A55s, and nothing in our Windows does CPU DVFS, so they stay at what S-Boot
left (about 1.05 GHz). XNU raises them to 1690 MHz through ACPM DVFS domain 2 on every boot (measured
1689 MHz). sbthermal now does the same under its watchdog:

- `Parameters\CpuKhz` (default 1690000; 0 = never touch the clock). Allowed: the vendor DT's A55 levels from
  S-Boot's clock up, 1053000, 1248000, 1456000, 1690000, 1794000, 1898000, **1950000 (the top level)**. ACPM
  sets the voltage from its own table and may clamp; `State\A55Khz` (CMU readback) shows what it really ran.
  Nothing beyond the table: no overclocking (that would need voltage tables we cannot safely change).
- A second level machine on the **hottest** probe (same thresholds and hysteresis): Normal → CpuKhz, Warm
  (≥ 70 °C) → at most 1456 MHz, Hot and above (≥ 80 °C) → S-Boot's 1053 MHz. It starts at Stop, so the boost
  comes after 2 s of cool readings. Three unanswered requests and the boost gives up for that boot.
- The clock is read back from the CMU (read only, `sbth_a55_khz`, libsb's decode): `State\A55KhzBoot`
  (before any change), `A55Khz`, `CpuTargetKhz`, `CpuLevel`, `CpuRequests`, `CpuFailures`.
- Domain 2 only; ACPM sets the voltage from its own table, as on iOS. sbthermal is now the **single ACPM owner**
  in Windows: all ACPM IPC code is one file (`starbite/win/common/sbw_acpm.c`), and every transaction, its
  own and sbmali's BUCK6 writes, runs under one kernel mutex (`SBTH_CB_ACPM_LOCK/UNLOCK` on the callback
  object, taken in the caller's thread). `struct sbth_state` is frozen (a driver already built passes exactly
  that size); the CPU data has its own query, `SBTH_CB_QUERY_CPU`.

### 8.5 The 2x scaled desktop (WG1 in software)

sbdisplay with `Parameters\Scale` = 2 (the INF's default) reports one 720 × 1480 mode. WARP and DWM then draw
a quarter of the pixels; each present doubles the changed rectangles 2 × 2 into the 1440 × 2960 firmware
framebuffer (`sbdc_blit`: one 64-bit store per source pixel per row, never reading the write-combined
destination). Copy mode only (no DECON or DSIM writes); text is half as sharp (about 285 ppi). Hardware
scaling in DECON would need a scaler-capable DPP channel reprogrammed (S-Boot's channel 0 is a G-type, the
registers are unresearched, and reads of clock-gated channels wedge the SoC), and a smaller firmware GOP
mode gains nothing without that scaler, so both wait. Page flips remove tearing but copy the same bytes,
so they come after this.

## 9. What can be tested off the phone

| What | How | Where |
|---|---|---|
| Thermal core: code → °C (one-/two-point), filter, levels with hysteresis, Blind | host test with recorded codes, including the code-0 glitch | `starbite/win/tests/` (cc on the Mac) |
| DECON flip sequencing, dirty-rect union of two presents, rotation blits, AOR table and the DSIM packet words | host test against a fake register file that checks the order of writes and that only allowed offsets are written | same |
| Mali page tables (AArch64 4K and legacy), VA allocator | host test: walk the tables like the GPU and compare | stage 3 |
| Job descriptors | already: libsb's mesa-golden test | `starbite/tools/unit-test.c` |
| Drivers compile and link for ARM64 (only ntoskrnl and HAL imports, load config with the security cookie), test-signed, sbdisplay's catalog | `starbite/win/build.sh` over the shared L0 kit: `tools/win/build.sh wdm`, `sign.sh`, `makecat.py` (`$SBWIN`, WDK/SDK 10.0.26100.6584) | Mac, done |
| Offline service install / disable on a SYSTEM hive copy, with the dirty-hive check | shared `tools/win/hive-service.py` | lima, done on an old hive copy |
| INF syntax | `infverif` needs Windows; we check by hand and on the phone | — |
| Mesa 26.2 + patches build for Windows ARM64 | meson cross build | lima / iCloud overflow |
| Panfrost on v6 through the WDDM shim | Linux on the phone already runs the same Mesa on the G72: a Linux-side `pan_kmod` test of the command format (same job chains, DRM backend) | phone under Linux (main session) |

Not testable off the phone: anything in dxgkrnl's handling (DDI acceptance), DECON timing, interrupts,
the GPU itself.

## 10. Phone tests (requested through the main session, one at a time)

| # | Test | Needs | Expect | Risk / undo |
|---|---|---|---|---|
| T1 | `sbthermal` loads at boot | sbthermal.sys + service key | `State`: Level 0 (Normal), GPU ~35-50 °C idle, TripArmed 3, TripC 115, few rejects | read-only driver; undo Start = 4 offline |
| T2 | ACPI device present | a BOOT image with the SBGPU blob (§4.2) | pmsg `=SSDT installed SBGPU`; touch still works; `ACPI\STRB0072` exists with no driver (Enum\ACPI\STRB0072 in the offline SYSTEM hive) | firmware flash; undo: previous BOOT image |
| T3a | `sbdisplay` replaces Basic Display, **copy mode** (Flip 0, Brightness 0: no DECON/DSIM write) | T2 + sbdisplay | desktop unchanged; `State`: Started 1, Identify 0, Window 5, Mode 1, CommitStatus 0, Presents counting | black screen possible: person forces restarts (§4.3), kill file, older BOOT |
| T3b | flips on (Parameters\Flip = 1) | T3a passed | no tearing when dragging windows; `State`: Mode 2, FirstFlip, Flips counting, no FlipLatchTimeout | as T3a; a latch failure falls back to copy by itself |
| T4 | Brightness slider (Parameters\Brightness = 1) | T3a | Settings shows a slider; 100 → 0 dims the panel; `BrightnessResult` 0 | AOR-only, S-Boot's sequence; worst case the panel stays at one brightness until reboot |
| T6 | CPU boost (swap sbthermal.sys; `Parameters\CpuKhz` absent = 1690000) | T1 | `State`: A55KhzBoot ≈ 1053000, A55Khz ≈ 1690000, CpuTargetKhz 1690000, CpuFailures 0, CpuLevel 0; Windows visibly snappier | low-moderate: the little cluster runs warmer (the watchdog throttles at 70/80 °C); an ACPM hang is not expected (it answered every request on iOS); undo CpuKhz 0, the old .sys, kill file |
| T7 | 2x scaled desktop: sbdisplay copy mode, `Scale` 2 (bound instead of sbmali) | T2, after T5 | Windows at 720 × 1480 (Settings > Display), `State`: Scale 2, ModeWidth 720, Mode 1, Presents counting; smoother | a new display driver may black-screen: person forces restarts (§4.3), kill file, old BOOT |
| T5 | `sbmali` bring-up (replaces sbdisplay for the test) | T1, T2, the PnP install route | `State`: ThermalGate 0, Probe 0, GpuId 0x62210010, ShaderPresent 0x5771777, BuckOld 0x33, BuckRaise 0, `Legacy_*` and `A64_*`: Runs 20, WriteOk 20, ClearOk 20, Faults 0; Done 1, BuckRestore 0 | 950 mV on vdd_g3d (as iOS since 2026-10-28), 260 MHz, ~40 short runs; watchdog gates it. If Windows' boot path left the G3D domain power-cycled without the TZPC restore, the first GPU read is an SError (bugcheck): the boot guard then disables the driver after two tries; kill file; Start = 4 |

Each request to the main session lists exact files, commands, expected values and the undo.

### 10.1 Results

**T1 passed (2026-11-07, main session; Windows 22631, sbthermal.sys sha256 0af2c9d6…, ~5 min at the desktop,
clean restart; offline SYSTEM copy `build/win/results-1/SYSTEM` in the main checkout, clean 678/678).**
`State`: Level 0, GPU 42.586 °C, hottest 43.620 °C (slot 5, the little cluster Windows runs on), max seen
45.689 °C (boot), Readings 1208, Rejected 0, BlindEvents 0, BlocksLive 3, TripArmed 3, trip 115.000 °C on
both blocks, LevelChanges 1 (Stop → Normal at start), GuardBoots 0. No bugcheck in pmsg.
Reading of the values: nine probes report exactly 42586 m°C. The trim is two-point with te2 − te1 = 58 codes
for 60 °C, so one code = 1.034 °C and 42586 = 17 codes above 25 °C: probes at the same temperature give the
same code. The trip decodes to exactly 115 °C (S-Boot's te1 + 87), which cross-checks the conversion. The
big cores (Windows never runs on them) and the idle GPU sit at the same temperature; ISP (slot 10) 43.305 °C.

**T2 passed (2026-11-07, main session; the win-usb-host track's v3 firmware with our blob):** pmsg zone 0
`=SSDT installed SBGPU (159 bytes, rev 1, at dispatch)` (= `build/win-gpu/StarbiteGpu.aml`, the interrupt-free
stage 2 table); Windows 22631's offline SYSTEM (`build/win/results-usbh0-v3/SYSTEM-r` in the main checkout)
has `Enum\ACPI\STRB0072\0` with HardwareID `ACPI\VEN_STRB&DEV_0072`, `ACPI\STRB0072`, `*STRB0072`,
ConfigFlags 0x40 (no driver yet), and a BootConfig listing the four memory ranges from 0x16030000; the xHCI
next to it kept working. The shared DXE now installs at dispatch (its ReadyToBoot installs never reached
Windows). sbdisplay and sbmali can bind to it.

**T5 run 1 (2026-11-07, sbmali 513dfbe8): stopped safely at the thermal gate, GPU untouched.** The
self-installing service (`sbpnp`, cmd.exe as LocalSystem) died twice within 3 s without writing its log
(System 7034; suspected the same 0xC0000094 process-start crash as other programs on this Windows), so the
main session bound sbmali to `ACPI\STRB0072\0` offline (`tools/win/hive-service.py bind`; no certificate is
needed for a kernel load under testsigning). The driver started (contiguous memory at 0x8_FFCC_0000) and
wrote `ThermalGate 1`: the watchdog's callback object did not exist yet. System.evtx puts that boot at
10:30:23-10:34:40 (~257 s) and sbthermal's 728 readings at ~182 s of it, so it came up ~75 s after boot
began, after sbmali's 30 s wait. Fix: 10-minute wait, per-step breadcrumbs (§5.1a). No bugcheck.

**T5 run 2 (sbmali 3ca3fd09, sbthermal 0af2c9d6): again ThermalGate 1, after a 370 s wait, while sbthermal
ran the whole boot (1208 readings).** Cause: sbthermal created `\Callback\StarbiteThermal` without
`OBJ_PERMANENT` (removed during the review, §13.1 #10). A named object that is not permanent leaves the
namespace when its last handle closes, and `ExCreateCallback` closes its own handle at once, so no other
driver could ever open it. Fixed: `OBJ_PERMANENT` again (consumers still treat a stale `seq`/time as Blind,
and sbthermal has no unload routine). GPU untouched both times, no bugcheck.

**T6 passed (2026-11-07, sbthermal b5ad89cf; SYSTEM copy `build/win/results-combo1/SYSTEM-r` in the main
checkout):** A55KhzBoot 1053000 (Windows really ran the A55s at S-Boot's 1.05 GHz until now), A55Khz 1690000
(CMU readback after the ACPM request), CpuTargetKhz 1690000, CpuRequests 2 (the CPU level machine starts at
Stop, so its first request is S-Boot's own 1053000, then 1690000 once cool), CpuFailures 0, CpuLevel 0;
watchdog Level 0, GPU 41.6 °C, hottest 43.3 °C, highest seen 49.8 °C over 728 readings, 0 rejected. Task
Manager's "base speed" still reads 1.05 GHz: a static figure, not the clock. Next: CpuKhz 1950000 (the user
asked for the maximum; temperatures at 1690 MHz were fine).

**Round 3 (sbthermal 280e851e + sbmali 8877ddec, CpuKhz 1950000; `build/win/results-r3/SYSTEM-r`):** ACPM
granted the A55s' top level: A55Khz **1950000**, CpuLevel 0, CpuFailures 0, highest seen 55.0 °C, hottest at the
end 47.8 °C, GPU 42.6 °C. sbmali did not run: its own guard refused it (GuardBoots 2). The guard was raised in
`DriverEntry` and cleared only when the test thread ended, and KMDF does not release hardware at system
shutdown, so the earlier waits for the watchdog, cut short by restarts, counted as crashes. Fixed: the guard is
armed only right before the first GPU access, and a D0Exit callback (also at shutdown) stops the thread,
which puts the GPU and BUCK6 back and clears the guard.

**T5 passed (2026-11-07, sbmali 8877ddec + sbthermal 280e851e, A55 at 1950 MHz; `build/win/results-r4/SYSTEM-r`):
the Mali-G72 ran its first jobs under Windows.** Step 7, ThermalGate 0 after 41 s (83 tries; the watchdog loaded
late and sat at its start level Stop for its first 2 s), Probe 0, PmuStatus 0x10000f, GpuId 0x62210010 (r0p1),
ShaderPresent 0x5771777, MmuFeatures 0x2830, AsPresent 0xff, JsPresent 7, Coherency 0, PLL 260 MHz, BUCK6
0x33 → 0x68 → restored (BuckRestore 0). Legacy and AArch64: 20 runs each, WRITE_VALUE and 64 × 64 fragment
clear 20/20, 0 faults, 0 timeouts, every job done within the 20 µs polling step. GPU 44.7 °C at most; clean
restart, no bugcheck, GuardBoots 0. Both table formats work for these jobs; the exynos-linux failures were
with real workloads, so the comparison continues with a real draw (T8).

**T7 passed (2026-11-07, sbdisplay 5fbece03, Scale 2; `build/win/results-t7/SYSTEM-r`): the user: "it is working
faster".** State: Started 1, Mode 1 (copy), Scale 2, ModeWidth × ModeHeight 720 × 1480 on the 1440 × 2960
framebuffer (pitch 5760, format 22 = X8R8G8B8, 0xCC000000), MmioMapped 7, Identify ok (Window 5, trigger
0x3061 = enabled and unmasked as the UEFI left it), CommitStatus 0, Presents counting (1024 logged), Flips 0,
BrightnessInterface 1 (Windows asked for it; Brightness 0, so no DSIM writes). The failures logged are the
expected ones: QueryInterface 0xC00000BB (STATUS_NOT_SUPPORTED) for interfaces other than brightness (QAI 47),
QueryDeviceDescriptor 0xC01D0001 (no EDID). Clean restart, no bugcheck. sbdisplay stays bound as the
user's daily display driver. Its guard now clears after 1 minute of presents (10 minutes would count every
shorter daily session); sbmali can run as a standalone service (Start 1/2) next to it, mapping the Mali
itself, so T8 needs no rebinding.

**T8 attempt 1 (combo2 firmware; `build/win/results-nopptt/SYSTEM-r`): not run.** sbthermal refused itself
(GuardBoots 2): the firmware's PPTT made two boots bugcheck early, and the watchdog's guard (cleared only
after 120 s) counted them, so the CPU stayed at 1.05 GHz and sbmali (standalone) waited at the gate. Fix:
sbthermal's guard clears after 10 s, by which time its own risky steps (the first TMU/CMU reads, the CPU
boost request at ~2 s) are done.

**T8 run 1 (sbmali 3fc6376a standalone next to Basic Display, m3probe firmware; `build/win/results-m3probe/SYSTEM-r`):
the real draw works under Windows, and the 572 MHz clock path too.** Step 7, Done 1, TriAlloc 7, Triangle 1;
legacy and AArch64 20 runs each (Parameters\Runs was still 20 from the T5 bind): WRITE_VALUE, clear and XNU's
Mesa triangle (tiler + fragment) 20/20, the triangle's 86528 pixels identical in every run, 0 foreign pixels,
0 faults, slowest job 90 µs; GPU at most 43.6 °C, 0 throttles; GPU clock and BUCK6 restored. The clock: the
GPU ran at 572 MHz. `GpuClockRestore` is only written after a successful raise, and the raise passed the
PLL_G3D cross-check (no `GpuClockMismatch`); the `GpuKhzReq`/`GpuPllKhz` 260000 seen afterwards were the
restore writing the same value names, now separate (`RestoreKhzReq`, `RestorePllKhz`, plus `GpuPllKhzMax`).
To do: 200 runs per format (Runs 200 or the value deleted) for the page-table comparison.

### 10.2 Next phone tests

| # | Test | What it proves | Risk |
|---|---|---|---|
| T7 | 2x scaled desktop (sbdisplay, copy mode) | smoother desktop (4x fewer pixels for WARP/DWM); Desktop Duplication on our display-only driver | a new display driver may black-screen (§4.3) |
| T8 | sbmali v2 as a standalone service (`hive-service.py add sbmali`, next to sbdisplay): the GPU clock and a real draw. ACPM DVFS domain 4 to 572 MHz (BUCK6 rewritten after it), XNU's Mesa-built triangle (tiler + vertex + fragment, `xnu/src/starlte_gpu_tri.h`, proven on this phone under iOS) into a private buffer checked pixel by pixel, 200 runs per table format with clear + triangle, temperatures throughout | the clock path, the tiler, shaders, and the legacy-versus-AArch64 decision on a real workload | sustained GPU load for ~1 min at 572 MHz: the watchdog throttles (338 MHz at 70 °C, 260 at 80, stop at 90); otherwise as T5 |
| T12 | sbmali + the stage-4 scheduler (§6.4): 100 rounds of queued chains + the triangle on scattered pages, one deliberate MMU fault and the recovery | the kernel half of stage 4 on the real GPU, fault recovery on hardware | as T8; the fault is the kind Panfrost recovers from daily |
| T13a | sbwddm full mode on the stage-2 SSDT (§6.8): bring-up under WDDM, then a logged refusal (no interrupts) | dxgkrnl accepts the full driver; the G72 under WDDM; guard and fallback | that boot runs Basic Display (slower); then sbdisplay mode |
| T13b | sbwddm full mode with StarbiteGpu4 (Mali interrupts) + sbumd.dll | DWM on the Mali, or how far it gets | black desktop likely first: the guard returns sbdisplay on the next boot |
| T11 | Mesa on Windows without WDDM (wddm-mali-g52's M3/M4 shape): Mesa 26.2 + their patches cross-built for ARM64, Panfrost reaching sbmali through a Panfrost-style IOCTL interface (CREATE_BO, SUBMIT, WAIT_BO, ...), a D3D11 test program using Mesa's d3d10umd as a software driver | D3D11 rendered by the Mali under Windows | a user-mode program has to be started (the user taps it; services running cmd.exe crash on this Windows) |
| 4a | sbwddm skeleton (WDDM 1.3, display half = sbdisplay incl. 2x scaling) | Windows accepts our full driver | black desktop possible (wddm-mali-g52 run 6) |

## 11. Stages and done-when

| Stage | Done when |
|---|---|
| 2a | T1 passes: watchdog runs on the phone's Windows and reports sane temperatures |
| 2b | T3a, T3b and T4 pass: our driver drives the panel tear-free with working brightness |
| 3 | T5 passes, including the page-table comparison |
| 4a | sbwddm with sbdisplay's display half + sbmali's render half starts with a softpipe d3d10umd UMD and the desktop still works (unproven: wddm-mali-g52's run 6 got a black desktop; behind the kill switches and the 10-minute guard) |
| 4b | the M4-style triangle as a hardware device on the Mali |
| 4c | §6.3 answered; then DWM on the Mali, measured against WARP (CPU time, fps) |

## 12. Licences and credits

- Our drivers: GPL-2.0-only (like libsb and the XNU GPU code they port).
- wddm-mali-g52: GPL-2.0 (`malidod`, `malikm`, `maliwddm` are read and followed; `sbdisplay.c` follows
  malidod closely and says so at the top). Its Mesa patches are MIT like Mesa; its `win32-compat/` shim
  headers are GPL-2.0.
- Microsoft's KMDOD sample: MS-PL; used as a reference for the DDI sequence only, no code copied (MS-PL is not
  GPL-compatible).
- Mesa: MIT. Linux Panfrost: GPL-2.0 (register and quirk facts). Samsung kernel (panel, TMU, DECON facts):
  GPL-2.0. exynos-linux (page-table quirk): GPL-2.0, fact only.
- viogpu3d: BSD-3, design reference.

## 13. Review

Adversarially reviewed on 2026-11-07 by a separate agent given this document, our sources, the
wddm-mali-g52 tree and the Linux Panfrost sources. It found 22 problems (4 critical). What changed:

### 13.1 Review findings and what was done

| # | Severity | Finding | Done |
|---|---|---|---|
| 1 | critical | Stage 2 code not in the tree | Timing: the review read the tree before the code landed. Now in `starbite/win/` with host tests |
| 2 | critical | The boot guard cannot catch "running but black"; an offline `Start = 4` can be undone by Windows replaying a dirty hive's logs; no network on this phone | §4.3 rewritten: three undo paths in order (old BOOT image, kill file, clean-hive registry edit); sbdisplay's guard needs 10 healthy minutes and the person forces restarts on a bad screen; first test in copy mode (T3a); kill file implemented in both drivers |
| 3 | critical | Contiguous primaries in an aperture-only WDDM design failed for wddm-mali-g52 (black screen; dxgkrnl wants CPU-visible segments for shared primaries) | §6.1 marks primaries open: a CPU-visible segment on a firmware-reserved region, copy-flips until then |
| 4 | critical | Holding DMA buffers back at Stop/Critical causes TDR storms | §6.1: Stop keeps executing at 260 MHz with gaps; Critical completes fences without executing |
| 5 | major | ACPM sets the voltage on every DVFS request; the XNU order is DVFS, then BUCK6 950 mV again | §5.1 steps 2 and 7, §5.4 |
| 6 | major | `Patch` has work: VidMm pages in what the patch list references | §6.1 |
| 7 | major | A full driver must pin target-mode frequencies (display-only must not) | §6.1 Display |
| 8 | major | Stage 4a "desktop still works" is unproven | §11 4a, §6.3 |
| 9 | major | wddm-mali-g52's M5.2 ran in WinPE 22621; its UMD still uses the shim's own D3DKMT device; WDDM 1.3 did load on installed Windows | §1 table, §6.3 |
| 10 | major | No watchdog liveness; `Create = TRUE` by a consumer proves nothing; queries reach every callback | `seq` + `time_100ns` in the state, consumers open with `Create = FALSE` and treat stale as Blind, no unload routine (§8.3, code) |
| 11 | major | Blind only when all probes fail | The code already went Blind on a missing GPU temperature (probe, CORE_EN or emulation); §8.2 wording fixed |
| 12 | major | Interrupt storm risk: Mali and DECON EXTRA lines connected without their status being cleared | Stage 2 SSDT has no interrupts; added per stage with the status clear (§4.2) |
| 13 | major | No GPU TLB invalidation on unmap | §5.1 step 4, §6.1 |
| 14 | major | Flip timeout, allocation-failure and move-rectangle gaps | Code: timeout → copy into both buffers after one full redraw; moves copied as dirty; allocation failure = copy mode (§7.3) |
| 15 | major | Bugcheck screen invisible if DECON scans the other buffer | Bugcheck writes go to both buffers (§7.3) |
| 16 | minor | AArch64 default contradicts what runs; INA bits; PTW memattr | Legacy default until T5; 48 VA bits; NC tagged unverified (§3, §5.1) |
| 17 | minor | Start order of the watchdog and a PnP GPU driver | 30 s bounded wait, then GPU off (§5.1) |
| 18 | minor | The 115 °C trip was never seen firing | §8.2 |
| 19 | minor | Match s9p's identification; DSB, not DMB, before the flip | Code reads the shadow bank, finds the window by geometry, writes +0x40/+0x44, uses `__dsb(SY)` |
| 20 | minor | Registry override table needs a higher OEM revision | §4.2 |
| 21 | minor | wddm-mali-g52's shim headers are GPL-2.0 | §12 |
| 22 | minor | The DECON `early_event` fix is built, not booted | §6.3 |

Checked and found correct by the review: the SPI → GSIV numbers, all MMIO addresses and sizes, the firmware
framebuffer lying outside Windows' RAM, the TMU probe map and vendor thermal zone, DVFS domain 4 and its
levels, BUCK6 0x33 = 618.75 mV as measured, the AOR sequence and the forbidden panel commands, the DSIM
registers, the job flush bits, the G72's Panfrost features and issues, the registry override's limits.

## 14. Sources

- wddm-mali-g52: https://github.com/gahingwoo/wddm-mali-g52 (README, docs/PLAN.md, docs/M5-PLAN.md,
  docs/M5.2-PLAN.md, docs/results/m5.1-board-2026-10-02.md, drivers/malidod/malidod.c), read 2026-11-07 at e8bcd36
- Mesa d3d10umd removal: https://www.phoronix.com/news/Mesa-Drops-Direct3D-10-UMD
- Registry ACPI table override: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/microsoft-asl-compiler
- WDDM DDIs: `d3dkmddi.h`, `dispmprt.h` (WDK), Microsoft "Display-only driver (KMDOD)" sample and DDI reference
- Linux `drivers/gpu/drm/panfrost/{panfrost_issues.h, panfrost_features.h, panfrost_gpu.c, panfrost_mmu.c, panfrost_regs.h}` (mainline, lima VM `/mnt/build/src/mainline`)
- Vendor DT `device/dtb/starlte.dts` (G3D thermal zone, gpu_dvfs_table), `ref/starlte-kernel`
- Our own: `xnu/src/starlte_gpu.c`, `xnu/src/StarbiteMali.cpp`, `starbite/libsb/{mali,mali_desc,tmu,acpm}.c`,
  `tools/linux/port/src/gpu/drm/s9p/s9p-decon.c`, `shim/starlte/em.c`, `build/em-research-*.md`, NOTES.md
