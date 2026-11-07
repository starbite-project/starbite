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
| 3 | `sbmali.sys`: KMDF bring-up (power, MMU, a job) ported from StarbiteMali | designed (§5) |
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

### 6.3 The open question that can stop stage 4

**Does DWM composite on an adapter whose UMD has only the D3D10/10.1 DDI?** Microsoft's certification rules
require D3D9, D3D10 and D3D11.1 DDIs for D3D10-class hardware; DWM's own requirement is not documented.
viogpu3d's WinUI 3 apps imply yes **[claim]**. If not, d3d10umd needs a D3D11 DDI table (large, on an
unmaintained frontend), or a D3D9 one. Test (cheap once stage 4 loads): run a D3D11 hardware-adapter
program, then check with `dxdiag` / ETW which adapter DWM uses.

Other stage 4 risks, in order: primaries (§6.1, open); the TDR without preemption on 18 cores at
260-572 MHz; a WDDM 1.3 full driver on installed 23H2 (it loaded on wddm-mali-g52's installed Windows, but
with a black screen first; their run 6 under a full driver gave "desktop and RDP black", which on our
phone is the no-network black-screen case of §4.3, so stage 4a runs only behind the kill switches and the
10-minute guard); TGSI leaving Mesa before we are done (we pin 26.2); DECON flips at 60 Hz (the Linux
DECON driver hit a 30 fps flip cap; the `early_event` fix in K2 is built but not yet booted).

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
| T5 | `sbmali` bring-up (replaces sbdisplay for the test) | T1, T2 | GPU_ID 0x6221, cores ready, WRITE_VALUE ok, fragment clear of a buffer ok, legacy and AArch64 tables 20 runs each with 0 faults | 950 mV on vdd_g3d (as iOS since 2026-10-28); watchdog gates it; undo Start = 4 |

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
