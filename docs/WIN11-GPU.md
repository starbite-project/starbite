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

### 6.56 T48 read; T49 = scratch-page KMD + SV_VertexID + GD diagnostic; T49b = no-IDVS A/B (2026-11-12)

- **T48 (`build/win/results-t48`; 0139 list-all): not installed.**
  - Faults in live allocations 0 (T47: 64), so listing every buffer does remove the eviction faults.
  - But timeouts 54 (18), every one a `dwm.exe` `indexed-vertex` job; sbfps -10..-13%; sbdrag -27..-45% (it ran before
    GD, so GD did not skew it).
  - **Geometry Dash did not crash there:** our GD1 step gave it 40 s after Steam's 180 s start on the A55s, then
    `taskkill`ed it while it was loading (427 MiB, no exception).
- **sbwddm 7008b9e4:**
  - the unmap paging operation points an evicted allocation's PTEs at a zeroed scratch page with read-only leaf
    0x241 (Mali LPAE: no write bit; Linux io-pgtable-arm), so a stale read returns zeros instead of a translation
    fault and GPU reset;
  - FreeAllocation still unmaps (a use-after-free still faults);
  - `Parameters\ScratchUnmap` (default 1); `Live_ScratchMaps`/`Live_ScratchPtes`; the fault log shows `pte S`.
- **sb-patches/0141 (SV_VertexID):**
  - d3d10umd emits TGSI `VERTEXID_NOBASE`, which becomes `load_vertex_id_zero_base`; Bifrost v6 had no case for it
    ("Unhandled intrinsic", value never written);
  - now lowered to `load_vertex_id - load_first_vertex`, with a per-draw sysval (index_bias / start): D3D11.3 §8.16;
  - host-compiled evidence: `build/win/umd-vid/host-evidence/`.
- **sb-patches/0149:** `C:\ProgramData\Starbite\noidvs` (or PAN_NO_IDVS=1) compiles vertex shaders without IDVS, to test
  whether IDVS is what hangs DWM (T49b).
- **sb-patches/0145:** the GL ICD presents straight from the mapped BGRA back buffer (no per-frame malloc/copy).
  - The remaining cost is GDI's one read of write-combined memory, roughly 50-110 ms per full-screen frame
    (§6.35's 87-180 MB/s). A cached CPU view or a D3DKMT present is next.
- **sb-gd.bat:** a GD diagnostic replacing the sweep (360 s start, 300 s of 30-second rows, shots, events).
- **Stage `sbwddm-t49`** (sbumd e01fa73f = 0137, 0138, 0141, 0149 on sb-int; 0139 reverted). T49b = the same stage
  with the noidvs flag.

### 6.57 Full-mode rotation (landscape) (built 2026-11-12, not yet on the phone)

Settings > Display > Display orientation in full mode. Until now sbwddm's path offered identity only. Our scanout
(DECON's IDMA channel 0) cannot rotate, so rotation must come from Windows or from the driver's CPU copies.

**The WDDM rules (Microsoft's documentation):**
- **The primary keeps the panel's orientation.** "The Microsoft DirectX graphics kernel subsystem uses non-rotated
  surface dimensions to create the shared primary surface." The rotation reaches the driver as the path's
  `ContentTransformation.Rotation` in `DxgkDdiCommitVidPn` and `DxgkDdiUpdateActiveVidPnPresentPath`. Angles are
  counter-clockwise. Support is reported per path through `pfnUpdatePathSupportInfo` in
  `DxgkDdiEnumVidPnCofuncModality`.
  ([Supporting Rotation in a Display Miniport Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver),
  [D3DKMDT_VIDPN_PRESENT_PATH_ROTATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ne-d3dkmdt-_d3dkmdt_vidpn_present_path_rotation))
- **The driver rotates only presents with `Flags.Rotate`.** The flag means "rotate the presentation data to match the
  current orientation of the screen during the presentation bit-block transfer". Without it the driver must not rotate,
  "even if the driver determines that the current orientation of the screen is rotated from the presentation data".
  `Rotate` is a blt flag; nothing in DXGKARG_PRESENT or DXGKARG_SETVIDPNSOURCEADDRESS asks for a rotated flip.
  ([DXGK_PRESENTFLAGS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_presentflags))
- **DWM pre-rotates.** The clone-mode table says that when the target is rotated and `Rotate` is not set, "the
  rotation-aware client has already properly oriented the content of the source". For a portrait-first device, "the
  Desktop Window Manager (DWM) rotates its content in advance to match the proper orientation". DWM's flips (and its
  own present blts from its back buffer) therefore arrive already turned into the panel-oriented primary, and the
  scanout needs nothing. Geometry agrees: at 90/270 an unrotated 2960 × 1440 desktop cannot be held in a 1440 × 2960
  primary. (Same page, "Clone-mode behavior" and "Clone-mode requirements starting with Windows 8.1 Update".)
- **Clients that are not rotation-aware are DXGI's job.** A full-screen DXGI swap chain without
  `DXGI_SWAP_CHAIN_FLAG_NONPREROTATED` gets "automatic image rotation"
  ([DXGI_SWAP_CHAIN_FLAG](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag)).
  The runtime carries it out in the UMD's `pfnBlt` (`DXGI_DDI_ARG_BLT.Rotate`,
  [DXGI_DDI_ARG_BLT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_arg_blt))
  or in a kernel present blt with `Rotate`. **sbumd's `_Blt` (sb-patches/0107) logs a non-identity `Rotate` and
  draws unrotated.** Fullscreen-exclusive D3D games in landscape are the case to watch: the umdlog line
  `sbumd: Blt ... rotate N`.
- **Rectangles of a `Rotate` blt:** the documentation does not say which orientation `DstRect`/`pDstSubRects` use. We
  follow the KMDOD sample, as sbdisplay does (§7.5b): the source is the desktop-oriented image and the rectangles are
  desktop coordinates. `Live_RotBltWide` counts rotated blts whose rectangle reaches past the panel's width, which only
  desktop coordinates can do at 90/270.
- **`SupportSmoothRotation`** (already set): orientation changes come through
  `DxgkDdiUpdateActiveVidPnPresentPath` without a new VidPN
  ([DXGK_DRIVERCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_drivercaps)).
  **`StatusRotation`** is the angle of the display itself, for monitors with an orientation sensor
  ([DXGK_CHILD_STATUS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/ns-dispmprt-_dxgk_child_status)).
  Ours is `D3DKMDT_MOA_NONE`, so it stays 0.

**Implemented** (`sbwddm/maliwddm/display.c`, `command.c`; `Parameters\Rotation` = 0 gives exactly the old behaviour):
- **VidPN:**
  - `EnumVidPnCofuncModality` offers Identity, Rotate90, Rotate180 and Rotate270, plus Offset0 (as sbdisplay).
  - `IsSupportedVidPn` also checks the path's rotation.
  - `CommitVidPn` and `UpdateActiveVidPnPresentPath` record it in `Rot`. A rotation we never offered (an OFFSET value)
    is refused by UpdateActive but only logged at commit, because a refused commit can leave a full-mode boot without
    a desktop.
  - `QueryVidPnHWCapability` reports `DriverRotation`.
  - Source, target and monitor modes stay 1440 × 2960.
- **Blt presents with `Flags.Rotate`:**
  - `DxgkDdiPresent` records the path's rotation in the op.
  - `CopyBltRot` copies through sbdisplay's `sbdc_blit_xf`: the same KMDOD maps, the tiled 90/270 path, and the
    scalar 180 path. It writes into the destination primary's pages and into the scanned buffer.
  - Big copies are split over the CPUs as in T27 (ParallelBlt, not when one CPU won the A/B). A chunk is 16 desktop
    lines, which become whole panel rows (`sbdc_xf_blit_lines`), so CPUs never share a row.
  - A rotated blt's full-screen source is never shown whole by the refresh (T13s), because it is not the panel's
    frame.
  - Fills with `Rotate` have their rectangle turned.
- **Flips:** shown as they are (DECON flips, T24/T25 unchanged). They are counted while rotated, and any flip that
  carries `Rotate` is counted too.
- **New code in `sbdc_core` (host-tested):**
  - `sbdc_xf_panel_rect` gives the exact panel rectangle a copy writes.
  - `sbdc_xf_lines` and `sbdc_xf_band` cut a rectangle into bands.
  - `sbdc_xf_blit_lines` copies one chunk of a split job.
- **`tests/run.sh`, three new checks:**
  - the panel rectangle equals the bounding box of the written pixels, and all of it is written;
  - a rectangle copied in 16-line bands, last band first, is identical to the whole copy, and each band is whole
    panel rows;
  - jobs of 1-5 rectangles copied in scrambled chunk order are identical to the rectangles copied whole.
  - Each runs 256 cases over the 4 rotations × 3 scales at real sizes. A one-line mutation of the 270 map fails two
    of them.

**Switches** (sbwddm's service key, `Parameters`):
- `Rotation` (default 1): offer 90/180/270. 0 = identity only, as before.
- `RotateFlips` (default 0), for an A/B on the phone if a landscape desktop comes out turned:
  - 1 = flips made while the path is rotated go through the emulated scanout (MwRefreshScanout), which copies them
    rotated when the flipped primary has the desktop's size. At 180 that is any full-screen primary; at 90/270 only a
    landscape-shaped one.
  - Panel-shaped primaries at 90/270 are copied as they are (`Live_RotFlipNoFit`).
  - Primaries the GPU drew straight into a scanout buffer (T13z/T24) cannot be turned in place, so they are still
    flipped (`Live_RotFlipAliased`). The switch therefore only does anything with `PrimaryInFb` = 0.

**State:**
- `Live_Rotation`: 1 identity, 2 = 90, 3 = 180, 4 = 270.
- `Live_RotatedBlts`, `Live_RotBltMs`, `Live_RotBltWide`: rotated blts and fills, their CPU time, and the coordinate
  check above.
- `Live_RotatedFlips`, `Live_FlipRotateFlag`: flips while rotated, and flips carrying `Rotate`.
- `Live_RotPathAsked`: the last rotation Windows asked for.
- `Live_RotFlipCpu`, `Live_RotFlipNoFit`, `Live_RotFlipAliased`: the RotateFlips flips.
- Once per start: `RotAllowed`, `RotateFlips`. At each change: `CommitRotationAsked`, `PathRotationAsked`, `Rotation`.

**What to check on the phone** (Settings > Display orientation > Landscape, or tools/win `sbrotate`):
- The desktop is right side up and fills the panel. Taskbar text reads left to right with the panel held landscape.
- If "landscape" and "landscape (flipped)" come out swapped, the KMDOD direction is the suspect, as in §7.5b.
- `Live_Rotation` = 2 or 4. `Live_RotatedFlips` climbs. `Live_FlipRotateFlag` stays 0.
- `Live_RotatedBlts` and `Live_RotBltMs` stay small. If they grow, someone presents unrotated content with `Rotate`
  (CDD/GDI); `Live_RotBltWide` then tells which coordinates the rectangles were in.
- **Touch lines up.** Windows turns digitizer coordinates with the display's orientation. Our target reports as an HDMI
  output, not an integrated panel, so if touch stays in portrait the digitizer-to-display mapping (Tablet PC
  Settings) is the thing to look at, not the driver.
- **fps stays as in portrait:** `Live_RatePresentCpuMs`, sbdrag, sbfps. DWM's pre-rotated frames cost the GPU the same,
  and the CPU copies only `Rotate` blts.
- No `sbumd: Blt ... rotate` lines in the umdlog for DWM. They would mean DXGI expects the UMD to rotate.
- The bugcheck screen stays in portrait, because `SystemDisplayWrite` writes in the panel's orientation.

### 6.58 T49-T51 read: faults/hangs fixed, landscape, Geometry Dash's frame time (2026-11-12)

- **T49 (installed 21:13):**
  - faults 82 -> 6, `JobsTimedOut` 18 -> 0 with the scratch page (`Live_ScratchMaps` 12,322), so DWM's "IDVS hangs"
    were stalls on evicted memory;
  - the 6 left are js 0x51 (INSTR_INVALID_ENC): shader code read from a scratch-mapped page;
  - sbfps flip 641 / blt 825 (T47: 593 / 829).
- **T50:** GeometryDash.exe never started (no ICD log, no WER). Steam did not launch it. sb-gd.bat now logs Steam.
- **T51 (installed 22:24): landscape.** sbrotate 270 -> 90 at 2960x1440 @ 60 Hz; sbd3d native, x64 and landscape all
  exit 0. `Live_RotatedFlips` 8,311 with `FlipRotateFlag` 0, so DWM pre-rotates as documented. `RotatedBlts` 270 (266 ms).
- **Geometry Dash's frame (T51, A55 normal entry, landscape 2960x1440, 0146 x64):** a steady 14 fps.
  - Per frame, interval 71 ms = present 52 (copy out of write-combined memory 48 with 4 threads, GDI 5) + about
    19 ms of game and driver CPU.
  - map (GPU wait) 0.1 ms.
  - **The GPU is not the limit.** The zero-copy present (T52) removes the 52 ms.

### 6.59 T52 read: zero-copy GL draws right but nothing shows; are windowed D3D11 swap chains shown at all? (2026-11-12)

- **Zero-copy (0150/0151) renders correctly and fast:** sbglzc 51.2 fps (GDI) -> 148.9 fps (zero-copy). GD menu
  ~40 frames/s by the driver's submit counts (T51: 14). GD's swap chain buffers hold the right menu picture (sbumd's
  present samples). No bugcheck, sbd3d arm64/x64/landscape exit 0.
- **But the window never reaches the screen:** the GD snaps 28-gd-1..10 (GDI shot and DWM dump) show the desktop and
  the console, no GD (T51: GD covered the screen). In the 2 s zero-copy run the KMD saw 1 present (GDI run: 118), so
  DWM composed nothing new.
- **DWM never opened an app swap chain buffer:** dwm.exe's umdlog has OpenResource only for DComp surfaces (1024x1024,
  32x32, 576x576, one 2976x1440), none for GD's 2960x1440, sbglzc's 1280x701 or sbfps's 2960x1440 buffers (created
  shared + DISPLAYABLE_SURFACE, misc 0x20002). App presents reach sbumd as DXGI_DDI_PRESENT_FLAGS 0x1 (Blt, no
  destination); DWM's own as 0x2 (Flip). The KMD never saw a non-primary flip (Live_EmuFlipsNotPrimary 0), so it is
  not acting on them. **So sbfps's 600-800 fps (since at least T47) may be frames nobody composes**, and windowed
  D3D11 games would be invisible too. Not yet shown either way.
- **ZC1's "2 failed" are only the readback:** glReadPixels needs a CPU map of the opened shared texture, and the shim's
  mmap of a kind-2 (opened) BO fails ("mmap(..., size=3620864) failed: Invalid argument"). Rendering is unaffected.
- **Not installed.** T53's GD driver (731600a5) is the GDI build + 0152 threading, without zero-copy: unaffected.
- **T54 (VIS1, sb-vis.bat):** screenshots while sbfps (flip, blt) and sbglzc (zero-copy flip, zero-copy DISCARD, GDI
  control) run ~20 s each (`SBGLZC_SECS`). Shows whether any windowed swap chain reaches the screen.
- **Seen in passing:** msedge.exe (v100) fail-fasts in ucrtbase (0xc0000409) many times a day (gd-app-events).

### 6.60 T53 read (threaded driver: no gain) and T55 staged: the async present (sb-patches 0153) (2026-11-13)

- **T53 A/B, GD menu, glstats steady lines:** A (plain) 13.9-14.5 fps, interval ~70 ms, copy ~47 ms, gdi ~5 ms;
  B (0152 threaded) 15.7-16.5 fps, interval ~61 ms, copy ~37 ms, gdi ~5 ms. The non-present part is ~19 ms in both:
  threading removed none of it. B's small gain is all in the copy, which varies run to run (T51: 38-50 ms). B used
  +230 MB working set (~700 vs ~465 MB) with memory load 87-91 %. **Not adopted.**
- **Why threading can't help here:** the present maps the frame (waits for everything recorded) and copies it on
  the game's thread, so the driver thread's work can't overlap anything.
- **0153 async present:** the copy and GDI of frame N go to one kept helper thread (with the copy pool) while the
  game draws frame N+1: frame time max(copy + GDI, game) instead of the sum. stw presents the back buffer and then
  exchanges it (`stw_st_swap_framebuffer_locked`), so the presented texture is not drawn into during the next frame;
  the job references it, the next present waits for the job first (one in flight). Only when the READ map is the BO
  itself (`panfrost_sb_bo_cpu_range`, a 13-line helper in pan_resource.c); the map is let go at once. GDI through
  the helper's own GetDC/ReleaseDC. Off unless `C:\ProgramData\Starbite\glasync` or `SB_GL_ASYNC=1`.
  glstats: `mode async`, `async wait`, `frames ... async N`.
- **T55** = T53 + the 0153 GD driver; the A/B flag file is now named in sb-gd-ab.txt (`glasync`).

### 6.61 T54 read: no windowed D3D11 swap chain reaches the screen; T56 (VIS2) staged (2026-11-13)

- **Shown:** VIS1's shots 32/33 were taken while sbfps's topmost full-screen window ran its FLIP_SEQUENTIAL and then
  its DISCARD swap chain (677.9 / 833.6 fps). Both show the wallpaper and the consoles, **not the window**. The GDI
  control works (sbglzc's GDI run: 430 KMD presents, readback PASS). So the HWND swap chain path is broken for
  every D3D11 app, flip model and blt model alike, not only for the zero-copy GL present. With 6.59 (DWM never opens
  an app swap chain buffer; app presents reach sbumd as Blt with no destination; the KMD never sees them), the
  frames stop between DXGI/dxgkrnl and DWM.
- **Lost:** shots 34-36 (sbglzc zero-copy flip/discard, GDI). sb-vis.bat called `sbshow.exe` after `pushd glzc`; it
  now uses `%D%\sbshow.exe`. sbglzc zero-copy timed runs: 291.9 fps (9 KMD presents), DISCARD 312.6 fps (7); GDI
  50.1 fps (430).
- **T56 (VIS2, sb-comp.bat, sbcomp.exe):** the same 1200x700 topmost window presented five ways (GDI control, HWND
  flip, HWND blt, DComp swap chain, DComp surface), 12 s each, a snap each, and an ETW trace (DXGI, DxgKrnl Base +
  Present, Dwm-Core) per mode as CSV (tracerpt). Explorer's DComp surfaces reach DWM (DWM's OpenResource of
  1024x1024 atlases), so a DComp route may work where HWND swap chains don't: a way to show GD's zero-copy frames,
  and a pointer to what the HWND path lacks.

### 6.62 T55 read: async present +35 %; T57 staged: cached GL back buffers (sb-patches 0154, sbwddm d85e0d3a) (2026-11-13)

- **T55 (GD menu, A55s, 2960x1440, steady glstats):** A (sync) 13.2-14.2 fps, interval 70-76 ms, copy ~48 ms;
  B (0153 async) 18.0-19.2 fps, interval 52-56 ms, copy ~46 ms, gdi ~5 ms, async wait ~26 ms. The game's ~26 ms now runs
  beside the copy: the frame is the copy + GDI. B's snaps show the menu right; no exceptions. B's working set is
  ~200 MB above A's, as in T53, so that is the game's second launch, not the async present. **Adopted: async is the
  default from 0154 on** (off: `glsync` file or `SB_GL_ASYNC=0`).
- **The copy is the limit: 17 MB from write-combined memory, ~370 MB/s on 4 threads.** T26/T27 showed a kernel "cached"
  view of the same pages reads as slowly (Windows maps them with their write-combined attribute).
- **0154 + sbwddm d85e0d3a:** the window back buffers are cached pages from the start, and their stale lines are
  dropped before each CPU read.
  - KMD: `MW_ALLOC_CPU_CACHED` -> `DXGK_ALLOCATIONINFOFLAGS.Cached` (allowed in a non-coherent segment: VidMm flushes
    when paging); `MW_ESCAPE_SB_CACHE` = DC CIVAC over a byte range through the KMD's cached view (no user-mode DC CIVAC
    on Windows; GD's ICD is emulated x64). Counters Live_CachedAllocs/CacheOps/CacheOpsNotCached/CacheOpFails/
    CacheOpMB/CacheOpMs; `SBW_LIVE_MAX` 192 -> 224.
  - Shim: Panfrost's `PANFROST_BO_WB_MMAP` -> `MW_ALLOC_CPU_CACHED` (refused -> write-combined, logged); DRM `SYNC_BO`
    (inlined ops) -> the escape; an older KMD (no escape) is noticed once and left alone.
  - Panfrost: `PAN_BO_SB_CPU_CACHED` -> `PAN_KMOD_BO_FLAG_WB_MMAP` for DISPLAY_TARGET resources when the ICD asks
    (`glcached` / `SB_GL_CACHED=1`); a READ map of a WB_MMAP BO invalidates the mapped range first (glReadPixels stays
    right); a CPU write is cleaned at unmap; `PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL` on Windows. The BO cache only
    reuses a BO for the same flags.
- **T57** = T55 + this KMD + the 0154 GD driver; A = async, B = async + cached (`glcached`).

### 6.63 T56 read (VIS2): HWND swap chains never show; DComp swap chain present rejected; a NULL-layout draw crash (sb-patches 0155) (2026-11-13)

- **Shots (sbcomp, 1200x700 topmost window):** GDI yellow shows. HWND FLIP_SEQUENTIAL (red) and HWND DISCARD (magenta):
  **the window is absent**, the console behind it shows. sbumd's side is fine: presents at 64/67 fps (Present(1, 0),
  vsync-paced), centre colours right, `pfnPresentCb` succeeds; the flip-model swap chain is emulated by the runtime
  (sbumd stats: "rotate id 638", every present Flags 0x1 = Blt, no destination).
- **DComp swap chain (dcsc):** the first present's `pfnPresentCb(src, dst 0)` fails **0x88760873
  (D3DDDIERR_INVALIDHANDLE)**, then DXGI_ERROR_DEVICE_REMOVED. First explicit error on this path.
- **DComp surface (dcsurf): sbumd crashed**, 0xC0000005 at `panfrost_draw_vbo+0x72c` reading 0x348: `ctx->vertex`
  (offset 0xE060, the vertex-elements state, found by an offsetof probe) was NULL. DirectComposition's internal draw has
  no input layout (IASetInputLayout(NULL), positions from SV_VertexID), which D3D allows; d3d10umd's `update_velems`
  bound nothing. **The sbtex "vertexid" crash is the same address** (T56 sbtex.exe-6256).
- **sbumd's DXGI DDI is 1.0** (Device.cpp fills DXGI_DDI_BASE_FUNCTIONS only; D3D10.0/10.1/11.0 interfaces), while the
  KMD reports WDDM 1.3 without DirectFlip. Working theory: Windows takes the WDDM 1.2+ presentation route for this
  adapter, and a DXGI 1.0 UMD's emulated windowed presents never become something DWM composes (and a composition
  swap chain's present is refused). The ETW traces that would show it need administrator rights (logman: "Access is
  denied" in the non-elevated autotest); a RunOnce-elevated capture is the way.
- **0155 (sbumd 3a59aeec):** `update_velems` binds an empty vertex-elements state when no layout is set. T58 checks it
  (sbtex vertexid should exit 0) and reruns VIS2 so the DComp surface path can be seen.

### 6.64 T57 read: cached back buffers never mapped (IgnoreSync); 0156; T59 staged (2026-11-13)

- **T57 (`build/win/results-t57`): clean, nothing installed.** gputest as T55 (sbfps 662/828, sbglzc zc 179 fps).
  - **GD A** (async, write-combined): ~18.5 fps (181-191 frames per 10 s), copy avg ~46-48 ms, async wait ~27 ms,
    329 MB at the end.
  - **GD B** (async + cached): no frame presented. 16,567 lines `MESA: error: mmap(..., size=17186816, prot=3,
    flags=0x1) failed: Invalid argument`, no glstats line, 685 MB at the end.
  - Hive: Live_CachedAllocs 3 (the KMD made them; no refusal logged), Live_CacheOps 0 (never got that far).
- **Why:** the shim maps a BO by locking its allocation with `IgnoreSync`. D3DDDICB_LOCKFLAGS: IgnoreSync cannot be
  set "with a cached allocation when cache coherency is not supported by the graphics adapter". Our aperture segment
  is not CacheCoherent, so every lock of a cached allocation failed.
- **0156** (shim, `src-tc` only = GD's ICD): a cached BO is locked once right after it is made, without IgnoreSync (no
  GPU work can reference it yet, so the lock does not wait), and stays mapped until GEM_CLOSE like every BO. If that
  lock fails too, the allocation is destroyed and remade write-combined (logged once). Runtime-resource allocations
  never ask for cached pages. GD ICD libgallium_wgl ed17e3b3, opengl32 13d0746d.
- **T59** = T57 + that ICD (`build/win/stage/sbwddm-t59`): B should log `cached allocations on`, no mmap errors, and
  show copy avg well under A's ~46 ms with a correct picture.

### 6.65 T58 read: no-layout crash fixed; a DComp surface reaches the screen, black; T60 staged (2026-11-13)

- **T58 (`build/win/results-t58`; sbumd 3a59aeec = 0155): clean, nothing installed.**
  - `sbtex hw vertexid exit 0` (first pass); every other gputest program exit 0 as T57 (sbtex softpipe `cbindex`
    fails as in every softpipe run). Drag 52.7/53.1 moves/s, sbfps 631/801 and 547/824, sbglzc zc 181 fps: as T57
    within noise (PACE 2.27 ms vs 1.67, a single sample). The only sbcomp WER report is T56's crash (old sbumd).
  - **Install candidate:** 3a59aeec with KMD 79ce848e (`--install-default` of the T58 stage), for the user to decide.
- **VIS2 again:**
  - gdi: shown (yellow). hwnd / hwndblt: 73 / 65 fps, 0 failures, window absent from the shot (console behind shows).
  - dcsc: first Present 0x887a0005 (device removed), as T56.
  - **dcsurf: 571 frames, 0 failures; the shot shows an opaque black 1200x700 rectangle** at the window's place: DWM
    composes the DComp visual. sbumd's own samples of the presented atlas (1216x704, shared, BindFlags 0xa8) are all
    zero, and between presents it saw **3 draws (prim 4, no layout, 96 vertices) and 0 ClearRenderTargetView** calls,
    while sbcomp clears the BeginDraw texture each frame (hwnd mode: 1 clear per frame reaches sbumd).
  - Reading: the runtime does not pass a clear of a DComp atlas surface as a clear. Likely a ClearView emulation
    limited to the update rectangle (sbumd has no D3D11.1 ClearView) or a copy from a scratch texture; whichever,
    its draws write nothing visible.
- **0157** (sbumd fa68467d, dump only): with `umdlog\trace` or `trace-<exe>`, the first 64 draws also print the pixel
  shader, its textures with a centre texel, VS/PS cb0-3 (first 8 floats, buffers referenced only in dump mode), the
  viewport and clears so far. **sb-comp.bat** takes its modes from `sb-comp-modes.txt` and traced modes from
  `sb-comp-trace.txt`.
- **T60** (`build/win/stage/sbwddm-t60`): T58 + fa68467d, VIS2 = `hwnd dcsurf`, dcsurf traced.

### 6.66 Big cores are the GD/fps target; T61 = T59's GD A/B on win11-big (2026-11-13 afternoon)

- **The user:** the big-core entry is their Windows ("I hate the small cores"). T49-T60 all booted `win11-gpu`
  (A55s). From T61 on, GD and fps are measured on `win11-big`.
- **Same install, same driver:** both rows boot `ufs:WINDOWS (NTFS)`. `windows-big3i.uefi` = the win11 row's
  `windows-t32s-nocache.uefi` (SBGPU, USB, WLAN, SMBIOS) with only Star2LteSsdtDxe swapped (docs/WIN11-M3-CLEAN.md
  on the big-core branch, sections 27-32). sbwddm ran in full mode there (big3-1794: WddmMode 1). The installed
  default (T51) is therefore the big-core entry's driver too.
- **What blocked unattended big-core cycles:** only the cap. The big-core module's A55 guard feeds WDT1
  (`WTCNT = WTDAT` every 0.5 s while KUSER InterruptTime advances, M3Early.c v10 free run), so sbwdog stopping its
  own feeding at FullCapSec could not reset a Windows that is alive but never restarted. Covered anyway: gputest's
  own restart, sbwdog's clean restart, bugcheck + AutoReboot, and the guard for a frozen clock. T49-T58's test boots
  all ended in a clean restart.
- **sbwdog 24b4ae14 (T61):** `ArmCount()` (the loader's arm sequence with a raw count; `Arm()` uses it). At the cap,
  on a CPU whose MIDR is not a Cortex-A55, it loads WDT1 with 16 counts (~20 ms). The guard's reload then reloads 16
  too, the same mechanism as the guard's own forced reset. State `BigCore` and `CapForced`. A55 path unchanged.
  starbite/win/build.sh now links sbcl_core.c into sbwdog (T35's ledger; the line had lost it).
- **gpu-cycle:** no change. `--test-key win11-big` passes its catalog check (uefi, wdt 82, wdt_feed own). The clean
  boot stays `win11-auto` (A55; sbxta puts XtaCache back to Start 2).
- **T61** (`build/win/stage/sbwddm-t61`): T59 + sbwdog 24b4ae14:
  `gpu-cycle.sh .../sbwddm-t61 OUT --test-key win11-big --full-sec 2700 --no-trace --keys 1 --afbc 0`.
  It reads out GD A/B fps and copy avg on the M3s against the A55 baseline (~18.5 fps, ~46 ms copy) and the guard's
  `=M3H` temperatures. T59 (the same A/B on the A55s) is optional.

### 6.67 T61 read: GD on the big cores ~2x the A55s; A/B scenes differed; T62 (A B A2 B2) staged

- **T61** (win11-big, 14:26-14:51): exit 0. The test boot ended in gputest's clean restart at 1201 s. sbwdog
  `BigCore 1`, `CapForced 0`; no bugcheck. sbd3d arm64/landscape/x64 exit 0, sbfps flip 649 / blt 829 fps.
- **GD2 [A]** (async present): **34.3 fps** average over 31 glstats rows (copy 14.7 ms, gdi 4.2, present 5.0, interval
  29.5) on the menu with the terms Notice. The A55s gave ~18.5 fps with a ~46 ms copy (T57 A).
- **GD2 [B]** (+ cached back buffers): the 0156 path ran (`cached allocations on (... locked without IgnoreSync)`),
  0 mmap failures. **21.1 fps** (copy **5.1 ms**, map 3.6, gdi 10.7, present 7.8, interval 49.6), but the game was in
  a level (Attempt 32 -> 140, drawn right): the first gameplay frames in a test. Who started the level is unknown
  (stray touches on the big-core row, 6.66's touch report, are one candidate). Two scenes, so the A/B is open; the
  cached copy itself is 3x faster.
- **Thermal:** `=M3H t41 hi51 s0 c2 g0 n6`: peak 81 C, 6 throttles to 1066 MHz (part of A's min 20.3 fps).
- App crashes: only WpnUserService / MoUsoCoreWorker c000001d (winsqlite3, the known big-core gap).
- **T62** (`build/win/stage/sbwddm-t62`): T61 + sb-gputest.cmd `twice` (sb-gd-ab.txt `glcached twice`: A B A2 B2).

### 6.68 Frame-time lows are a first-class metric (0158); T62 carries it (held until after 20:00)

- **The user:** "the frame drops are extreme, like 1% low is ~2 fps". The 10 s averages hid it. T61's per-window
  worst interval (glstats `interval max`): A 24 of 31 windows >= 100 ms (74 .. 181 ms typical, then 264, 395,
  1537 ms); B 39 of 43 >= 100 ms (up to 8381 ms). So a visible hitch about every 10 s, and some multi-second stalls.
- **sb-patches 0158** (`wgl.c`, GD's ICD only; libgallium_wgl b3befb40): present intervals go into 1 ms buckets per
  window and for the run. New lines every 10 s:
  - `gllows` (window) and `gllows run`: 1% low = 1000 / p99 interval, 0.1% low = 1000 / p99.9, plus frames over
    100 ms and over 250 ms;
  - `glworst`: the window's slowest interval, with the game thread's CPU time inside it (GetThreadTimes: CPU ~ wall
    means the game computed, e.g. x64 translation on the M3s without XtaCache; CPU << wall means it waited), the
    process's page faults inside it, and map (GPU wait) / copy / gdi / async wait / present of the frames on both
    sides.
- Thermal stays on the host side: the guard's `=M3H` (peak, throttle count) from zone0.
- T62 (`build/win/stage/sbwddm-t62`) now ships it (A B A2 B2, big cores). The user wants no phone work 15:30-20:00,
  so T62 runs after 20:00.

### 6.69 Why the user's own big-core boot ran GD at ~0.3 fps: test cycles left full mode off

- **User report (19:15):** a normal boot of "Windows 11 (big cores)" ran GD at ~0.3 fps, then froze. T61 measured
  ~34 fps on the same entry.
- **Cause (process, not drivers):** every gpu-cycle test install writes `GuardAutoClear=0` and resets
  `FullGuardBoots=0`. The full-mode test boot counts itself (`FullGuardBoots=1`) and, with auto-clear off, never
  clears it. Every later boot therefore refuses full mode: T61's post hive has `FullGuardBootsAtStart 1,
  FullRefused 1, WddmMode 0`. Only `--install-default` sets `GuardAutoClear=1` / `FullGuardBoots=0` again. The last
  one was T51's (22:24, 11-12), and T52 was the next test cycle (23:42), so every user boot since has been
  display-only (no GPU): GD drew in software. Test cycles also leave `LiveSec/LiveFlushSec 1/1` (a hive flush every
  second) and their own drivers.
- **GD's GL driver** is app-local in the game folder, which `--install-default` never touches. sb-gd.bat removed it
  after each test (no kit marker) or overwrote the user's copy (marker).
- **Fix:** `--install-default` with T61's stage plus the four blt switches every test ran with
  (`--scan-flip 1 --cached-blt 1 --parallel-blt 1 --blt-no-drain 1`; install-default defaults them to 0), then
  `tools/win/gd-kit-push.sh STAGE/gputest` (new: the GD driver into the game folder with the kit marker, read back,
  refuses a foreign opengl32.dll; `--show` is read-only). After every test cycle, chain both again.
- sb-gd.bat (T62): the user's GD driver copy is kept aside (`*.user`) and put back after the test.

### 6.70 What the user feels: user-session pull and T63 (a measured user-style session on the big cores)

- **User report after the 6.69 fix** (big cores, 19:2x-19:5x): GD Stereo Madness ~15 fps, Grief ~3 fps; "Windows is
  extremely slow"; after quitting GD the shutdown took over 5 minutes.
- **tools/win/user-pull.sh OUT** (read-only, from TWRP, before any cycle: installs empty umdlog) and
  **tools/win/user-report.py OUT**: GD frame times per log (fps, windows with a frame >= 100 / 500 ms, the worst
  intervals; 0158's gllows/glworst when present), sbwddm/sbwdog/sbthermal/sbxta state, boots and shutdowns
  (System 12/13/41/1074/6005/6006/6008), service trouble (7000-7043), Diagnostics-Performance 100-110 / 200-207
  (boot and shutdown durations, the services and apps that delayed them), app crashes and hangs (1000/1002).
- **T63** (`build/win/stage/sbwddm-t63`), T61's drivers plus the 0158 GD driver. gputest holds only
  `sb-user.bat`; sb-gputest.cmd hands over to it at logon. The session:
  - 180 s idle with typeperf every 5 s (`sb-perf-counters.txt`: CPU per core, DPC/interrupt, queue, memory,
    paging, disk, per process);
  - GD via Steam, with clicks from `sb-click.ps1` (DPI-aware mouse_event: terms OK, Play, the first level), then
    360 s;
  - WM_CLOSE, timed;
  - 120 s idle;
  - a timed `shutdown /r /t 0`.
  Run with `live_sec=10 live_flush=60` in the environment (gpu-cycle takes those from it), so the driver logs at
  the user's rate.
- **Suspects on paper** for the slowness: the winsqlite3 crash loops on M3 boots (WpnUserService, DiagTrack,
  OneSync, UsoSvc: each crash also starts WER); x64 Steam + steamwebhelper translated from scratch on every
  start (no XtaCache on the M3); memory (4 GB, GD ~700 MB private); the guard's throttle to 1066 MHz at 80 C.
  The 5-minute shutdown points at a service that would not stop. 7043 / 203 events will name it.

### 6.71 T64: a translation cache of their own for the big cores (sbxta's SwapCache mode)

- **Why.** Steam and GD are x64. On the big-core row the XtaCache service has been off since 2026-11-11 (sbxta.sys
  42056cc, `DisableOnM3` default 1): the shared cache holds translations made on the A55 (LSE, LDAPR) that fault on
  the ARMv8.0 M3. So every Steam/GD start on the big cores translates from scratch, and code first reached mid-level
  is translated on the game's thread: a suspect for slow starts and for hitches (0158's `glworst` game-thread CPU).
- **The mode was already there, unused.** sbxta.sys has `Parameters\SwapCache` (renames at system start,
  same-directory, ReplaceIfExists FALSE): M3 boot `XtaCache -> XtaCache.a55`, `XtaCache.m3 -> XtaCache`; A55 boot
  the reverse. With `DisableOnM3=0` the service runs on the M3 row too and fills the M3's own folder (xtajit's
  feature latch is off on the M3, so those translations are ARMv8.0). Registry only; no new binary.
- **gpu-cycle.sh `--sbxta K=V[,K=V]`** (any installing mode): DWORDs into `Services\sbxta\Parameters`, refused when
  sbxta is not installed; rehearsed on T61's post hive (set and back).
- **sb-user.bat** logs sbxta `State\Result`/`Swap`, the XtaCache service state and the cache folders at start and
  before the restart, and "steam.exe up after N s".
- **T64** (`sbwddm-t64` = T63 + that sb-user.bat): T64a cold, T64b warm, both `--sbxta DisableOnM3=0,SwapCache=1`;
  restore with `--sbxta DisableOnM3=1,SwapCache=0`. Compare start times and hitches with T63.

### 6.72 The user's session and T63 read

- **The user's post-fix session (19:21-19:40) ran on the A55 row** (sbxta/sbthermal/sbwdog all A55; the only Windows
  boot between the 19:18 install-default and the night pull). GD at 2960x1440: copy 61 ms/frame uncached on the A55s
  (T61 on the M3s: 14.5 ms, cached 5 ms) -> ~15 fps. Windows' clock on these boots runs 2 h behind.
- **Their "5-minute shutdown" is not in Windows:** T63's boot logged Diagnostics-Performance 200 for it:
  ShutdownTime 34.5 s (user session 20.1 s, services 5.0 s, kernel 3.0 s); System 1074 -> 6006 +28 s -> 109 +4 s ->
  13 +2 s; sbwdog PrevBootText "clean shutdown or restart after 1093 s". (The first pull missed 109/13: not yet
  flushed into the copy.) The wait must be after Windows (loader / next OS) or before the restart request.
- **T63 (big cores, USR1):** ran in full (20 min, own restart, clean). GD up 5 s after the Steam URL (Steam already
  running from logon). Clicks reached the level select, not the level (Play applied > 6 s later; the card click
  landed in the page change): fixed for T64 (wait 15 s, card centre 50%/31% twice). Menus/level select: 40.7 fps avg,
  1% low 19.2, 0.1% low 4.8, 39 frames > 100 ms; the 3 worst (11.1, 6.6, 6.6 s) are the game thread busy
  (10.1/6.3/6.0 s CPU, 60-100k page faults): loading/translation, T64's target. Close: WM_CLOSE, gone in 4 s.
- **The idle desktop on the big cores is busy:** 80 % CPU on all 3 cores, run queue 4.8, 360 MB available, 2200
  pages/s, disk 440 % busy. Top: steamwebhelper 40 %, System 35 %, svchost (the c000001d loops + WerFault),
  MsMpEng 16 %, OneDriveSetup 10 %. Under GD (300 % = 3 cores): GD 142 %, dwm 25, MsMpEng 24, System 24, sblinkd
  15-28 %. Commit 3.75 GB idle -> 4.65 GB with GD (4 GB RAM).

### 6.73 T62 read (cached buffers: copy 3x faster, no fps gain) and T65 staged: the GD GL driver as ARM64EC

- **T62 (big cores, menus, glcached A/B twice):** A (cached off) 34-36 fps, copy 14.7 ms; B and B2 (cached on)
  31.4-33.5 fps, copy 5.7 ms, map 3.4 ms (cache maintenance). gllows: A 31.9 fps avg, 1% low 17.5; B 30.2 / 14.9;
  B2 29.2 / 14.1. **A2 is void:** B's "t+5: gone" was a false reading, GD (pid 2472, cached on) ran on for 656 s,
  A2's Steam URL only focused it. With the async present the copy is off the game thread, so a faster copy can't
  raise fps: the game thread's ~28 ms/frame is ~5 ms GPU waits (1 submit + 1 blocking wait per frame), ~2.6 ms
  present, ~20 ms game + driver CPU.
- **Both GD and its GL driver are x64,** so all of Mesa/Panfrost's CPU work is translated by xtajit64 (and on the
  M3 row without a translation cache). The ARM64EC build (6.4x of WIN11-GPU-CAPS.md: built 11-11, never run in GD)
  runs the driver natively inside the x64 process.
- **build-tc-ec** (`~/sbmesa/tc-ec-build.sh`, clang/lld 20.1.2, cross-ec.ini) from src-tc = 0158 **+ 0140** (applied
  to src-tc now; without it util/format took the MMX/SSE paths and failed): opengl32-gd-ec.dll bc64cc58,
  libgallium_wgl-gd-ec.dll bc2b9626 (build/win/gd-ec-0158). CHPE v2: 8.84 MB ARM64EC, 4.9 KB AMD64 thunks; the
  present copy compiles to its NEON path.
- **sb-gd.bat (T65):** flag file `glec` selects `*-gd-ec.dll`; clicks into Stereo Madness; a "gone" is re-checked
  after 3 s; a game still running at a run's end is killed before the files are put back.
- **T65** (`sbwddm-t65` = T62 + those, sb-gd-ab.txt `glec twice`): A/A2 x64, B/B2 ARM64EC, big cores.

### 6.74 T60 read: the DComp surface's clear arrives as a draw that writes nothing

- All regular tests pass (sbtwo, sbtext, sbtex, triangle, sbdrag). VIS2 hwnd: the app's clear arrives (1 clear per
  frame, 0xff00008c), the window is not composed (as T58). VIS2 dcsurf, per frame into the 1216x704 atlas:
  1. **the clear, emulated by the runtime:** a 4-vertex triangle strip, no input layout, no VS constant buffer, PS cb0
     = (0, 0, 0.55, 1) = sbcomp's colour 0,0,140; viewport 1200x700; scissor (0,0)-(1201,701);
  2. 96 vertices from the atlas into a 64x320 texture; 3. 96 vertices from it back into the atlas.
  Every present's samples are 0x00000000 and both textures' centres stay 0: draw 1 writes nothing, so 2 and 3 copy
  nothing. Scissor and viewport cover the window; predication is set and off.
- **Next (zero-copy track, after the GD work):** dump draw 1's VS/PS TGSI (where its positions come from: SV_VertexID
  alone, or a VS buffer that is not bound) and its blend/rasterizer/depth state; compare with sbtex's passing
  vertexid test.

### 6.75 T64 read: no M3 cache was made (the service needs its folder); GD in the level at ~33-37 fps

- **The swap worked, the cache did not start:** M3 boots: `to M3: XtaCache->.a55 0x0, .m3->XtaCache 0x0` (the
  second only as "not tried": there was no XtaCache.m3 yet), A55 clean boots: `to A55: XtaCache->.m3 0x0,
  .a55->XtaCache 0x0` (the first likewise "not tried"). On both M3 boots XtaCache (LocalSystem, LaunchProtected 2,
  ServiceSidType 1) stopped with **7023 ERROR_FILE_NOT_FOUND**: it does not create C:\Windows\XtaCache. So T64a and
  T64b both ran without a cache; T64 = two more T63-style sessions, now in the level.
- **Fix (not staged):** an empty `C:\Windows\XtaCache.m3` with the security descriptor of the A55's XtaCache
  folder (ntfs-3g `system.ntfs_acl` copy from TWRP, or sbxta creating it with that descriptor). Value is limited:
  Steam is already running at logon (GD up 5 s after the URL); the cache would mostly cut GD's one-time 6-12 s loading
  stalls (game thread 6-10 s CPU, 60-100k page faults) at each start.
- **GD in Stereo Madness (clicks fixed; screenshots in the level, GD's counter 37):** T64b 35.4 fps avg over 10 s
  windows, gllows 33.0 fps, 1% low 18.2, 0.1% low 6.2, 20 frames > 100 ms (T63 on menus: 39); copy 5-7 ms.
- **Big-core crash loops cost more than the crashing services:** each boot 25 WpnUserService, 8 MoUsoCoreWorker,
  7 OneSyncSvc, 6 DiagTrack, 6 PushToInstall c000001d crashes, and 7031/7034 for the services sharing their svchost
  (WpnService, PushToInstall, Task Scheduler, Themes, WMI, Group Policy, IP Helper, Server, SENS, Shell Hardware
  Detection, UsoSvc, User Manager, Application Information, Web Account Manager): on 4 GB the services are grouped,
  so one c000001d restarts the whole netsvcs host. The service list for sbxta v2 (user's decision) should then be
  WpnService, WpnUserService, PushToInstall, DiagTrack, OneSyncSvc, CDPUserSvc, UsoSvc, wuauserv.

### 6.76 T65 read: the ARM64EC GD driver is ~7 % faster, no crash; user kit gd-kit-ec-0158

- **One big-core boot, Stereo Madness, A B A2 B2** (test logging rate LiveSec 1, so all four run a bit lower than
  T64's ~35 fps): steady 10 s windows x64 A 28.6 / A2 28.4 fps, ARM64EC B 30.5 / B2 30.7; gllows run x64 26.7 /
  26.5 fps (1% low 13.7 / 15.2, > 100 ms 66 / 21), EC 28.8 / 29.3 (1% low 15.6 / 15.6, > 100 ms 29 / 30). Interval
  ~33 ms EC vs ~35 ms x64; copy ~17 ms both (uncached), ~5.5 ms GPU waits per frame both. GD up 5-10 s; no
  exception in any GD log; the only Application crashes are the known winsqlite3 c000001d services.
- **So the driver's own CPU was ~2 ms of the ~35 ms frame** after translation; the rest is the game (x64,
  translated) and per-frame waits. Next levers: the per-frame blocking wait (1 submit + 1 wait per frame), the
  present (7-9 ms in this boot), and the game's own translated CPU (XtaCache for the M3, 6.75).
- **Kit:** `build/win/gd-kit-ec-0158/` (opengl32-gd.dll = EC bc64cc58, libgallium_wgl-gd.dll = EC bc2b9626) for
  `tools/win/gd-kit-push.sh`; proposed as the user's GD driver in place of the x64 b3befb40/ed17e3b3.

### 6.77 debloat-1: one step for a lighter big-core Windows (the user's yes pending), with undo

- **The user's "5-minute shutdown"** (their words, via the peer): the desktop was too slow to reach the Start menu
  for ~5 min, then they shut down from a terminal; the shutdown itself was quick (34.5 s, 6.72). That session was
  on the A55 row; T63/T64 show the big-core desktop overloaded the same way (6.72, 6.75).
- **`tools/win/debloat.sh apply|undo|v1|show OUT`** (TWRP, no boot): sbxta v2 (6328cd28 source brought onto this
  branch with its README and hive tool; built 6328cd2, b304123f, signed, 0 LSE/LDAPR) + DisableOnM3Services
  WpnService WpnUserService PushToInstall DiagTrack OneSyncSvc CDPUserSvc UsoSvc wuauserv; HKLM RunOnce
  StarbiteDebloat -> `starbite/win/debloat/debloat-online.cmd` (HKCU Run/RunOnce exported, Steam and OneDrive*
  values removed; elevated: Defender exclusion for the Steam folder); GD kit = gd-kit-ec-0158. Undo: list off
  (the next A55 boot restores sbxta\Saved), RunOnce StarbiteUndebloat (re-import, exclusion removed), x64 kit.
  Rehearsed offline on the user's SYSTEM copy (set, param, off). The SOFTWARE edit is hive-runonce.py, used for
  c-clean-win on 11-12 (ran elevated).
- **Measure after apply (T66):** sbwddm-t64 again on win11-big, compare with T63.
- **Lost on the big-core row only:** notifications, mail/calendar sync, Connected Devices, telemetry, Windows
  Update, Store installs; both rows: Steam/OneDrive no longer start at logon, Steam's folder not scanned on access.
- **Note:** build/win/results-t60 ... results-t65 were moved (verified copies) to
  /Volumes/SBWORK/starbite-build-archive/win-1114/ in the 11-14 disk emergency; t49-t58 and old stages are in
  .../win-1113/.

### 6.78 touch-1: the split drags (touchdiag-2) and the late taps (press-and-hold off)

- **The user (11-14, big cores, after debloat-1):** GD ~30 fps, uneven, taps ~1 s late; drags "always" split into
  several press/release pairs and jump to other points; after quitting GD the taskbar and wallpaper stayed black for
  minutes.
- **Drags:** the installed touch driver is still the DriverStore S6SY761Touch (user hive 11-13: ImagePath unchanged,
  no Diag* values): the big-core session's touchdiag-2 (cbd9f5a, 070dad9a, staged 11-11) never ran. Its root-cause
  analysis (notes win-m3-clean-touch-rootcause.md): the driver turns ONE failed touch I2C read into "all fingers up"
  (Device.c 2771/2804), and the still-held finger's next MOVE becomes a new down; Starbite's own touch code retries
  3x. touchdiag-2 = the same driver + 3 tries 0.5 ms apart + Diag* counters and a press/release/all-up trace, so the
  next drag either stops splitting (DiagI2cRetryOk > 0) or shows which of the other causes it is (chip releasing,
  queue flush, Windows-side).
- **Taps:** GD is not touch-aware, so Windows promotes taps to mouse clicks and, with "press and hold for right-click"
  on, holds the click back until it knows the touch is not a hold. touch-online.cmd sets HKCU Wisp\Touch
  TouchMode_hold = 0 (old value exported; untouch-online.cmd restores it). From the next sign-in.
- **`tools/win/touch.sh apply|restore|show OUT`** (TWRP; hive-touchdiag.py + hive-runonce.py, rehearsed set/restore on
  the user's SYSTEM copy); then one unattended small-core boot `gpu-cycle.sh --measure sbwddm-t61` proves the driver
  loads (DiagBuild, TouchD0Status 0, DiagSelf) before the user relies on touch; then the usual restore.
- Source and tools brought onto this branch from claude/win-m3-clean-947e73 7d67410d: starbite/win/s6touchdiag,
  tools/woa/win-m3/hive-touchdiag.py.

### 6.79 The user's session after debloat-1 (big cores, 11-14 ~07:37-07:50 local): the loops are gone; sihost aborted

- **debloat-1 works:** sbxta `6328cd2 v2`, State\Svc `M3: WpnService 2->4 ok; WpnUserService(_418b9) ... wuauserv 3->4
  ok`, Saved holds the originals. In that boot: **0 service crashes and 0 7031/7034** (T63/T64: ~50 crashes and
  ~20 group restarts per boot). One 7040: wuauserv disabled -> demand start at 05:46:51 (Windows' own update repair;
  sbxta leaves a re-enabled service alone on the A55 row, harmless).
- **GD (ARM64EC kit):** 38.6 fps avg over 10 s windows, gllows 33.6 fps, 1% low 14.7, 24 frames > 100 ms in 230 s;
  the 3 worst (9.1, 7.0, 5.3 s) are again game-thread CPU with 47-100k page faults (loading/translation).
- **The black taskbar/wallpaper:** sihost.exe (the shell infrastructure host) failed fast at 05:47:51 (ucrtbase
  c0000409, code 7 = abort()); a new explorer.exe started 05:48:39. Before it: 05:45:08 DCOM 10010,
  ShellExperienceHost did not register in time. sihost has no sbumd log (it did not open a D3D device through us);
  DWM/Explorer/GD logs show 0 failed allocations; no Resource-Exhaustion event. Cause unknown; Edge also aborts at
  start (3x c0000409, code 5).
- **Next data:** touch.sh apply now also sets WER LocalDumps (full, 2 each, C:\sbdumps) for sihost.exe,
  explorer.exe and ShellExperienceHost.exe (hive-localdumps.py from 7d67410d, rehearsed on the SOFTWARE backup);
  user-pull.sh pulls the shell/Edge/DWM Report.wer files and the 4 newest C:\sbdumps dumps.

### 6.80 touch-1 check boot: the retry works; the 250 ms read timeouts remain -> touchdiag-3 (25 ms)

- **Check boot (A55, unattended, 10:0x):** PASS: DiagBuild `s6touchdiag cbd9f5a`, TouchD0Status 0. Someone touched
  the phone during it, so there is a real drag: 1 press, 511 moves, 1 release over 4.3 s, and inside it 2 reads
  that never completed: status 0xC00000B5 (IO_TIMEOUT) after the full 250 ms (DiagSpbMaxUs 252348), each followed by a
  successful retry (DiagI2cRetry 2, DiagI2cRetryOk 2) and a "266 ms without an event" gap. With the old driver each
  would have been an all-up = the drag split in 3 (the user's symptom), on the small cores too. Self-test: back to
  back 1/200 reads timed out (250 ms), 200 us apart 0/200 (avg 424 us, max 489 us): mechanism 4 of the root-cause
  note (a sequence whose completion never arrives), more likely with sequences back to back.
- **touchdiag-3** (`build/win/touch-3/s6touchdiag.sys` 2529a7cf, build td3-25ms, signed, 0 LSE words): the
  touch-event reads (TouchReadWithRetry) use their own 25 ms timeout (SBTOUCH_READ_TIMEOUT_MS; ~50x a good read);
  D0 entry, commands and the self-test keep 250 ms; DiagReadTimeoutMs records it. A stuck read now costs ~25 ms.
  The same pipeline builds touchdiag-2's source to the same size (40448 B; the archived 11-11 file was 37888 B).
- **`touch.sh update OUT`:** pushes only the touchdiag-3 file (service must already use s6touchdiag.sys); then the
  same check boot (PASS = DiagBuild `s6touchdiag td3-25ms`, TouchD0Status 0).
- The real fix of the cause is in the HSI2C controller driver (root-cause note: completion without waiting for the
  bus to go idle; re-poll INT_STATUS before re-enabling), not ours yet.

### 6.81 Landscape the right way round by default (the user, 11-14)

- **The user:** "i want windows to be in landscape and not landscape flipped by default".
- **Where it lives:** Windows keeps the orientation per monitor in `Control\GraphicsDrivers\Configuration\<set>\00\00`
  `Rotation` (D3DKMDT_VIDPN_PRESENT_PATH_ROTATION) and applies it at every start; sbwddm only offers all four
  (Parameters\Rotation 1). The user's hive (11-14 session): our panel's set `NOEDID_42525453_32373030_...` has
  Rotation **2** (90) = what the user calls landscape flipped; sbwddm State\Rotation 2 agrees. The other landscape
  is **4** (270). (The MSNIL* set is Basic Display's, portrait, left alone.)
- **`tools/win/hive-orientation.py IN show | IN OUT set N`:** sets Rotation in every saved path target of our panel;
  rehearsed on the user's SYSTEM copy: only that value changes. **`touch.sh orient OUT 4`** applies it from TWRP
  (backup, clean-hive check, read-back); run last, after the touch-3 check boot and restore.
- Touch follows: Windows turns the digitizer's coordinates with the display orientation (as it does now at 90).
  If the user ever picks another orientation in Settings, Windows saves that instead (their choice wins).

### 6.82 touchdiag-2 on the big cores (the user, 11-14): ~10 % of touch reads hang there -> touchdiag-3 = 25 ms x 5 tries

- **The user:** "touch is just as bad, lots of taps and selection box starting at new places while dragging"; Steam +
  GD took 5+ minutes to come up.
- **session2 (big-core boot, touchdiag-2):** DiagSelf back to back **15/200** reads timed out, 200 us apart **22/200**
  (avg 19 / 31 ms, max 0.25 / 0.80 s; A55 check boot: 1/200 and 0/200). In the drags: DiagI2cRetry 67, RetryOk 37,
  DiagI2cError 11 (all 3 tries failed -> ALL-UP), DiagSpbSlow 32, DiagGap100 22 (max 17.8 s idle aside, 0.5-1.1 s
  inside drags), DiagEventsMax **64** = the chip's event stack ran full during those gaps, and the trace shows the
  backlog coming out as release/press pairs at nearly the same spot within 0-3 ms (e.g. 122915 ms: release, press,
  release after a 1145 ms gap), plus press-while-active after each gap. So on the M3 row the retry alone cannot keep
  up: each hung read costs 250 ms, two or three in a row end the contact or let the chip's buffer overflow.
- **touchdiag-3 (rebuilt):** touch-event reads 25 ms timeout (SBTOUCH_READ_TIMEOUT_MS) and **5 tries**
  (SBTOUCH_TRIES; DiagTries records it): an all-up needs 5 hung reads in a row (~1e-5 at 10 %), the worst stall is
  ~125 ms, so the chip's 64-event stack (~0.5 s of moves) no longer fills. build td3-25ms-5x, fa9f2a52, signed,
  0 LSE words. `touch.sh update OUT` (check-boot PASS string `td3-25ms`).
- **The cause underneath** is the HSI2C controller driver on the M3 row (a sequence whose completion never arrives,
  ~10 % there vs ~0.5 % on the A55s; root-cause note mechanism 4/1). Fixing that driver (complete after MASTER_BUSY
  clears / re-poll INT_STATUS before re-enabling) is the real cure; touchdiag-3 makes it tolerable meanwhile.
- **Steam 5+ min:** debloat-1 removed Steam's autostart; a cold Steam start on the M3 row (no XtaCache there, 6.75)
  translates Steam's Chromium helper from scratch (T63: steamwebhelper 40 % CPU for minutes after logon). Option:
  `touch.sh steam OUT` (RunOnce StarbiteSteamOn re-adds `"...\steam.exe" -silent` to the user's Run key); the
  lasting fix is the M3 translation cache (6.75: XtaCache needs its folder).

### 6.83 The hung touch reads' cause: the HSI2C interrupt handler clears bits it never processes (sbhsi2c-1)

- **Source:** ref/exynos9810-woa/drivers/Exynos9810Hsi2c (MS-PL; the installed driver's source per its README),
  copied to `starbite/win/sbhsi2c/`. HSI2C10 serves only the touchscreen (ACPI\SAMS9810).
- **The bug:** `OnInterruptIsr` reads `INT_STATUS & mask` into `interruptStatus`, disables interrupts, then calls
  `ControllerAcknowledgeInterrupts`, which **reads INT_STATUS again** and writes back (clears) everything set by then;
  only the first read is ORed into `device->InterruptStatus` for the DPC. A bit that arrives between the two reads is
  cleared and lost. In a touch read (1-byte write + 8-byte read) the DPC drains the FIFO on RX_ALMOST_FULL and then
  waits for TRANSFER_DONE (`ControllerProcessInterrupts`); a TRANSFER_DONE landing in that window is never seen, the
  sequence never completes and the touch driver's timeout cancels it (I2cLastRequestStatus 0xC0000120 CANCELLED in
  the user's session2 hive; the driver's own watchdog count 0). Timing-dependent, hence far more often on the M3s.
- **Fix:** acknowledge exactly the captured bits (`ControllerAcknowledgeExact`, write-1-to-clear, no re-read); a later
  bit stays pending and raises the interrupt again when the DPC re-enables it. Nothing else changed.
- **Build:** `tools/win/build.sh kmdf` of driver/device/controller.cpp with SpbCx 1.0 headers + spbcxstubs.lib and WPP
  stubbed (`sb_wpp_stub.h` + three `.tmh` stand-ins; the trace calls compile to nothing): `build/win/hsi2c-1/sbhsi2c.sys`
  8489ac43 (build sbhsi2c-1), signed, 0 LSE words, imports ntoskrnl/HAL/WDFLDR (SpbCx binds through WDF).
- **Install:** `touch.sh hsi2c OUT` = `tools/win/hive-imagepath.py` set Exynos9810Hsi2c -> sbhsi2c.sys (original in
  Parameters\SbOrigImagePath; set/restore rehearsed on the user's session2 SYSTEM: restore leaves the key identical);
  `touch.sh hsi2c-restore OUT`. Check boot PASS: a new DiagClock line, TouchD0Status 0, DiagSelf 200 reads per pass
  (errors should drop to ~0 on both passes).

### 6.84 touch-4 check: the exact-ack fix alone did not remove the hangs; sbhsi2c-2 (poll rescue + cancel snapshot)

- **touch-4 check boot (A55, touchdiag-3 + sbhsi2c-1):** PASS (td3-25ms-5x, TouchD0Status 0, fresh DiagClock), but
  DiagSelf 2/200 back to back and 2/200 spaced timed out (250 ms each; the self-test keeps 250 ms on purpose), vs
  1/200 + 0/200 before. Small numbers, but no sign the exact-ack fix removed the cause.
- **The HSI2C interrupt is EDGE-triggered** in the DSDT (`Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive)`,
  Device I2C0 SAMS9810): a firing the ISR does not claim, or a condition that rises while the line is already high,
  never comes back; the controller just sits with INT_STATUS & INT_EN set until the client cancels. I found no such
  path by reading (every re-enable is a 0->1 of INT_EN), so: measure and rescue at once.
- **sbhsi2c-2** (a8c74b93, build sbhsi2c-2, signed, 0 LSE): sbhsi2c-1 +
  - `OnPollTimer`: a one-shot high-resolution 2 ms timer armed when a transfer starts and re-armed while one is
    active; if `INT_STATUS & mask` has bits and the ISR has not taken them (`InterruptStatus` 0), it does what the ISR
    does (disable, exact-ack, OR into InterruptStatus) and runs the DPC routine; counted in **I2cPollRescue**. A lost
    edge then costs ~2 ms instead of the client's timeout. Stopped in ControllerCompleteTransfer.
  - `OnCancel`: before the abort, INT_STATUS, INT_EN, TRANS_STATUS, FIFO_STATUS, the software mask and InterruptStatus
    into **I2cCancelRegs**; **I2cCancelCount**; **I2cCancelPending** counts cancels with `INT_STATUS & INT_EN` set (=
    a condition nobody serviced). I2cTelemetryVersion 4.
  - Reading the next pull: I2cPollRescue > 0 and the hangs gone = lost edges, fixed; I2cCancelCount > 0 with
    I2cCancelPending 0 and TRANS_STATUS busy = the bus/chip itself stalls (then the touch chip side is next).
- `touch.sh hsi2c OUT 2` installs it (ImagePath already set by touch-4: only the file and the read-back change).

### 6.84b The user's touch-4 session (big cores, 10:4x): drags whole, the hangs remain

- session3 hive (BigCore 3, M3): td3-25ms-5x + sbhsi2c-1. **DiagI2cError 0** (no all-up in 41 presses / 2099 moves),
  DiagI2cRetry 260, RetryOk 214 (the rest needed a 3rd+ try), DiagSpbMaxUs 108.7 ms (= 4 x 25 ms + retries),
  DiagEventsMax 14 (the chip's buffer never filled; touchdiag-2: 64), DiagGapMaxMs 1016 (one gap, idle or a stall).
  The user: "drags are fine on the desktop", one fluke.
- But DiagSelf on the M3 still **15/200** back to back (2/200 spaced): sbhsi2c-1's exact acknowledge did not change
  the hang rate. I2cTelemetryVersion 3 (sbhsi2c-1), LastRequestStatus CANCELLED, ISR 5301 vs 4808 transfers.
  touch-5 (sbhsi2c-2: poll rescue + cancel snapshot) is the next step to tell a lost edge from a real bus stall.

### 6.88 touch-5 check: lost interrupts exist (2 rescued); the remaining hang = a transfer done but no TRANSFER_DONE

- **touch-5 check boot (A55, td3 + sbhsi2c-2):** PASS (I2cTelemetryVersion 4, D0 0, fresh clock). DiagSelf 1/200
  back to back, 0/200 spaced. **I2cPollRescue 2** (DPC 241 vs ISR 239): two interrupt conditions sat unserviced and
  the poll took them. **I2cCancelCount 1, I2cCancelPending 0**, I2cCancelRegs `is=00000000 ie=00000F80 ts=00080001
  fifo=01000100 mask=00000F80 soft=00000000`: nothing pending, TRANS_STATUS = the same value as right after reset
  (not MASTER_BUSY), both FIFOs empty, and the software mask without the data interrupt = every byte of the current
  transfer had moved. So the transfer finished on the bus and TRANSFER_DONE never showed (or was cleared unseen).
- **sbhsi2c-3** (5d976386, signed, 0 LSE): sbhsi2c-2 + in the 2 ms poll: if the active transfer has no bytes left,
  nothing is pending, the ISR holds nothing and TRANS_STATUS is not MASTER_BUSY on **two consecutive polls** (>= 4 ms;
  a touch transfer takes < 0.5 ms), it is completed as if TRANSFER_DONE had come (**I2cPollSynthDone**). If the bus had
  not really finished, the next phase fails with a NACK and the touch driver retries (25 ms x 5). I2cTelemetryVersion 5.
  `touch.sh hsi2c OUT 3`.

### 6.88b touch-6 check (sbhsi2c-3): no hung read left

- Check boot (A55, 15:4x): PASS, I2cTelemetryVersion 5. **DiagSelf 0/200 back to back and 0/200 spaced** (max 2.4 ms /
  5.4 ms, avg 421 / 447 us; before: 1-2 per 200, each 250 ms). **I2cPollRescue 1, I2cPollSynthDone 1, I2cCancelCount 0**:
  one lost interrupt and one finished-but-unreported transfer were each completed by the poll within a few ms, and no
  read had to be cancelled. ISR 776 / DPC 778 / transfers 772.
- Still to see: the big-core row under the user's drags (the M3 hang rate was ~10x the A55's): DiagI2cRetry should
  drop to ~0, I2cPollRescue/SynthDone carry the rescues. touchdiag-3's 25 ms x 5 stays as the safety net.

### 6.88c The user's big-core session with touch-6: "basically fixed"; the chip ends a contact after ~24 s

- session4 (BigCore 3, td3-25ms-5x + sbhsi2c-3): **I2cPollRescue 644, I2cPollSynthDone 255** over 12473 transfers
  (~6200 reads): on the M3 roughly 1 transfer in 7 needed the poll, and all completed within a few ms.
  DiagI2cRetry 9 (all ok), DiagI2cError 0, DiagSpbMaxUs 54 ms, DiagEventsMax 13, DiagSelf 0/200 + 0/200 (max 5.9 /
  6.1 ms). I2cCancelCount 9 (= the 9 retries' cancelled first tries; one with soft=0x80: a TRANSFER_DONE taken by the
  ISR but not yet processed when the client's 25 ms ran out).
- **The drag that let go:** two long drags end in a chip RELEASE after 24.04 s (71375 -> 95415 ms) and 24.20 s
  (96074 -> 120270 ms); the next press is 0.66 s later at another place (the user pressing again). No all-up, no
  gap before them. A cutoff at the same ~24 s twice = the touch controller's firmware ending a long contact (Samsung
  touch ICs re-baseline / drop long-held touches), not our drivers. Fix candidates: the chip's own settings (the Android
  sec_ts driver's commands; not in the repo) or, in s6touchdiag, re-sending a contact the chip drops while it keeps
  reporting moves (it doesn't here: the finger simply vanishes). Needs the chip documentation; parked.

### 6.85 Zero-copy on screen is the top track again: T66 (VIS3) = VIS2's ETW, finally elevated

- **The user (11-14):** touch-4 works ("drags are fine"); GD ~30 fps, taps less late but still late; "wheres the work
  on the zero copy ... which pushes 300". Honest scope: zero-copy removes the frame copy + GDI hop (sbglzc 51 -> 149
  fps on the A55s, a light program), but GD on the M3s is bound by its own translated game-thread CPU (~20 of ~28-33
  ms per frame, 6.73/6.76): there it would mainly cut latency and the copy's memory traffic, not triple fps.
- **Where it stands:** windowed swap chains (flip and blt model) draw right; sbumd's `pfnPresentCb(src, dst 0)`
  returns S_OK; the KMD never gets a present for them; DWM never opens their buffers (6.59/6.61/6.63); DComp swap chain
  first Present -> INVALIDHANDLE; DComp surface composes but its runtime-emulated clear writes nothing (6.74). The
  missing evidence is what dxgkrnl does with the windowed present: VIS2 has had an ETW session for that since T56, but
  `logman` needs administrator rights and the autotest is not elevated.
- **T66 (`sbwddm-t66`, T61 drivers):** `tools/win/vis-arm.sh OUT` sets HKLM RunOnce StarbiteVis ->
  `C:\starbite\vis\sb-vis-elev.cmd` (`starbite/win/autotest/`), which waits for the gputest files and runs
  `sb-comp.bat` (modes hwnd hwndblt dcsc dcsurf) elevated; the stage's `sb-user.bat` (`sb-vis3-wait.bat`) waits for
  `vis-elev.done` and restarts. Output: `pres-<mode>.csv` (tracerpt) per mode in umdlog.
- **GD tap latency now (one line):** no longer Windows' press-and-hold wait; what is left is frames in flight at ~30
  fps: GD reads input once per frame (~33 ms), the async present shows frame N while N+1 is drawn (+1 frame), and the
  GDI/redirection bitmap reaches the panel at DWM's next composition (+~16-33 ms): ~80-120 ms, all of it shrinking
  with frame time (and zero-copy would drop the copy/GDI hop).

### 6.87 T67 (XTA2): the big cores' own translation cache, with the folder the service needs

- **Why now:** the user's Steam + GD took 5+ min on the big cores: debloat-1 took Steam out of autostart, and a cold
  Steam start on the M3 row translates everything from scratch (no XtaCache there since 11-11; T64's SwapCache try
  failed with 7023 because C:\Windows\XtaCache did not exist on the M3 boot).
- **`starbite/win/debloat/xta-m3-folder.cmd`** (HKLM RunOnce StarbiteXtaM3, elevated): on a big-core boot in SwapCache
  mode (XtaCache.a55 present, XtaCache missing) `mkdir C:\Windows\XtaCache`, copy the parked folder's access rules
  (`SetSecurityDescriptorSddlForm(src.Sddl, 'Access')`; inherited SYSTEM-full rules if that fails), `sc start XtaCache`,
  log to `C:\ProgramData\Starbite\debloat\xta.log`; on any other boot re-arm itself. After that, sbxta's renames keep
  the M3's folder as XtaCache.m3 on small-core boots and bring it back on big-core boots (6.71).
- **`tools/win/runonce-arm.sh NAME SCRIPT WINDIR OUT`:** the general "push a .cmd + HKLM RunOnce" step (vis-arm.sh is
  the T66 special case). **user-pull.sh** now also pulls `C:\ProgramData\Starbite\debloat\*.log`.
- **T67** (`sbwddm-t67` = T64 + sb-user.bat waiting up to 600 s for GD, since Steam now starts cold): runonce-arm, then
  T67a (cold) and T67b (warm) with `--sbxta DisableOnM3=0,SwapCache=1`; restore with the mode off until read.
- Risk: translations made on the M3 by xtajit64 with its ARMv8.0 latch (6.71); a bad one would crash steam/GD with
  c000001d/c0000409 in T67a/b, never on the user's boots (the restore turns the mode off).
- **For the analysis of T66:** `tools/win/etw-summary.py pres-<mode>.csv` (PIDs, event kinds, the app's present path).

### 6.89 Why no windowed swap chain was ever shown: sbumd said DXGI_STATUS_NO_REDIRECTION (sb-patches/0159)

- **Microsoft's DXGI presentation path** ([DXGI Presentation Path](https://learn.microsoft.com/en-us/Windows-hardware/drivers/display/dxgi-presentation-path)):
  with DWM on, DXGI "opens a view of a shared resource that is a render target for the DXGI producer and a texture for
  DWM" and calls the driver's BltDXGI with the Present flag; only with DWM off does it call PresentDXGI with the Blt
  flag and no destination. Every windowed present we logged since T52 was the latter (Flags 0x1, no destination).
- **The reason, in our source:** d3d10umd's `CreateDevice` ends with `return DXGI_STATUS_NO_REDIRECTION;` (upstream
  Mesa: the UMD comes from a software rasterizer). [Supporting the DXGI DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-the-dxgi-ddi)
  reserves that value for "software rasterizers" that do not share hardware with DWM: it tells DXGI not to use the
  shared resource presentation path. So DXGI took the DWM-off path for every windowed swap chain on our adapter, the
  present went nowhere (no KMD present, DWM never opened the buffers), and the window stayed invisible: T52 GD
  zero-copy, T54 sbfps flip/blt, T56/T58 VIS2 hwnd/hwndblt; very likely Edge's GPU process failures too.
- **0159** (src-int 770743d, `tools/win/mesa/sb-patches/0159-d3d10umd-dxgi-redirection.patch`): `CreateDevice` returns
  S_OK when `malikm_use(pDevice->sbDev)` (the device is on the Mali; logged "DXGI shared-resource presentation
  (redirection) on"), NO_REDIRECTION otherwise; `_Blt` (our 0107 GPU blit) flushes when `Flags.Present` is set and
  counts UmdNote_30. **sbumd cf13c58e** = the 0157 tree + 0159 (build-int-a64/ec + build-int-x link, verify 35/35, 0
  fails), `build/win/umd-0159/`. Unsigned like every sbumd so far.
- **T66 now carries it:** VIS2 (ETW, elevated) + sbglzc64 fps + sb-vis.bat shots with sbumd cf13c58e, so the shots
  answer directly whether windowed swap chains (and the zero-copy GL present) now reach the screen.
- Not addressed: the DComp swap chain's INVALIDHANDLE and the DComp surface's black clear (6.74); DXGI 1.2 flip model
  (true zero-copy to DWM) would need the DXGI 1.2 DDI; with 0159 a windowed frame costs one GPU blit, not a CPU copy.

### 6.91 T66 read: with sbumd 0159 a windowed blt-model swap chain reaches the screen; flip model not yet

- **T66** (big cores, sbumd cf13c58e, elevated VIS2 + ETW; 22:20-22:31): VIS-ELEV ran elevated (`elevated 1`), all four
  ETW CSVs written (26-60 MB each).
  - **sbcomp hwndblt (DISCARD, blt model): the magenta 1200x700 window is ON SCREEN** in the shot: the first windowed
    D3D11 swap chain ever composed on this phone. 51.2 fps, 0 failures. 0159 was the cause.
  - sbcomp hwnd (FLIP_SEQUENTIAL): 7.9 fps (T58: 73), and the shot is black apart from a small red patch: the flip
    model still does not work (it would need the DXGI 1.2 DDI / flip support).
  - dcsc (DComp swap chain): now runs, 43.7 fps, 0 failures (T56/T58: device removed at the first present); shot black.
  - dcsurf: black rectangle as before (the clear emulation of 6.74).
  - sbfps fullscreen-sized flip 279.7 fps / blt 41.8 fps; its BitBlt shots are all black (inconclusive).
  - sbglzc (zero-copy GL, flip by default): the KMD now gets its presents (109 / 487 per run, before 0-9); 31.5 fps
    flip, 56.5 fps discard vs GDI 148.6; its window was already gone at shot time; "2 failed" = the known readback limit.
  - tracerpt did not decode DxgKrnl's event names (task numbers only); a manifest-based decoder is needed to read the
    traces further.
- **But DWM crashed 4 times in T66's boot** (Application 1000, Windows clock UTC; T66 VIS ran 18:22-18:26):
  18:22:43 and 18:23:57 dwmcore.dll **0x8898008D** at +0x16AB84 (during sbcomp hwnd = flip model, and dcsc), 18:25:19
  dwmcore.dll c0000005 at +0xE5780 (sbglzc default = flip), 18:26:06 **d3d11.dll c0000005** at +0x59434 in dwm.exe (at
  the end of sbglzc discard). The flip model's "black screen" is DWM crashing and restarting. So with 0159 the
  blt path works but flip-model / composition swap chains (and possibly a blt chain's teardown) crash DWM:
  **0159 is not for the user's install until that is understood.** WER reports AppCrash_dwm.exe_* are in
  results-t66/ours/pre/wer (listing); their Report.wer / dumps are the next read.
- **0161** (src-tc da894e7, `sb-patches/0161-wgl-zero-copy-default-discard.patch`): the GD ICD's zero-copy swap chain
  defaults to DISCARD (blt model, 1 buffer); `SB_GL_ZC_SWAP=flip|flipdiscard` remain. ARM64EC build `build/win/gd-zc-0161`
  (opengl32 35ec684e, libgallium_wgl 0997dd1b).
- **T68 GO** with that build: GD on the big cores, A/A2 async GDI present vs B/B2 zero-copy (blt model into DWM).

### 6.93 T68 read: GD zero-copy works on screen and stays stable, but is slower (Present waits); C: ran full

- **T68** (big cores, sbumd 0159, GD ICD 0161 EC; 00:15-00:56): GD up 105 s cold, then 5 s each (Steam warm).
  - A (async GDI): glstats steady **48.2 fps**, gllows 44.7 fps, 1% low 25.0; A2: 38.1 / 36.0 / 19.6.
  - B / B2 (glzc, zero-copy, DISCARD): `zero-copy present, 2960x1440 B8G8R8A8, swap effect 0`, 26 / 24 lines of 300
    frames at **~23 fps**: per frame GL wait ~5 ms, copy+mark 0.3-0.4 ms, **Present ~30 ms**, 0 present failures,
    slowest ~50 ms. So the frame reaches DWM through DXGI's blt path every time, but Present() blocks ~2 vblanks.
  - **No DWM or sihost crash** in the boot (only msedge c0000409): the blt path with 0159 holds up for a game.
  - C: fell from 1126 MiB to 113 and **0 MiB free** during the GD runs (memory load 93 %, commit free 676 MiB: the
    pagefile grew into the rest of C:); the B-run screen shots were skipped by sbshow's 600 MiB guard. After the boot
    TWRP sees 1.2 GB free (11-13: 3.3 GB). `tools/win/c-cleanup.sh` now also covers C:\sbdumps (shell dumps; copied to
    the Mac first), umdlog pres-*.csv (T66's ETW CSVs) and Windows\XtaCache.m3 (T67's parked cache, only in the safe
    layout).
- **0162** (src-tc 7f1e897, `sb-patches/0162-wgl-zero-copy-novsync-flag.patch`): `C:\ProgramData\Starbite\glzcnovsync`
  turns zero-copy on and presents with sync interval 0 (logged once). ARM64EC `build/win/gd-zc-0162` (opengl32
  d00863eb, libgallium_wgl 2254f46f).
- **T69** (`sbwddm-t69` = T68 + 0162, sb-gd-ab.txt `glzcnovsync twice`): A/A2 GDI vs B/B2 zero-copy at interval 0.

### 6.94 T69 read: interval 0 does not help - the blt-model zero-copy present is paced by DWM; keep the GDI present

- **C: cleanup first** (`c-cleanup.sh --delete`, 01:3x): 1184 -> **2152 MB free** (freed 968 MB: Users\...\Temp
  598 MB, C:\sbdumps 213 MB, T67's XtaCache.m3 128 MB, starbite\auto\gpu 19 MB, ...; copies on SBWORK). During T69's
  GD runs C: still fell to 808 MiB (pagefile growth), not to 0.
- **T69** (big cores, sbumd 0159, GD ICD 0162 EC; 01:36-02:19): A 46.8 / A2 38.6 fps (glstats steady; gllows 43.5 /
  36.4, 1% low 23.8 / 20.0) vs B 23.8 / B2 22.9 fps zero-copy with **sync interval 0** (`sync interval: 0
  (glzcnovsync)`): Present still ~25 ms per frame (T68: ~30 ms with the game's interval). In the same process sbumd
  blocks little (540-560 submits, 4-13 ms blocked per 10 s). So the time is DXGI's windowed blt present waiting until
  DWM has taken the previous frame from the redirection surface: the game is paced at DWM's composition rate (the
  KMD's PresentsPerSecMax 56, last 46), while the GDI present is not paced (its extra frames are simply not shown).
- **Conclusion for now:** the GDI async present stays the user's mode; zero-copy (0151/0160-0162) stays opt-in. A
  real gain needs the flip model (DXGI 1.2 DDI: DWM composes from the app's buffers / independent flip), whose
  windows still crash DWM with 0159 (6.91) - a larger project. The frame rate the user sees is bounded by DWM's
  composition rate on our driver in landscape, which is the next thing to measure (DWM's own frame times).

### 6.95 The flip-model DWM device loss: what T66's logs say, sbumd 0163 (diagnostics + three candidates), T70 staged

(2026-11-15 13:35) The real zero-copy gain for Geometry Dash needs a flip-model swap chain (6.94: the blt model is
paced by DWM). With 0159 a flip-model window makes DWM lose its device (6.91). Reading T66's DWM umdlogs and the
DxgKrnl ETW again, in order:

1. sbcomp creates a 2-buffer FLIP_SEQUENTIAL chain; the buffers are created shared (misc 0x20002: SHARED +
   displayable). DWM's device opens both (OpenResource, 1200x700).
2. DWM's next submission that touches them enters the kernel (DxgKrnl Render, id 169) and is never queued (no
   QueuePacket 178 for it). pfnRenderCb returned success to the shim; sbwddm saw no failing DDI call.
3. From then on DWM's device is removed: its DXGI Present returns 0x887A0005 within 23 us. DWM recreates the device;
   on each new device the removal comes back while it rebuilds its swap chain (it reopens the same two buffers), and
   after ~9 rounds DWM fails fast with 0x8898008D (WGXERR_NO_HARDWARE_DEVICE). The other crash shapes (c0000005 in
   dwmcore / d3d11.dll) are the same loss hit at other moments.

So the kernel puts DWM's device into an error state for a submission that references the opened flip buffers, without
failing a call we can see. What the shim does with an opened (kind 2) allocation that a stock driver would not:
- it lists every named BO with WriteOperation = 1; DWM only reads an app's flip buffer;
- 0138 forces every opened allocation into every submission of the device (so the KMD always has its address),
  whether or not that submission uses it: after the open, each DWM Render "writes" both flip buffers;
- and one DDI hole: the runtime uses D3D10 DDI 0x000a0009 / version 0x177a, so the DXGI base table is the DXGI 1.1
  one with an 8th slot, pfnResolveSharedResource, which upstream d3d10umd never fills. DXGI calls it on a shared
  surface's owner before another device reads it.

**sb-patches/0163** (src-int, on 0157 + 0159; sbumd d7285e58, ARM64X verify FAILS: 0), diagnostic, not for the user:
- flag `devstate`: after every Render, open, failed lock and present the shim asks the system for the device's
  execution state (D3DDDICB_ESCAPE with Flags.DeviceStatusQuery, a `{UINT State}` buffer; 1 active, 2 reset, 3 hung,
  4 stopped, 5 out of memory, 6 DMA fault) and logs each change, with the allocation list of the Render that
  changed it (handles; w = written, * = opened from another process);
- flag `zcro2`: kind 2 allocations are listed without WriteOperation;
- flag `zcnolist2`: kind 2 allocations are not forced into every submission;
- `_ResolveSharedResource` (flushes the pipe) fills the DXGI 1.1 slot when the interface is 10.x and the version is
  at least WDDM 1.1's;
- LockCb failures are logged (first 12); flags are read at device creation and again at every open, so a running DWM
  takes a new set without a restart.

**T70 (FLIP1)**, `build/win/stage/sbwddm-t70` (T61 drivers + sbumd 0163): `sb-flip.bat` (staged as sb-elev.bat, run
elevated by the generic `sb-elev-run.cmd` from HKLM RunOnce; `sb-elev-wait.bat` as sb-user.bat) runs sbcomp hwnd 12 s
under base / ro2 / nolist2 / both, then hwndblt as the control: a shot in each, DWM's pid before / at the shot /
after. `runonce-arm.sh ... dwm.exe` also sets WER LocalDumps for dwm.exe (mini dumps: `SB_DUMP_TYPE=1` in
hive-localdumps.py; C: is tight). Reading it: the state code and the Render that caused it in the base run name the
kernel's reason; a config with "DWM SURVIVED", the red window in its shot and sbcomp at vsync rate is the fix
(T66: 7.9 fps while DWM looped). Next after that: the GD ICD's zero-copy chain as FLIP_SEQUENTIAL against the async
GDI present.

### 6.96 Night of 11-15: what the flip model can give GD (frame budget), GD ICD 0164, T72 (SMOOTH1) staged

(2026-11-15 22:50) The user's goal, via the peer: "Windows super smooth ... hundreds of fps in Geometry Dash".

- **GD's frame budget, re-read from T68/T69** (big cores, in the level):
  - async GDI present: 38-48 fps, i.e. 21-26 ms per frame. The game thread hands a frame to the copy thread and waits
    when that thread is still busy; copy (5-17 ms) + GDI (4-10 ms) is what sets the pace.
  - zero-copy on the blt model: 23 fps = ~43 ms per frame, of which Present ~25-30 ms (DWM-paced) and the GL wait
    ~5 ms. **What is left for the game and the driver's CPU is ~8-12 ms**, not the ~20 ms 6.73 derived from the GDI
    path (that figure included the wait for the copy thread).
  - So a present that does not block (flip model, interval 0) should land at ~13-17 ms per frame = **60-75 fps**
    with the 5 ms GL wait still on the game thread, and ~80-120 fps once that wait moves off it (a present thread,
    as the async GDI present has). Beyond that the game's own translated x64 CPU is the bound (XtaCache on the M3,
    6.87), and the panel shows 60 Hz whatever the game's counter says. Estimates until T71 measures them.
- **GD ICD 0164** (src-tc 5218bdc, `sb-patches/0164-wgl-zero-copy-flip-model-flags.patch`): flag files
  `C:\ProgramData\Starbite\glzcflip` (FLIP_SEQUENTIAL, 2 buffers) and `glzcflipd` (FLIP_DISCARD) turn zero-copy on
  with a flip-model swap chain and sync interval 0 (`glzcflipvsync` keeps the game's interval). ARM64EC
  `build/win/gd-zc-0164` (opengl32 87bf8955, libgallium_wgl f0a90dcb). T71 = this against the async GDI present,
  on the sbumd that T70 says keeps DWM alive.
- **T72 (SMOOTH1)**, `build/win/stage/sbwddm-t72` (T61 drivers = the user's install): `sb-smooth.bat` (as
  sb-user.bat) runs the new `sbsmooth.exe` (`tools/win/mesa/tests/sbsmooth.cpp`, 6051a36e) at logon and ~1, 2.5 and
  5 min later, sbdrag at the end, typeperf every 2 s. sbsmooth per run: each CPU's MHz and a fixed loop's time pinned
  to it (a throttled core shows), the timer resolution and what Sleep(1) takes (a game's 1 ms sleep), thread wake-up
  times, DWM's refresh, the frame pace of a repainting 1200x700 window and of a full-screen one (fps, p50/p95/p99/max,
  DWM late/dropped/missed, KMD GPU busy and present wait/copy, per-CPU busy with DPC+interrupt), and the time from an
  injected mouse move to the window and to the composed frame. It answers: does DWM hold 60 fps here, what stops it
  when it does not (GPU, the present copy, a CPU), how late is input, and how long after logon the desktop is slow.
- `tools/win/mesa/tests/build-test.sh NAME` builds a test program the way sbcomp/sbdrag were built (VM clang-cl,
  ARM64, ~/sbwin kit).

### 6.97 T70 read: DWM's device is stopped for listing an app's flip buffers as written; read-only fixes it (0167)

(2026-11-16 01:00) **T70** (`build/win/results-t70`, big cores, sbumd d7285e58 = 0163; menu after 257 s):

| run (sbumd switches) | sbcomp hwnd | DWM | DWM's own frame |
|---|---|---|---|
| base (devstate) | 2.5 fps | restarted (0x8898008D) | - |
| ro2: opened allocations listed without WriteOperation | **56.8 fps** | **survived** | the red window (82.9 % of the frame) |
| nolist2: opened allocations not forced into every submission | 6.3 fps | restarted | - |
| both | **61.3 fps** | **survived** | the red window |
| control: hwndblt (blt model), base | 52.3 fps | survived | magenta window |

- **The reason, from the device-state log:** `device 1 execution state 1 -> 4 at a Render` (4 =
  D3DDDI_DEVICEEXECUTION_STOPPED), the first Render after `OpenResource` of the two 1200x700 buffers, and that
  Render's list has them marked written (`400079c0w*`). Every recreated device dies the same way at its 4th-10th
  submission. With the write mark off there is no state change at all. So the system stops a device that claims to
  write a flip-model buffer it only opened to read; DXGI then reports device removed, and DWM gives up after ~9
  devices. The missing `ResolveSharedResource` (filled in all runs) and the forced listing (0138) are not the cause.
- **Why we marked it:** the shim marked every buffer a submit names as written, because Panfrost's submit ioctl
  carries no access flags.
- **0167** (src-int b8730f0, `sb-patches/0167-...patch`, sbumd d39b83ab, ARM64X verify FAILS: 0):
  - Panfrost tells the shim which buffers the batch writes (`malikm_submit_writes`, from its own per-batch access
    flags, right before the submit); an allocation opened from another process is listed with WriteOperation only
    if it is in that set. A blt-model app still writes DWM's surface (it is the blit's destination); DWM reading an
    app's buffer no longer does. Everything else is listed as before. 0138's forced listing stays (read).
  - MSAA (0165) is offered only with the flag file `msaa`, until the conformance run has checked it.
  - 0166: geometry shaders are refused cleanly on a driver without a geometry stage.
- **T71 (FLIPGD)**, `build/win/stage/sbwddm-t71`: `sb-flipgd.bat` runs sbcomp hwnd / hwndblt / dcsc / dcsurf with
  no flag files (the fix itself, incl. DirectComposition), then Geometry Dash A (async GDI), B (`glzcflip`:
  zero-copy on FLIP_SEQUENTIAL, interval 0; GD ICD 0164), B2 (`glzcflipd`: FLIP_DISCARD), A2.

### 6.98 T72 read (SMOOTH1): the desktop composes at 60 fps with one frame of input delay; what costs CPU

(2026-11-16 01:08) **T72** (`build/win/results-t72`, the user's T61 drivers, big cores; sbsmooth at logon, +1, +2.5,
+5 min; all four runs alike, so the desktop is not slower right after logon in this boot):
- **Frame pace:** a repainting window gets 61.8-62.5 fps, interval p50 16.0 ms, p95 16.6-17.3, p99 17.1-19.7, worst
  21-64 ms, none over 100 ms; full screen the same (p99 18.6-22.5). The panel reports 60.3-61.8 Hz. DWM's own frame
  counter still does not advance (`cFrame`, known).
- **Input:** an injected mouse move reaches the window in 0.5-0.7 ms and is on a composed frame after 15-20 ms on
  average (p95 20-28 ms): one refresh.
- **Scheduler and timer:** a waiting thread runs 6-11 us after its event (p99 14-27 us, worst 294 us); the timer
  resolution is already 1 ms; `Sleep(1)` takes 2.0 ms.
- **CPUs:** three cores, each **1794 MHz fixed** (the same fixed loop takes 120 ms on each; 130-153 ms at logon
  while they are busy). No frequency scaling: Windows sees max = now = 1794.
- **KMD:** GPU busy 21-30 %, present wait 28-34 ms/s, present copy 50-64 ms/s, 70-84 presents/s, 3.3-3.6 jobs per
  present, no thermal holds.
- **Window drag** (sbdrag): 53.9 moves/s, 18.5 ms per move (worst 35 ms), but **present copy 355 ms/s** and 14.1
  jobs per present: moving a window costs a third of a core in the KMD's CPU blts.
- **Who uses the CPU on an idle desktop** (CPU time over the ~5 min session): System 1:47 (the kernel, our present
  path included), one svchost 0:55, **sblinkd 0:52 (a steady ~17 % of a core, 107 MB)**, dwm 0:35, Defender 0:26.
  Total load 9-30 % idle, 60-72 % while the test window animates. Memory: 1.2-1.4 GB available, 2.2-2.3 GB
  committed, pagefile 0-5 %.
- **Read:** composition and input are at the display's rate; the desktop's smoothness problems are CPU supply
  (three cores at 1.8 GHz, a quarter of one taken by background work) and the CPU cost of GDI window moves, not the
  compositor's pace. Levers, in order: sblinkd's 17 %, the svchost (which service: tasklist /svc next time), the
  big cores' clock (thermal permitting), GPU blts for window moves instead of CPU copies.
- **msedge** crashed three times in T70's boot (c0000409 in ucrtbase, i.e. abort). Chromium aborts on purpose when
  its GPU process cannot be kept alive; with the user's sbumd it has no working windowed present. To re-test on
  0167 (flip model and DirectComposition working): a browser start is the next real-app test.

### 6.99 T74 read (WT1): the first conformance score of the user's driver - 74 of 111 d3d10core tests pass, 20 crash

(2026-11-16 01:12) **T74** (`build/win/results-t74`, the user's T61 drivers, big cores): Wine 10.0's d3d10core suite
through `sbwt.exe`, each test function in its own process. The runner worked first time: the whole suite took about
two minutes. **74 pass, 17 fail, 20 crash, 0 time out.**
- **Crashes (20), by kind:**
  - a call through address 0 where the test creates or sets a geometry shader or stream output
    (`test_create_shader`, `test_index_buffer_offset`, and by their content `test_geometry_shader`,
    `test_stream_output*`, `test_layered_rendering`, `test_clear_state`, `test_multiple_viewports`, ...): the missing
    geometry stage (0166 turns these into clean refusals; `gsemu` is the fix);
  - `abort` (c0000409) in `test_cube_maps`, `test_sample_c_lz`, `test_unbound_multisample_texture`, and an illegal
    instruction in `test_combined_clip_and_cull_distances`: the old shader translator's path (the offline harness
    predicted `abort()` for `switch` and undefined behaviour for cull distances);
  - access violations inside the driver in `test_texture`, `test_texture1d`, `test_update_subresource`,
    `test_resinfo_instruction`, `test_shader_input_registers_limits`: not yet resolved to a function.
- **Wrong results (17):** multiple render targets (targets 1-3 keep the clear colour), fragment coordinates, face
  culling in some winding/cull combinations, colour write masks, independent blend, instanced draws (per-instance
  data), `CopySubresourceRegion`, relative addressing of vertex inputs, the immediate constant buffer, depth bias
  (832 of 21002 checks), depth-stencil sampling, alpha-to-coverage, pipeline statistics, two format-compatibility
  cases, one clear of a render-target view.
- **What it means:** this is the list behind "unstable": a D3D10 app that uses a geometry shader, a `switch`, a
  cube map with comparison sampling or more than one render target either dies or draws wrong on the user's driver
  today. T76 runs the same suite on the next tree with the old and the new shader translator
  (`tools/win/winetests/wt-compare.py` puts the logs side by side).

### 6.100 T71 read: the flip model works for every window kind; Geometry Dash shows 60 on it (GDI: 32-41)

(2026-11-16 02:04) **T71** (`build/win/results-t71`, big cores, sbumd d39b83ab = 0167, no flag files for part 1):
- **FLIP2, one DWM process (pid 1108) for the whole boot:** hwnd (FLIP_SEQUENTIAL) 60.2 fps, hwndblt (DISCARD)
  52.8, **dcsc (DirectComposition swap chain) 61.3, dcsurf (DirectComposition surface) 55.5**, 0 failures each.
  With the user's sbumd none of the four was composed (T56-T60); with 0159 alone the flip kinds killed DWM (T66).
- **Geometry Dash, Stereo Madness, ~5 min each** (GD ICD 0164, ARM64EC):

| run | present | fps | hitches |
|---|---|---|---|
| A | async GDI (the user's mode) | 41.2 avg, 1 % low 22.2, 0.1 % low 8.3 | 24 over 100 ms, 14 over 250 ms |
| B | zero-copy, FLIP_SEQUENTIAL, interval 0 | 55.4 median of 300-frame windows (max 58.7) | 26 over 100 ms, worst 1.4 s (one) |
| B2 | zero-copy, FLIP_DISCARD, interval 0 | 54.9 median (max 57.5) | 1 over 100 ms, worst 152 ms |
| A2 | async GDI | 36.4 avg, 1 % low 18.5, 0.1 % low 7.9 | 20 over 100 ms, 13 over 250 ms |

  - **The game's own counter in the shots: 60 on the flip path, 32 on GDI.** 60 is Geometry Dash's cap, so the
    present path is no longer what limits it; the 300-frame windows average ~55 because they include level
    restarts.
  - Per frame on the flip path: GL wait 5.6-5.9 ms, copy 0.5 ms, **Present 0.6-0.7 ms** (blt model in T68/T69:
    25-30 ms), WAR wait 1.5-1.9 ms; 0 present failures; the game is on screen in DWM's frame.
  - C: fell to 232 MiB free by the fourth run (pagefile growth), as in T68.
- **Candidate for the user** (their yes needed): `build/win/stage/sbwddm-user-0167` (T61 kernel drivers + sbumd
  d39b83ab) and `build/win/gd-kit-ec-0164` with the flag file `glzcflipd` (FLIP_DISCARD: fewest hitches here);
  `gd-kit-push.sh DIR --flag NAME / --no-flag NAME` sets it from TWRP. INSTALL.md there has the install and the
  way back. What it changes besides the game: every D3D window is now presented through DWM properly (blt, flip,
  DirectComposition), which is what Edge/WebView2 and WinUI apps need; that part has run in four test boots
  (T68-T71) but not in a user session.
- **Next for speed:** the 5.6 ms GL wait and 1.5 ms WAR wait sit on the game thread; with the game's cap raised
  they are what bounds it (~8 ms of waits + the game's own time). A present thread, as the GDI path has, moves
  them off it.

### 6.101 T76b does not count: the runtime rejected that driver and the suite ran on the Basic Render Driver

(2026-11-16 02:18) **T76b** (`build/win/results-t76b`, sbumd 98e9e6a6) printed 110 pass / 1 fail in both passes,
but every one of the 112 test outputs says `Adapter: L"Microsoft Basic Render Driver"`: Direct3D could not create a
device on our adapter and the tests fell back to WARP. So T76b says nothing about either shader translator. (T74
ran on "Starbite Display", T71's programs too.) Side result: the suite's expectations are met 110/111 by WARP on
this phone, so the tests themselves are sound here.
- **Why the device was refused:** that DLL had MSAA (0165) on unconditionally. In each process our log ends right
  after `CheckMultisampleQualityLevels R32G32B32_FLOAT x4: 1`. The working theory (inferred, not shown): the
  runtime cross-checks multisample answers inside a format family, and `R32G32B32_TYPELESS` was translated to a
  non-renderable UNORM format (0 levels) while its FLOAT member answered 1. sb-nir 1e2a6c6 creates the typeless
  format as FLOAT. 0167's `msaa` flag (off by default) keeps this away from every other test.
- **Tooling:** `sbwt.exe` now marks a test `[NOT OUR ADAPTER: ...]` and prints a WARNING line under the summary;
  `wt-compare.py` prints `!!` for a log whose tests ran on another adapter. `winetests/build.sh` waits until the VM
  reads what the Mac wrote (a just-rewritten source was twice seen truncated in the VM).
- **T77 updated** (`build/win/stage/sbwddm-t77`): sbumd 35bae4d5 (sb-nir 1e2a6c6, all conformance fixes so far),
  the new runner, seven passes: O (no flags), N (nirshaders), G (+gsemu), M (+msaa: is the device accepted now?),
  D (+d3d11ddi), F (+fl11), X (dxgi).

### 6.102 T78 staged (APPS1): everyday apps on the candidate driver

(2026-11-16 02:24) `build/win/stage/sbwddm-t78` = the candidate for the user (sbumd 0167) + `sb-apps.bat`: Edge on a
local animated page (`sb-web.html`: CSS animation, 2D canvas, WebGL; its window title carries the page's fps and the
WebGL renderer string), Settings, Calculator, Notepad, Paint, a File Explorer window and Task Manager, one at a
time: 25 s each, a shot, an alive check, the window title; then sbsmooth and sbdrag on this driver (against T72),
the crash events and `tasklist /svc`. It answers: do browsers and WinUI apps start and show on 0167 (Edge aborts
on the user's driver, 6.98), and does the candidate keep the desktop's pace.

### 6.103 GD ICD 0168: the zero-copy present on a thread of its own; T79 staged

(2026-11-16 02:24) T71's flip path leaves ~7.5 ms of waits per frame on the game thread (GL wait 5.6 ms, the copy's
wait 1.5 ms). **0168** (src-tc da5173f, `sb-patches/0168-wgl-zero-copy-present-thread.patch`, ARM64EC
`build/win/gd-zc-0168`: opengl32 d17d12ef, libgallium_wgl 8588f8ca), flag file `glzcasync`:
- the game thread, at SwapBuffers, waits until the frame before is completely through (shown, and its copy has
  read its texture, which is what frees the other texture), queues an event behind the frame it just flushed
  (`malikm_kmt_frame_mark`: SignalSynchronizationObject2 with a CPU event, no wait under the device lock; the old
  `malikm_kmt_frame_wait` held that lock for the whole wait, which would have stopped the game's own submits), hands
  the frame over and goes on drawing into the other texture;
- the present thread waits for the event, copies into the swap chain buffer, presents, and waits for the copy.
- A frame then costs max(game, present side) instead of their sum. A resize or a destroy drains the thread first;
  three failed presents in a row still switch the window to GDI. Not run yet.
- **T79** (`build/win/stage/sbwddm-t79`, candidate drivers, `sb-gdruns.bat` + `sb-gd-runs.txt`): B (`glzcflipd`),
  C (`glzcflipd glzcasync`), B2, C2.

### 6.104 T78 read: on the candidate driver Edge sets off a storm of DWM job timeouts and screen shots go black - candidate on hold

> **Corrected by 6.112:** the onset was not Edge. The timeouts start seconds after Calculator opened and last while it stays open.

(2026-11-16 05:06) **T78** (`build/win/results-t78`, the candidate: T61 kernel drivers + sbumd d39b83ab):
- **What the two captures show (they disagree, so "works" is not established):**
  - DWM's own composed frames (`dump-dwm-app-*`): every app's window is there. Edge with its page (CSS animation,
    2D canvas, WebGL), Settings, a dark Notepad window, Calculator (first its splash icon, later its "0"), a File
    Explorer window with its command bar, Paint, Task Manager's dark window; one DWM process for the whole boot.
    Some content areas are dark where content is expected (Explorer's file list).
  - The screen shots (`shot-app-*`, sbshow's own capture): Edge and Settings show; **Calculator, Notepad and
    Explorer are completely black, taskbar included; Paint and Task Manager are 91 % black** (the peer's read,
    confirmed). Whether the panel was black then or the capture failed during the GPU-timeout storm is not known.
  - Calculator's process is `Calculator.exe`; the script looked for another name.
- **Edge is not accelerated:** the page's title says `WebGL: ANGLE (Google, Vulkan 1.2.0 (SwiftShader ...))`, 27-31
  fps: Chromium fell back to its software renderer. Crash events: `msedge.exe` c0000409 in ucrtbase three times at
  logon and once later, and a `bad_module_info` c0000409 three seconds after the test started Edge. c0000409 in
  ucrtbase is `abort()`: most likely Mesa aborting inside Edge's GPU process (the old shader translator aborts on
  `switch`, 6.99), after which Chromium gives up on the GPU. Its GPU process is sandboxed, so sbumd could not write
  a log there.
- **The blocker:** from the moment Edge started (190.9 s into the boot) to the end, **404 GPU job timeouts and 404
  GPU resets**: every hang record is a `dwm.exe` `indexed-vertex` job in the vertex/tiler slot that was still
  running at the KMD's 752 ms limit (js 0x08, no MMU fault, the job's draw looks ordinary: 144 indices). DWM's own
  log shows it blocked on the GPU for more than half of each 10 s window in 25 of 49 windows, with waits of 1.5-2.2
  s. sbsmooth right after the apps: **0.5 fps** for the windowed test, GPU "101 % busy"; the full-screen test, which
  hides the desktop, 61.7 fps. Closing Edge did not end it.
  - T71 (40 min on the same sbumd, GD and the four test windows) and T72 / T74 (the user's sbumd) have none of
    these in DWM's log. The counters come from leftover cells in the pulled hive (the follow-up display-only boot
    overwrites the State key), so from T80 on the test saves the key itself.
  - History: the same signature (DWM IDVS jobs timing out) in T46-T48 was the GPU stalling on memory VidMm had
    unmapped; the scratch page fixed reads (6.56, 6.58). What stalls here is not known yet: memory pressure with
    seven apps, something Edge's dying GPU process leaves behind in DWM's scene, or the tiler heap.
- **Consequence:** the candidate (`sbwddm-user-0167`) is **on hold**. It makes the game path and every window kind
  work, but a browser start made the desktop unusable in this test. Whether the user's current driver does the
  same is not known (they reported a desktop too slow to reach the Start menu).
- **T80 / T80b** (`sbwddm-t80` = candidate, `sbwddm-t80b` = the user's T61): `sb-apps2.bat` reads the KMD's
  Live_JobsTimedOut / GpuResets / FaultCount / HangCount after every step: the WinUI apps first, then Edge without
  GPU, Edge with its GPU process unsandboxed (so sbumd logs there), Edge as normal; sbsmooth before and after; the
  whole State key saved at the end.

### 6.105 T77 produced nothing: the new tree's driver crashes DWM and LogonUI at boot

(2026-11-16 06:10) **T77** (`build/win/results-t77`, sbumd b59839d2 = sb-nir d4cb682, no flag files at boot): the boot never
reached the logon. WER: 3 x `dwm.exe` and 3 x `LogonUI.exe`, c0000374 (heap corruption) and one c0000005 in ntdll's
heap. No sbumd log was written for either process, so the fault is before the shim's first line: at DLL load, in
`OpenAdapter` / `GetSupportedVersions`, or in `CreateDevice` before the bind. The watchdog ended the boot at 3600 s
and no conformance pass ran (the `sbwt-*.log.out` folders in that pull are T74's and T76b's).
- So "with no flag file the tree behaves as before" was wrong for this build, and neither the offline harness,
  the sanitizer runs nor the review could see it: none of them executes the Windows-only frontend or the DLL.
- Known good on the phone from that branch: only 98e9e6a6 (cf67267, T76b), which loaded and created devices (and
  was then refused by the runtime over its MSAA answers). The regression is in cf67267..d4cb682.
- A first read of what runs before the first log line (the version list, `CreateDevice` up to the bind, the DLL's
  imports, which are identical to the working builds) found nothing. DWM's crash dumps are on the phone
  (`C:\sbdumps`, armed in T70) and come with T79's user-pull.
- **Process change:** a driver build from a tree that has never booted gets a short boot-and-logon check first
  (a few minutes, one small test), before any long test is queued on it. T77 cost an hour of phone time for nothing.

### 6.106 T77's boot crash found: a C++ initializer from generated code, run by the wrong half of the ARM64X DLL

(2026-11-16 07:12) DWM's mini dumps from T77's boot (`results-t79/ours/user/dumps/dwm.exe.2112.dmp`, `.2984.dmp`; walked with a
small mini-dump reader against the build's lld map) both show the same stack:
`dxgi -> LoadLibrary -> sbumd!_DllMainCRTStartup -> dllmain_crt_process_attach -> _initterm ->
[initializer of vtn_bindgen_dummy_instance] -> atexit -> _onexit -> ucrtbase -> heap corruption (c0000374)`.
- **The object:** `vtn_bindgen2` (Mesa's generator for precompiled OpenCL kernel bindings) puts a static object
  with a constructor and a destructor into the C++ file it generates, to register the kernels' printf strings.
  The geometry-shader work linked `src/poly`'s generated `libpoly.cpp` into the driver, the first such object in
  sbumd. (The builds that load, 0167 and T76b's, have none.)
- **Why it corrupts the heap:** sbumd.dll is ARM64X (an ARM64 and an ARM64EC half linked by our own
  `link-arm64x.sh`). Both halves' `.CRT$XCU` initializer entries end up in one table between both halves'
  `__xc_a` / `__xc_z` markers, so the native start-up also calls the **ARM64EC** copy of the initializer
  (`...@@$$hYAXXZ` in the map). That copy calls the ARM64EC CRT's `atexit`, whose exit table
  (`module_local_atexit_table`, the second copy at +0x1A6E730) the native start-up never initialised: ucrtbase
  decodes zeros as pointers and frees garbage. It happens at DLL load in every process that loads the driver, before
  any driver code runs, which is why there was no driver log.
- **Fix (sb-nir 430b803 + the build):** no static object in a Windows build (the generator's block goes inside
  `#ifndef _WIN32`; the prebuilt tool is wrapped by `~/sbmesa/native-nir/vtn_bindgen2`, which the sb-nir cross
  builds now find first), and **`build-nir-x/check-init-tables.py`** in the build: it fails the build if the image has
  any C++ dynamic initializer (run on old maps: T77's and the tessellation build fail it, 0167 and T76b's pass).
  The underlying merge of the two halves' initializer tables is still there, so any future C++ static with a
  constructor would do the same; the check is what guards it.
- **Lesson kept:** `tools/win/smoke-stage.sh` + `sb-smoke.bat`: a five-minute boot check before any long test on
  a build from a tree that has not booted.

### 6.107 T79 read: with the present thread the game thread waits 0.2 ms per frame; Geometry Dash sits at its 60 cap

(2026-11-16 07:12) **T79** (`build/win/results-t79`, candidate drivers, GD ICD 0168, Stereo Madness, ~5 min each):

| run | present | fps (median of 300-frame windows / max) | per frame |
|---|---|---|---|
| B | FLIP_DISCARD, in line | 54.7 / 58.7 | GL wait 5.7 ms + copy 0.6 + Present 0.7 + copy wait 1.5, all on the game thread |
| C | + present thread (`glzcasync`) | **58.3 / 59.6** | game thread waits **0.22 ms** at the hand-over; the thread: GL wait 5.6, copy 0.5, Present 0.5, copy wait 4.5 |
| B2 | in line | 55.0 / 57.7 | as B |
| C2 | + present thread | **58.3 / 59.6** | game thread 0.16 ms |

- No present failures, no crash, in all four; frames over 100 ms: 5 / 5 / 3 / 3.
- So the present path now costs the game ~0.2 ms a frame, and the game runs at its own 60 fps limit (the windows
  average 58 because of level restarts). Going above 60 is the game's frame-rate setting, not the driver.
- This is with the candidate sbumd (0167), which is on hold for the desktop problem of 6.104; the game result
  itself stands.

### 6.108 The row "Windows 11 (new GPU)": the 0167 driver as its own entry, the other rows on the driver they have

(2026-11-16 15:03; built and staged, **not run on the phone yet**: T83 / T83b are the first run.) The user asked for the
driver with the GPU fixes as a separate entry on the Starbite OS list, next to their Windows entries, which stay as
they are (T61's sbumd 6d759c44).

- **The problem.** Every Windows row starts the same install, and two of them the same firmware image; Windows
  cannot tell rows apart by hardware, and a driver set is installed per device, not per boot. The UEFI image is
  compressed and not ours to patch per row, and the loader passes nothing to Windows.
- **The row signal: `wdt`.** The loader arms cluster 1's watchdog with the row's `wdt` (`wdt_arm`, count = seconds
  x 793.457, truncated: 82 s = 65063 = 0xFE27, 80 s = 63476; 60 s = 47607 and 30 s = 23803 are in
  STARBITE-HEALTH.md). The firmware stops the watchdog but leaves WTDAT, and `sbwdog.sys` (boot start) records it in
  `HKLM\SYSTEM\CurrentControlSet\Services\sbwdog\State\WtDat` in its DriverEntry, before it arms anything itself.
  All 28 pulled hives of 11-15/16 (rows with `wdt = 82`) say 65063. The new row gets **`wdt = 80`**; nothing else in
  the loader, the firmware or a kernel driver changes. A row without `wdt` (the plain `win11` row) is never armed
  by the loader: WTDAT is then S-Boot's own value from that boot (S-Boot runs before every boot and leaves the
  watchdog counting), not a left-over 80 s. Not measured: no pulled hive is from such a boot.
- **The selector: `sbsel.dll`** (`starbite/win/sbsel`, 52 KB, ARM64X, C only, no initializer tables, build.sh).
  The adapter's `UserModeDriverName` (display class key of `acpi\strb0072`, the only place the hive names the
  driver) becomes `sbsel.dll`. Its `OpenAdapter10` / `OpenAdapter10_2` (the two functions both sbumd builds export
  for the runtime) load `System32\sbumd-ng.dll` when WtDat is 63400..63560, otherwise, or on any failure,
  `System32\sbumd.dll`, and forward. The real driver stays loaded for the life of the process. With the file
  `C:\ProgramData\Starbite\sbsellog` each process writes one line to its own `umdlog\sbsel-<pid>.log` (exe, pid,
  WtDat, driver): that is how a test sees what DWM loaded. Without the file it writes nothing.
- **Files.** `sbumd-ng.dll` = sbumd 0167 (d39b83ab). `sbumd.dll` is not written by the install; the standard rows
  load the same bytes as today, through 52 KB of new code. Kernel drivers are shared (both sets use sbwddm
  d85e0d3a). A test cycle's `sbumd.dll` push still lands on the standard rows only.
- **The game: GL kit 0169** (src-tc 2c9c8ff, `sb-patches/0169`): on the new row (same registry value) `glzcflipd`
  and `glzcasync` count as present, so Geometry Dash presents zero-copy on a FLIP_DISCARD swap chain with the
  present thread (T79: its 60 cap) without flag files; `ngzcoff` turns that off. On every other row: flag files
  only, i.e. the GDI present of today. The kit is one pair of files in the game folder for all rows, so installing
  it changes the game's GL driver binary on the standard rows too (same code path as 0158 there; T83b measures it).
- **Tools.** `tools/win/hive-umdname.py` (offline hive: show / set sbsel.dll / set sbumd.dll; round trip on a copy
  of the user's hive: only that value differs, and setting it back restores the key's values byte for byte),
  `tools/win/sbsel-install.sh` (show / install / restore / log on|off, TWRP; syntax-checked only),
  `tools/win/ng-stage.sh` (stages `sbwddm-ng-0169` and `sbwddm-t83`), `starbite/win/autotest/sb-rows.bat` (T83).
- **The row** (`starbite/win/sbsel/catalog-row.toml`): key `win11-ng`, "Windows 11 (new GPU)", version
  "23H2 · new GPU driver · games; some apps black", `files = ["windows-big3i.uefi"]` (already in the store),
  `wdt = 80`, `wdt_feed = "own"`. It packs (sbstore2 catalog: wdt 80 in the extension table). **The live catalog
  is full: 32 rows of the loader's CAT_MAX 32.** One row has to be dropped for it; that choice is not mine.
- **Review (a second reader, before the first run):** fixed: `sbsel-install.sh show` died silently when a file was
  missing; the test log is now one file per process (a shared file created by DWM's account would have been
  read-only to the user's processes); the lazy load is interlocked; the T83 verdict also fails when a process of
  the wrong driver is loaded "now" (tasklist), not only by the log lines. Accepted and written down: the real
  driver now stays loaded when the runtime unloads its user-mode driver (it used to get a fresh image).
- **Limits.** (1) The store setting "watchdog off" or "one value for every entry" (other than 80 s) removes the
  signal: the new row then runs the standard driver; "one value" = 80 s would put every watchdog row on the new
  driver. A boot in which sbwdog does not start keeps the previous boot's value. (2) A process that cannot read the State key loads the standard driver. (3) The
  0167 driver's open problems come with the row (6.104: DWM job timeouts from Edge's start, black classic-app
  windows in screen shots). (4) Nothing here has run on the phone: the selector has never been loaded by Windows.
- **T83 / T83b** (`build/win/stage/sbwddm-t83`, T61's drivers + sb-rows.bat): one boot of `win11-ng`, one of
  `win11-big`. Each logs the WtDat it sees, the driver name, the three files' hashes, a blt and a flip window
  with shots, DWM survived or not, a Geometry Dash run with kit 0169, the selector log and a verdict
  (new row: every process `-> sbumd-ng.dll`; standard row: every process `-> sbumd.dll`).

### 6.109 Late touches in Windows: what is measured first (T84), and the fix candidate in the GL kit (0170)

(2026-11-16 15:22; staged, **nothing run yet**.) The user (11-16): Geometry Dash decently smooth, but touches arrive a lot
late. Mouse-to-frame was ~15 ms in T72, so the suspect is the touch path.

- **What is known.** The user's install is touch-6 (s6touchdiag td3-25ms-5x + sbhsi2c-3, 6.88c): a touch read
  takes ~0.45 ms, on the M3 row 1 transfer in 7 needs the 2 ms poll, DiagI2cError 0. The driver reads every event
  of an interrupt and completes the HID report in the same pass. "Press and hold for right-click" is off
  (HKCU Wisp\Touch TouchMode_hold 0, touch.log 11-14). None of that explains "a lot late".
- **Hypothesis (not measured):** Geometry Dash reads only the mouse (GLFW's WM_LBUTTONDOWN/UP). For such a window
  Windows promotes a touch to a click, and it may send the button-down only once it knows what the touch is, i.e.
  when the finger lifts or moves. Then every tap is late by the time the finger stays down and a hold does nothing.
- **T84** (`build/win/stage/sbwddm-t84`, `tools/win/mesa/tests/sbtouchlat.cpp`, `sb-touchlat.bat`): injected touches
  (InjectTouchInput; taps 60 ms, holds 400 / 1500 ms, drags) into four kinds of window (plain, press-and-hold
  disabled by window property, gestures blocked, RegisterTouchWindow); logged per touch: when WM_POINTERDOWN, the
  promoted WM_LBUTTONDOWN, WM_LBUTTONUP, a right click, WM_GESTURE and WM_TOUCH arrive after the touch-down, and
  whether the click came "at once", "during the hold" or "at or after the release". Injection enters at win32k, so
  this measures Windows' part; the digitizer chain's part is in the touch driver's own counters of the user's
  session (user-pull before any test boot).
- **GL kit 0170** (src-tc 3d8ee0f, `sb-patches/0170`, `build/win/gd-kit-ec-0170`; switch: flag file `gltouch` or
  `SB_GL_TOUCH=1`, off by default): the kit runs inside the game and sees its window when the GL framebuffer is
  made. With the switch on it wraps the window procedure: a touch pointer going down becomes WM_MOUSEMOVE +
  WM_LBUTTONDOWN at that moment, moves become WM_MOUSEMOVE, the release WM_LBUTTONUP; the pointer message is
  answered so Windows promotes nothing, and mouse messages Windows still derives from touch are dropped. A second
  finger landing while one is down is a new press (up, then down), for two thumbs alternating; the button goes
  up when the last finger lifts. Only attached from the window's own thread; a real mouse passes through.
- **T84 part 2** (same boot): `sbtouchlat-x64.exe ... gl`, an x64 program with a GL window on kit 0170 (files in the
  test folder only), the switch off, then on. On should show the click with the touch-down.

### 6.110 The late touches' first cause, from the user's own session: the touch chip's edge zone (touchdiag-4)

(2026-11-16 16:04; read from `build/win/results-touch-user`, the pull of the user's 11-16 session; touchdiag-4 built, **not
run**.) The session ended without a clean shutdown, so the hive is dirty and its logs do not connect to the
primary file (primary sequence 262254, log entries 262622..262692: `hive-replay.py` applies none). For reading
only, all 71 log entries were applied to a scratch copy in sequence order; the touch driver rewrites its values
every second, so they are in every entry. The copy says (state 285 s into that boot):

- **The driver chain is not late.** 4840 interrupts, 132 presses, 131 releases, 4575 moves; DiagI2cError 0,
  DiagI2cRetry 0, DiagSpbSlow 0; the slowest touch read 9.4 ms, the slowest whole interrupt pass 9.5 ms; the chip's
  event stack never held more than 3 events (DiagEventsMax 3); reports completed 4835. So from the chip's interrupt
  to the HID report: about 1 ms, worst 10 ms.
- **The trace (last 64 events) shows two kinds of tap.** Away from the edge (x 554..3976 of 4096) a tap is press,
  then release 74..133 ms later: a finger. With **x < ~160** (twelve taps: x 38..154) the release follows the press
  after **8..10 ms, every time**: one scan of the chip, shorter than any finger can touch. They come at a tapping
  rhythm (every 0.5..0.7 s), and in runs at one spot (y ~3970: a corner of the panel).
- **What that is.** x < 160/4096 is the 56 panel pixels along one long edge. The Samsung driver for this chip
  (`ref/starlte-kernel/drivers/input/touchscreen/sec_ts`) has a grip "edge zone" whose chip default is **60**
  (`sec_ts_set_grip_type`: the zone is only sent when it is not 60; `SEC_TS_CMD_EDGE_AREA` 0xAB). The chip holds a
  touch that lands there back, to tell a tap from the hand gripping the phone, and reports press + release together
  once the finger has lifted; a hold there is a grip and is not reported. On Android the system sets these zones
  per orientation; on Windows nothing did. In landscape the long edge is the bottom (or top) of the screen, where
  the thumbs rest: a tap there reaches Windows when the finger LIFTS, i.e. late by the whole contact (~0.1 s and
  more), and holding there does nothing. That fits "touches arrive a lot late" while the game runs smoothly.
  Inference from the trace and the source; the chip has no public documentation.
- **touchdiag-4** (`build/win/touch-4/s6touchdiag.sys` 5b2cfc3f, build `td4-edge0`, signed, 0 LSE words, same
  pipeline as touchdiag-3): after sense-on at D0 it writes `AB 00 00` (edge zone 0 pixels). The write's status and
  the value go to `DiagEdgeStatus` / `DiagEdgeZone`; a failed write is not fatal (touch then works as before).
  Nothing else changed. `tools/win/touch.sh update OUT 4` pushes the file (the service already uses
  s6touchdiag.sys); `touch.sh update OUT` goes back to touchdiag-3.
- **Not known:** whether the chip takes 0 as "no zone" (the Android sysfs example uses 200), and whether the dead
  zone (0xAC) or the landscape command (0xAD) also matter. The answer is in the next user trace: taps with x < 160
  lasting as long as the others = fixed. Only a finger can test this; the check boot only shows the driver loads
  and the command is acknowledged.
- **Still open beside it:** Windows' own touch-to-click promotion for a mouse-only game (T84, 6.109) and the GL
  kit's switch (0170). Each is measured separately.
- **Also seen in the pull:** C: has 1.3 GB free of 22 GB (94 % used). No hibernation involved (HiberbootEnabled 0,
  no hiberfil.sys): the dirty hive is from the session's end, not from Fast Startup.

### 6.111 Two gap-list items built offline: a 32-bit driver (T85) and OpenGL for every program (T86)

(2026-11-16 17:06; both **compiled only**, staged, never run. Full write-ups: `docs/WIN11-GPU-X86.md`, `docs/WIN11-GPU-GLSYS.md`.)

- **32-bit x86 sbumd** (gap list item 4; Mesa branch `sb-x86` = 0167 + 3 commits, `sb-patches/0172`;
  `build/win/umd-x86/sbumd.dll` cc015f8e, PE32). Today 32-bit programs get no GPU: the adapter has no
  `UserModeDriverNameWow`. The audit found the UMD-to-KMD structures 32/64-clean (fixed-width, no pointers, 64-bit
  members on multiples of 8; checked by a compile-time layout test for i686, x86_64, aarch64 and arm64ec), so the
  kernel driver needs no change. GPU addresses stay 64-bit. Known limit: every visible buffer stays CPU-mapped, so
  a 32-bit game's GPU memory competes for its 2 GB of address space. Not known until a boot: whether ARM64
  Windows reads `UserModeDriverNameWow` for x86 processes at all (the test prints the names dxgkrnl.sys knows).
  Install: `tools/win/sbextra-install.sh x86 install|clear` (`tools/win/hive-umdname-wow.py`). **T85**
  (`sbwddm-t85`, the 0167 driver set so DWM is the same generation; `sb-x86.bat`): a 32-bit D3D11 program
  (`sbd3d32.exe`: exit 0 = on our driver with the right pixels), the kernel driver's own record of a 32-bit bind
  (`UmdNote_48/49`), then the first 12 d3d10core tests as 32-bit programs.
- **System-wide OpenGL** (gap list item 5; Mesa branch `sb-glsys` = the GL kit tree + `sb-patches/0171`). The
  adapter's `OpenGLDriverName` names `sbglicd.dll` (60 KB, ARM64X, C only, no initializer tables), which loads
  `sbgl-arm64.dll` or `sbgl-x64.dll` (ARM64EC) by process type and forwards the 19 `Drv*` entry points. Mesa's GL
  driver itself cannot be one ARM64X file with our linker recipe: each half has a C++ dynamic initializer (the
  GLSL compiler's `builtins`), the T77 pattern (6.106). Switch files in `C:\ProgramData\Starbite\glsys\`: `off`,
  `off-<exe>`, and `only` + `on-<exe>` (an allow list: the first test gives the driver to the test program
  alone). With a system DLL name the driver reads its flag files from `glsys\`, so the game's `glzc*`/`gltouch`
  switches do not reach other programs, and a process that cannot reach the Mali gets Microsoft's OpenGL instead
  of softpipe. Install: `tools/win/sbextra-install.sh gl install STAGE OUT [EXE...]|clear`
  (`tools/win/hive-opengl-icd.py`: three values, clear removes exactly those). **T86** (`sbwddm-t86`, T61's driver
  set; `sb-glsys.bat`): `sbgltest` as a native, an ARM64EC and an x64 program through the SYSTEM opengl32
  (renderer/version, 30 shader frames read back, swap timing), then the two ways back (switched off by file; no
  Mali in the process) and DWM unchanged. Not known until a boot: whether opengl32 falls back when the ICD
  refuses, REG_MULTI_SZ vs REG_SZ, whether a bare file name resolves to System32. GL tops out at 3.1; 32-bit
  programs are unchanged.
- `sbextra-install.sh` was rehearsed end to end on a fake phone folder with a copy of the user's hive (show, x86
  install, gl install with an allow list, show, gl clear, x86 clear): after both clears the adapter key equals
  the original and `System32\sbumd.dll` is untouched.
- **A mistake of mine found on the way:** `sb-smoke.bat`, `sb-rows.bat` and `sb-apps2.bat` read the KMD's live
  counters from `Services\sbwddm\State`, where they are not (they are in the device key `Enum\ACPI\STRB0072\0\
  Device Parameters\sbwddm`, and two of the four names did not exist). T80/T80b would have reported 0 timeouts
  after every step whatever happened. Fixed and restaged (T80, T80b, T81, T83) before any of them ran.

### 6.112 T78 re-read: not Edge. One compositor draw hangs while a XAML window (Calculator) is shown; diagnostics staged

(2026-11-16 17:17; a second, independent read of T78's data and of both drivers' code: `docs/WIN11-GPU-T78-REREAD.md`. It
**corrects 6.104**. The new builds are compiled only.)

- **Not Edge.** DWM's own log is clean for 100 s with Edge up and after Edge was killed. The first blocked window
  is 02:50:46-02:50:56; `calc.exe` was started at 02:50:43.8. 6.104 read the KMD's "190.9 s" as seconds since boot;
  the KMD counts from its own start (`adapter.c` `StartTime`), which puts the first hung chain 1.5-6 s after
  Calculator's start (the offset is inferred from where the blocked windows fall, hence the range).
- **Calculator was never closed in T78** (the script looked for `CalculatorApp.exe`; `Calculator.exe` is in the end
  task list), and the storm lasted from its start to the session's end. It paused only while sbsmooth's full-screen
  window covered the desktop (61.7 fps) and was back for the windowed test (0.5 fps).
- **The hung job is the same draw every time**: in all 31 hang records recovered from the hive (of 404), a DWM
  indexed-vertex job of 60 vertices and 144 indices, IDVS with a varying shader, at chain position 6-20, still
  ACTIVE at the 752 ms limit. Not one MMU fault in the session; memory was not short (836-1352 MiB free).
- **T71 had the same hang**: its hive holds 10 identical records in the session's last seconds, placed after the
  sign-out screen (LogonUI, also XAML) started. T79 (0167, 33 min of Geometry Dash) 0; T70, T72, T74 0. No boot on
  the user's T61 driver ever opened a XAML app beyond the shell, so T61 is not cleared.
- **The black screen shots are a consequence**: shots before the first hang are fine on the same driver; at a
  timeout the KMD drops the rest of that submission (`command.c` Reap) and reports it complete, so what DWM reads
  back was never drawn. The panel itself was not black (`Live_FbLiveSecs` 538 of 585 s).
- **Cause: not known.** Three cases the T78 data cannot tell apart: the job touches an address with no page; it
  writes an allocation that is on the read-only scratch page; or memory is fine and the vertex shader / tiler does
  not finish for this draw (a shader the old translator compiles wrongly would look exactly like this: ACTIVE, no
  fault). 0167 itself is unlikely (the KMD never reads the write bit) but not excluded.
- **Diagnostic kernel driver** (`build/win/kmd-b78/sbwddm.sys` be555bb4, signed, 0 LSE words; the unchanged tree
  rebuilds to the installed d85e0d3a apart from stamp and signature): at a job timeout it also records the hung
  job's 20 pointers with the GPU page-table state of each (none / page / scratch, the PTE attributes, the owning
  allocation, mapped or evicted, when last unmapped and listed), the state of the process's biggest allocation
  (DWM's tiler heap), the address-space status and lock registers, the MMU interrupt mask, and JS_HEAD at half
  the limit (stuck vs slow). Registry: `Live_HangPtrs`, `Live_HangPtrsFirst`. `Parameters\VtTimeoutMs` exists as a
  mitigation switch (default 750 = unchanged). I reviewed the diff line by line before building it.
- **Diagnostic sbumd b78** (Mesa branch `sb-b78` e2037d3 = 0167 + `sb-patches/0173`; `build/win/umd-b78` 57502837):
  log only (wall clock on the stats lines, a line when the KMD's timeout counter moves, opens and closes of other
  processes' allocations with the clock, a full-allocation-list counter, and after a timeout a bounded per-draw
  trace giving each job's GPU address, to match the KMD's JS_HEAD), plus one behaviour change: a resource's
  allocation is no longer destroyed after an idle wait that gave up (flag `b78nowait` = old way).
- **`sb-apps3.bat`**: the three clocks tied together; counters 5 and 12 s after every app start and after its close;
  the hang records saved to `umdlog\hang-<step>.txt` whenever the count grew; a step that keeps Calculator open
  (with Notepad on top, two shots, a task list) and then closes it.
- **Stages** (`tools/win/b78-stage.sh`, all with the diagnostic KMD): `sbwddm-t87s` (boot check of the pair),
  `sbwddm-t87` (apps3 on the pair; run a second time with the flag `noidvs`: does the hang need the IDVS path),
  `sbwddm-t87c` (the user's T61 sbumd: the control), `sbwddm-t88` (the new-tree sbumd with the flag `nirshaders`:
  DWM's shaders through the new translator; only after T81 passed). `tools/win/sbflag.sh on|off NAME` sets such
  flags from TWRP (they must exist before DWM starts).
- **Found on the way:** flag files under `C:\ProgramData\Starbite` and the umdlog are probably invisible to
  AppContainer processes (Calculator, Edge's GPU process: no sbumd log exists for them), so `noafbc` may read as
  absent exactly there. Not verified. And the new tree carries a follow-up to 0167 (`sb-nir` 75514cd: the write
  set goes to the device of the submit's fd, not the thread's last device) that the candidate d39b83ab lacks;
  found by reading, never run.

### 6.113 A shader cache on disk for the GL kit (T89 staged)

(2026-11-16 17:42; write-up: `docs/WIN11-GPU-SHADER-CACHE.md`. The cache logic, entry format and driver identity ran on the
Linux host; the Win32 file code is **compiled only**.)

- **Why:** our Mesa builds had `-Dshader-cache=disabled`, so every start recompiles every shader (GLSL to NIR to
  the Bifrost compiler, on three slow cores). Mesa 26.2 has no disk cache on Windows at all: meson refuses the
  option there, `disk_cache_os.c` is a TODO, and Panfrost's driver id comes from an ELF note a PE file lacks.
- **Added** (Mesa branch `sb-cache`, `sb-patches/0174` and `0175`): a Windows backend for the multi-file cache with
  the same layout and entry bytes as Linux, entries uncompressed (no zlib needed); the driver id = a hash of the PE
  link stamp, section table and Mesa commit, so every relink invalidates the cache and re-pushing the same kit does
  not; a damaged or truncated entry is rejected, deleted and rewritten; location
  `%LOCALAPPDATA%\Starbite\shader-cache`, 64 MiB limit, fails soft; off by `C:\ProgramData\Starbite\glnocache`.
  0175 fixes a Mesa bug the cache exposed (cached programs never reached `link_shader`, so the second start still
  recompiled every fragment shader: 96 hits / 48 misses on the host, 144 / 0 with the fix).
- **Host results:** Mesa's cache unit tests pass on the new backend; a GLES program on Panfrost over drm-shim gives
  identical output cold, warm, with every entry damaged, a broken index, an unusable directory, two processes, kills
  and a tiny limit; 300 programs: 362 ms without the cache, 63 ms warm (on the Mac: only the ratio carries over).
- **T89** (`sbwddm-t89`, T61's drivers, `sb-gdruns.bat`, kit `build/win/gd-kit-ec-cache`): Geometry Dash three
  times: A cold (cache empty), B warm, C with `glnocache`. Read: the kit's `shader cache: first frame, N s after
  process start` line of each run and its hit/miss counts (B should have 0 misses).
- **For sbumd (D3D):** Panfrost's binary cache would be hit as it is once the option is on; still needed there: the
  `noidvs` switch in the key, a per-half tag for the ARM64X DLL, and DWM / logon excluded at first. Not built.

### 6.114 Chain results, part 1: T81 (the new tree boots) and T84 (Windows holds a touch's click until the finger lifts; kit 0170 fixes it)

(2026-11-16 18:11; `build/win/results-t81`, `build/win/results-t84`; both measured on the phone.)

- **T81, PASS.** sbumd 63cb8404 (`sb-nir` 430b803: the new translator, geometry shaders, stream output,
  tessellation, the D3D11 DDI, the conformance fixes; all flags off) reaches the logon, DWM is the same process at
  the end, a blt-model window runs at 49.0 fps and a flip-model window at 60.3 fps on our adapter, the first 12
  d3d10core tests pass 12/12, and the KMD's counters are 0 (timeouts, resets, failed jobs, TDR engine resets). The
  boot crash of T77 is gone. This is the tree's first run on the phone.
- **T84 part 1: for a mouse-only window Windows sends the click when the finger LIFTS.** Injected touches
  (InjectTouchInput), times after the touch-down:

  | touch | WM_POINTERDOWN | WM_LBUTTONDOWN | verdict |
  |---|---|---|---|
  | tap, 60 ms | +10 ms | +65 ms (release +63) | at the release |
  | hold, 400 ms | +10 ms | +404..413 ms (release +402..408) | at the release |
  | hold, 1500 ms | +10 ms | +1503..1507 ms (release +1501..1505) | at the release |
  | drag, 150 px in 300 ms | +10 ms | +55 ms | when it starts to move |

  The same in all four window kinds: nothing registered; press-and-hold disabled by the tablet window property
  (and it is already off for the user: TouchMode_hold 0); every gesture blocked; RegisterTouchWindow (which does
  get WM_TOUCH at +10 ms, but the promoted click still comes at the release). So in a game that reads only the
  mouse (Geometry Dash) every tap arrives late by the whole time the finger is down, and a hold does nothing
  until the finger lifts, when down and up arrive together. The touch itself is there after 10 ms.
- **T84 part 2: GL kit 0170 turns the touch into the click itself.** A GL window on the kit (Mali-G72, Panfrost),
  switch off: as above (click at +68..90 ms for a 60 ms tap, +408..416 for 400 ms, +1513..1521 for 1500 ms). Switch
  on (`SB_GL_TOUCH=1` / flag file `gltouch`): **WM_LBUTTONDOWN at +10..27 ms** for every tap, hold and drag,
  WM_LBUTTONUP at the release; the kit's own count: 13 presses, 13 releases, 333 moves, 0 stray touch-made mouse
  messages. Not yet run inside the game itself (the test window is ours; GLFW's handling of the handed-over
  messages is by reading only).
- **touchdiag-4 check, PASS:** DiagBuild `td4-edge0`, TouchD0Status 0, **DiagEdgeStatus 0** (the chip acknowledged
  `AB 00 00`), DiagEdgeZone 0, both self-tests 0 errors of 200. Whether "0" really removes the edge zone needs a
  finger: the user's next session's trace (taps with x < 160 lasting as long as the others).
- **So "touches arrive a lot late" had two causes on top of each other:** the chip held edge taps until finger-up
  (6.110), and Windows holds every tap's click until finger-up for a mouse-only game. The second is the bigger
  one (it applies everywhere on the screen). The fix for the second is the kit switch, per game.

### 6.115 Chain results, part 2: T82, the new tree's first conformance scores

(2026-11-16 18:31; `build/win/results-t82`, sbumd 63cb8404; measured. 14 minutes for eight passes: a test function takes
0.1-2 s.)

| pass | flags | suite | pass | fail | crash | note |
|---|---|---|---|---|---|---|
| (T74) | the user's T61 driver | d3d10core | 74 | 17 | 20 | the baseline of 11-16 |
| O | none (old translator) | d3d10core | **95** | 5 | 11 | 21 more pass, none lost |
| N | nirshaders | d3d10core | 84 | 19 | 8 | 13 tests that pass in O fail here; 2 better |
| N | nirshaders | d3d11 (163) | 122 | 31 | 10 | the D3D11 suite at the D3D10-level interface |
| G | nirshaders gsemu | d3d10core | 88 | 23 | **0** | geometry_shader, stream_output, clear_state, create_shader newly pass |
| M, D, F, T | ... msaa ... | | | | | **do not count: ran on Microsoft Basic Render Driver** |
| X | nirshaders | dxgi (55) | 37 | 12 | 6 | |

- **The conformance fixes work on the phone:** with every new feature off, the new tree passes 95 of 111 against the
  user's 74, and nothing that passed before fails. Left in O: 5 fails (combined_clip_and_cull_distances, depth_bias,
  immediate_constant_buffer, pipeline_statistics_query, stream_output) and 11 crashes (clear_state, clip_distance,
  create_shader, cube_maps, geometry_shader, index_buffer_offset, layered_rendering, multiple_viewports,
  sample_c_lz, stream_output_resume, unbound_multisample_texture).
- **The new translator is not ready to be the default:** 13 tests that pass with the old one fail with it
  (buffer_srv, copy_subresource_region, depth_stencil_sampling, initial_texture_data, null_sampler,
  rtv_depth_slice, shader_input_registers_limits, swapchain_flip, texture1d, texture, texture_compressed_3d,
  unbind_shader_resource_view, update_subresource): they look like one or two common texture load/sample bugs.
- **Geometry shaders run on the phone** (pass G: no crash left, geometry_shader and stream_output pass).
- **`msaa` makes Windows refuse our driver** (every test of M, D, F, T on the Basic Render Driver, as in T76b), so
  the D3D11 interface, feature level 11 and tessellation were **not tested**. T82b (`sbwddm-t82b`) repeats D, F, T
  without `msaa`, plus the d3d11 and dxgi suites on the old translator.
- **Step 3 of the chain did not run:** the store update for the new row failed on the main session's side
  (`--omit store-to-card-2.img: no such entry`), so the selector was not installed and T83/T83b are still to come.
- In work offline, on this data: the 13 translator regressions, the msaa refusal, the remaining crashes.

### 6.116 Chain results, part 3: T87 pins the stall on one compositor vertex shader that does not finish

> **Corrected by 6.118:** T87n shows the vertex shader finishes; the stuck stage is the tiler. "One shader" below is true of the draws, not of the cause.

(2026-11-16 18:54; `build/win/results-t87s`, `build/win/results-t87`; measured with the diagnostic KMD be555bb4 and sbumd b78.)

- **T87s, PASS:** the diagnostic pair boots (blt 49.6 fps, flip 58.8 fps, 12/12, counters 0).
- **T87 reproduces T78 on purpose, step by step** (job timeouts after each step): logon 0; Settings 0; **Calculator:
  1 after 5 s, 10 after 12 s, 22 when closed**; Notepad, Paint, Explorer, Task Manager: stays 22; **Calculator
  kept open: 24, 34, 46 (20 s), 60 with Notepad on top, 85 after two shots**; 5 s after closing it 96, 15 s after
  100; then nothing more, **Edge included (stays 100)**. So: Calculator on screen = about one timeout a second;
  Calculator gone = it stops within seconds. Not Edge, not the other classic apps.
- **What the hung job's memory looks like** (`umdlog/hang-*.txt`, `Live_HangPtrs`, every record alike): all 16
  pointers of the job (next, indices, tiler context, position, uniform buffers, samplers, push uniforms, state,
  varying buffers and descriptors, viewport, thread storage, the vertex draw's state, uniforms, push, thread
  storage) are `[1 2c1 d ... M -1]`: a page is there, read-write, owned by DWM, mapped, never unmapped. DWM's
  tiler heap (64 MiB) is mapped at its first, middle and last page. None of DWM's 419 allocations is on the scratch
  page. The address space is not busy (`as 0x0`), MMU interrupts are unmasked (`mmumask 0xff00ff`), no fault is
  pending. **`half-head` = `head`**: at half the limit the job slot is at the same job as at the limit.
  So it is not missing memory, not an evicted allocation, not a locked address space and not a slow chain:
  **one job is stuck with everything it points at in place.**
- **Which job:** an indexed-vertex (IDVS) job of 60 vertices and 144 indices, as in T78. DWM's draw trace (sbumd
  b78, 1222 draws logged after the first timeout) has 377 draws of exactly that shape, **all with one vertex
  shader** (`vs 00000209B5379880`, code at GPU 0xa246000; that shader is used by 387 draws in all, 377 of them
  60/144). Most of those draws complete; about one a second does not.
- **Reading:** the remaining explanation is the vertex shader itself (as the old translator and the Bifrost
  compiler turn it out) or the tiler on its output: for some input the job never ends. Which shader it is and what
  is in it is not known yet. The record does not include the vertex buffers behind the attribute descriptors
  (one indirection further than the pointers recorded), so "the vertex data is missing" is excluded only by the
  absence of any MMU fault, not by a direct look.
- **Next, staged:** sbumd b78b (`sb-b78` c3b663c, `sb-patches/0176`, e506e2bd): every vertex-shader variant logs its
  pointer and GPU code address, and with the flag file `nirdump` writes its NIR and compiled binary to
  `umdlog\nir-<pid>-<n>.txt`. **T87d** (`sbwddm-t87d`, the same test with `nirdump`) names the shader and shows its
  code. Already queued and now more telling: T87c (does the user's T61 driver hang the same way), T87n (`noidvs`:
  the same shader as separate vertex and tiler jobs: which half sticks), T88 (the new translator compiles the
  shader differently: does the hang go away).

### 6.117 Chain results, part 4: T87c, the user's own driver stalls the same way

(2026-11-16 19:05; `build/win/results-t87c`: the user's T61 sbumd 6d759c44 with the diagnostic KMD be555bb4; measured.)

- **The same storm on T61.** Job timeouts after each step: logon 0; Settings 0 (one failed job, no timeout);
  **Calculator: 0 at 5 s, 11 at 12 s, 35 when closed**; Notepad, Paint, Explorer, Task Manager: stays 35;
  **Calculator kept open: 38, 48, 59 (20 s), 73 with Notepad on top, 99 after two shots; closed: 109 and it stops**;
  Edge: stays 109. The Calculator shot is 100 % black, the two calc-hold shots 47 % black, every other shot fine.
- **The same job:** indexed-vertex, 60 vertices (Invocation 0x3b), 144 indices (0x8f), primitive word 0x18178208,
  negative base vertex offset; every recorded pointer `[1 2c1 d ... M -1]`, tiler heap mapped 1/1/1, 0 scratch,
  `half-head` = `head`. Identical to T87 on the 0167 driver.
- **So the stall is not a regression of 0159-0167.** It is in the driver the user runs today, and it shows
  whenever a window of that kind is on screen (Calculator; by T71's record the sign-out screen too). The user's
  earlier report of a desktop too slow to reach the Start menu fits (the Start menu is the same UI toolkit), but
  that link is a guess, not measured.
- **Consequences.** (1) The reason the 0167 candidate was put on hold (6.104) does not distinguish it from T61:
  on this point both behave alike, and 0167 adds working flip-model windows and the 60 fps game path. Whether it
  replaces T61 as the default is the user's call again. (2) Finding the shader's fault fixes a problem of the
  everyday Windows, not only of the new builds.
- Caveat: T87c ran T61's sbumd on the diagnostic KMD, not on the user's d85e0d3a. The KMD difference is the hang
  records only (and `VtTimeoutMs` at its default).

### 6.118 Chain results, part 5: T87n, it is the tiler, not the vertex shader; a lead in our own wait code; three switches staged

(2026-11-16 19:22; `build/win/results-t87n`: sbumd b78 with the flag `noidvs`, diagnostic KMD; measured. The lead and the
switches are from reading; not run.)

- **T87n:** with IDVS off the storm is the same (Calculator: 9 after 12 s, 20 when closed; kept open: up to 83, 93
  after closing; 0 from every other step), but every hang record now says **`job tiler`** with its dependency (the
  separate vertex job of the same draw) already done: `tiler idx 34 dep 33/32`. So the vertex shader ran to its
  end. **6.116's reading (the shader does not finish) is wrong.** What sticks is the tiler: the stage that reads
  the index buffer and the shaded positions and bins the triangles. With IDVS that work is inside the
  indexed-vertex job, which is why that job hung there.
- **What can make a tiler never finish with all memory present:** its input. Either the positions it is handed are
  bad, or the indices do not belong to the vertex range the job was built for (the job carries a base vertex
  offset = minus the smallest index the driver found when the draw was issued; an index outside that range makes
  the tiler read "positions" from memory that is not this draw's).
- **A lead in our own code (read, not shown to be the cause):** DWM streams XAML geometry through dynamic vertex
  and index buffers (the hung draw's base vertex offset is negative and different every time; the index pointer
  walks through a buffer and jumps to a new one every few frames). When such a buffer is mapped with DISCARD,
  Panfrost asks the shim whether its BO is idle and, if so, lets the app overwrite it in place. The shim's
  `WAIT_BO` reads the number of the submission that last named the BO **before** `sb_wait_seq` sends a
  held-back vertex/tiler chain (T30's merge) and gives that chain's BOs their number; `wddm_idle` and
  `SYNCOBJ_WAIT` have the same order. For a BO that only a held chain names, the answer is then "idle". If that
  can happen in DWM (it needs a wait between a held vertex/tiler chain and its fragment chain: another thread on
  the device, or a fragment submit that failed), the app rewrites indices or vertices under a queued job.
  I could not show from the code that the window is reachable in DWM's single render thread, so this is a lead,
  with the general pattern (a buffer reused in place while a job still reads it) as the thing to test.
- **sbumd b78c** (`sb-b78` 6042775, `sb-patches/0178`, ba4fbe2e; includes the shader dump of b78b), three flag files,
  each read once per process:
  - `newbo`: a whole-buffer DISCARD map never reuses the BO in place. If the timeouts stop, a dynamic buffer was
    rewritten under a job (whatever said "idle");
  - `nomerge`: no vertex/tiler chain is held back (every chain its own Render);
  - `seqfix`: the held chain is sent before a wait reads what to wait for (the three places above).
- **T91** (`sbwddm-t91`, diagnostic KMD + b78c, `sb-apps3.bat`): three runs, one flag each. Read: the Calculator
  steps' timeout counts against T87's (22 and 100).

### 6.119 Chain results, part 6: T88, the new translator in the compositor: the tiler hang stays, and the desktop is black (the known texture bug)

(2026-11-16 19:30; `build/win/results-t88`: sbumd 63cb8404 with the flag `nirshaders` present at boot, diagnostic KMD; measured.)

- **The Calculator hang is unchanged:** job timeouts 0 until Calculator, 28 when it is closed, 105 after the
  kept-open step, nothing from the other apps; the hung job is again the indexed-vertex job. A different shader
  compiler for the same draw does not help, as T87n already implied (it is the tiler's input, not the shader).
- **The desktop itself is broken with this build's new translator:** 2418 failed jobs before the first test step,
  9994 by the end, all without a timeout: fragment jobs ending with job status **0x5A (address range fault)**; the
  first failure of the boot was 0x51 (invalid instruction encoding). Every screen shot is 100 % black.
  This is fix 1 of build A seen from the compositor's side (the translator did not declare the shader's textures,
  so the shader descriptor says "0 textures" and every lookup is out of range). T90 measures build A; a run of
  this test on build A with `nirshaders` is the check that the desktop composes with the new translator.
  The single 0x51 is not explained by that fix and has to be looked for in T90's records.
- So `nirshaders` must stay off for DWM on 63cb8404 (it is off by default; this run set it on purpose).

### 6.120 Chain results, part 7: T85, 32-bit programs draw on the Mali for the first time

(2026-11-16 19:38; `build/win/results-t85`: the x86 sbumd cc015f8e in SysWOW64 with `UserModeDriverNameWow`, the 0167 driver
set for 64-bit; measured.)

- **Windows hands 32-bit programs our driver.** dxgkrnl.sys knows the value name (`X86 dxgkrnl knows
  UserModeDriverNameWow`; it also has `...Wow2`, `...X64`, `...Wsa32/64`, `...WSL`). The 32-bit test program:
  `device: feature level 0xa000 on "Starbite Display (Galaxy S9 DECON)"; sbumd.dll LOADED`, `PASS clear`,
  `PASS triangle`, exit 0. The kernel driver's own record of a 32-bit bind is there (`UmdNote_48` 1, the pid in
  `UmdNote_49`).
- **The first 12 d3d10core tests as 32-bit programs: 12 pass**, none on another adapter. DWM alive, KMD counters 0.
- So the audit's claim holds on the phone: the kernel interface is the same for 32-bit callers, no kernel change.
- **Not yet known:** the whole suite as 32-bit; a 32-bit program presenting to a window while DWM runs the user's
  T61 driver (this run had 0167 on the 64-bit side); real 32-bit apps; memory behaviour in a 2 GB address space.
  **T85b** (`sbwddm-t85b`: T61's driver set + the x86 driver + all 111 d3d10core tests as 32-bit,
  `sb-x86-full.bat`) covers the first two.
- `UserModeDriverNameX64` exists as a value name: x64 processes could be given a driver file of their own, which
  would be a simpler way than one ARM64X file for both. Not used.
- The chain cleared the value again after the test, as planned.

### 6.121 Chain results, part 8: T86, the system's OpenGL loads our driver in native and x64 programs

(2026-11-16 19:46; `build/win/results-t86`: `sbglicd.dll` + `sbgl-arm64.dll` + `sbgl-x64.dll` registered with an allow list of the
three test programs, T61's D3D driver set; measured. `GLSYS VERDICT: PASS`.)

- **All three kinds of process get the hardware driver through the SYSTEM opengl32.dll:** a native ARM64 program,
  an ARM64EC program and a plain x64 program each report `renderer "Mali-G72 MC18 (Panfrost)", GL 3.1`, 864 pixel
  formats (828 from the ICD), **30 of 30 shader frames read back right**, glGetError 0. The front's own lines show
  the ARM64 half loading `sbgl-arm64.dll` and the x64 half `sbgl-x64.dll`.
- **Speed in a 640x480 window** (the kit's default GDI present): 41.4 fps native, 46.8 ARM64EC, 47.4 x64, 65-72 of
  300 frames over 50 ms. The first `DescribePixelFormat` took 2.5-2.8 s in the first two processes (driver load and
  screen creation), 0.15 s in the third.
- **Both ways back work:** switched off by `glsys\off-<exe>` the program gets Microsoft's "GDI Generic" GL 1.1 with
  our DLL not loaded; a process that cannot reach the Mali gets the same with the DLL loaded and no ICD pixel
  formats; neither crashes. So opengl32 does fall back when the ICD refuses, REG_MULTI_SZ is accepted for
  `OpenGLDriverName`, and a bare file name resolves to System32 (three things that were unverified in 6.111).
- DWM is the same process at the end.
- The chain cleared the registration after the test. Still open before it is installed for every program: the
  present path (GDI, ~45 fps here; the zero-copy path is behind the game kit's flags), GL 3.1 only, no 32-bit
  build, and a first real program other than our test.

### 6.122 Chain results, part 9: T89, the shader cache did not switch on (the run measured nothing about it)

(2026-11-16 20:22; `build/win/results-t89`: T61's drivers, GL kit `gd-kit-ec-cache`; measured.)

- Runs A and B (no flag files) both log only `sbumd: shader cache: off (switched off, or its directory cannot be
  used)` and `off; first frame, 21.5 s / 19.2 s after process start`. There is no `gl shader cache:` decision line
  and no `on, <dir>` line. Run C (`glnocache`) prints its own off line as designed and was cut short by the test's
  time limit (each game run takes ~12 minutes, three do not fit in 1800 s).
- So on Windows the cache is refused somewhere between "not switched off" and "directory usable", without saying
  why. The Win32 file code had only been compiled, never run; this is that gap. Sent back with the evidence:
  find the failing step, log every failure with its error code, fall back to a directory under
  `C:\ProgramData\Starbite` when the per-user one cannot be made.
- Useful by-product: the game runs on this kit (the 0170 code base) on a standard row through the GDI present as
  before; Steam's cold start took 135 s to the game, the second start 10 s.

### 6.123 Chain results, part 10: T83 / T83b, both rows boot the right driver

(2026-11-16 21:04; `build/win/results-t83/ng` and `/std`; the selector 7f6214b0 installed, row `win11-ng` in the store; measured.)

| | T83, row `win11-ng` | T83b, row `win11-big` |
|---|---|---|
| WtDat that Windows recorded | **63476** = new GPU | **65063** = standard |
| selector lines (one per process) | 14 x `-> sbumd-ng.dll`, 0 x `sbumd.dll` | 13 x `-> sbumd.dll`, 0 x `sbumd-ng.dll` |
| DWM | `row NEW-GPU -> sbumd-ng.dll`, survived | `row standard -> sbumd.dll`, survived |
| loaded at the end (this account) | ng 2, std 0 | ng 0, std 2 |
| blt window / flip window | 51.2 / 61.1 fps | 64.7 / 67.2 fps |
| Geometry Dash, GL kit 0169, no flag files | `row: new GPU`, zero-copy + present thread, **57-58 fps** in its 300-frame windows, 0 present failures | no zero-copy lines = GDI present, **32.0 fps** average (as with kit 0158) |
| KMD counters | timeouts 0, resets 2, failed 2 | all 0 |
| verdict | `ROWS VERDICT PASS` | `ROWS VERDICT PASS` |

- **The row signal works:** the loader's 80 s reaches Windows as WtDat 63476 through the firmware, and the 82 s
  rows read 65063. The selector picks by it in every process that logged a line, DWM included (so the State key is
  readable from DWM's account).
- **The standard row is unchanged in what it loads** (`System32\sbumd.dll` 6d759c44, checked by hash before and
  after the install) and now loads it through the selector; nothing in T83b differs from earlier runs on that row.
- **The game kit is row-aware as designed:** the same two files give the fast path on the new row and the old
  path on the standard one. Kit 0170 (the same plus the touch switch) is the one to install.
- Cosmetic: on the new row sbcomp prints "sbumd NOT loaded" because it looks for a module called sbumd.dll.
- The 2 failed jobs on the new row are not explained (no timeout; the records were not saved by this script).
- Not tested: the plain `win11` row (no watchdog, cannot run unattended); it reads whatever S-Boot left in the
  watchdog register, which is not 80 s.

### 6.124 Chain results, part 11: T91 `newbo`, the lead of 6.118 is refuted; `idxcopy` staged (T92)

(2026-11-16 21:19; `build/win/results-t91-newbo`: sbumd b78c with the flag `newbo` (its line is in DWM's log), diagnostic KMD;
measured.)

- **No change:** Calculator 2 / 11 / 23, kept open up to 87, 113 after closing; the Calculator shot 100 % black.
  With a whole-buffer DISCARD map never reusing a BO in place, the tiler still hangs. So the stall is **not** a
  dynamic buffer rewritten in place after a DISCARD (6.118's lead, in the form `newbo` tests). The `seqfix` change
  is still correct code (a wait must not read its number before sending the held chain) but it is not this bug.
- Also checked by reading, and found in order: Panfrost's min/max index cache is invalidated on every write map
  (d3d10umd maps the whole buffer, so the whole cache goes); the adapter's memory segment is not cache-coherent
  and VidMm maps CPU views write-combined, so a CPU write is in memory before the submit (only allocations with
  `MW_ALLOC_CPU_CACHED`, the GL kit's back buffers, are cached, with explicit cache operations).
- **Next split, sbumd b78d** (`sb-b78`, `sb-patches/0179`): flag file `idxcopy`: an indexed draw from an index
  buffer gets a private copy of its indices in the batch's memory at draw time, and its vertex range is computed
  from that copy. **T92** (`sbwddm-t92`): the same test with `idxcopy`. Timeouts gone = the indices the tiler read
  were not the ones the job was built for; unchanged = indices cleared, and what is left is the positions the
  vertex stage hands the tiler (then: dump them).
- Still worth running: `nomerge` and `seqfix` on `sbwddm-t91`, and `nirdump` (which also runs on the t92 build).

### 6.125 Chain results, part 12: `nomerge` and `seqfix` unchanged too; the kernel driver now saves the hung draw's indices and positions (T93)

(2026-11-16 21:45; `build/win/results-t91-nomerge`, `results-t91-seqfix`; measured. The new KMD is compiled only.)

- **`nomerge`** (no vertex/tiler chain held back; `b78c switches: nomerge 1` in DWM's log): Calculator 21, kept open
  85, after closing 111. **`seqfix`**: 21, 85, 95. Both as without any switch. With `newbo` (6.124) that is three
  suspects of our own buffer and submission handling cleared: not a buffer reused after DISCARD, not the chain
  merge, not the wait order.
- **Stop guessing, look at the data.** Diagnostic KMD **b78e** (`build/win/kmd-b78e/sbwddm.sys` d6e4a0c6, signed, 0 LSE
  words): when a vertex/tiler chain is about to time out, the poll routine also maps (as it already maps the job's
  page: only RAM pages the GPU's own page table has there, into address space reserved at AddDevice) the pages of
  the hung draw's **index data** and of its **position buffer**, and the hang record takes up to 512 bytes of
  indices and 1024 bytes of positions. Registry: `Live_HangData` (the newest hang) and `Live_HangDataFirst`:
  the indices as numbers, the job's base vertex offset and invocation word, the positions as hex dwords.
  Read offline: are all indices inside the vertex range the job was built for (index + base in 0..count-1)? Are
  the positions finite and sane?
- **T93s / T93** (`sbwddm-t93s`: boot check of KMD b78e + sbumd b78d; `sbwddm-t93`: `sb-apps3.bat`, no flag files; the
  script now also saves the two new values in `umdlog\hang-<step>.txt`). T93 answers what T92 (`idxcopy`) tests
  indirectly, and for the positions as well.

### 6.126 Chain results, part 13: T90s / T90, fix build A: 99 of 111 with everything off; the run stalled when `nirshaders` went on system-wide

(2026-11-16 22:40; `build/win/results-t90s`, `build/win/results-t90`: sbumd a8a6dca8 (`sb-nir` ea8a080), the user's kernel
drivers; measured.)

- **T90s, PASS:** build A boots (blt 48.0 fps, flip 60.7 fps, 12/12, counters 0).
- **T90 pass O (all switches off): 99 pass, 4 fail, 8 crash**, exactly build A's prediction (T82: 95 / 5 / 11; the
  user's T61: 74 / 17 / 20). Left: FAIL pipeline_statistics_query, immediate_constant_buffer, stream_output,
  combined_clip_and_cull_distances; CRASH layered_rendering, create_shader, clear_state, index_buffer_offset,
  geometry_shader, stream_output_resume, clip_distance, multiple_viewports.
- **Then the run stopped.** The pass script logged "pass N ... flags nirshaders" at 19:56:34 and created the flag
  file; nothing more was logged in the remaining 35 minutes (the test boot's own timer restarted Windows at 2401
  s). The test runner for pass N never opened its log; no test process started after the flag (the newest of the
  112 driver logs of test processes is from pass O).
- **What the flag did meanwhile:** it is a file, so it applies to every process. Windows' own Settings app started
  65 s later and its driver log begins `shader translator: DXBC -> NIR`. The kernel driver counted **134 failed
  jobs** by the end of the boot, none before the flag: every record is a fragment job with status **0x51
  (invalid instruction encoding)**, no MMU fault, no timeout. So shaders of a XAML program, through the new
  translator and the Bifrost compiler, contain instructions this GPU (architecture 6) rejects. The same status
  was the first failure of T88's boot.
- **Why the script stalled is not established.** The system was alive (48,621 submissions, 5,776 presents, a clean
  timed restart). The runner flushes every line it prints and has a 60 s limit on its first child, so it either
  never started or was blocked at start; a console host that stopped responding would do that, but nothing
  recorded shows it.
- **Consequences.** (1) Passes N, G, M, D, F, T, X of build A did not run: the translator fix, the MSAA fix, the
  D3D11 interface and tessellation are still unmeasured. (2) A feature switch must not be a system-wide file
  during a test: `sb-wt-env.bat` sets `SBSW_<name>=1` in the test processes' environment instead (the driver side
  comes with fix build B) and starts a one-minute heartbeat (`sb-beat.bat`: the time and the programs that do
  not respond). (3) Fix round B is in work on this data: the environment override, the 0x51 encodings, the 12
  tests left in pass O, and the shader dump for the new tree.

### 6.127 Fix build B: switches per process, and a dump of the shaders the GPU rejects (T94s, T94, T95 staged)

(2026-11-16 22:51; Mesa branch `sb-nir` f36a7ef = build A + 2 commits, `sb-patches/0180`, `build/win/umd-nir-fixB/sbumd.dll`
7bd85fde; ARM64X check and the initializer check pass; **compiled only**. The full round B (the 0x51 encodings, the
12 tests left in pass O) was cut short when the night's model limit was reached; what is here is what the
morning's tests need first.)

- **Every switch per process tree** (`fixB-1`): `SBSW_<NAME>=1` in a process's environment turns the switch on there
  (and in its children), `=0` off even if the flag file exists; no variable = the flag file decides, as before.
  Covers every switch read through `malikm_switch` (nirshaders, gsemu, msaa, d3d11ddi, fl11, tessemu, noidvs,
  noafbc, devstate, zcro2, zcnolist2, the umdlog trace files) and the three that read the file directly before
  (noidvs in Panfrost, the trace in Debug.cpp, the device flags). One log line per switch the environment decides.
  `sb-wt-env.bat` uses it: a conformance pass's features reach the test programs only.
- **The shader dump** (`fixB-2`): with the switch `nirdump`, every shader the NIR translator is given is written to
  `umdlog\dxbc-<pid>-<n>.dxbc` as a DXBC container (the token stream + a chunk "SBSG" with the DDI's signature
  entries), and every compiled variant of every stage logs `sbumd: <stage> variant <ptr> code 0x<gpu address>,
  <n> bytes`; with `nirdump` also its NIR and binary in `umdlog\nir-<pid>-<n>.txt`. The host harness reads `.dxbc`
  files (round trip checked: 40 Wine shaders converted to that format translate, validate and compile exactly as
  their originals). So the shaders that gave INSTR_INVALID_ENC on the phone can be compiled on the Mac with the
  compiler's assertions on, which names the instruction.
- **Staged:** `sbwddm-t94s` (boot check), `sbwddm-t94` (the eight passes O N G M D F T X through `sb-wt-env.bat`,
  with the heartbeat), `sbwddm-t95` (`sb-apps3.bat` with the flag FILES `nirshaders` and `nirdump` set from TWRP for
  the whole boot, as T88 did: DWM, Settings and Calculator through the new translator, every shader dumped). All with
  the user's kernel drivers.

### 6.128 Chain results, part 14: T93s / T93, the stuck draw's indices are valid; its positions are all zero

(2026-11-17 01:52; `build/win/results-t93s`, `build/win/results-t93`: diagnostic KMD b78e d6e4a0c6 + sbumd b78d; measured.)

- **T93s, PASS:** the KMD that maps the hung draw's data boots (blt 48.3 fps, flip 60.9 fps, 12/12, counters 0).
- **T93:** the storm as always (Calculator: 2 after 5 s, 23 when closed, 115 after the kept-open step); 11 hang
  records with the new data (`umdlog\hang-*.txt`, `Live_HangData`):
  - **indices valid in every record:** 144 indices of 16 bits, values 840..899 with base vertex offset -840, so the
    tiler addresses vertices 0..59 of a 60-vertex draw; none outside. (So `idxcopy`, T92, is already answered.)
  - **positions:** in **7 of the 9 records** that captured them, all 60 positions are **exactly (0, 0, 0, 0)**: w = 0,
    no screen position at all. The other two hold values that look like leftover memory ((1,1,1,1) alternating
    with (x, 0.5, 0, 0)), not positions of this draw. Two records had no position view (the page was not mapped).
- **Reading.** With IDVS the position shading happens inside the stuck job, so this alone does not tell "the vertex
  shader computed zeros and the tiler cannot bin w = 0" from "the job stuck before it wrote anything". Two runs
  decide, both staged:
  - **T93n** = T93 with `noidvs` (asked of the main session for tonight): the vertex job ends first (T87n), so its
    positions are what the tiler read. Zeros there = the vertex shader really outputs zeros for this draw.
  - **T96** (`sbwddm-t96`, sbumd **b78f** a11903c1 = b78d + `sb-patches/0181`, flag `posguard`): a vertex position
    of exactly (0,0,0,0) is replaced by (0,0,0,1) (a zero-area point, culled). Timeouts gone = the tiler hangs on
    w = 0 input, and the guard is a working fix while the reason for the zeros is looked for (a constant buffer
    the shader reads as zero is the first suspect: w = 0 for every vertex needs the transform's last row to be 0).
  The T92 `nirdump` run (tonight) writes that vertex shader's NIR and binary.

### 6.129 The stuck draw's vertex shader: its position is the vertex data itself. Likely cause: no store barrier before a submission (T97 staged)

(2026-11-17 02:17; `build/win/results-t92-idxcopy`, `results-t92-nirdump`; measured, plus reading. The fix is compiled only.)

- **T92 `idxcopy`:** unchanged (18 timeouts when Calculator is closed, 108 after the kept-open step), as T93's valid
  indices predicted. Its log also showed the flag FILE `nirshaders` still on the phone (left by T90's stalled run;
  the restores do not touch flag files). Harmless for every build that ran tonight (none has the new translator);
  the main session removes it after the night's last restore.
- **T92 `nirdump`:** 285 vertex-shader variants dumped, 52 of them DWM's. The draw trace names the hung draws'
  shader by its code address 0xa12f000: `umdlog\nir-1096-4.txt`, 3648 bytes, IDVS, 4 constant buffers, 6 vertex
  inputs, 8 outputs (a composition shader with a perspective transform for 3D-transformed visuals). Its position
  output is
  `vec4(in0.x * cb1[0].x + in0.z * cb1[0].z,  in0.y * cb1[0].y + in0.z * cb1[0].w,  0.0,  in0.z)`:
  **w is vertex attribute 0's z**, and x and y come from attribute 0 too. So T93's all-zero positions mean the GPU
  **read zeros where attribute 0's data should be**, for all 60 vertices of the draw. Not a shader that computes
  zeros, not the constant buffers.
- **Why the GPU would read zeros (or stale data, the two odd records of 6.128):** DWM writes these vertices just
  before the draw, through a CPU mapping of the buffer; our segment is not cache-coherent and VidMm maps it
  write-combined. On ARM64 such stores sit in the writing core's write buffer until they drain; an observer outside
  the CPU (the GPU) sees them only after a store barrier on that core. Panfrost on Linux gets that barrier for free:
  the submit ioctl rings the GPU from the same CPU behind the kernel's own barrier. Under WDDM, `Render` only
  queues: the GPU is started later by `DxgkDdiSubmitCommand` on VidMm's scheduler thread, usually on another core,
  and **nothing in our user-mode driver issues any barrier** (no DSB/DMB in `malikm_drm.c`; checked). So a draw can
  start before its freshly written vertices are in memory. With most shaders that means one frame of wrong
  geometry nobody notices; with this one it means w = 0 and a tiler that never finishes. That fits every
  observation: intermittent (~1 per second), only while that kind of window is composed, the same draw shape, the
  vertex job finishing (T87n) and the tiler sticking, no MMU fault, buffer reuse irrelevant (T91 `newbo`: new pages
  are zero too), and present in the user's T61 driver (same shim).
- **Fix candidate, sbumd b78g** (`sb-b78` 7859ad8, `sb-patches/0182`, `build/win/umd-b78g` 8591b165): `DSB SY` on the
  submitting core at the top of `w_render`, before every submission to the kernel; flag file `nodsb` turns it off
  for an A/B. The instruction is in both halves of the linked DLL (checked by disassembly).
- **T97** (`sbwddm-t97`: KMD b78e + sbumd b78g, `sb-apps3.bat`, no flags): timeouts 0 with Calculator open and kept
  open = confirmed. Then the barrier goes into every driver tree: the user's T61 line, 0167, the new tree, and the
  GL kit (same shim; Geometry Dash's vertex data takes the same path).
- Still running tonight: T93n (`noidvs` positions: should be zeros too), T96 (`posguard`: hides the symptom).

### 6.130 The store barrier in the drivers the user runs (ready for when T97 confirms)

(2026-11-17 02:22; **compiled only**; `sb-patches/0183`, `build/win/patchwork/add-dsb.py`, `build-dsb.sh`.)

- **Which source the user's driver is:** T61's sbumd 6d759c44 was built at 08:19 on 11-13 in `build-int-x`
  (`sbumd-arm64x.6d759c44.dll`), one minute before `src-int` commit fbc32c3 (0155) was recorded: it is fbc32c3.
- **T61 + barrier:** `src-dsb61` = fbc32c3 + 0183 only, `build/win/umd-dsb61` **63b76fff**. **0167 + barrier:**
  `src-dsb67` = b8730f0 + 0183 only, `build/win/umd-dsb67` **61fc9096**. Both: ARM64X check 0 fails, 0 initializers,
  the DSB SY in both halves (disassembly). Nothing else differs from what the user runs.
- **Staged:** `sbwddm-t98` (the user's kernel drivers + T61+barrier, `sb-apps3.bat`) and `sbwddm-t98n` (+ 0167+barrier):
  the Calculator test on exactly the drivers that would be installed. Install sets: `sbwddm-user-dsb61` (for
  `gpu-cycle --install-default`, the standard rows) and `sbwddm-ng-dsb67` (`sbsel-install.sh install`, the new row's
  `sbumd-ng.dll`).

### 6.92 The black taskbar after Geometry Dash: sihost aborts after Game Bar's presence server fails (dump read)

- **The dump** (`results-t67/a/user/dumps/sihost.exe.3376.dmp`, 223 MB, WER LocalDumps from touch-1; T67a 18:43 UTC,
  big cores, GD running): exception c0000409 at ucrtbase+0x76ECC (`brk #0xF003`, fail-fast 7 = abort). Frame chain
  (x29 walk, `mdstack.py` in the scratchpad): RtlUserThreadStart -> KERNELBASE+0x10B78 -> **sihost.exe+0x2FE0** (its
  unhandled-exception filter) -> ucrtbase terminate -> abort. The stack below is full of **twinui.pcshell.dll** (the
  taskbar/Start/shell experience), SHCore and combase return addresses; the only HRESULT on it is **0x80040154
  REGDB_E_CLASSNOTREG** (x6). sihost had no D3D/DXGI/sbumd module loaded: not our GPU driver.
- **The trigger, twice:** GD up 18:39:34 -> System 10010 DCOM `Windows.Gaming.GameBar.PresenceServer.Internal.
  PresenceWriter` did not register in time (18:39:42) -> sihost abort 18:43:00. The user's 11-14 morning session: 10010
  for the same server at 05:44:03, sihost abort at 05:47:51. Same ~3 min gap: the shell's game-presence path (Game Bar)
  gets "class not registered" and the exception escapes twinui.pcshell. No sihost crash on 11-13 (GD ran there too);
  both crashes are after debloat-1 (WpnService/WpnUserService off on the big cores), which Game Bar's presence
  writer may need: a second suspect.
- **Staged fix (user's yes):** `starbite/win/debloat/gamebar-off.cmd` (HKLM RunOnce via `tools/win/runonce-arm.sh
  StarbiteGameBarOff starbite/win/debloat/gamebar-off.cmd 'starbite\debloat' OUT`): exports, then HKCU
  GameConfigStore GameDVR_Enabled 0, GameDVR AppCaptureEnabled 0, GameBar UseNexusForGameBarEnabled 0 /
  AutoGameModeEnabled 0 / ShowStartupPanel 0 (Game Bar's game detection off). Undo: `gamebar-on.cmd`
  (StarbiteGameBarOn). If the abort persists with Game Bar off, the next test takes WpnService/WpnUserService off the
  big-core list.

### 6.90 Disk housekeeping (11-14 night) and where things are now

- **Archived to `/Volumes/SBWORK/archive/`** (one file per group, listing counts checked before deleting):
  `win-1114-hive-scratch.tar` (build/win/hive rehearsal copies), `win-1114-stages-t60-t65.tar` (stages that ran; by the
  main session), `win-1114-old-trackK.tar.gz` (hive-t13a..h, install-t32b/c*, gl-present/gl-present2/gl-zerocopy/
  gl-kit/gl-tc, t54, user-t30-0715, user-1113, gd-0158, gd-ec-0158; 208 entries, gzip -t ok). results-t60..t65 are in
  `starbite-build-archive/win-1114/`.
- **VM (`~/sbmesa`):** the superseded worktrees src-zc (0150/0151, now ported into src-tc), src-glp2 and src-glp2ec
  were removed with `git worktree remove` (branches sb-zc, sb-glp2, sb-glp2ec kept) together with build-zc-x64,
  build-glp2-x64 and build-glp2-ec; then `fstrim` (Mac 1.8 -> 4.0 GB free). Live trees: src-int (sbumd: 0157 + 0159),
  src-tc (GD ICD: 0158 + 0140 + 0150 + 0151 + 0160; build-tc-x64, build-tc-ec).

### 6.86 BeamNG.drive on the S9: feasibility (the user's long goal; no phone time)

Facts marked [K] are general knowledge about the game, not checked against a current release.
- **GPU API, the first blocker.** BeamNG's Windows renderer is Direct3D 11 at **feature level 11_0** [K] (it also
  has a Vulkan renderer [K]). Ours is D3D11 at **FL 10_0** (sbumd offers the D3D10.0 DDI, 6.6; 10_1 is a switch away),
  and FL 11_0 needs SM5 + tessellation: the Mali-G72 has no tessellation and d3d10umd translates SM4 only
  (WIN11-GPU-CAPS.md, "Not FL 11_0"). Vulkan would need PanVK on Bifrost v6/v7, which upstream does not cover as a
  conformant driver [K]. So today the game would refuse to create its device or fall to WARP (CPU rendering).
- **CPU: x64 translated on 3 M3 cores.** The Chromebook's Celeron N4000 runs it natively [the user]. The M3s at
  1.8-2.0 GHz run x64 through xtajit64 with no persistent cache on the big-core row (6.75) and XtaCache's 7023 to fix
  first; BeamNG's soft-body physics is CPU-heavy [K]. Expect well below the N4000's native speed until the cache works,
  and the 80 C guard throttling under sustained load (T61: 81 C, 6 throttles in a GD run).
- **Memory:** 4 GB RAM; the big-core desktop already sat at ~3.75 GB committed idle before debloat-1 (T63), GD took
  it to 4.65 GB. BeamNG asks for 8 GB [K] (the Chromebook runs it in 4 GB, so it can start lower). The pagefile is on
  C: with ~3 GB free.
- **Install location:** C: (sda27) is 22 GB with ~3.3 GB free; BeamNG needs tens of GB [K]. Options: (1) USB storage
  through the S9's port in host mode under Windows: the xHCI controller enumerates under Windows (T2, 6.x), but a USB
  stick as a Windows drive has never been tried here, and host mode needs OTG power to the port; (2) the microSD card:
  Windows has no SD host driver on this phone that we know of (the card holds the OS store, owned by Starbite);
  (3) shrink something on UFS. (1) is the one to test first: plug the user's 64 GB stick, see if Windows mounts it.
- **What blocks first:** the renderer (FL 11_0 / Vulkan), before CPU or memory matter. Without it, the only route is
  a renderer-level translation to what we have (e.g. D3D11 FL 11 on top of GL 3.x/ES 3.1 does not exist without
  tessellation) or WARP at a few fps. Realistic order if the user wants to pursue it: USB storage test (cheap), the
  M3 translation cache (helps everything), then a hard look at Vulkan on Bifrost (big project).

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
