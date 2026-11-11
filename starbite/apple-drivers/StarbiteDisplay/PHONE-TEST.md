# StarbiteDisplay: first phone test (milestone M8, display part)

Not run yet. Nothing in this folder has touched the phone. The integrating session builds the kernel
(INTEGRATION.md) and runs the host steps; you only press buttons if the phone has to be brought back by hand.

**Question:** does the DECON show a buffer of ours when only its address is written (a flip), and do its frame
interrupts reach XNU?

**Pass line (pmsg):**

```
StarbiteDisplay: M8 PASS: frame flips by address <N>, vsync IRQ counted <M>
```

with N >= 60 and M >= 120. The line is printed only from the DECON's own counters: a flip counts when the hardware
cleared SHADOW_REG_UPDATE_REQ after our write, a vsync only when the frame-start interrupt fired (the timer's
polling and synthetic ticks are counted apart and can never make this line).

## Two steps, smallest first

### M8-a: self test, no userland needed (about 15 s)

The driver itself flips 120 times between S-Boot's framebuffer and a second buffer with colour bars, then stops
the boot. It needs only the kernel up to IOKit matching, so it can run on the M2/M3 kernel already, before M4-M7.

Boot-args added to the test line (the rest as the current `tools/cmdline/apple-m1.txt`, without `sbstop=pe`):

```
sbstop=display sbdisp=flip sbdisp_selftest=120 sbdisp_stop_s=45 sbdisp_panel=0 sweetfbstats=0
```

- `sbdisp_panel=0`: no DSIM packet at all in this first test (one new thing at a time).
- `rd=` may stay as it is; whether the root mounts does not matter. `wdt=-1` stays.

What you see: the boot picture and vertical colour bars alternating for about 8 seconds, with a white line that
steps down the bars. No tearing should be visible. Then the phone restarts into TWRP by itself.

Expected log lines, in order:

```
STARBITE HEALTH: sbstop=display: test boot (0423): the watchdog is never fed (it resets in sbwdt=.. s ...
StarbiteDisplay: decon mapped, idma mapped, dpp mapped, dsim mapped (HS command mode), interrupts registered, panel policy 0
StarbiteDisplay: DECON as expected: window 5 on channel block 0 reads 0xcc000000, 1440x2960, trigger 0x1281, frame count ..
StarbiteDisplay: AppleCLCD 1440x2960 stride 5760 at 0xcc000000, canvas 828x1792, default surface .., mode flip, vsync by DECON interrupt
StarbiteDisplay: self test: 120 flips between 0xcc000000 and 0x9....... (flip mode)
StarbiteDisplay: self test: 120 of 120 flips taken by the DECON (0 later than two refreshes); flips latched +120, vsync IRQ +4.., FRAME_COUNT polled +0, synthetic 0: ok
StarbiteDisplay: M8 PASS: frame flips by address 120, vsync IRQ counted 4..
StarbiteDisplay: mode flip: flips requested 120 latched 120 replaced 0 ...
STARBITE: sbstop=display: stage reached, resetting into TWRP
```

(The window and channel numbers are what the device tree's defaults suggest; whatever the phone reports is logged
and is fine as long as the "as expected" line appears.)

How each failure reads:

| Log | Meaning | Next |
|---|---|---|
| `idma MISSING (reg 1)` / `dpp MISSING (reg 2)` | the ADT's decon node has only DECON's range | board file change in INTEGRATION.md step 1 |
| `DECON left alone: ...` | S-Boot/loader state is not what the probe knows; **nothing was written** | the line names the reason and three registers |
| `no contiguous ... below 4 GiB` | no room in the low RAM window for the 17 MiB buffer | reserve a carve-out (INTEGRATION.md, "Memory") |
| `self test: 0 of 120 flips taken` + `M8 FAIL` | the DECON never took the shadow update (trigger, or the request bits) | `sbdisp=copy` is unaffected; look at trigger value in the probe line |
| flips taken but the bars never showed, or showed garbage | the IDMA does not read plain physical addresses (SysMMU on), or the buffer is above the limit | NOTES.md open question 2 |
| `vsync IRQ +0, FRAME_COUNT polled +N` + `M8 FAIL` | flips work, interrupts do not arrive (GIC routing / enable) | flips are proven; fix interrupts with StarbiteGICv2, re-run |
| `synthetic` large, polled 0 | no frames at all: TE trigger masked | trigger value in the probe line |

`M8 FAIL` also resets into TWRP by itself (at 45 s of uptime), with the counters line before it.

### M8-b: with the OS (after M7)

Same kernel, the iOS 26 root, no `sbdisp_selftest`. Boot-args added:

```
sbstop=display sbdisp=flip sbdisp_stop_s=240 sbdisp_stop_flips=300 sbdisp_panel=1 sbdisp_bl=120 sweetfbstats=1
```

Pass: the same PASS line once backboardd's swaps have caused 300 flips; `SWEET FBSTAT` shows a lit frame;
`brightness 120 -> AOR 0x.... : sent`. Then, as M8 in the plan asks, a normal boot without `sbstop` where you
touch the screen (that part belongs to StarbiteTouch's test). `sbdisp=scale` (the DPP scales, canvas-sized
buffers) is a third, separate boot, only after flip mode has passed.

## Dead-man (independent of this driver and of XNU)

- `sbstop=display` on kernel **0423 or later**: XNU never feeds the watchdog the loader armed, so the SoC resets
  `sbwdt=` seconds after hand-off **whatever XNU does** (a hang, a spin with interrupts off, a panic loop), and
  0422 has already set INFORM3 so that reset lands in TWRP. This driver's own stop (`starlte_sbstop_now`) is only
  the fast way out; the watchdog is the guarantee.
- The catalog row must be one the loader launches with its watchdog (**not `no_launch`**: those get none). Check
  the HEALTH line: if it ends in `WARNING: the loader armed no watchdog`, do not rely on the test returning.
- Use `sbwdt=90` for M8-a (the driver's own limit is 45 s) and `sbwdt=300` for M8-b.
- House rules as always: one phone; dry run and verified backup first; boot by catalog key; `store-flash && boot`
  chained; one phone job at a time; not Android between 22:00 and 09:00.

## If the screen stays dark after the test

The driver sent no panel command in M8-a (`sbdisp_panel=0`) and never changes the DECON's mode, format, clocks or
the DSI link; the flip registers are re-programmed by S-Boot on every boot. A restart restores the picture. If the
phone does not come back to TWRP by itself within `sbwdt` seconds: hold Volume Down + Power for 7 seconds, then
Volume Up + Bixby + Power for recovery.
