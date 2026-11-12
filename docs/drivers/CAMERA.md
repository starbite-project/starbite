# Starbite OS: the front camera (S5K3H1)

Files: `starbite/sbos/kern/cam.c` (power, clocks, sensor, CSIS1 and its DMA, the stream, the probe),
`starbite/sbos/kern/cam_isp.c` (the image pipeline on the processor), `starbite/sbos/kern/cam.h` (the API),
`starbite/sbos/kern/host/cam-test.c` + `cam-test.sh` (the host test).

Status, 2026-11-12: written and host-tested. **Nothing has run on the phone yet.** an earlier port's XNU driver ran the same
sequence on this phone and streamed the front camera at 30 frames a second (its CHANGELOG). The rear camera (S5K2L3)
and the iris camera are not started.

## How it works

The sensor sends RAW10 over two MIPI lanes to CSIS1. CSIS1's WDMA writes each frame straight to RAM. No ISP firmware
is loaded. The processor makes the picture from the raw frame (`cam_isp.c`).

## The wiring

Sources: the phone's device tree (`device/dtb/starlte.dts`), Samsung's GPL-2.0 fimc-is2 driver, and the facts an earlier port's
driver recorded on this phone (register values and sequences only; no an earlier port code is used).

| What | Where |
|---|---|
| Sensor | I2C 0x35 on hsi2c@108F0000 (USI08_I2C, PERIC1), 400 kHz. Mode word 0x1081102C. Pins gpp5-2 SCL, gpp5-3 SDA, function 3. CMU_PERIC1 gates 0x2050, 0x2098, 0x20EC, 0x20F0, Q-channel 0x303C |
| Sensor registers | model ID 0x0000, revision 0x0002, frame count 0x0005 (0xFF = not streaming), mode_select 0x0100, hold 0x0104, exposure 0x0202, analog gain 0x0204 (0x20 = 1x), digital gain 0x020E (0x100 = 1x) |
| Reset | gpg2-5 (0x104300C0), high = running. gpg2-4 is the torch: never written |
| MCLK | CIS_CLK1 of CMU_TOP: mux 0x1A24101C bit 0 = 0 (OSCCLK, 26 MHz), divider 0x1A241818 = 0, gate 0x1A24201C, Q-channel 0x1A243008. Out on gpc1-5, function 2 |
| Supplies (S2MPB02 0x59, hsi2c@10920000) | BUCK2 VDD_EXT_1P3_PB02 1.3 V (0x0D bits 7:6, 0x0E = 0x90); LDO5 VDDD 1.05 V (0x22 = 0x24); LDO6 VDDIO 1.8 V (0x23 = 0x30); LDO13 VDDA 2.8 V (0x2A = 0x58); LDO16 VDDAF 2.8 V (0x2D = 0x58). Bit 7 = on |
| Power domain | ISPPRE: PMU 0x14064048 / status 0x1406404C. Parent DPU (0x14064024), checked only. DTZPC SMC 0x82000410 (1 restore after on, 0 save before off, 0x16210004). MEMORY_ISPPRE_OPTION 0x14064730 = 2 before off |
| ISPPRE clocks | CMU_TOP CLKCMU_ISPPRE_BUS: mux 0x1094 = 1, divider 0x1890 = 1 (233 MHz; the boot's ~50 MHz overflows CSIS1), gate 0x209C. CMU_ISPPRE 0x16200100 bit 4, Q-channels 0x3010..0x306C bit 1 (never 0x303C: it aborted on the phone) |
| D-PHY | isolation PMU 0x14060730 bit 0, reset SYSREG_ISPPRE 0x16210500 bit 2, registers 0x16180000. Settle 36 at 720 Mb/s (Samsung's `fimc-is-hw-settle-10nm-lpp.h`, checked) |
| CSIS1 | 0x16240000, VC0 RAW10 1632 x 1224, two lanes. WDMA 0x162C0000 (+0x400 its common). DMA common of all four CSISs 0x162F0000 |
| SysMMU | 0x16310000: never touched. Off after the domain's power-on, so the WDMA's addresses are physical |
| Mode | Samsung's setfile A, 1632 x 1224 RAW10, 30 fps, 2 lanes at 719 Mb/s: 85 register writes (`fimc-is-cis-3h1-setA.h`), then 0x3092 = 0 |
| Bayer | GRBG (Samsung's `fimc-is-cis-3h1.c`: GR_BG). Black level 64 of 1023 |
| Pixel layout | each RAW10 pixel in 16 bits, little-endian, value in bits 9:0 (PACK12 off), 3264 bytes a line |

## Memory for the frames

32 MiB at **0xD8000000-0xD9FFFFFF**:

| Offset | Size | Use |
|---|---|---|
| 0x0000000 | 4 x 4 MiB | the WDMA's ring (pointer i writes slot i % 4) |
| 0x1000000 | 4 MiB | slot 4: stands in for a slot held for a still |
| 0x1400000 | 2 MiB | the preview picture (XRGB) |
| 0x1600000 | 2 MiB | the still picture (XRGB) |
| 0x1800000 | 2 MiB | the still as a BMP file |
| 0x1A00000 | 6 MiB | free |

Why this range is safe:
- The device tree has no reserved-memory node there. The panel's buffers end at 0xCF600000 (S-Boot's at 0xCC000000,
  the Next flips at 0xCD200000 and 0xCE400000). secure_camera starts at 0xE1900000. The modem, ABOX and the
  ramoops/pmsg areas are all above 0xF0000000.
- libsb maps 0xC0000000-0xFFFFFFFF non-cacheable, so the CPU sees what the DMA wrote with no cache work.
- Nothing else in Starbite OS uses 0xD0000000-0xE0000000 (checked with grep over `starbite/` and `shim/`).
- It is below 4 GiB, which the WDMA needs, since it takes 32-bit addresses.
- an earlier port's WDMA wrote this same range on this phone.
- The DT's dynamic `camera`/`camera_contig` ION pools have no fixed address, and `fimc_is_lib` (0xFF800000) is the ISP
  firmware's, which we do not load. Neither is used.

## The sequence (`cam_poll` steps)

1. **look**: map the memory. Check that the DPU domain is on. Bring up the S2MPB02's bus (fled.c's way; its CMU gates
   must already be open). Read the chip's 0x1D (must be 0x90) and the six supply registers. Stop if a supply is on at
   another voltage, because that means someone else is using it.
2. **power**: CIS_CLK1 at 26 MHz, reset low, then BUCK2, VDDAF, VDDA, VDDIO, VDDD, with 200 us after each.
3. **power2** (1 ms later): the sensor's bus (closed gates opened, mode 4, pins function 3), 10 us, reset high, MCLK on
   its pin.
4. **ID** (9 ms later): read the model ID, revision and frame count. If there is no answer, try again 20 ms later.
5. **ISP**: switch ISPPRE on and send the restore SMC. Set the bus clock to 233 MHz, BUS_USER and the Q-channels. Read
   CSIS1's version, which must not be 0 or all ones.
6. **fill**: fill the five slots with 0xA5. That value is not a RAW10 value, so the slot markers and the "written"
   count can see what the DMA wrote.
7. **link**: D-PHY isolation off and reset released, its timing set, CSIS1 reset and configured, the DMA common set,
   and the ring given to the WDMA, which is switched on.
8. **mode**: the 85 setfile writes, 0x3092 = 0, the first exposure, the link enabled, then 0x0104 = 0.
9. **stream** (8 ms later): 0x6028 = 0x4000, 0x0100 = 1, 0x0216 = 0.
10. **run**: every ~5 ms, look for the newest finished slot. A slot is finished when its last 8 bytes are no longer the
    fill. If several are finished, take the one just behind the WDMA's pointer. Then compute stats on every 16th cell,
    apply grey-world white balance (smoothed), render the preview, re-arm the slot, and run auto-exposure every third
    frame. If no frame arrives for 1.5 s, it fails and switches everything off.

**Off** (`cam_close` / `cam_abort` / any failure, ≤ ~100 ms):
1. Stop the WDMA and abort its frame.
2. Tell the sensor to stop (0x0100 = 0) and wait up to 60 ms for frame count 0xFF.
3. Reset the link and restore the DMA common.
4. D-PHY reset and isolation as found.
5. Domain off: memory option, then the save SMC, then power off, then the option as found. A domain found on is left on.
6. MCLK low, reset low, bus pins to inputs.
7. Supplies off in the order VDDAF, VDDD, VDDA, VDDIO, BUCK2, each to its found value (BUCK2's voltage too).
8. Pins, CIS_CLK1 and the sensor's bus back as found.

Supplies that were found on are left on. The S2MPB02's bus (USI10 and gpp6-0/1) stays set up, as fled.c leaves it,
because the torch uses the same bus.

Guards: `wr()` refuses any address outside the camera's blocks. `pmic_put()` only takes the six supply registers with
their camera values (or the found values when switching off), and reads every write back. GPIO banks are written
read-modify-write on the camera's own bits only. Every wait has a time limit.

## The image pipeline (`cam_isp.c`)

The steps:
- **Demosaic.** Each 2 x 2 Bayer cell becomes one pixel (R, the mean of the two greens, B). The picture is therefore at
  most 816 x 612 before rotation.
- **Scale.** Nearest cell to the requested size, with rotation (0/90/180/270) and an optional mirror.
- **Black level, white balance, gamma.** The black level (64) is taken off. White balance uses grey-world gains, kept
  within 0.5x-4x. Gamma is 2.0 (a square root), via a table.
- **Output.** XRGB8888 (0xFFRRGGBB), the panel's format.
- **Still.** A 24-bit bottom-up Windows BMP. The Files / Photos code can save it as it is.

Not done yet: lens shading, a colour matrix, noise reduction, sharpening, JPEG.

## API for the Camera app (`cam.h`)

```c
struct cam_req q = { CAM_FRONT, 360, 480, CAM_PORTRAIT_SELFIE, 1, 0, 0 };  /* facing, w, h, xform, ae, exp, gain */
cam_open(&q);            /* 0 = started; then call cam_poll() every ~10 ms until cam_close() */
cam_poll();              /* 1 running/starting, 0 off, < 0 failed (cam_why()) */
cam_frame(&pic);         /* newest preview: pic.px (XRGB), w, h, stride (pixels), seq */
cam_still();             /* ask; cam_still_get(&shot) returns 1 when ready: shot.bmp/bmp_len + shot.px/w/h */
cam_close();             /* synchronous, everything off and as found */
cam_why();               /* plain words: "Front camera on", "The front camera does not answer", ... */
cam_fps_x10(); cam_model_id(); cam_state();
```

- Preview size: up to 816 on each side and 2 MiB. Good choices are 360 x 480 (portrait) or 480 x 360.
- Rendering costs about w x h cells read from non-cacheable memory per frame.
- The still is 816 x 612 (612 x 816 rotated). It is rendered over 6 polls while its raw slot is held out of the ring.
  It is not mirrored.
- The front sensor sits at 270 degrees, so `CAM_PORTRAIT_SELFIE` = `CAM_ROT270 | CAM_MIRROR`. This has not been seen
  yet. If the picture comes out upside down, use `CAM_ROT90 | CAM_MIRROR`.
- **Flash:** the front camera has no LED, so `cam_flash_available()` returns 0. The UI can use a white screen. For the
  rear camera, when it comes, `fled_pulse()` (1250 mA, 125 ms) or `torch_set()` will be the flash/torch. Both are on
  the same S2MPB02 bus, and that is fine because the kernel is single-threaded.
- Strings for the UI come from `cam_why()`, for example "Starting the front camera", "Front camera on",
  "Camera off", "The camera's power chip does not answer", "A camera supply is in use at another voltage",
  "The front camera does not answer", "The camera's power domain did not come on",
  "No picture from the camera", "The camera stopped sending pictures", "The camera's memory is not available".

## The probe

`int drv_cam_probe(int level, unsigned *wait_ms); void drv_cam_abort(void);` with budget **20000 ms**
(`CAM_PROBE_BUDGET_MS`). The levels also stop themselves: level 1 after 6 s, level 2 after 19 s.

- **Level 0 (read-only):**
  - Reads the reset, MCLK and bus pins; the USI modes and gates; CIS_CLK1 and CLKCMU_ISPPRE_BUS; and the PMU
    (DPU, ISPPRE, memory option, D-PHY isolation).
  - Reads CSIS1's version only if ISPPRE is already on.
  - Reads the S2MPB02's supplies only if its bus is already set up (by the torch). Otherwise it says so.
  - No register write, no I2C write, no controller reset.
  - Result: `PASS read-only look: ISPPRE off, DPU on, ...`.
- **Level 1 (active, silent):**
  - Takes a snapshot of 25 registers and the six supply registers.
  - Opens the front camera at a 64 x 48 preview, waits for 4 frames, then closes.
  - Logs the model ID, the last frame's statistics (mean R/Gr/Gb/B, min, max, green standard deviation, values above
    10 bits), how much of the frame the DMA wrote, the frame rate and the DMA errors. No picture leaves the phone.
  - Checks that the snapshot is the same afterwards.
  - PASS needs: at least 95% of the frame written, no non-RAW10 values, not one flat value, and everything back as
    found.
  - `PASS front camera: ID read, frames reached memory, everything put back`.
  - `FAIL <cam_why()>`, for example `FAIL The front camera does not answer`.
- **Level 2 (attended):**
  - A 360 x 480 selfie preview for 10 s, drawn by `hal.show`. The kernel's default draws into S-Boot's scanout
    (0xCC000000) at x 120, y 400. If the attended image scans out another buffer, set `show`.
  - Then a still. The log gives its size, BMP length and mean R/G/B.
  - Then the same put-back check as level 1.
  - `PASS front camera: preview shown 10 s, still taken, everything put back`.

## Host test

`sh starbite/sbos/kern/host/cam-test.sh` (`-v` traces the log): **1207 checks, 0 failed**.

What it covers:
- The ISP: colour bars at full and half level, from both RAW16 and packed RAW10; piecewise rendering; a red cast fixed
  by grey world; all rotations and the mirror; the BMP format; the stats.
- Probe level 0, with no writes of any kind, both with the S2MPB02's bus down and up.
- Probe level 1 on a good phone and with supplies found on.
- Probe level 1 with 11 injected faults: the S2MPB02 silent, the wrong ID, VDDA not reading back, the sensor silent,
  ISPPRE stuck off, the SMC refused, no frames, DMA errors, no memory, the DPU off, a supply at another voltage.
- Probe level 2.
- The API: preview, still, the stream after the still, refused requests, and an abort after every one of 40 steps.

Every case checks that:
- writes stay in the camera's own registers and bits, and the torch's pin is untouched;
- the ISPPRE blocks are never accessed with the domain off;
- the SMCs come in order;
- only the camera's six S2MPB02 registers are written;
- the supplies switch on in the order BUCK2, VDDAF, VDDA, VDDIO, VDDD and off in the order VDDAF, VDDD, VDDA, VDDIO,
  BUCK2, always with the reset low, MCLK off and the bus pins off;
- everything ends as found.

Three deliberate mutations were all caught: two supplies swapped, the CIS_CLK1 gate not restored, and a neighbouring
GPIO bit written.

## Risks

- **The sensor bus's clock.** The HSI2C timing assumes a 400 MHz input clock. If the real clock is lower, the bus is
  just slower.
- **Opened CMU gates.** The USI08_I2C gates are opened if they are closed, and closed again after.
- **The ISPPRE bus clock.** It is raised to 233 MHz without asking ACPM for more INT voltage, as an earlier port did. Android
  runs it at 466 MHz.
- **Reads that may hang.** Level 0 reads CSIS1 only when ISPPRE is on. If its clock were gated, that read could hang
  rather than abort.
- **The S2MPB02's ID check.** It uses 0x1D = 0x90, as fled.c does. If that check is wrong, the camera refuses
  cleanly with "not the expected one".
- **The model ID.** The 3H1's exact model ID is not in our sources. The probe accepts any ID that is not 0 or 0xFFFF
  and logs it. If the high byte is 0x30 or 0x31, it says "a Samsung S5K3 series ID".
- **Missed frames.** A preview render must finish before the WDMA comes round to the same slot again (3 frames,
  100 ms). If it does not, the frame tears; nothing breaks.
- **The setfile's source.** The 85 setfile values were cross-checked against an earlier port's recorded table. They have not
  been checked against Samsung's GPL file, because the sparse checkout does not have `fimc-is-cis-3h1-setA.h`. The lead
  can check them with `git show HEAD:drivers/media/platform/exynos/fimc-is2/sensor/module_framework/cis/fimc-is-cis-3h1-setA.h`.

## Left to do

- Run probe levels 0, 1 and 2 on the phone.
- Use the frame-end interrupt instead of polling.
- Lens shading and a colour matrix from the module's calibration, if it can be read.
- JPEG.
- The rear S5K2L3: CSIS0, its own setfile and lanes, the AK737x focus, the OIS, the AK7372 aperture, and the LED flash.
