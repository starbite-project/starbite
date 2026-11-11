# StarbiteDisplay: register facts, sources, open questions

Marks: **[V]** verified in a source we hold (file named); **[S]** stated by an earlier port's public driver, which says it
runs these writes on an S9, not reproduced by us; **[I]** inferred. Nothing here has run on our phone yet.

Hardware: DECON_F 0x16030000 (ADT `decon`), DSIM0 0x16080000 (ADT `dsim`), Samsung S6E3HA8 AMOLED, 1440 x 2960,
MIPI DSI command mode, 4 lanes, DSC on. Frames are started by the panel's TE pulse (DECON "HW trigger").

## Credits and licences

- `StarbiteDisplay.cpp` Apple-facing half: moved from our `SweetFramebuffer.cpp` (xnu/series-12377 0046..0398, APSL).
- Flip and VGF0 scaler sequences and the Catmull-Rom coefficient table: after **an earlier port,
  `refport-kernel/drivers/display/dpu/exynos_dpu.c`** (APSL-2.0 by an earlier port's `LICENSES.md`, row "the commits
  04xx ... its * files"). Panel key / display-off / sleep sequences cross-checked with
  `drivers/display/panel/s6e3ha8.c` (same licence). Rewritten around a testable back end; their memory pool
  and PSR code were not taken. Nothing under `build/refport27/private-in/` was opened.
- Vendor kernel (`ref/starlte-kernel/drivers/video/fbdev/exynos/dpu_9810/regs-decon.h`, `regs-dpp.h`, `regs-dsim.h`,
  `decon_reg.c`, `decon_dsi.c`) and `starbite/libsb`: GPL, read as register documentation only. No code copied.
- `shim/starlte/em.c`: the task brief calls it ours and not GPL, but **its first line is
  `SPDX-License-Identifier: GPL-2.0-only`**. I treated it as documentation only (the packet facts are also in
  `build/em-research-brightness.md`) and wrote `hw/sbd_dsim.c` and `hw/sbd_panel.c` without copying from it.
  See open question 1.

## DECON_F (block 0)

| Register | Offset | Facts | Source |
|---|---|---|---|
| GLOBAL_CONTROL | 0x000 | bit 8 command mode, bit 5 idle, bit 4 running | [V] regs-decon.h |
| INTERRUPT_ENABLE | 0x040 | 13 frame done, 12 frame start, 4 extra, 0 master; mask 0x3111 | [V] regs-decon.h, decon_reg.c `decon_reg_set_int` |
| EXTRA_INTERRUPT_ENABLE | 0x044 | 8 resource conflict, 4 time-out | [V] |
| INTERRUPT_PENDING | 0x04c | same bits as enable; cleared by writing the bit | [V] decon_reg.c `decon_reg_get_interrupt_and_clear` |
| EXTRA_INTERRUPT_PENDING | 0x050 | 8, 4; write to clear | [V] |
| SHADOW_REG_UPDATE_REQ | 0x060 | bit 31 global, bits 0-5 windows; hardware clears it when taken | [V] regs-decon.h + `decon_reg_wait_for_update_timeout`; "taken at the next frame" [S] |
| HW_SW_TRIG_CONTROL | 0x070 | 0 HW trigger enable, 4 HW trigger masked, 8 SW trigger, 12/13 polarity. S-Boot leaves 0x3070 (masked); **our loader writes 0x1281** | [V] regs-decon.h; shim/starlte/fbtext.c:12; em-research-brightness.md §2 |
| DATA_PATH_CONTROL_0 | 0x214 | bit 4w: window w on | [V] |
| DATA_PATH_CONTROL_1 | 0x218 | bits 4w..4w+2: window w's DMA channel | [V] |
| FRAME_COUNT | 0x2a0 | counts frames | [V] offset; "one per refresh" [S] |
| WIN_START_POSITION(w) | 0x1008 + 0x30w | y << 16 or x, 14 bits each | [V] |
| WIN_END_POSITION(w) | 0x100c + 0x30w | last line << 16 or last column | [V] fields; **inclusive end [I]**, checked at run time: the scaler refuses unless the boot window reads 0 .. (1439, 2959) |

Interrupt lines of the `decon` node (board `spi = [0xb6, 0xb7, 0xbc, 0x1a5]`): index 0 FIFO (video mode only),
**1 frame start, 2 frame done, 3 extra** [V decon_dsi.c `decon_register_irq`, the 9810 branch]. The vendor counts
the frame-start interrupt as vsync [V]. The panel's TE itself is a GPIO interrupt (`te_eint`), not used here.

Trigger enable/disable pairing (`enable = EN set + MASK clear`, `disable = MASK set + EN clear`) [V decon_reg.c
`decon_reg_set_trigger`]. A flip with the trigger masked would never be taken, so `sbd_decon_flip` unmasks first [S].

## DPU_DMA / IDMA (block 1, 0x16070000) and DPP (block 2, 0x16020000)

Channel blocks at 0x1000 steps: G0, G1, VG0, VG1, VGF0, VGF1 at 0x16071000.. (IDMA) and 0x16021000.. (DPP)
[V device/dtb/starlte.dts `dpp@0x16021000..26000`, each with its IDMA `reg`]. Which block is which channel type, and
the DATA_PATH_CONTROL_1 number of each (0 VG0, 1 VGF0, 2 VGF1, 3 G1, 4 VG1, 5 G0): **[S]**. The probe does not need
the names for plain flips (it follows the window's own channel number); the scaler needs "1 = VGF0 = block 4".

| Register | Offset | Facts | Source |
|---|---|---|---|
| IDMA_ENABLE | 0x000 | 4 SFR update force, 2 busy | [V] regs-dpp.h |
| IDMA_IRQ | 0x004 | status 21 config error, 19 read slave error, 17 deadlock | [V] |
| IDMA_IN_CON | 0x008 | 19-26 IC_MAX, 11-15 format, 8-10 rotation/flip, 6-7 AFBC, 3 block | [V] |
| IDMA_SRC_SIZE | 0x010 | height << 16 or stride in pixels | [V] |
| IDMA_IMG_SIZE | 0x018 | height << 16 or width | [V] |
| IDMA_IN_BASE_ADDR_Y / C | 0x040 / 0x044 | 32-bit physical address; both written for RGB | [V] offsets; "both" [S] |
| IDMA_CFG_ERR_STATE | 0x870 | 11 error bits | [V] |
| DPP_IN_CON | 0x008 | 0-2 format (0 = ARGB8888), 3 alpha select | [V] |
| DPP_SCALED_IMG_SIZE | 0x02c | height << 16 or width | [V] |
| DPP_MAIN_H/V_RATIO | 0x044 / 0x048 | source / destination, 12.20 | [V] offsets; format [S] |
| coefficients, positions | 0x200.., 0x290.., 0x5b0.. | layout as in `sbd_regs.h` | **[S]** only |

Formats (IDMA_IN_CON): 0 ARGB8888 ... 3 BGRA8888, 16-19 the four 2:10:10:10 orders [V regs-dpp.h]. The driver never
changes the format: it keeps what S-Boot programmed (the loader calls it ARGB8888 [V fbtext.c:5]; memory order
B,G,R,A = Apple's 'BGRA'). The 10-bit codes are listed for open question 4; unused.

Limits:
- **32-bit addresses.** XNU's low RAM window 0x92000000..0xaf800000 qualifies; bank 2 (0x880000000) never does
  [V tools/board/starlte.board `[memory]`].
- **Nothing scanned out from 0xe0000000 up** reaches the panel [S, exynos9810_mem.h]. Enforced as
  `scan_limit` (boot-arg `sbdisp_limit`). Our low window ends below it, so this only matters for carve-outs.
- **256-byte base alignment** [S]. Scaler: sizes 16..4096, down to 1/2, up to 8x [S].
- No SysMMU set-up: the IDMA reads physical addresses, as it does for S-Boot's buffer **[I]** (SysMMUs "left in
  bypass", plan row 27). If the DPU's SysMMU were enabled, flips to new addresses would fault: the self test shows it.

## DSIM0 (block 3) and the panel

All from `build/em-research-brightness.md` (which cites regs-dsim.h, dsim_drv.c, S-Boot's binary and S-Boot's log)
and confirmed against the loader's EngineerMode, which sends the same AOR packets on this phone.

- Ready test: CLK_CTRL 0x20 bit 20 (HS clock requested) and CONFIG 0x4c bit 18 clear (command mode) [V].
- Write: payload words to 0x5c (little-endian, zero padded), then the header to 0x58
  (`type | d0 << 8 | d1 << 16`, bit 24 = bus turn-around; **bits 25-31 kept as read**, as the vendor and S-Boot do),
  then wait for INTSRC 0x50 bit 28 (write-1-to-clear) or FIFOCTRL 0x68 bit 10 with header count 0 [V].
- 50 ms time-out, one retry of the sequence: S-Boot's own 5 ms poll timed out once in its log [V SBLOG:937].
- FIFOCTRL is never written; DSIM is never reset; PLL, clocks and escape mode are never touched [V "things to avoid"].
- Commands with parameters go as DCS long writes (0x39), two-byte ones included; a bare command as 0x05 [V].
- Brightness is AOR, not DCS 0x51: `F0 5A 5A`, `B1 hi lo`, `F7 03`, `F0 A5 A5` [V S-Boot's table at sboot 0x1348c0].
  Range 0x000c (brightest) .. 0x0ba3 (S-Boot's darkest). Level 0-255 maps to 0x0b76 .. 0x000c with the lit lines
  (2979 - AOR) growing with the square of the level **[I: my curve]**; Android instead also changes gamma (CA), which
  needs the panel's calibration maths and is deliberately not sent.
- Display off/on `28` / `29` and sleep in/out `10` / `11`, each between the level-1 key `9F A5 A5` .. `9F 5A 5A`,
  waits 20 / 100 / 30 ms **[S]**. Our own research note says sleep-in may need a full panel init to come back
  **[I, conflicting]**: so the default (`sbdisp_panel=1`) sends neither; a dark screen is a black frame.
- Panel ID: DCS read 0x04, 3 bytes, `91 04 44` on this phone [V S-Boot log line 418].
- Allow-list in `sbd_panel_cmd_allowed`: F0 9F B1 F7 28 29 10 11. Everything else is refused in code, in particular
  C0/C1 (panel flash), C8/B3/A1 (calibration), B5/F4/FC (supply voltages), CA, 01, 34 (TE off would stop the DECON's
  trigger), 53, B0.

## Apple side: the ABI kept

Selectors 3, 4, 5, 6, 7, 8, 12, 18, 28, 72 and the swap record offsets are unchanged from SweetFramebuffer [V
docs/MACOS-METAL.md §3.3: the same table decoded from the macOS 26.5 KDK's IOMobileGraphicsFamily]. The record's
two layouts are host-tested (`hw/sbd_swap.c`). Vsync notification: type 5, six 32-bit halves (count, time, 0).

## Colour and capability properties: what was missing

Evidence: this Mac's macOS 27.2 QuartzCore, disassembled read-only from the shared cache with
`tools/macos/metal/re/dsc.py` on 2026-11-11, plus `ioreg -c IOMobileFramebufferShim` on this Mac. **Checked on 27.2
only**; iOS 26, macOS 26.5 and Sequoia 15 must be re-checked with docs/MACOS-METAL.md §8's commands (symbol names
below are the search keys).

1. **`CA::WindowServer::AppleDisplay::update_display_limits()`** (0x18ddbb2e4) reads these from the IOMFB service [V]:
   `DisplayPipeStrideRequirements` {StrideLinearHorizontal, StrideLinearVertical},
   `DisplayPipePlaneBaseAlignment` {PlaneBaseAlignmentLinear},
   `IOMFBMaxSrcPixels` {MaxSrcBufferWidth, MaxSrcBufferHeight, MaxSrcRectWidth, MaxSrcRectWidthForPipe,
   MaxSrcRectTotal, MaxSrcRectHeightForPipe, PixelClock, VideoClock, MaxVideoSrcDownscalingWidth,
   IOMFBMaxCompressedSizeInBytes, **IOMFBCompressionSupport**, MaxSrcRotRectTotal, **IOMFBAsyncRGBCompressionSupport**},
   `IOMFBAsyncSwapDisabledKey`, `IOMFBNumLayers` {MaxNumLayers, FullFrameRequired}, `MaxDisplayBandwidthLimitKey`,
   `PCCCabalEnable`, **`IOMFBRGBMultiPlaneSupportKey`**, `IOMFBYUVSupportKey`, `IOMFBYUVPackedUnSupported`,
   `IOMFBYUV420UnSupported`, `W40a_Blending_OK`, `IOMFBSupportsICC`, `IOMFBDisplayRefresh`
   {displayMinRefreshInterval, displayRefreshStepMachTime}, `IOMFBSupportsRotation`, `IOMFBDisableFlip`,
   `IOMFBBlendDisabledKey`, `IOMFBRGBSupportsCursorOnSCLregionKey`, `IOMFBCompressedSourceSurfaceWidth`,
   `IOMFBSwapAppleLookSupported`, `IdleCachingMethod`, `DisplayRotation`.
   It logs "kIOMFBSourceLimitsKey missing!" and friends when the size keys are absent.
2. **The defaults when a key is absent are "supported"** for several of them [V]: the capability word starts as
   `(old & 0xed81) | 0x103c`, i.e. bit 2 (multi-plane RGB, set from IOMFBRGBMultiPlaneSupportKey) and bit 12
   (compression, cleared only if IOMFBCompressionSupport is present and 0) are on. Today's AppleCLCD published only
   `external` and `DisplayPipePlaneBaseAlignment` **as a number**; 27.2 asks for it **as a dictionary**
   (`copy_display_property(..., CFDictionaryGetTypeID())`), so even that one was ignored there.
   So yes: the old driver never said what it cannot scan out, and CoreAnimation assumed Apple's formats.
3. **But the 10-bit framebuffer choice itself does not come from the driver** [V].
   `CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format()` (0x18e09f1c4) returns 'BGRA' only when display flag
   bit 3 is clear. That bit is set in `AppleDisplay::AppleDisplay` when `CADeviceUseCIF10()` is true, and
   `___CADeviceUseCIF10_block_invoke` (0x18dfea874) is: MobileGestalt **`HasExtendedColorDisplay`**, overridden by the
   preference **`CAEnableDeepFramebuffer`** if that is set. With the bit set the function picks 'b3a8' (then the
   compressed or 'w40a' variants by the capability word); with EDR headroom above a threshold it picks 'RGhA'.
   The identity we present (iPhone12,1, MacBookAir10,1) says "extended colour display", so CoreAnimation asks for
   10 bits whatever this driver publishes. The plan's sentence "QuartzCore needed a byte patch because the driver
   didn't say which colour formats it supports" is therefore **half right**: the properties were missing (2), but
   publishing them cannot by itself turn 'b3a8' into 'BGRA' on 27.2.
4. What this driver now publishes (`AppleCLCD::publishProperties`): the dictionaries of (1) in the shapes the real
   service uses, with honest values: no compression, no multi-plane RGB, no YUV, no ICC, no rotation, 4 layers,
   scale 1, 60 Hz fixed, plus `DisplayWidth/Height`, `DisplayAttributes` {ProductAttributes: LegacyManufacturerID
   0x4c83 ('SDC'), ProductID 0x910444, ManufacturerID, ProductName; TiledDisplayInfo {}} and `IODisplayParameters`
   {brightness, linear-brightness: 0..65536}.
5. What still has to happen for the byte patch to go (not this driver's to do, see INTEGRATION.md):
   either `HasExtendedColorDisplay` answers false for our device (MobileGestalt: from the identity/ADT; where it
   reads it is not traced, the key table is obfuscated), or the preference `CAEnableDeepFramebuffer = false` is set
   in the root (a plist, not a code patch; IOS-26-Hot does this [V NOTES.md:1869]), or StarbiteMali learns to render
   'b3a8' / 'w40a' and this driver scans plane 0 out as a 2:10:10:10 format (the IDMA has the codes; bit order not
   checked) - open question 4.

EDR: nothing is published (`EDRPotentialHeadroom`, `EDRReferenceHeadroom` are read by
`IOMFBDisplay::update_brightness_capabilities` from a brightness-capabilities dictionary that comes through a
selector we do not know [I]). With no headroom CoreAnimation's EDR branch stays off, which is what an 8-bit SDR
pipe wants.

## Design notes

- **Three ways a frame reaches the panel**, chosen per swap (`AppleCLCD::present`):
  1. *by address* ("zero copy"): one full-canvas BGRA layer whose pixels are one contiguous piece below the limit:
     its physical address goes to the IDMA. Needs the surface service to allocate display surfaces that way
     (today's SweetSurface buffers are ordinary wired pages, so this path will stay cold until driver 19 does);
  2. *drawn, then flipped*: the frame is copied (scaled if the canvas is smaller) into the buffer the DECON is not
     reading, then that buffer's address is flipped in. Tear-free, still a copy;
  3. *in place*: a swap without layer 0 (the pointer) is blended into the front buffer, with save-under (0396).
  With `sbdisp=copy`, or when anything about the DECON is not as expected: SweetFramebuffer's copy into S-Boot's
  buffer, unchanged.
- **Locking**: the DECON state is under an `IOSimpleLock` (flips from threads against the interrupt filter); DSIM
  sequences under an `IOLock`; attach/detach (which sleep) only before the refresh client starts.
- **Vsync sources**, in order: frame-start interrupt; `FRAME_COUNT` polled by a 60 Hz timer when no interrupt
  delivered a refresh in the last period; a synthetic tick when the hardware had no frame at all. The counters keep
  the three apart, so "vsync IRQ counted" cannot be claimed by the timer.
- The kernel text console and a panic still draw into S-Boot's buffer. In flip mode that buffer is one of the two
  (visible half the time); in scale mode it is not shown at all. The panic text is in pmsg either way.

## Open questions

1. `shim/starlte/em.c` says GPL-2.0-only in its header; the brief says it is ours and not GPL. Which is right? (No
   effect on this driver: nothing was copied.)
2. Is `DPU` SysMMU really in bypass after S-Boot + our loader? The self test answers it on the phone.
3. Is INTERRUPT_ENABLE left on or off by S-Boot, and are SPIs 0xb7/0xbc/0x1a5 routed to us (Group 0, GIC driver)?
   The driver enables them itself and falls back to polling; the PHONE-TEST pass line needs the real interrupt.
4. 10-bit scan-out: would 'b3a8' plane 0 match IDMA format 16-19? Needs the surface layout and a phone test.
5. Which selector carries brightness on iOS 26 / Sequoia (`IOMobileFramebufferSwapSetBrightness`,
   `GetBrightnessControlCapabilities` are imported by CoreAnimation)? Unknown; they are answered with zeros and
   logged once. Brightness works through `setProperties` and `sbdisp_bl=` only.
6. Does S-Boot leave AOR at 0x0440? Unknown (research note §6.1); the driver does not read it back.
7. A second DECON window for the pointer layer (true hardware cursor) instead of blending: not done.
8. `sweetfbdump` / `sweetfbcrop` were not ported (`sweetfbstats` was).
