# StarbiteDisplay: integration

For the integrating session. Nothing outside this folder was edited.

## 1. Board file / ADT (needed for flips)

`tools/board/starlte.board`, block `decon`: the base address of a flip is not in DECON_F's range but in the DPU's
DMA block, and the scaler is in the DPP block. Add both as further `reg` entries (the driver takes them by index):

```toml
[[block]]
name = "decon"
compatible = ["starbite,decon", "samsung,exynos9-decon"]
reg = [0x16030000, 0x10000,      # 0: DECON_F
       0x16070000, 0x7000,       # 1: DPU_DMA: common + IDMA G0 G1 VG0 VG1 VGF0 VGF1 (dts dpp@.. second reg)
       0x16020000, 0x7000]       # 2: DPP: common + the six channels (dts dpp@0x16021000..26000)
spi = [0xb6, 0xb7, 0xbc, 0x1a5]  # 0 FIFO, 1 frame start, 2 frame done, 3 extra
```

- With only entry 0 the driver starts, logs `idma MISSING (reg 1)`, and stays in CPU-copy mode with polled vsync.
- `dsim` stays as it is (`reg[0]` = DSIM0; the D-PHY entry is never mapped).
- `/arm-io/disp0` stays (device_type `display-subsystem`, `canvas-width`, `canvas-height`, compatible
  `samsung,a5xelte-display`): AppleCLCD attaches there, because IOMobileFramebuffer.framework reads the canvas from
  the service's provider. A second compatible `starbite,display` may be added; the personality lists both.
- mkadt-board.py's `--check` will see the three ranges as distinct (they are).

## 2. Files

Built in (M1-M8), add to `iokit/conf/files.arm64` (C++ as `optional iokitcpp`, the hw C files likewise or under
`bsd/conf/files`, any place that compiles plain C):

```
starbite/apple-drivers/StarbiteDisplay/StarbiteDisplay.cpp
starbite/apple-drivers/StarbiteDisplay/hw/sbd_decon.c
starbite/apple-drivers/StarbiteDisplay/hw/sbd_dsim.c
starbite/apple-drivers/StarbiteDisplay/hw/sbd_panel.c
starbite/apple-drivers/StarbiteDisplay/hw/sbd_swap.c
```

and **remove `iokit/Kernel/arm/SweetFramebuffer.cpp`** for the STARLTE config: this driver defines the same classes
(`IOMobileFramebuffer`, `AppleCLCD`, `IOMobileFramebufferUserClient`) and the globals other files take from it
(`sweet_fb_first_present` for 0421's dead-man, `sweet_fb_cur_x/y/seq` for SweetHID, `sweet_fb_hold_until` and
`sweet_fb_last_surface` for herolte_gpu.c). Include paths: this folder, and `iokit/Kernel/arm` for `SweetSurface.h`.

`STARBITE_DISPLAY_LEGACY` (default 1) is the switch for those ties. It also uses `sweet_console_muted`,
`sweet_last_present_abs` (SweetHID.cpp), `starlte_gpu_present_*` (starlte_gpu.c) and `starlte_sbstop_*` (0423).
Build with `-DSTARBITE_DISPLAY_LEGACY=0` once SweetHID and the in-kernel GPU present are gone (compile-checked both
ways); `sbstop=display` then needs another route to the stop function.

## 3. Personalities (KernelConfigTables.cpp, or Info.plist in a kext)

```
{ 'IOClass' = StarbiteDisplay; 'IOProviderClass' = IOPlatformDevice; 'IONameMatch' = 'starbite,decon'; },
{ 'IOClass' = AppleCLCD;       'IOProviderClass' = IOPlatformDevice; 'IONameMatch' = ('starbite,display', 'samsung,a5xelte-display'); },
```

Replace today's AppleCLCD entry. The provider class is whatever StarbitePE publishes for arm-io children (today
IOPlatformDevice). AppleCLCD waits up to 3 s for StarbiteDisplay, which waits up to 2 s for the `dsim` nub; without
them it runs as SweetFramebuffer did.

## 4. Needs from other drivers

| From | What | Without it |
|---|---|---|
| StarbitePE / StarbiteARMIO | nubs for `decon`, `dsim`, `disp0` with device memory and interrupts | copy mode / polled vsync |
| StarbiteGICv2 | SPIs 0xb7, 0xbc, 0x1a5 (INTID +32) delivered, level-triggered | vsync polled from FRAME_COUNT (the M8 pass line needs the real interrupt) |
| StarbiteSurface (19) | `SweetSurface.h` interface as today (`sweet_surface_copy`, `_create_physical`, `_create_buffer`, `_port_in_task`, the public fields). **For frames to go out with no copy at all**: allocate full-canvas BGRA surfaces physically contiguous, below 4 GiB (and below 0xe0000000), base aligned to 256 bytes, `memory` prepared so `performOperation(kIOMemoryIncoherentIOStore)` works | every frame is drawn into our buffer and then flipped (tear-free, one copy) |
| StarbiteMali (20) | nothing new. `sweetfb_gpu=1` (the Mali scales into S-Boot's buffer) keeps working **only with `sbdisp=copy`** | - |
| Userland / identity | the 10-bit framebuffer choice comes from MobileGestalt `HasExtendedColorDisplay` and the preference `CAEnableDeepFramebuffer`, not from this driver (NOTES.md, "Colour and capability properties", point 3). To retire sbca's / sbmac's QuartzCore byte patch: set `CAEnableDeepFramebuffer = false` in the root's CoreAnimation preferences (a plist), or make the identity answer "no extended colour display" | CoreAnimation asks for 'b3a8' / 'RGhA' as before |

## 5. Memory

Flip mode allocates one 1440 x 2960 x 4 buffer (17 MiB), scale mode two canvas-sized ones (2 x 6 MiB for 828 x 1792),
with `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` (contiguous, below 4 GiB, write-combined) at AppleCLCD's
start. That needs the low RAM window (0x92000000..0xaf800000) to be XNU-managed and not yet fragmented; the driver
starts early enough today. If the allocation ever fails the driver says so and stays in copy mode. A carve-out in the
board file for display buffers is the alternative; not needed for M8.

**M8b (2026-11-12) showed the allocation can never succeed:** the Mac payloads give XNU only DRAM bank 2
(0x880000000 + 0x70000000), so no XNU page is below 4 GiB. Since M8c (kernel patch 0434) the buffers come from the
board's carve-out first: `[memory] display` = the decon node's `starbite,scanout-carveout` <0xa0000000 0x4000000>
(u64 base, size), in the low window, clear of the loader's crash buffers (0x9A000000..0x9F030000) and UFS DMA
(0xaf000000..). It is refused if any page is XNU-managed (pmap_valid_address), overlaps S-Boot's framebuffer or fails
bufferOK; buffers are bump-allocated (the last one freed is reusable) and mapped write-combined. The allocation path
stays as the fallback for ADTs without the property.

## 6. Boot-args

New: `sbdisp=flip|scale|copy`, `sbdisp_irq=0`, `sbdisp_panel=0..3`, `sbdisp_bl=N`, `sbdisp_selftest=N`,
`sbdisp_limit=`, `sbdisp_stop_s=`, `sbdisp_stop_flips=`, and the stage name `sbstop=display`.
Kept: `sweetfb_hz`, `sweetfb_noconsole`, `sweetfb_native`, `sweetfb_lw`, `sweetfb_lh`, `sweetfb_gpu` (copy mode),
`sweetfbstats`.
Changed: the canvas now comes from the ADT (`canvas-width/height` of disp0) first; `sweetfb_lw/lh` only when the ADT
has none; `sweetfb_scale` is no longer needed (a canvas smaller than the panel is always scaled).
Gone: `sweetfbdump`, `sweetfbcrop`, `sweetfb_copythreads` (the copy is single-threaded here), `a5fb_*`.

Daily lines keep working unchanged in behaviour with `sbdisp=copy`. Recommended until M8-a has passed on the phone:
add `sbdisp=copy` to every daily command line, so the first boot that writes DECON registers is the watchdog-protected
test.

## 7. Checks

```
sh starbite/apple-drivers/StarbiteDisplay/test/run.sh
python3 tools/apple/kcheck.py starbite/apple-drivers/StarbiteDisplay/StarbiteDisplay.cpp \
    starbite/apple-drivers/StarbiteDisplay/hw/sbd_decon.c starbite/apple-drivers/StarbiteDisplay/hw/sbd_dsim.c \
    starbite/apple-drivers/StarbiteDisplay/hw/sbd_panel.c starbite/apple-drivers/StarbiteDisplay/hw/sbd_swap.c \
    -I build/xnu-12377/iokit/Kernel/arm
```
