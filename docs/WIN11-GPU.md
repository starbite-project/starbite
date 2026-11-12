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
- Feature level: as built, sbumd offers **only the D3D10.0 DDI**. In `State.h`, `SUPPORT_D3D10_1` is 0 and so is
  `SUPPORT_D3D11`, so `GetSupportedVersions` lists the three 10_0 interfaces only. `GetCaps` answers every
  query with zeros, and only the DXGI 1.0 base table is filled. The D3D11 runtime therefore creates devices at
  feature level 10_0 at most (checked 2026-11-10 against the source; the 10_1 code paths exist behind the switch).
  T32c logs every OpenAdapter/CreateDevice interface and every GetCaps type the runtime asks for (§6.42).
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

**cycle-t13s-1 (01:34):**
- **DWM's back buffer**, now shown on the panel, is black everywhere except the spinner's rectangle.
- **The apps' frames have content**, and reach DWM's surfaces through 280 present blts that our KMD copies on
  the CPU.
- **T13t instruments that path:**
  - KMD: source and destination content of each non-primary blt;
  - KMD: DWM's whole back buffer at each primary blt;
  - UMD (sb-patches/0108): DWM's GPU view of every surface it opened.

**cycle-t13t-1 (02:00):**
- **DWM's GPU sees the windows' content in the surfaces it opened** (LogonUI's wallpaper 32/32, the user tile
  white).
- **Yet its composed frame is all zeros:** a transparent clear with nothing drawn on top.
- **The non-primary blts are DWM's own buffer-to-buffer copies.**
- **T13u** logs, per sampled present, what was drawn since the last one: draw counts, the render target against
  the presented resource, the last draw's state, clears, viewport and scissor, and predication
  (sb-patches/0109).

**cycle-t13u-1 (02:27):**
- **What DWM draws:** each frame is one DrawIndexed (16-bit, 6 or 48 indices, triangle list, restart 0xffff)
  into the presented 1440x2960 BGRA target, with a full viewport, no depth and no active predication. The target
  stays all zero.
- **T13v:** `tools/win/mesa/tests/sbdwm.cpp` adds DWM's features one at a time (16-bit indexed, index offset,
  constant buffer, premultiplied blend, a 1440x2960 BGRA target, then all together), on the Mali and on
  softpipe.

**cycle-t13v-1 (02:50):**
- **sbdwm on the Mali:** indexed draws, index offsets, 32-bit indices, blending and the 1440x2960 BGRA target all
  pass.
- **A vertex-shader constant buffer fails:** the transform reads as zero and nothing is drawn. DWM's own draw,
  rebuilt, comes out all zero. softpipe passes everything.
- **So DWM's black frames come from constant buffers on the Mali path.**
- **T13w bisects it:** b0 (the uniform-lowering path) vs b1 (UBO), vertex vs pixel shader, and initial data vs
  UpdateSubresource vs DYNAMIC Map. Each variant draws the value it read.

**cycle-t13w-1 (03:10):**
- **Constant-buffer slot b0 reads wrong** in both shader stages and with every fill method; **b1 works.**
- **The route at fault:** cb0 (`CONST[0]`) goes through tgsi_to_nir's `load_uniform` and Bifrost's
  `lower_uniforms_to_ubo` to UBO 0, while `CONST[1+]` takes the plain UBO path.
- **T13x (sb-patches/0110):** d3d10umd maps D3D slot n to gallium buffer n + 1 / `CONST[n + 1]`, so every D3D
  constant buffer takes the working path.

### 6.19 T13x: the Windows 11 lock screen on the panel, drawn by the Mali (2026-11-09, ~03:30)

**cycle-t13x-1:**
- **Tests:** sbdwm hw 0 failed (cb and dwm PASS); DWM's sampled presents 48/52 with pixels; the cycle and hive
  clean.
- **The panel:** about 100 s into the full-mode boot, the camera shows the **Windows 11 bloom lock-screen
wallpaper on the panel.** DWM composed it on the Mali through sbumd (Mesa d3d10umd + Panfrost) and sbwddm, and
the KMD's emulated scanout copied it to the framebuffer.

What it took, after the GPU and kernel driver themselves (T12, T13a-c):
- **0102:** D3D10 texture opcodes for a NIR-only driver.
- **0103:** runtime allocations for shared, presented and primary resources.
- **sbwddm:** the cache-correct CPU views; the stale-present fix (MwRender); flips carried out; an emulated
  scanout that shows DWM's back buffer.
- **0105:** GPU texture copies.
- **0110:** constant buffers kept off gallium slot 0, which reads wrong on this path.

The bugs found on the way, each by a test that isolated it:
- tgsi_to_nir has no D3D10 sample opcodes (T13b);
- a cached view of write-combined pages (T13d);
- render submissions replayed stale presents (T13h);
- Panfrost's CPU texture copies, which read zeros or stale data through an opened surface's mapping (T13o);
- DXGI's Blt stub, harmless here because DWM never calls it (T13r);
- cb0 through the default-uniform route reads wrong (T13w).

**What is left:**
- **Speed:** the emulated scanout copies on the CPU from write-combined memory, up to 0.7 s per whole frame.
  Next: copy only on a new source and let DWM's dirty rectangles do the rest (T13y). After that, the copy on
  the GPU, or DWM's frames rendered straight into the framebuffer.
- **Robustness:** the JS 0x51 job (one per process); SV_VertexID (Panfrost's Bifrost backend has no
  load_vertex_id_zero_base on this path).
- **The normal boot path:** full mode as the default, behind the guard. The guard still never clears itself.

**cycle-t13y-1 (03:21):**
- **The sign-in screen on the panel.**
- **But DWM's frames arrive as flips of its primaries,** and every flip cost a whole-frame CPU copy: 240, up to
  1.4 s each.
- **T13z:** a full-screen primary with the framebuffer's layout gets the framebuffer's physical pages at its GPU
  address when VidMm maps it (Parameters\PrimaryInFb). DWM then renders straight into what DECON scans:
  single-buffered, no CPU copy.

**cycle-t13z-1 (03:33):**
- **The sign-in screen:** no whole-frame CPU copies (Refreshes 0); the primaries render into the framebuffer.
- **JS 0x51 is one job per boot,** not one per process.

### 6.20 T14a: the default-boot guard (2026-11-09)

**Health for the default boot.** Healthy means all of these, a minute after start:
- the framebuffer showed content for 30 s, a second counting when at least 8 of 16 points across the whole
  panel are not black;
- DWM sent 30 frames;
- 30 GPU jobs completed.

A black panel with a spinner does not pass.

**Who may clear the guard:** `Parameters\GuardAutoClear`.
- Code default 0, and unattended cycles pin 0.
- The user's attended setup (`gpu-cycle.sh --install-default`) sets 1: a healthy boot re-arms full mode.
- A failed boot (black, a hang, a forced restart) leaves the next one display-only.
- sbwdog stays Unattended=0 there: feeding only.
- The win11 row needs the GPU firmware (windows-t13b.uefi). On combo3b, full mode refuses to start.

**The first failed job of a boot** is captured: slot, chain, failing job address (JS_HEAD), tail, status, and
when.

`--full-sec N` makes longer soak boots.

**cycle-t14a-1 (03:46):**
- **The new health rule on the phone:** FbLiveSecs 268 of 277 s (FbLiveLast 16/16), FullHealthy 1. The guard
  stayed armed (GuardAutoClear 0 in cycles).
- **The once-per-boot failed job:**
  - **Where:** slot 1 (vertex/tiler/compute), 12 s after start, after 200 jobs. The chain starts at
    0x034b8700 and the failing job is at 0x034b8c40, inside LogonUI's bo 27 (0x34b8000-0x34c8000, NOEXEC,
    64 KB), a descriptor pool made just before LogonUI's first submit. That first frame was 4 Draws + 4
    instanced DrawIndexed into a 1024x1024 target.
  - **What:** status 0x51 (INSTR_INVALID_ENC), on the first frame of the first GPU client, never again.
  - **Suspects:** a precompiled libpan kernel that is wrong for the G72 (v6), or the first execution of a
    freshly uploaded shader. Jobs start with a clean+invalidate flush, which makes a stale cache unlikely.
  - **Next:** a dump of the job descriptor and its shader at failure time. That needs a pre-reserved mapping and
    a copy in the DPC before the fence signals, since Panfrost reuses the descriptor pool right after. Deferred:
    it is recovered by one reset, and nothing visible depends on it.

### 6.21 The 15-minute soak, and a frame-rate test (2026-11-09)

**soak-t14a-1 (03:57-04:24, a 900 s full-mode boot):**
- **The panel:** content for 873 of 883 s sampled (FbLiveSecs), FullHealthy 1.
- **GPU:** 26,183 jobs, 1 failure (the known first-frame JS 0x51), 0 timeouts, 0 MMU faults. 0 whole-frame CPU
  copies.
- **DWM:** 1,264 presents and 724 flips. A static sign-in screen, so this is not a frame rate.
- **Thermal:** 0 holds. GPU at most 50.9 °C, hottest sensor 61.2 °C.
- **The end:** a clean restart at 900 s, and the hive was clean.

**T14b:** `tools/win/mesa/tests/sbfps.cpp`, a full-screen window rendering as fast as it can (a clear plus a
moving quad, with a DYNAMIC constant buffer), 20 s with a blt swap chain and 20 s with a flip one. It logs fps
and the slowest frame.

**cycle-t14c-1 (04:35):**
- **The first measured frame rate:** a full-screen D3D11 app with a blt-model swap chain ran at **81.7 fps** on the
  Mali (1,226 frames in 15 s, slowest 192 ms).
- **The flip-model run crashed** after ~6 s at ~16 fps.
- **The panel showed the lock screen after logon.** DWM kept its 3 framebuffer-aliased primaries throughout, so
  it was composing the lock screen over the session.
- **T14d:** the framebuffer's centre pixel every 10 s (`FbCtrNNN`), a crash logger in sbfps, and WER reports in
  the pull.

**cycle-t14d-1 (04:47):**
- **sbfps:** blt model 79.8 fps, flip model 15.0 fps.
- **The FbCtr timeline:** the panel tracks DWM (lock screen, sign-in, the dark welcome background), and the
  lock/sign-in UI stays over the session.
- **Why the flip model is slow:** d3d10umd's RotateResourceIdentities created and freed a new 17 MB temporary
  per flip. It was shareable, so it never went through Panfrost's BO cache.
- **T14e:** the temporary is kept per device and made without the share binds (sb-patches/0111), and flips of
  framebuffer-aliased primaries skip the KMD's queue-drain wait.

**cycle-t14e-1 (04:59):**
- **Blt model 76.2 fps.** The flip run stopped after ~4 s.
- **sbfps's umdlog:** from then on every submit failed in dxgkrnl ("Render failed"); our KMD never saw them.
  The WER reports were TextInputHost only.
- **Cause:** d3d10umd's copy-based RotateResourceIdentities left the app's buffer 0 on the allocation the flip
  had just handed to DWM.
- **T14f (sb-patches/0112):** non-primary runtime-backed flip chains rotate identities, i.e. Panfrost swaps the
  BOs with their allocations the way its own rename path does. Primaries keep the copy rotation.

### 6.22 The user's first session on the GPU, and T15 (2026-11-09)

The user ran t14a as the default (`--install-default`, win11 row on windows-t13b.uefi) and reported: it works, but
much flickers or never draws, and it is slower than CPU rendering. Data: hive (clean) and 53 umdlogs
(build/win/results/user-20261109; the session ran 11:59-12:09).

- **GPU memory ran out from 12:03** (CREATE_BO → ENOMEM). DWM died and restarted 8 times in 3 minutes: the
  flicker and the black flashes. Then dwm, LogonUI, TabTip and TextInputHost fell back to softpipe (CPU rendering
  through the full driver), which is slower than Basic Display.
  - **Cause:** Mesa's tiler heap, 128 MB per pipe screen (one per D3D device). Linux's panfrost grows it on GPU
    faults; ours is allocated whole. Per process that is 130-436 MB, against the 1 GiB aperture segment.
- **Second D3D devices never drew.** Every Explorer and most TextInputHost processes have two devices. Each
  submission of the second was refused (Render 0xC0000001 → EIO), so the taskbar, Start and the touch keyboard
  never drew.
  - **Cause:** the libdrm shim kept one global device per process; the second CreateDevice rebound it, so every
    submission's allocation list held the other device's allocations.
- **Slow on the GPU too:**
  - The user's DWM made 19,134 waits for 17,386 submissions. Each `WAIT_BO` drained the whole GPU, including
    Mesa's zero-timeout "is this buffer idle?" probes from its BO cache, so the CPU and the GPU never overlapped.
  - The full driver-call trace (`umdlog\trace`, left over from test cycles) was on in every process.
- **Flips:** all 985 flips were of framebuffer-aliased primaries (counted "black": the allocation's own pages are
  never written). With PrimaryInFb all of DWM's primaries alias the one scanned buffer, so DWM draws each frame
  onto the visible panel: tearing and half-drawn frames during animations.

**T15** (stage `sbwddm-t15`):
- **sb-patches/0113** (sbumd):
  - one shim state per runtime device, named by Panfrost's fd (and its duplicates, via Mesa's os_dupfd_cloexec);
  - the d3d10umd entry points select their device (`malikm_use`);
  - tiler heap 64 MB in dwm.exe, 32 MB elsewhere;
  - per-device submission events in a ring, with `WAIT_BO` waiting only for the submission that last named the BO
    (a zero timeout answers busy) and syncobjs carrying sequence numbers;
  - closed BOs freed once their device's work at close time is done, without draining;
  - residency-only allocations listed as read;
  - failed allocations logged with the runtime's HRESULT (UmdNote_25 to 28).
- **sbwddm:** segment 1.5 GiB; `Live_AllocMB`, `Live_AllocMBPeak`, `Live_AllocVaFails`, `Live_AllocFails`.
- **gpu-cycle.sh:** `--install-default` turns the trace off.
- **gputest:** sbtwo (several devices per process, six at once, a pace line).
- **Next (T16):** real DECON flips, with each full-screen primary in a scanout buffer of its own and
  SetVidPnSourceAddress writing the IDMA base (§7.3).

### 6.23 T15 on the user's phone, and T16: textures read through the wrong index (2026-11-09)

**T15 (attended, `--install-default`; 13 photos, hive, 11 umdlogs in build/win/results/user-t15):**
- **What held:**
  - GPU memory peaked at 945 MB (`Live_AllocMBPeak`), with no failed allocations and no refused submissions;
  - DWM did not restart;
  - the taskbar and Start drew (every second D3D device now has its own shim device).
- **What was still wrong:**
  - no text in XAML apps or LogonUI (Settings showed cards and toggles without a letter);
  - no profile picture;
  - dithered taskbar icons;
  - stale regions;
  - heavy flicker;
  - Starbite link about 8x slower.
- **The umdlogs:** every XAML and Direct2D frame is `DrawIndexedInstanced(6 indices, N instances)` into 1024x1024
  shared atlases.

**The cause:**
- d3d10umd ran Panfrost in its legacy texture mode (§6.10), because tgsi_to_nir lacked the D3D10 opcodes.
- In that mode SAMPLE becomes TEX on the **sampler's** unit (`LOG_UNSUPPORTED` when the indices differ), and the
  resource swizzle is dropped.
- Direct2D samples its glyph atlas (and image masks) next to another texture through one sampler, so text read
  texture 0 instead.

**T16 (sb-patches/0114):**
- tgsi_to_nir implements SAMPLE, SAMPLE_B/C/C_LZ/D/L/I/I_MS and SVIEWINFO:
  - separate texture and bare-sampler variables, so the texture and sampler indices are separate (Panfrost
    emits texture and sampler tables independently);
  - the view swizzle and texel offsets applied;
  - a SAMPLE outside fragment shaders becomes lod 0.
- d3d10umd stops using the legacy mode (`SB_OLD_TEX_OPS=1` restores it).
- **Host test:** `tools/win/mesa/tests/host/ttn_sample.c` runs TGSI shaders through tgsi_to_nir, Panfrost's
  lowering and the Bifrost compiler for the G72, natively in the lima VM. All fragment cases compile:
  - two textures and one sampler (texture_index 1, sampler_index 0);
  - B/L/D, compare, load, and buffer load.
  SVIEWINFO and the vertex-shader cases are validated in NIR only: the harness lacks the driver's system-value and
  varying setup, and the legacy controls crash there too.
- **sbtex:** `twotex`, `swizzle` and `a8` tests added.

**Left for T17:**
- real DECON flips: the aliased single scanout buffer tears and shows half-drawn frames;
- Desktop Duplication's CPU reads from uncached allocations, the Starbite link slowdown (sblinkd copies
  dirty rects to a staging texture and maps it).

### 6.24 The T15 restart bugcheck, the registry storm, and a way out of a dirty hive (2026-11-09)

- **The bugcheck:** the user's Start → Restart was very slow, then 0x7E (`B1=c0000005 B2=fffff80910661cd4`,
  TWRP pstore).
  - Matching the page offset `cd4` against T15's sbwddm (rebuilt with a link map, code byte-identical) leaves
    one memory access, `MwDestroyAllocation+0xd8`. That load cannot fault (the same page was just touched by
    the mutex acquire). sbwdog has none.
  - The module is open until the crash dump is read. The bugcheck was followed by ~80 UFS writes (a dump in
    pagefile.sys, which the next boot moves to `Minidump\` / `MEMORY.DMP`).
  - `tools/win/kdump.py` reads small (triage) dumps: drivers, the faulting module+offset, exception record and
    ARM64 context from the saved stack, symbolised with our link maps. `starbite/win/build.sh` now keeps
    `sbwddm.map` / `sbwdog.map`.
- **The registry storm:** MwLog called ZwFlushKey after every value. The guard thread logs ~26 `Live_` values
  a second, so the SYSTEM hive was flushed ~26 times a second for the whole session (UFS and registry
  contention, a slow shutdown).
  - The dirty hive's primary is at sequence 142366 while its logs hold 152455-153965, so the hive cannot be
    replayed offline (`hive-replay.py` applies nothing). Windows must repair it itself.
  - The guard thread now logs in batches (`LogBatch`) and flushes once per `LiveFlushSec`.
  - gpu-cycle sets `LiveSec` / `LiveFlushSec` to 1/1 in test cycles and 10/60 in `--install-default`.
- **Out of the dirty hive with Unattended=0:**
  - sbwdog's one-shot file switch `\Windows\Starbite\sbwdog.unattended`: arms, restarts and caps as
    Unattended=1, then deletes the file. The PMU is now mapped writable at load; writes are still gated.
  - `gpu-cycle.sh --clean-by-file STAGE OUT` uses only file writes: sbwdog.sys and the switch. It also saves the
    pagefile head and pulls the dumps after the repair boot.
- **sbtext:** DirectWrite (grayscale, ClearType), WIC and Direct2D shapes on the Mali versus WARP.

### 6.25 Direct2D's integer vertex attributes, T18 (2026-11-09)

- **T17 on the user's phone:** no bugcheck, and the hive stayed clean across Restart, but rendering looked the same
  (no sign-in name or picture).
  - The 0114 UMD was active (no process used the legacy texture mode).
  - LogonUI's own surfaces did hold pixels.
- **T17's unattended cycle (sbtext, Mali against WARP):**
  - Direct2D's rounded rectangle had the right shape but was pure red (1,0,0,1) instead of blue.
  - The WIC bitmap was two edge lines.
  - Text and the ellipse outline were missing.
  - sbtex `twotex`, `swizzle` and `a8` passed.
- **Cause:**
  - Bifrost's `bi_emit_load_attr` gives a float32-typed attribute load the F32 register format, so the hardware
    converts a pure-integer attribute (R32_UINT, …) to a float value.
  - tgsi_to_nir types every D3D10 input as float32; TGSI has no input types.
  - Direct2D's vertex shaders unpack packed integer attributes with bit operations.
- **sb-patches/0115:** float32 attribute loads use `.auto32`, so the attribute's format decides, as Bifrost already
  did for integer loads. Float, unorm and scaled formats are unchanged; integer formats keep their bits. GL always
  pairs integer attributes with integer inputs, so it is unaffected.
- **sbtex:** `uintattr` and `instattr` tests added.
- **Also seen:** sbfps was cut off by the cycle's 300 s restart, not stalled. Its flip run reached about 65 fps
  (T14: 15).

### 6.26 T18's result and T19: integer attributes per element (2026-11-09)

- **T18 (every attribute loaded with `.auto32`):**
  - sbtex `uintattr` and `instattr` passed, and sbfps completed (flip 56 fps, blt 76 fps).
  - Direct2D filled its target with transparent black, so `.auto32` is not a valid load for non-integer formats
    (Panfrost only ever uses it for integer ones).
- **T19 (sb-patches/0115 replaced):**
  - Bifrost is back to F32 for float32 loads.
  - Panfrost records a mask of pure-integer vertex elements when they are created (`panfrost_vertex_state.sb_int_mask`,
    the first member, so the arch-independent bind can read it).
  - The bound mask is part of the vertex shader variant key (`panfrost_vs_key.int_attribs`).
  - At variant compile, `load_input`s of those elements are retyped to uint32, the register format GL's integer
    attributes use. Input i is element i: d3d10umd places elements at their input register, and tgsi_to_nir's
    driver_location is that register.

### 6.27 T19 = T18; T20 looks at Direct2D's layouts and shaders (2026-11-09)

- **T19's sbtext matched T18's byte for byte** (sbumd 49ea86c3 was pushed).
  - Retyping only integer-fed inputs gives T18's result, so Direct2D does use integer vertex attributes.
  - With them raw, every Direct2D colour (the clear too) is zero. R32_UINT itself works (`uintattr`).
- **T20 (sb-patches/0116, diagnostic, only while `umdlog\trace` exists):** dumps element layouts, shader TGSI and
  the first 64 draws per process.
- **sbtex:** R8G8B8A8_UINT, R16G16_UINT, R16G16B16A16_UINT and R32G32B32A32_UINT inputs, plus an R8G8B8A8_UNORM
  control.

### 6.28 T20's trace: Direct2D's flat integer varyings; T21 (2026-11-09)

- **Direct2D's vertex shader** (TGSI from T20's dump):
  - It reads `R16G16_SINT` and `R32_SINT` elements, per vertex and per instance, and converts them with `I2F`.
    sbtex's integer-format tests all pass.
  - It passes raw integer bits on, `OUT[2] = (IN[4].x, IN[3].y, IN[3].x)`.
- **The pixel shader** reads that input `CONSTANT` and computes buffer offsets with `UADD`/`UMAD`, then
  `SAMPLE_I` into a UINT buffer view and a 2D texture.
- **The cause:** tgsi_to_nir marks TGSI shaders `separate_shader` and types varyings float32. Panfrost skips its
  untrusted-type rules for separate shaders, so the integers went through F32 varying formats, which is lossy
  (varyings.rst).
- **sb-patches/0117:** the rules apply to separate shaders too (VS highp → U32; FS flat → U32, smooth → F32).
  The host test compiles a flat-integer FS to `LD_VAR_FLAT.v4.u32`.

### 6.29 T21: still black; Direct2D's buffer views; T22 (2026-11-09)

- **T21:** sbtext unchanged (only the bitmap moved slightly).
- **The trace:** Direct2D's frame is four draws: two instanced glyph runs, a bitmap strip, and a 1696-vertex shape
  strip. The clear is a ClearRenderTargetView.
  - The text pixel shader reads a `Buffer<uint>` view.
  - The shape and bitmap vertex shaders index a 250-entry constant array (`CONST[2][TEMP]`) with an integer
    attribute.
- **sb-patches/0118:** Bifrost (arch < 9) reads buffer textures through attribute descriptors, which Panfrost
  emits only for shaders whose `info.texture_buffers` is set (GLSL's sampler lowering sets it, tgsi_to_nir did
  not). tgsi_to_nir now sets it; the host test shows the attribute slot.
- **T22's tests:** sbtext draws each element alone against WARP (tiles differing and tiles all zero); sbtex adds
  `cbindex` and `bufload`.

### 6.30 T22 fixed Direct2D; T23 measures a drag (2026-11-09)

- **T22:** sbtext 0 failures (text, ClearType, bitmap and shapes equal WARP; all isolation tests clean); sbtex
  `cbindex` and `bufload` pass.
  - The black tiles were the buffer-view bug as well.
  - T22 is the user's default, and the sign-in screen shows its text.
- **Speed:** the user reports a window drag at about 2 fps. T23 measures it:
  - **sbdrag:** 300 SetWindowPos moves, each followed by DwmFlush. It takes DWM timing info plus the KMD
    accumulators via escape 0x5B02, with and without sblinkd.
  - **KMD:** accumulators for GPU busy, the present queue wait and the present CPU copy, as `Live_Rate*`.
  - **sb-patches/0119:** per-device 10 s umdlog stats (submits, blocked waits, CPU maps).
  - **gpu-cycle `--no-trace`:** a test cycle without the per-call umdlog trace.

### 6.31 The user's T22 session: OOM crashes; T24 real flips (2026-11-09)

- **The user's T22 session:** drags about 4 fps without sblinkd and 2 with it, drops below 1 fps, constant flicker,
  "the driver crashed", and a 10-minute shutdown.
  - The KMD saw no GPU fault, timeout or reset.
  - The UMD had 2 failed allocations (D3DERR_OUTOFVIDEOMEMORY from VidMm at 841 MB peak; the KMD never refused one).
  - LogonUI crashed on a NULL gallium context (`cso_create_context+0xd0`) and sblinkd on a memcpy into an unmapped
    BO (`panfrost_ptr_map+0x2e8`). Both were symbolised with a link map of a rebuilt T22 UMD.
- **sb-patches/0120:** CreateDevice fails with E_OUTOFMEMORY instead; Panfrost drops a replacement BO it cannot map.
- **Real flips (`ScanFlip`):**
  - two contiguous scanout buffers next to the firmware framebuffer, after `sbdc_identify`;
  - each full-screen primary is aliased to a buffer of its own at MAP_APERTURE;
  - a present-flip is carried in its submission and executed in Kick when the queue reaches it (IDMA0 base, shadow
    request, TE trigger), with `FbVa` following the scanned buffer;
  - a latch watchdog in the guard falls back to T13z, and DECON goes home on stop;
  - a test-cycle default; off in `--install-default` until proven.

### 6.32 T23's drag numbers; T25 (2026-11-09)

- **sbdrag (traced):** 4.9 moves/s, about 204 ms per move, the same with and without sblinkd.
  - GPU busy 6.8 %, present queue wait 0, present CPU copies small.
  - DWM's UMD makes about 135 submissions a second and rarely blocks (8-16 ms per 10 s).
  - DwmGetCompositionTimingInfo's cFrame did not advance.
- **Two likely causes:**
  - The vsync interrupt reported `ScanAddr`, which only SetVidPnSourceAddress sets and DWM's present-flips never
    call, so dxgkrnl never saw a flip completed.
  - Per-submission CPU cost on the A55s: every live allocation listed for residency, and a completion event created
    and signalled for every submission.
- **T25:**
  - The KMD records a flip's source segment address in DxgkDdiPatch and reports it from the vsync once the queue
    reaches the flip.
  - sb-patches/0121 lists unnamed BOs only until first resident, with a full refresh every 64 submissions
    (`SB_LIST_ALL=1` restores), and queues completion events only when a wait needs one.

### 6.33 T25's drag numbers; T26 cached blt reads (2026-11-10)

- **sbdrag (T25, untraced):**
  - with sblinkd: 44.7 moves/s, avg 22.4 ms, max 399 ms;
  - without: 38.4 moves/s, max 478 ms.
  - Present copy 330-537 ms/s, almost all from the KMD's CPU copies of DWM's blt presents (2,349 of them).
  - DWM's UMD waits blocked for up to 740 ms. The copies run inside SubmitCommand, which holds the queue.
- **The copies read their source through a write-combined view.** Uncached reads on the A55s are the cost.
- **T26 `CachedBltReads`** (driver default 0, test cycles 1, `gpu-cycle.sh --cached-blt N`):
  - A second, cached view of the allocation's MDL (`AllocationVaCached`) is used for blt sources only.
  - Before the copy, each source rectangle's lines are cleaned and invalidated (`dc civac`, then `dsb sy`).
  - 16 sample pixels are compared with the write-combined view; a mismatch turns cached reads off for the boot.
  - Counters `Live_CachedBlts`, `Live_CachedBltMismatch`.
- **Flip address at Present:** DxgkDdiPresent records a flip's source segment address. Patch overrides it on a move.
  T25's Patch-only capture saw 94 of 3,599 flips. Counter `Live_FlipAddrPresent`.
- A real GPU blit for these presents (a KMD-built Mali job) stays the next step if cached reads are not enough.

### 6.34 T26's result; T27 split blt copies (2026-11-10)

- **cycle-t26-1:**
  - Every flip now carries its address (`Live_FlipAddrPresent` = `Shown` = 3,568).
  - Every blt was read through the cached view (2,551, no mismatches).
  - Present copy was still 475-490 ms/s, so the cached view read as slowly as the write-combined one. Windows
    probably mapped it with the pages' existing attribute.
  - Drag: link 30.6 moves/s, max 866 ms; nolink 42.7, max 312 ms.
- **The user's install from T26** keeps `CachedBltReads` 0 (install-default needs `--cached-blt 1`).
- **T27:**
  - **Split copies:** blt copies of >= 64k pixels are split in 16-row chunks over the submitting thread and 3
    helper threads (`Parameters\ParallelBlt` 0/1/2). Mode 1 alternates one CPU and split for 32 copies each, then
    keeps the faster.
  - **View test:** a one-off read-speed test (write-combined, cached cold, cached hot) decides whether cached reads
    stay on.
  - **Bounding box:** more than 16 dirty rectangles are copied as their bounding box, not the full DstRect.
  - **Counters:** copy sizes, time spent creating CPU views, the longest present, the image name of the process
    sending the big blts, and the IRQLs SubmitCommand runs at.

### 6.35 T27's result; T28 primary identity rotation (2026-11-10)

- **cycle-t27-1 (installed):**
  - **sbdrag:** link 33.9 moves/s, max 62.9 ms, present copy 120 ms/s; nolink 34.0 moves/s, max 118 ms.
  - **View test:** write-combined 87 MB/s; the "cached" view 65 MB/s cold and 87 MB/s hot, so it is write-combined.
  - **Split copies:** 79 MPx/s against 45 MPx/s on one CPU.
  - **SubmitCommand runs at DISPATCH_LEVEL.** DWM sends the big blts, with up to 123 rectangles.
- **Per move ~29.5 ms, GPU busy 44 %, 21-22 jobs per present.** d3d10umd still rotated DWM's three primaries by copying
  (4 full-screen GPU copies per flip), and each Panfrost blit flushes all batches before and after.
- **T28 (sb-patches/0122):**
  - Primaries rotate identities too. Since T24 each has its own scanout buffer. `SB_PRIMARY_COPY_ROTATE=1`
    restores the copies.
  - The present readback runs only for the first 24 presents (`SB_SAMPLE_ALL=1` restores every 64th).
  - The umdlog stats gain rotations (identity / primary / copy), blits and flush-alls per 10 s.

### 6.36 T28's result; T29 GDI blt rectangles, no flush after blits (2026-11-10)

- **cycle-t28-1 (installed):**
  - **sbdrag:** link 46.2 moves/s, avg 21.6 ms, max 44.6 ms; 14.2 jobs per present (T27: 22); GPU busy 28.7 %;
    present copy 210 ms/s.
  - **DWM:** about 56 primary identity rotations a second, no copy rotations.
  - **Flip counters clean:** ScanCur moves, FlipAddrShown = ScanFlips, no fallback, no "Render failed".
- **The KMD's CPU copies** are 1440x2960 → 1440x2960 non-primary blts that never pass through our UMD (all of DWM's
  presents are flips). They are most likely GDI's (CDD), with up to 123 dirty rectangles; above 16 the bounding
  box was copied.
- **DWM's UMD:** about 2.7 blits and 10 flushes of everything per frame (Panfrost flushes all batches before and
  after every blit).
- **T29:**
  - `MW_MAX_RECTS` 128 (bounding box only above that).
  - sb-patches/0123 drops the flush after a blit (`SB_BLIT_FLUSH_AFTER=1` restores it), so a copy-forward blit and
    the frame drawn over it share one render pass.

### 6.37 T29's result; T30 one Render per batch (2026-11-10)

- **cycle-t29-1 (installed):** level with T28.
  - sbdrag 44.0 / 42.8 moves/s, copy 224 ms/s.
  - No bounding-box copies, but GDI's rectangles cover ~2.1 MPx per big blt anyway.
  - DWM composes ~53 frames a second and submits ~16.7 job chains per frame.
- **T30 (sb-patches/0124):**
  - Panfrost marks a vertex/tiler submit followed by its fragment submit (requirements bit 31, Windows only). The
    shim holds it and sends both chains in one Render.
  - Any other call on the device sends a held chain first.
  - `SB_NO_MERGE=1` restores one Render per chain. The umdlog stats count merges.

### 6.38 T30's result; T31 GDI blts overlap the GPU (2026-11-10)

- **cycle-t30-1 (installed):** submissions halved (3,039 renders against 6,050), but moves/s stayed level (44.4 /
  41.6). Per-submission cost is not the limit.
- **What is left:** the GDI blt copy (~240 ms/s), which waits for the GPU queue to drain first, and DWM's pacing.
- **T31:**
  - **Tagging:** standard allocations are tagged by kind, and every allocation records its creator process.
    Counters show what the blts copy between and who made those surfaces.
  - **`BltNoDrain`:** a blt from a CDD shadow/staging surface into a non-primary skips the queue drain and
    overlaps the GPU.
  - **`NeonCopy`:** an in-boot A/B of RtlCopyMemory against a 64-byte NEON row copy.

### 6.39 T32a: the CPU name, SMBIOS in the firmware (2026-11-10)

Task Manager's CPU label was empty because the woa release publishes no SMBIOS: there is no SmbiosDxe, and the
`=FWTS A21` trace lists `acpi20=` only. The SYSTEM hive's `HardwareConfig\{0…0}` held only
`SystemBiosVersion 'EXYNOS - 1' 'EDK II - 10000'`.

**SmbiosDxe doesn't fit.** edk2's SmbiosDxe plus a platform module put the compressed DXE volume 3108 bytes over the
release's fixed slot (0x51000). It was still 2496 bytes over with SerialDxe blanked. Nothing in UEFI consumes the
SMBIOS protocol, since Windows reads the configuration tables.

**One module instead: `Star2LteSmbiosDxe`** (`tools/woa/modules-smbios`).
- `SmbiosBuild.c` writes the table byte by byte (portable).
- `SmbiosPlatform.c` puts the table and both entry points in one page below 4 GiB (EfiRuntimeServicesData):
  - SMBIOS 3.0 (`gEfiSmbios3TableGuid`);
  - 2.1 (`gEfiSmbiosTableGuid`);
  - version 3.3.
- Depex: the CPU and Runtime architectural protocols.

**The records:**

| Type | Content |
|---|---|
| 0 | Starbite firmware, UEFI supported |
| 1 | Samsung Galaxy S9 SM-G960F, family Galaxy S. No serial number; the UUID is zero (not present) |
| 3 | Samsung, hand held |
| 4 | **"Samsung Exynos 9810"**, ARMv8, MIDR_EL1, 4/4/4 cores/enabled/threads (the four A55s Windows runs, NUMPROC=4), max 1794 MHz, current 1690 MHz, external 26 MHz |
| 7 | Only with `STARBITE_SMBIOS_TYPE7=1`. L1 D/I 32 KiB 4-way, L2 512 KiB 16-way (the CLIDR/CCSIDR measurement, WIN11-USB.md §10) |

**Host test** (`modules-smbios/test/run.sh`): our own structure walk and checksums. Then dmidecode 3.5
`--from-dump` decodes all four variants (3.0 or 2.1 entry point, each with and without caches) with no complaint.

**Images** (`tools/woa/build-win-smbios.sh`): windows-t13b.uefi plus that one file.
- `build/win/stage/windows-t32s-nocache.uefi` (c3e43fd2…, types 0/1/3/4).
- `windows-t32s.uefi` (1e50bdd7…, with type 7).
- Checks on both:
  - identity rebuild of t13b's volume byte-identical;
  - `dxe-diff`: SEC/PEI identical, only Star2LteSmbiosDxe added;
  - bootshim identical.
- The no-cache image comes first because the PPTT with the same caches bugchecked Windows (0x7E, unexplained).

**What a cycle shows** (gpu-cycle.sh, T32):
- A21 `smb3=… smb=…`.
- `smbios (…): BIOSVendor 'Starbite' … SystemManufacturer 'Samsung' SystemProductName 'Galaxy S9'`.
- The gputest now starts with `reg query …\CentralProcessor\0 /v ProcessorNameString`, which is Task Manager's
  label. HARDWARE is volatile, so no hive holds it.

### 6.40 T32b: the S9's keys under Windows, Bixby held = clean restart (sbkeys) (2026-11-10)

The release's ACPI has no button device, so the keys did nothing in Windows. When the Start menu would not open
(user, T30), there was no clean way to restart.

**`sbkeys.sys` (starbite/win/sbkeys)** is a plain service like sbwdog (Type 1, Start 2).
- **Why not KMDF/PnP with an SSDT:** the GPIO page 0x14050000 is already a memory resource of the release's touch
  device TSP0, so a second device listing it would get a resource conflict. A PnP driver would also need a
  driver-store install from inside Windows.
- **The registers:** sbkeys maps the page with `MmMapIoSpaceEx(PAGE_READONLY | PAGE_NOCACHE)` and reads only GPA0
  CON/DAT (0x20/0x24) and GPA2 CON/DAT (0x60/0x64). These are the loader's own key bits (shim/starlte/menu.c):
  - Volume Up GPA0.3, Volume Down GPA0.4, Bixby GPA0.6, Power GPA2.4;
  - active low;
  - polled every 30 ms from a LOW_REALTIME thread.

**Patterns** (`sbk_core.c`; host test `starbite/win/tests/keys_test.c`, 41 checks):
- **Restart:** Bixby alone held 3 s → `NtShutdownSystem(ShutdownReboot)` from a second thread (sbwdog's clean
  restart: hives and NTFS flushed), once per boot.
- **Arming:** nothing counts until all keys have read released for 2 s in a row, so a pin stuck low can never
  restart.
- **Never a restart:**
  - Bixby with any other key;
  - Volume Down + Power (the ~7 s hardware force restart).
- **Interrupted holds:** a poll gap over 500 ms drops a hold.
- **Pin check:** a key pin that is not an input or EINT → sbkeys exits at start (PinsOk 0).
- **Optional SAS** (Parameters\Sas=1, default 0): Volume Up + Power, all keys up within 1.5 s → signals
  `Global\StarbiteSasRequest`. The SendSAS half belongs in sblinkd (link track, not edited here), plus the
  SoftwareSASGeneration policy.

**Off switches:**
- `C:\Windows\Starbite\sbkeys.off`;
- Parameters\Enable=0 (`gpu-cycle.sh --keys off`);
- Start=4.

**State** is in `Services\sbkeys\State`. **Unattended proof:** Parameters\SelfTestSec=N (one-shot) feeds "Bixby
held" from N s.

**gpu-cycle.sh** gets:
- `--keys 1|off`, `--sas 0|1`, `--keys-selftest N`;
- after the test boot, the lines `the test boot's hive is clean/DIRTY`, `sbkeys: …` and `smbios …`.

Stage `build/win/stage/sbwddm-t32` = T31 + sbkeys.sys 4660b7c4 + the gputest's `reg query` lines. Its INSTALL.md
has the run, the pass criteria and the install.

### 6.41 T32b: DWM's crash out of video memory (the user's Steam session) (2026-11-10)

**What happened:** the user ran T30 attended. Steam and Geometry Dash brought black screens and flicker, and then a
Start menu that "tweaks out". The pull is `build/win/user-t30-0715`. In it:
- **The crash:** `AppCrash_dwm.exe_ecabb674…`, c0000005 in sbumd 86046664 at offset 0x8140c.
- **Symbolised:** the same objects relinked with `/MAP` give identical code (the recipe: `ninja -t commands` for
  libgallium_d3d10.dll, with `/OUT` and `/MAP` redirected). The fault is in `panfrost_ptr_map` +0x2e8, the
  AFBC/AFRC staging path.
- **Why:** `pan_alloc_staging` returned NULL, and with `assert` compiled out the next line read
  `staging->plane…row_stride` from NULL.
- **The allocation:** the crashed DWM's umdlog ends with `allocation of 9672 KiB failed: 0x8876017c` twice.
  0x8876017c is D3DERR_OUTOFVIDEOMEMORY: the segment's 1.5 GiB CommitLimit was reached across all processes.
- **Why the retry failed too:** Mesa's retry after emptying its BO cache can't help under the shim. Closed BOs wait
  on the dead list (struct wdead, up to 63) until the GPU is done and keep counting.
- **The game:** Geometry Dash crashed 1.5 s later in its own libcocos2d.dll.
- **Older reports:** dwmcore c00001ad (STATUS_FATAL_MEMORY_EXHAUSTION) on 11-09 is the same pattern earlier.

**sb-patches/0125-map-failure-and-alloc-retry.patch:**
1. Every failure in `panfrost_ptr_map` releases the transfer and its staging resource and returns NULL with
   `*out_transfer = NULL` (`pan_ptr_map_fail`).
2. d3d10umd `ResourceMap` clears the transfer on failure and reports E_OUTOFMEMORY, a Map error the runtime expects
   (it was E_FAIL).
3. Shim CREATE_BO: on a failure with closed allocations pending, it waits for them, destroys them, and retries once.
4. The shim counts each device's live, peak and closed-not-destroyed allocation bytes. It prints them every 10 s
   (`sbumd: stats memory …`) and with every failure, so the next OOM names who holds the segment.

**Not changed:** the 1.5 GiB segment, and AFBC for textures DWM maps. Out of memory can still cost DWM a frame or a
device reset, but no longer an access violation.

**Stage:** `build/win/stage/sbwddm-t32b` = T32 + sbumd c878ede4 + `maps/sbumd.map`. From now on every stage keeps a
sbumd map.

### 6.42 T32c: bluescreens that leave a trace, a diagnostic sbumd, the AFBC A/B (2026-11-10 evening)

**What the user saw on T30** (evidence `build/user-1110-evening/`): much smoother, but:
- the touch keyboard half-rendered and transparent;
- File Explorer not rendering;
- frequent bluescreens.

**What the research pass found:**
- The TextInputHost presents are drawn but fully transparent (0/32 non-black): empty, not a crash.
- The only new crash is Explorer at sbumd +0x7e67c = `pan_resource_afbcp_update` +0xc. AFBC-P is off by default
  (`pan_force_afbc_packing` false, so `afbcp` stays NULL); a non-NULL `afbcp` there means a freed
  `panfrost_resource` was still reachable from a batch.
- The bluescreens left nothing to read, because every install since T17 forced CrashControl `CrashDumpEnabled 3`
  and the image has `AutoReboot 0`:
  - the bugcheck tried to write a dump through our UFS driver, which has no dump mode;
  - it never rebooted, so the user forced a restart;
  - the next Windows boot overwrote the pmsg ring holding `=UFSBUGCHECK`.

**T32c:**
1. **BSOD capture:** `gpu-cycle.sh` writes `CrashDumpEnabled 0` + `AutoReboot 1` on every install
   (`hive-service.py crashdump 0 --autoreboot 1`); `--crashdump 3` opts in.
   - A bugcheck now resets at once. The UFS driver's existing callback leaves `=UFSBUGCHECK BC= B1..B4` in pmsg,
     inside the area the loader snapshots at each boot.
   - No second record from sbwdog: nothing would read it.
   - **Follow-up (the loader's owner):** `shim/starlte/crash.c` could copy that line into the store crash log's
     reason for uefi launches, so it survives a Windows boot before TWRP.
2. **sb-patches/0126**, diagnostics only:
   - SetError with its function;
   - OpenAdapter10_2 / CreateDevice interface and version;
   - every GetCaps type;
   - each format's CheckFormatSupport answer;
   - whole-surface present stats: bbox, alpha histogram, colour > alpha;
   - present dumps of the shell's windows (only while `umdlog\dump` exists: test cycles);
   - the resource canary: a magic in `panfrost_resource`, checked in `panfrost_batch_read_rsrc` and `write_rsrc`;
     a dead resource is logged with the caller's RVA (`maps/sbumd.map`) and skipped.

   The gputest opens Explorer, Settings and TabTip at its end.
3. **AFBC A/B:** `C:\ProgramData\Starbite\noafbc` (`gpu-cycle.sh --afbc 0|1`) sets Panfrost's `noafbc` in every
   process. Two cycles with identical flags give the A/B.

Stage `build/win/stage/sbwddm-t32c` = T32b + sbumd 34e75e5d. INSTALL.md has the runs and the install.

**Next, by what 2 shows:** `SUPPORT_D3D10_1` = 1 (FL 10_1: what the shell asks for, per the logged
interfaces/GetCaps), and whatever SetError/CheckFormatSupport show the shell is refused.

### 6.43 Phase 1 plan: a kernel driver that fails like a real one (multi-week; not started)

T32c is diagnostics. "Proper" means faults stay inside the process that caused them and Windows' own recovery (TDR)
works. Today:
- **Faults:** a faulting job resets the GPU, drops the rest of its submission and still reports
  DMA_COMPLETED (`command.c`, the JobsFailed path). The process never learns its work failed; DWM keeps
  compositing garbage or empty surfaces.
- **One address space:** every process's allocations live in one GPU address space (AS0, one page table,
  `maligpu.c`). A bad pointer in one app can read or overwrite another's surfaces; a fault stops everyone's work.
- **TDR:** `RestartFromTimeout` is a no-op, and `ResetFromTimeout` resets the GPU but keeps no per-context state.
- **Never run under Driver Verifier.**

Steps, each with its own unattended cycle:
1. **Report faults as faults.** On a job or MMU fault, notify `DXGK_INTERRUPT_DMA_FAULTED` with the faulting fence,
   the node and the page-fault details. Then dxgkrnl marks the context, and the device, as guilty and removed: the
   app gets DEVICE_REMOVED and recreates its device, and DWM recovers instead of drawing nothing. Verify with a
   deliberate bad-VA job (sbtest) and check that only that process loses its device.
2. **Real TDR.** `ResetFromTimeout`: stop all job slots, flush and reset the MMU, reset the GPU.
   `RestartFromTimeout`: re-init the address spaces and the queue state, with fences resynchronised to dxgkrnl's
   last completed value. `QueryDependentEngineGroup`/`QueryEngineStatus` if dxgkrnl wants them (WDDM 1.3).
   Test: a job that never ends, then `TdrDelay` → recovery without a bluescreen.
3. **Per-process address spaces.**
   - The G72 has 8 address spaces (AsPresent 0xff).
   - Give each WDDM device its own page table and bind an AS at submit (LRU over the 8, as the Linux panfrost
     driver does).
   - Allocations get mapped into each device's table on use (residency list). Shared and primary allocations get
     mapped into every device that opens them.
   - This is the biggest change: sbwddm's single `GpuVa` per allocation becomes one per device; the shim already
     asks the VA per allocation (MW_ESCAPE_ALLOC_INFO).
4. **Driver Verifier.**
   - Run `verifier /standard /driver sbwddm.sys` plus special pool and IRQL checking, in a test cycle with the
     gputest. It needs a bugcheck record that survives: T32c's capture.
   - Then low-resources simulation, for the allocation-failure paths fixed in 0125.
5. **Hardening in step:**
   - check every escape and private-data size against the caller's buffer;
   - validate job chains' VAs against the submitting device's table (after 3, the MMU does this).

Exit criterion: a week of the user's normal use (Steam, Explorer, Settings, the keyboard) without a bluescreen.
Every app failure must be a removed device that the app or DWM recovers from.

### 6.44 T32c's A/B, and T33: dual-source blending (sb-patches/0127)

- **T32c A/B** (`build/win/cycle-t32c-A`, `-B`): both passed (0 failures, no CANARY, no SetError).
  - AFBC off: drag 57.8/57.3 moves/s against 42.0/41.4 with it on (present copy 46/36 against 238/291 ms/s). The
    user's default became T32c with `--afbc 0`.
  - TextInputHost's surfaces are identical in A and B, so AFBC is not the transparent keyboard.
  - Every format the shell asks for is supported. GetCaps is only ever asked type 130.
- **What the dumps show:** in TextInputHost, Explorer and SystemSettings, every pixel with colour > alpha lies in a
  glyph box. There, RGB is 255 and alpha is the coverage, i.e. text drawn onto transparent surfaces is not
  premultiplied. The keyboard's own surface holds only text and the acrylic noise tile; the key plates are the
  compositor's.
- **Cause:** `tgsi_to_nir` always maps a pixel shader's o1 to FRAG_RESULT_DATA1. Its dual-source branch is
  hard-coded off. Yet Panfrost advertises one dual-source target, so SRC1 blend factors reached the blend shader,
  which read r4-r7 that nothing wrote. Direct2D's text uses SRC1 blending.
- **0127:**
  - d3d10umd notes which pixel shaders write o1 and which blend states use SRC1 on RT0;
  - while both are bound, it binds a variant of the shader built through `tgsi_to_nir` with o1 moved to DATA0
    index 1, which Panfrost lowers to `blend2_pan`;
  - log lines `blend state #n uses SRC1` and `dual-source pixel shader variant #n created|FAILED`.
- **sbtext `transparent`:** white grayscale text on a transparent clear against WARP, all four channels, plus a
  colour>alpha count.
- **Stage:** `build/win/stage/sbwddm-t33` (sbumd 416c8caa, sbtext 8dbc65bd); run with `--afbc 0`.
- **Open:** if dwm.exe logs no SRC1 and the keyboard stays see-through, the next diagnostic is a switch-gated DWM
  frame dump while TabTip is up.
- **T33's run** (`build/win/cycle-t33-1`): every test passed, including `transparent` (0.00 against WARP); drag
  60.0/60.0.
  - **A crash:** a new **AppCrash_LogonUI** in T33's sbumd at `panfrost_delete_shader_state+0x8`. DestroyShader freed
    `dual_handle` on vertex and geometry shaders, whose 0127 fields are never initialised.
  - **The variant never fired:** blend states with SRC1 (SRC1_ALPHA/INV_SRC1_ALPHA) exist in 9 processes, but no
    pixel shader with o1 was ever bound with one, so no variant was made.
  - **The text was fine:** D2D text on transparent targets already matched WARP. The TextInputHost surfaces with
    colour > alpha are glyph/mask atlases, where white with coverage alpha is normal. The T32c reading above was
    wrong.
- **T33b (sb-patches/0128):**
  - DestroyShader touches the 0127 fields only for pixel shaders;
  - a log line `SRC1 blend bound with a pixel shader without o1`;
  - dwm.exe dumps its composed frame (half size, up to 8 per boot) whenever `umdlog\dwmframe` gets a new
    last-write time during the boot. The gputest rewrites it after opening Explorer, Settings and TabTip.
  - Stage: `build/win/stage/sbwddm-t33b` (sbumd 6c78df1c). The user's default stays T32c `--afbc 0`.
- **T33b's run** (`build/win/cycle-t33b-1`): passed, with no new WER reports (the LogonUI crash is gone).
  - Only two DWM dumps came out. Settings renders correctly; the Explorer dump shows the gputest's console.
  - The keyboard step was never reached: the `--keys-selftest 360` restart cut every gputest since T32c short before
    TabTip. Starting TabTip.exe alone doesn't show the Windows 11 keyboard anyway.
- **T33c:** `sbshow.exe` (tools/win/mesa/tests/sbshow.cpp) minimises the console, maximises Explorer and Settings,
  toggles the keyboard through `ITipInvocation`, lists the visible windows, and triggers each DWM dump. Run with
  `--full-sec 540` and no keys self-test. Stage `build/win/stage/sbwddm-t33c`.

### 6.45 T35: the crash ledger (sbwdog, the bluescreens' stop codes)

- **Why:** the user reports frequent bluescreens, but no stop code has survived since T32.
  - Every pmsg ring pulled has only the UFS driver's `=UFSBUGCHECK ARMED=…` line, because each boot rewrites the
    ring and the phone was back in Windows before TWRP.
  - The WER folders hold app crashes only, and the UFS driver has no dump mode.
- **The ledger** (`starbite/win/sbwdog/sbcl_core.[ch]`, in sbwdog, every boot): page 0xFED1E000 of the UEFI Device
  window (survives warm resets; next to pmsg 0xFED14000 and the guard's arbiter 0xFED1F000).
  - **Each boot:** running, plus its uptime every 5 s.
  - **Module snapshots** (ZwQuerySystemInformation 11) at 20/60/180 s and every 10 min.
  - **A bugcheck callback** writes `KiBugCheckData` (the code and 4 parameters), each parameter resolved to
    driver+offset. aux_klib.lib was the first try, but it needs MSVC's ARM64 GS and intrinsic runtime.
  - **IRP_MJ_SHUTDOWN** marks a clean end.
  - **The next boot** writes `State\PrevBootText` and friends, plus a 16-entry `History` ring.
- **Self-test:** `C:\starbite\auto\gpu\sbwdog.crashtest` (stage `sbwddm-t35-crashtest`) bugchecks with 0xE2 at
  150 s, once per file (its time is flushed to `State\CrashTestStamp` first).
- **Reading:** `tools/win/crash-report.sh DIR` (replays the hive logs).
- **Stage:** `build/win/stage/sbwddm-t35` (T32c plus sbwdog 178a1750); host tests in `starbite/win/tests/crash_test.c`.

### 6.46 T37: the sweep (finding the bugs nobody reported)

- **Why:** the user's reports (keyboard, Explorer, bluescreens) are only examples ("a lot more I didn't mention").
  The sweep opens a fixed list of ~27 surfaces one by one and keeps what the screen showed:
  - the shell: Start, Search, Quick Settings, Notifications, Widgets, Task view, Alt-Tab, Snap layouts;
  - File Explorer: `C:\`, System32, a context menu;
  - Settings: six pages;
  - apps: Task Manager, Notepad, Paint, Photos, Edge with `sweep.html` (text, gradients, backdrop blur, a 3D
    animation, canvas, WebGL), Store, a message box;
  - the touch keyboard with and without transparency effects.

  The same list on WARP is the reference. `tools/win/sweep-diff.py` ranks the differences.
- **Pieces:**
  - `sbshow`: `snap` = a GDI shot (quarter size) plus a named DWM dump (sb-patches/0130), plus `keys`, `alttab`,
    `type`, `click`/`rclick`, `close`, `dialog`, `image`, `disk`.
  - `starbite/win/autotest/sb-sweep.bat` (pushed as a .bat, so it stays out of Startup) and `sweep.html`.
  - The gputest calls the sweep, dumps the event logs (Application 1000/1001/1002, System 4101/141/117/41/6008) and
    the WER and LiveKernelReports listings, then restarts the phone.
  - `gpu-cycle.sh --reference`: FullMode=0 for the test boot, so DWM and the apps run on WARP.
  - sbwdog gives a display-only boot that runs a gputest the full-mode times. The clean boot after it doesn't
    qualify: the gputest deleted `armed`, and its log is from the previous boot.
- **sweep-diff:** per surface, our DWM dump (or our shot) against the reference shot on 12-pixel tiles at quarter
  size. It reports the share of tiles that differ, black holes (bright in the reference, dark in ours), our GDI
  shot against our DWM dump (a capture/readback bug when they differ), and the crash events in each surface's
  time slot. It writes `report.md` and a reference | ours | difference PNG per surface.
- **Stage:** `build/win/stage/sbwddm-t37`.

### 6.47 The user's history (pull 2026-11-11) and T38: linear STAGING/DYNAMIC textures (sb-patches/0131)

- **Sources:** `build/win/pull-1111` (448 WER reports) and `build/win/user-evtx-1111` (System/Application.evtx,
  parsed with python-evtx).
- **sblinkd.exe:** 14 crashes in T32c's sbumd at `pan_access_tiled_image_generic_aligned+0xaf4`.
  - It maps a D3D11 STAGING copy of the desktop (`starbite/link/win/sender/capture_win.c`).
  - `panfrost_should_tile` accepted it (no bind flags), so STAGING textures were u-interleaved or AFBC. Every Map
    de-tiled the 1440x2960 subresource on the CPU, and crashed there with AFBC off.
- **0131:** `panfrost_best_modifier` returns LINEAR for STAGING and DYNAMIC textures. GPU copies into them use the
  0105 blit; Map is a plain view.
- **Bluescreens:** Kernel-Power 41 always has BugcheckCode 0, there are no 1001 events, and the 6008s are mostly
  test-cycle resets. The ledger (6.45) is the source from now on.
- **Since T32b:** no keyboard/InputApp crash and no DWM crash in our builds.
- **For the big-core track:** 121 illegal-instruction (0xc000001d) crashes in winsqlite3.dll, all from 11-10 19:48
  UTC, when the M3 boots of the same install began.
- **Stage:** `build/win/stage/sbwddm-t38` (= T37 + sbumd 4828e13d).

### 6.48 T38's run and T39: the GPU faults' root cause (sb-patches/0133), the ledger moved, the disk

- **GPU faults in every cycle** (KMD counters in `Enum\ACPI\STRB0072\0\Device Parameters\sbwddm`): 51 resets in
  T32c-B, 27 in T33b, 8 in T34, 57 in T38's sweep. Every one is a translation fault on a read (MMU 0x289002c3).
- **The cause:** T38's canary fired in File Explorer: `panfrost_draw_vbo+0x8fc` handed it "resource 0x7FF9…" with
  magic "alwa", on a draw with a NULL input layout.
  - d3d10umd binds empty/NULL vertex buffer slots as user buffers pointing at a static `dummy[4]` in the DLL (svga
    takes user buffers; this path bypasses u_vbuf).
  - Panfrost reads `buffer.resource` as a `panfrost_resource`, so the draw's attribute address is garbage.
  - **0133:** empty slots get a real 4 KiB zero buffer (`Device::sb_null_vb`). `sbtex vertexid` (crashing since
    T13m) uses the same path.
- **File Explorer:** the list and navigation render; the XAML header (tabs, address bar, command bar) is black,
  because its draws are the ones above.
- **The disk:** C: had 432-482 MiB free (97 MiB at the end of the reference sweep). DWM got OUTOFVIDEOMEMORY 7 times
  while the KMD held at most 826 of 1536 MiB, so commit ran out.
  - **0132:** no dumps below 300 MiB free, and quarter-size DWM dumps.
  - **sbshow:** the same guard, plus memory figures before each shot.
- **The ledger:** 0xFED1E000 is cleared between boots (T38 `post/`: "no record"). It moved to 0xFED13C00, the last
  KiB of ramoops zone 0, which the loader only snapshots.
- **The reference:** it ran at 2220x1080 landscape. `--reference` now sets sbdisplay Modes=1 DefaultMode=0
  Rotation=0 (1440x2960 portrait).
- **KMD diagnostics:** rings of freed GPU ranges (256) and failed jobs (16), with each fault address matched against
  the freed ranges. Output: `Live_FaultLog` and `Live_FaultCount`.
- **Stage:** `build/win/stage/sbwddm-t39`.

### 6.49 T39's 0x7E (read from its own ledger) and T39c

- **The ledger's first real record** (`build/win/results-t39/clean1`): `boot 1: BUGCHECK 0x7E
  SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (c0000005, …) at 390 s; p2 = dxgmms1.sys+0x41cd4`. The zone-0 location works.
- **The KMD's last counters before it** (the next boot's hive):
  - 50 GPU faults, so 0133 did not remove them. The fault log shows two addresses (0x3811000, 0x24a64000), neither
    in a recently freed range.
  - 4 job timeouts at 355-361 s, then the dxgmms1 access violation at 390 s.
- **Likely chain:** 0133 lets `sbtex vertexid` (SV_VertexID, no vertex buffers; it crashed in user mode before) reach
  the GPU. Its job hangs; Windows' TDR runs recovery; our DxgkDdiResetFromTimeout is a no-op (6.43); dxgmms1 crashes.
- **Next:**
  - find why that draw hangs (Panfrost and vertex IDs without attributes);
  - make the KMD survive a TDR (Phase 1: real reset, report the hung fence);
  - check where the remaining faults at 0x3811000/0x24a64000 come from.
- **T39c:** T39 without 0133 (sbumd 16b44d96). The Mesa tree keeps 0133 on branch `sb-with-0133`; `main` has its
  revert.
- **T39c crashed the same way:** dxgmms1+0x41cd4 at 424 s, 4 job timeouts at 324-330 s. So 0133 is not the trigger.
  Every earlier run (T32c A/B, T33, T33b, T34, T38's 878 s sweep) had 0 timeouts.
- **Bisect (`build/win/stage/sbwddm-t40a/b/c`):** T38 plus only the zone-0 ledger, plus only the KMD fault rings,
  plus only sbumd 0132/0134.
- **Independently:** a GPU hang must not end in a bugcheck. That needs a real DxgkDdiResetFromTimeout (Phase 1).
- **t40a (T38 plus only the zone-0 ledger) crashed the same way** (328 s). So all three crashing builds share exactly
  the ledger at 0xFED13C00. T38 (ledger at 0xFED1E000) ran the same early sweep surfaces at the same time without a
  timeout. The other difference is C: free space (1.6 GB against 0.43 GB).
  - No mechanism yet: the page is a pre-boot diagnostic zone for UEFI's UFS and ACPI drivers, and nothing Windows
    runs touches it.
  - Next: a control run of plain T38 with the same free space.
  - The ledger itself recorded all three crashes correctly across 6 boots (`build/win/results-t40a/crash.log`).

### 6.50 The T38 control crashed too: the workload hangs the GPU, and our TDR handling turns it into a bugcheck (T41)

- **The control:** plain T38 (the user's default, `build/win/results-t38control`) hit the same 0x7E,
  dxgmms1+0x41cd4. So the ledger and T39's other changes are innocent.
- **The trigger is in the workload,** most likely Widgets:
  - this morning (0.43 GB free) its board showed grey placeholders only; now (1.6 GB) its web content loads;
  - the crash time matches the sweep's first steps;
  - Widgets is WebView2 (Chromium/ANGLE/D3D11);
  - the user's WER history has Widgets (WebExperience) crashes.
- **Why a hang ended in a bugcheck:**
  - our chain timeout (2000 ms) raced Windows' TDR (2 s);
  - `MwResetEngine` dropped the queue but returned `LastAbortedFenceId = LastCompletedFence`, so dxgkrnl's packet
    queue no longer matched ours;
  - `MwQueryEngineStatus` always said "responsive".
- **T41 (sbwddm 21950bef):**
  - chain timeout 750 ms, plus a 1500 ms submission budget (`SubStart`), so we hard-stop and complete the fence
    before Windows' TDR;
  - `ResetEngine`/`ResetFromTimeout` report everything dropped as aborted, up to `LastSubmittedFence`;
  - `QueryEngineStatus` reports overdue chains as unresponsive;
  - counters `Live_TdrEngineResets`, `Live_TdrAdapterResets`, `Live_TdrQueriedHung`.
- **T41's run (`build/win/results-t41`): passed.**
  - No bugcheck. The KMD recovered **45 hangs** itself (`Live_JobsTimedOut` 45); `Live_TdrEngineResets`,
    `Live_TdrAdapterResets` and `Live_TdrQueriedHung` are all 0.
  - One DWM process (no restarts), `sweep end` reached, tests as T38, no new WER reports.
  - **The trigger is not only Widgets:** Widgets was skipped (C: 980 MiB, under the 1.5 GB gate), and the hangs came
    during Quick Settings (Win+A: 19 s) and the Notification Centre (Win+N: 15 s), ShellExperienceHost's
    XAML/acrylic.
  - T41 became the user's default (13:50).

- **Still open:**
  - **why the GPU hangs:** find the surface, then the job;
  - **DMA_FAULTED instead of DMA_COMPLETED for faulted work** (Phase 1);
  - the GPU-fault fix (0133) once hangs are survivable.

### 6.51 T42: T41 + 0133 (null vertex buffers), and the user's big-core feedback (2026-11-11 afternoon)

- **The user (big-core entry, on T41):**
  - "seems smooth, theres the occasional graphical bug"; File Explorer works;
  - Geometry Dash still crashes on launch, and more generally "the graphics driver is simply not advanced enough".
  - The big-core entry is now their main Windows. Unattended cycles keep using the normal entry, since the driver
    is shared.
- **T42** (`build/win/stage/sbwddm-t42`): T41's kernel (sbwddm 21950bef, sbwdog 65d05c1c, sbkeys 4660b7c4) with
  **sbumd b7694ea2**, Mesa 0126-0134 *with* 0133, built from the VM branch `sb-with-0133` (master keeps the revert).
- **Why now:** T39's bugcheck came from our TDR handling, not from 0133 (6.49, 6.50). With T41 a hang that 0133
  might expose (e.g. `sbtex vertexid` reaching the GPU) costs a recovered timeout.
- **What it should show:** Explorer's header drawn (DWM dumps `07-*`, `08-*`), and fewer `Live_FaultCount` and
  `Live_JobsTimedOut` than T41's run.
- **Geometry Dash:** see docs/WIN11-GPU-CAPS.md §2. In short, it is an x64 OpenGL app (cocos2d-x via glew) and
  Windows has no OpenGL driver for our GPU, so it gets GDI's GL 1.1 and crashes in libcocos2d.dll.

- **T42's run (`build/win/results-t42`, 2026-11-11 19:58-20:17): passed the cycle rules.**
  - The ledger: "boot 1: clean shutdown or restart after 829 s". The 0x7E lines still in History are T39, T39c and
    the T38 control (before T41).
  - `sweep end`; tests as T41 (`sbtex softpipe` 1 and `sbtex vertexid` crashing in user mode are the known two;
    0133 does not change vertexid, which is T13m's missing `load_vertex_id_zero_base`).
  - `Live_TdrEngineResets`/`AdapterResets`/`QueriedHung` 0.
  - **Counters:** `Live_JobsTimedOut` 27 (T41: 45), but `Live_FaultCount` 115 (T41: 90). The faults are still
    level-3 read translation faults from many shader cores (`mmu 0x..9002c3`) at a few VAs (0x36ad000,
    0x3496000), not in recently freed ranges. So 0133 did not remove that fault class.
  - **No verdict on Explorer's header:** C: was at 841 MiB, and opening Explorer grew the pagefile (commit limit 4430
    -> 4892 MB) to 352 MiB free. Every shot from `07-explorer-c` on was skipped by the 600 MiB guard.
    - From T44 on, the gputest deletes its GL/UMD test files before the sweep.
    - The real fix is more free space on C: (the user's call).
  - **Plan:** not installed by itself. T44 carries the same sbumd: install T44 if it passes, else T42.

### 6.52 T44: GPU-hang forensics in the KMD (sbwddm 274f0837)

- **The gap:** T41's 45 timeouts left only `js 0x08 ... no MMU fault` in `Live_FaultLog`. That names neither the job
  type nor the client.
- **At each timeout** (`Reap(CheckTimeout)`, DIRQL under `SyncPoll`, just before the hard stop) the KMD records:
  - the slot, `JS_HEAD`/`JS_TAIL`/`JS_STATUS`/`JS_CONFIG`/`JS_AFFINITY`;
  - `GPU_FAULTSTATUS`/`GPU_FAULT_ADDRESS` and AS0's fault registers (offsets from Linux `panfrost_regs.h`,
    `common/maligpu.h`);
  - the first 16 dwords of the job descriptor at `JS_HEAD`.
- **How the descriptor is read safely:**
  - memory cannot be mapped at DIRQL, so when a timeout is due the poll DPC (DISPATCH) maps the page the GPU's own
    page table has at `JS_HEAD` into one page of system address space reserved at AddDevice;
  - this uses `MmMapLockedPagesWithReservedMapping` and a one-page MDL, write-combined, and only for RAM PFNs
    (`MmGetPhysicalMemoryRanges`);
  - the read happens only if `JS_HEAD` is 64-byte aligned and inside an allocation that one of the hung submission's
    chains starts in (each submission now remembers those). VidMm keeps that allocation alive until we report the
    fence;
  - the page is unmapped right after.
- **The records:** an 8-entry ring next to the Faults ring. The guard thread writes it at PASSIVE as `Live_HangLog`,
  plus `Live_HangCount`, `Live_HangViews` and `Live_HangViewFails`, with a sequence number so it skips half-written
  records.
  - Each line has the job type (word 4 bits [7:1]) and the creator process of the allocation.
- **Unchanged:** the timeouts (750/1500 ms) and the recovery semantics.
- **Cost:** the per-submission private data grows by 256 bytes.
- **The plan:** run T44 (with the OpenGL tests, WIN11-GPU-CAPS.md §6). If it passes, install it so the user's own
  sessions record hangs too.

### 6.53 T46: GPU-fault forensics (sbwddm 951c37e4)

- **Why:** T42 showed more GPU MMU faults than T41 (115 vs 90), all level-3 read translation faults from many
  shader cores at a few VAs, "not in a recently freed range". Two candidates:
  - VidMm evicted or unmapped an allocation that the GPU still reads through descriptors, because a later submission
    did not list it (memory load was 86-93%);
  - a garbage or overrun address.
- **`memory.c`:** every allocation joins a live list at creation and leaves it before it is freed. That list changes
  only inside `DxgkCbSynchronizeExecution`. Each allocation also counts its maps and unmaps, with the time of the
  last one.
- **`command.c`:** each Render gets a sequence number, stamped on every allocation in its allocation list, and each
  submission keeps its own.
- **At a fault** (ISR or poll, under the interrupt lock) the record finds the faulting VA and the slot's `JS_HEAD`
  among the live allocations:
  - inside one: its range, creator process, `mapped` and `pte` state, unmap count, and whether the running
    submission listed it;
  - in none: the nearest allocations below and above.
- **The job header** is read afterwards in the DPC through T44's reserved-page view, before dxgkrnl learns of the
  fence.
- **`Live_FaultLog` gains these fields;** new counters `Live_FaultsInLive`, `Live_FaultsInNone`,
  `Live_FaultDescReads`.
- **Recovery and timeouts are unchanged.**
- **Stage `sbwddm-t46`:** diagnostic, no install.

### 6.54 T45 and T46 read (night of 2026-11-11/12); T47 = the GPU-fault fix (sb-patches/0138)

- **T45 (the ARM64X sbumd installed; retry `build/win/results-t45b`): passed.**
  - No bugcheck; boots clean (873 s / 902 s).
  - Every native test passes on the ARM64X file's native view: triangle, sbtwo, sbtext, sbtex (minus the known
    vertexid), sbshare, sbdwm, sbfps.
  - **`sbd3d x64 exit 0`:** an emulated x64 program got a D3D11 device on "Starbite Display", with `sbumd.dll`
    loaded in the process, and its clear and triangle read back right. `sbd3d arm64 exit 0` too. Feature level 10_0
    (0xa000).
  - The lines are in `pre/gputest/gputest.log`. The peer's script looked in ours.log and reinstalled T44, so T45
    still has to be installed.
  - **GD1:** Steam started Geometry Dash within 75 s. Its GPU memory went 191 -> 308 -> 435 MiB in 30 s, one
    allocation failed and its retry succeeded, then the game exited by itself (no exception, no abort, no WER
    report).
  - At the same time **the system ran out of memory:** commit free fell to 15 MiB and C: to 60 MiB free. DWM
    fail-fasted (`c00001ad` in dwmcore), as did Steam's web helper (Chromium's `e0000008`) and Paint (`c0000409`).
- **T46 (fault forensics, `build/win/results-t46`): passed.**
  - It resolved 74 of 74 faults to a **live allocation of another process** (explorer, TextInputHost; 64 KiB
    surfaces), `mapped 0 pte 0` after unmaps, and **not listed** by the faulting job.
  - The faulting jobs are fragment jobs in DWM's job memory; the 13 hangs are DWM `indexed-vertex` (IDVS) jobs.
- **The cause:** the shim lists a BO the batch doesn't name only until it was resident once, then in every 64th
  submission (T25). Under memory pressure VidMm evicts what a submission doesn't name, and DWM's GPU reads other
  processes' window surfaces it did not name in that submission.
- **sb-patches/0138:** every allocation opened from another process (kind 2) goes into every submission.
- **T47** = the ARM64X sbumd 1114ac95 (0137 + 0138, both views rebuilt; static checks 0 failures) + KMD 951c37e4
  (T46's forensics), to measure the faults.
- **The memory pressure itself** (C: 0.06-0.6 GB free, a 631 MB pagefile) is the user's disk: WIN11-GPU-CAPS.md §2
  and the morning note.

### 6.55 T47 read; T48 = every live BO in every submission (sb-patches/0139)

- **T47 (`build/win/results-t47`, 04:27-04:46): passed and installed.**
  - No bugcheck; `sbd3d arm64` and `sbd3d x64` exit 0; Windows' TDR counters 0.
  - The WER folder only held T45b's reports (the same IDs).
  - Faults 82 (T46: 87), in live allocations 64 (74), job timeouts 18 (13). Commit free never fell below ~719 MB,
    so there was less pressure than T45b.
- **0138 did not fix the faults:** every resolved one is still `NOT listed`.
  - The allocations: a 1-page SystemSettings allocation (unmapped for 150 s, last listed 8,396 renders before) and
    TextInputHost and Taskmgr surfaces, read by fragment jobs.
  - So it is not only surfaces DWM opened. A process's own BOs reached through descriptors are evicted too: T25
    lists those once, then in every 64th submission of that process, and a rarely-submitting app goes thousands of
    renders without.
- **sb-patches/0139:** the shim's list-all mode (T25's `SB_LIST_ALL=1`) is now the default, for the runtime and the
  D3DKMT (GL ICD) devices.
  - Off with `C:\ProgramData\Starbite\listsome` or `SB_LIST_ALL=0`.
  - The cost T23 measured is on the A55s; the user's main entry is now the big cores.
- **T48** = T47 + sbumd ca3df477 (ARM64X, 0137-0139).
  - If faults fall to ~0, eviction of unlisted own BOs was the cause.
  - If they don't, the remaining reader is DWM with a stale descriptor of a surface it already closed.

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

### 8.6 Big-core boots: the guard owns ACPM DVFS (T34, 2026-11-11)

- **The decision** (main session, side-session notes `acpm-arbiter.md`): on boots where Windows runs on the M3s, the
  A55 guard owns the ACPM DVFS queue (channel 0) and governs the M3 cluster.
- **sbwddm:** never wrote that queue. Its only ACPM traffic is BUCK6 on the PMIC channel (id 2), once at GPU start
  and once at stop, and it sends no GPU DVFS.
- **sbthermal:** its A55 boost (`set_a55`, domain 2) was the one Windows writer on the queue, so it was the one to
  fix.
- **T34 (sbthermal db7ef111, `build/win/stage/sbthermal-t34`):** once per boot, before the first reading,
  sbthermal decides whether this is a big-core boot:
  - **reason 1:** the CPU it runs on is not a Cortex-A55 (MIDR_EL1);
  - **reason 2:** the arbiter page 0xFED1F000 is live (magic 'SBAR', version 1, and a heartbeat that moves within
    1.2 s; a magic alone can be stale after a warm reset).

  On a big-core boot it sends nothing on the DVFS queue for the whole boot. Its TMU readings, the watchdog gate and
  the ACPM lock for other drivers are unchanged.
- **Logged in `State`:** `Acpm` (the text), `BigCore`, `Midr`, `Arbiter`, `ArbiterHeartbeat`.
- **Host tests:** 12 new checks in `starbite/win/tests/host_test.c`.
- **Installing:** a file swap from TWRP (gpu-cycle.sh never pushes sbthermal); the steps are in the stage's
  INSTALL.md.

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
