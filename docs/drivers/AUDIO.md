# Starbite OS: media sound (playback and the main mic outside calls)

Files: `starbite/sbos/kern/abox_pcm.c/.h` (ABOX in bypass), `starbite/sbos/kern/snd.c/.h` (the sound service and
the probe), host test `starbite/sbos/kern/host/snd-test.c` + `snd-test.sh`. The call path (`abox.c` with Calliope,
`amp.c`, `codec.c`, wired from `modem.c`) is unchanged; this file reuses `amp.c` and `codec.c` through their public
calls and leaves `abox.c` alone.

Status: written 2026-11-12, host-tested, **not run on a phone yet**. The register facts come from Samsung's GPL
ABOX driver and from an earlier port's AP-only driver, which played through the speakers and recorded the main mic on this S9.

## 1. The path

```
playback  ring in abox_rmem -> RDMA0 -> SPUS chain 0 (no ASRC) -> SIFS1 -> UAIF1 (I2S master) -> MAX98512 0x3A (speaker)
                                                                                                  or 0x39 (earpiece)
                                                                     -> UAIF0 (I2S master) -> CS47L93 AIF1RX (headphones)
capture   CS47L93 IN3L (main mic) -> AIF1TX1 -> UAIF0 -> NSRC0 -> SPUM (no ASRC) -> SIFM0 -> WDMA1 -> ring in abox_rmem
```

ABOX's own Cortex-A7 stays off and no firmware runs ("bypass"). The AP programs the DMAs and polls their position.

## 2. Wiring and addresses

| What | Where | Source |
|---|---|---|
| AUD power domain | PMU 0x14064000 [3:0] = 0xF on, status 0x14064004 | flexpmu aud_on; kern/abox.c |
| DTZPC restore / save | SMC 0x82000410 (1 / 0, 0x17C10204) | exynos_pd_tz_restore / save |
| ABOX CPU | PMU 0x415C / 0x4160 / 0x4164 bit 28 (kept off) | abox.c abox_cpu_power / enable |
| CMU_AUD | 0x17C00000: PLL_AUD 1179.648 MHz (M 45 P 1 S 0 K 24319); AUDIF /16 = 73.728 MHz; UAIF0/1 /48 = 1.536 MHz BCLK | cmucal; kern/abox.c |
| ABOX SFR | 0x17C50000: ROUTE_CTRL0 0x40 (UAIFn SPK, 2 = SIFS1), ROUTE_CTRL1 0x44 (NSRC0 = 8, UAIF0), SPUS_CTRL0 0x200 (chain 0 = 0), SPUS_CTRL1 0x204 [18:16] = 0, SPUM_CTRL0 0x300 (bits 4, 7 clear), SPUM_CTRL3 0x30C bit 0 | Samsung abox.h, abox.c DAPM texts |
| UAIF0/1 | 0x500 / 0x510: CTRL1 0x0984F000 (I2S, 2 x 16, NB_NF), CTRL0 master; SPK_EN bit 0, MIC_EN bit 1 | abox_if.c; kern/abox.c |
| RDMA0 / WDMA1 | 0x17C51000 / 0x17C52100: CTRL0, BUF_STR +8, BUF_END +C, BUF_OFFSET +10, STR_POINT +14, VOL_FACTOR +18, STATUS +30 (PROGRESS 31, offset [28:16] x 16, count [12:0]) | Samsung abox.h, abox_rdma.c pointer |
| Sample banks | SPUS SBANK_RDMA0 +0x220, SPUM SBANK_NSRC0 +0x328: SIZE [29:20] x 16 bytes; read 0 after boot, given 512 | an earlier port (seen on this S9) |
| SysMMU_AUD | 0x17E00000 CTRL: found 4 (translating), set 0 (pass-through) | an earlier port (seen on this S9) |
| Pins | gpb1-0..3 (UAIF1), gpb0-1..4 (UAIF0): function 2, pull 1; pads released (PMU 0x4170 bit 28, 0x1424 bit 0) | DT aud-i2s*-bus; abox_pad_retention |
| Rings | abox_rmem 0xFB200000 + 0x1580000 (play) / + 0x15C0000 (capture), 64 KiB each, then 64 KiB of zeroes | ours; below 4 GiB, outside the call's regions |
| Amps | MAX98512 pair, HSI2C 0x108C0000, 0x39 earpiece, 0x3A speaker | amp.h |
| Codec | CS47L93 on SPI 0x10470000; main mic IN3L -> AIF1TX1 | codec.h |

## 3. Bring-up (apcm_up, then apcm_play_start / apcm_cap_start)

1. Refuse if the call driver owns ABOX (`abox_state()` BOOTING / READY / CALL).
2. AUD domain on (if off), wait 10 ms, DTZPC restore SMC must answer 0. IP_INDEX must read "ABOX".
3. ABOX's CPU switched off if the boot left it on (an earlier port saw it on); it stays off.
4. Clocks: MISC_CON 1, Q-channels (CPU clock, SysMMU, BCLK0, BCLK1), dividers, PLL_AUD (left alone if already right).
5. SysMMU CTRL 0 if it translates or blocks; read back.
6. Pins and pads. SYSPOWER_CTRL 1, QCHANNEL_DISABLE ACLK + BCLK_UAIF0 + BCLK_UAIF1 (an earlier port).
7. UAIF0/1 formats, nothing enabled.
8. Playback: ring zeroed, route, RDMA0's sample bank (512 if 0), RDMA0 BUF_*, VOL_FACTOR 0x800000 if 0, BURST_LEN 8
   if 0; UAIF SPK_EN first, then RDMA0 ENABLE (Samsung's POST_PRE order).
9. Capture: route, NSRC0's bank, WDMA1 programmed, WDMA1 ENABLE first, then UAIF0 MIC_EN (PRE_POST).

Stop: RDMA0 ENABLE off and wait until it reads 0 (40 ms, Samsung's barrier), 1 ms for the UAIF FIFO, then SPK_EN off.
Capture: MIC_EN off, then WDMA1. `apcm_down` puts every saved register back (UAIF CTRL0/1, routes, SPUS/SPUM, sample
banks, SYSPOWER, QCHANNEL_DISABLE, SysMMU CTRL, pins, Q-channel requests, MISC_CON) and powers the domain off only if
it switched it on. Every failure runs the same undo, so AUD is off again (or as found).

## 4. The sound service (snd.h)

- 4 streams (48 kHz stereo S16, 85 ms queue each, or a Linux shared ring) + 1 tone generator, mixed in 32 bits.
- Volume 0..15 per output, 2.5 dB a step (15 = 0 dB digital, 1 = -35 dB, 0 = mute). Defaults: speaker 10, earpiece
  10, headphones 8. The amp level is fixed per output: speaker `amp_speaker(1, 5)` (amp.c's cap: 15 dB under One UI),
  earpiece `amp_receiver(1, 3)` (One UI's handset gain). Headphones: digital unity, `codec_hp(1, volume)`.
- Route AUTO: headphones when `snd_jack(1)` and the hook exists, else the loudspeaker. Earpiece only on request.
  The amp outputs get a mono mix on both slots; headphones keep stereo.
- Tones (made in code): ring (659 / 831 / 988 Hz figure, repeating), alarm (4 x 1 kHz beeps, repeating), notification
  (784 then 1175 Hz, once), key click (6 ms 3 kHz tick), test (1 kHz -20 dBFS). 4 ms ramps, no clicks.
- Guard (stands in for DSM, which runs on ABOX's firmware): a peak limiter (instant attack, ~43 ms release) and a power
  budget on the mean square, averaged ~1.4 s and ~22 s; over either limit it takes 0.1 dB a period (9.4 dB/s) to
  -24 dB at most, under 70 % of both it gives 0.05 dB a period back.

  | Output | Peak ceiling | Short (1.4 s) | Long (22 s) |
  |---|---|---|---|
  | loudspeaker | -3 dBFS | -10 dBFS RMS | -18 dBFS RMS |
  | earpiece | -6 dBFS | -12 dBFS RMS | -18 dBFS RMS |
  | headphones | -1 dBFS | none | none |

- Polling: `snd_poll(now_us)` every 5 ms while `snd_busy()`. The mixer keeps 3 periods (10.67 ms each) ahead of
  RDMA0; a poll later than ~21 ms gives a gap (the DMA reads zeroes: played periods are zeroed), never stale sound,
  unless the kernel stalls longer than the whole ring (341 ms). Latency: 21..32 ms plus the app's own queue.
  When idle (`snd_busy()` 0) it needs no poll. The engine stops 2 s after the last sound: amps off first, then RDMA0,
  then the AUD domain.
- Capture: `snd_cap_open()` (codec mic on, WDMA1), `snd_cap_read()` 48 kHz mono S16 (the left slot = AIF1TX1 = main
  mic), 0.5 s queue, `snd_cap_level()` RMS of the last 100 ms.
- `snd_why()`: plain words for the UI ("Playing on the loudspeaker", "Sound is paused for a call", "Sound could not
  start: the amplifier did not start", ...).

## 5. Calls win

`snd_suspend_for_call()` stops everything (tone, output, capture, RDMA0) and powers ABOX down as found;
`snd_resume_after_call()` lets it play again. Queued stream frames are kept. modem.c (modem-core's file, not edited)
needs two lines in `audio_prepare()` / `audio_stop()`:

```c
snd_suspend_for_call();      /* in audio_prepare(), right before r = abox_boot(); */
snd_resume_after_call();     /* in audio_stop(), right after abox_off(); */
```

If a call takes ABOX without the first line, `snd_poll` notices (`abox_state()`), drops its state with
`apcm_forget()` and writes nothing more to ABOX. Note: modem.c boots ABOX when Cellular starts (VSS must be up before
the CP), so with these lines **media sound pauses whenever Cellular is on**, not only during a call. Playing media
while Calliope runs needs the firmware path (RDMA0 through Calliope's PCM IPC, with RDMA0's IOVA window mapped by
abox.c): section 10 adds it, and with the Calliope engine media shares the modem's Calliope instead of pausing.

## 6. Interfaces for apps and the Linux guest (proposals; app.c and sbapp.h are not edited)

Syscalls in the free "alarm and sound" range 75..79:

| Nr | Name | Arguments | Returns |
|---|---|---|---|
| 75 | `SYS_SND_OPEN` | - | a stream id 0..3, or SND_EBUSY |
| 76 | `SYS_SND_WRITE` | id, frames (user pointer, stereo S16 48 kHz), n | frames taken (never blocks; 0 when full) |
| 77 | `SYS_SND_CTL` | op, a, b: CLOSE id / SPACE id / TONE kind / TONE_STOP / VOLUME out v / MUTE on / ROUTE out / STATE (struct: output, volumes, muted, playing, why) | 0 or <0 / a value |
| 78 | `SYS_MIC_OPEN` | on (1 open, 0 close) | 0 or <0 |
| 79 | `SYS_MIC_READ` | buffer (mono S16), n | samples copied |

Kernel side: each app's stream ids are closed when the app exits; the app in front may play, apps in the background
only tones they were granted (alarm). The kernel copies frames from user memory in `SYS_SND_WRITE` (the stream queue is
kernel memory).

Linux guest: one shared page per stream, `struct snd_shring` (snd.h): `magic` 'SNDR', `frames` (power of two, at most
8192), `head` (frames produced, written by the guest), `tail` (frames consumed, written by snd), `xruns`, then the
interleaved S16 frames. The guest writes frames at `head % frames` and then advances `head` (a store barrier between);
snd reads up to `head` during each mix and advances `tail`. Nothing blocks; a guest that falls behind gets silence and
`xruns` counts it. The host attaches it with `snd_open()` + `snd_attach_shring(id, ring)`. Capture for the guest:
the same layout with the roles swapped (a later step).

## 7. The probe (drvprobe.h)

```c
int drv_audio_probe(int level, unsigned *wait_ms);   /* budget_ms 12000 (each level enforces its own: 2 / 12 / 10 s) */
void drv_audio_abort(void);
```

- Level 0 (read only, ~0 ms): PMU AUD status and the ABOX CPU bit; CMU_AUD PLL_AUD, SysMMU CTRL and IP_INDEX only if
  the domain is on (CMU_AUD is inside it); the amps' REV_ID through `amp_probe()` only if their HSI2C is already set up
  (USI07 in I2C mode, gpp4-4/5 in function, gates open: then amp.c writes nothing but the I2C transfers); the codec's
  ID through `audio_probe()` (reads it only if the codec is powered and out of reset). Lines:
  `drvprobe: audio: PASS read only: AUD domain off (nothing in it read), amps/codec as logged` (or `... on, PLL_AUD
  read ...`); `FAIL the amplifiers did not answer (0x39 / 0x3A on HSI2C 0x108C0000)`.
- Level 1 (silent, ~1.6 s, up to ~6 s with the sweep, budget 12 s): apcm_up; RDMA0 plays zeroes to UAIF1 for 1 s
  with the amps OFF, measuring the bytes per second from STATUS. If RDMA0 does not move: `apcm_dump` (7 read-only
  lines, see 8) and the **RDMA0 trial sweep** (276 settings, ~8 ms each, stops at the first that moves RDMA0: DYN_CLOCK_OFF
  0, QCHANNEL_DISABLE all, SBANK_LIMIT 4 at 4 shifts, CTRL0 single bits 1..18 and 23..31, CTRL1 single bits and 9<<s,
  then CTRL0 9<<s + one bit). A setting that moves RDMA0 is adopted (`apcm_trial_adopt`) and RDMA0's rate measured
  again for 0.5 s with it. Then WDMA1 from UAIF0 for 0.5 s with the codec untouched (with the adopted setting too;
  expected: a moving pointer at 192000 B/s, near-zero data); a dump if it does not move; everything down, the domain
  back as found. Polls every 50 ms (must be < 341 ms). Lines: `PASS RDMA0 -> UAIF1 and UAIF0 -> WDMA1 ran at 48 kHz
  16-bit stereo; all back as found`; `FAIL FOUND: RDMA0 runs at 48 kHz only with <setting>; WDMA1 moved with it too`
  (the setting to make the default); FAILs: `RDMA0 did not move, nor with any trial setting (dump above): media needs
  Calliope here`, `RDMA0 moved at the wrong rate ...`, `... WDMA1 did not move ...`, `ABOX did not come up in bypass
  ...`, `RDMA0 did not stop during the trials`, `the AUD domain is not back as found`, `out of time: everything put
  back`.
- Level 2 (attended, ~4.1 s, budget 10 s): 1 kHz at -20 dBFS through the real mixer: 1 s earpiece at amp level 0, 1 s
  loudspeaker at amp level 0 (the quietest), then 2 s of the main mic with its RMS and peak logged. Line:
  `PASS 1 kHz sent to the earpiece and the loudspeaker (ask: both heard, quietly?); mic recorded` with
  `drvprobe: audio: mic: N samples in 2 s, RMS r, peak p (of 32767)` before it.

## 8. Risks and what is left

**First phone run (drvprobe-adbf542, 2026-11-12):** AUD was on at boot; up, clocks, SysMMU (4 -> 0), pins, banks all
took; RDMA0 then read 0 bytes in 1 s (UAIF1 STATUS 0) and WDMA1 wrote 0 (STATUS 0x80000000: in progress, nothing
written; UAIF0 STATUS 2 = ERROR_OF_MIC: UAIF0 clocked and its FIFO overflowed, so the I2S side runs and the stall is
between the SPUS/SPUM and memory). Compared with an earlier port's bypass, which played on this phone, two things differed:
1. an earlier port makes the DTZPC restore SMC every time; we skipped it because the domain was already on. **Changed:** it is
   now always made (a refusal only fails when we switched the domain on ourselves).
2. an earlier port (default `abox_rdma_fmt=1`) writes RDMA0 / WDMA1 CTRL0's FUNC 0, FORMAT (16-bit stereo) and BUF_UPDATE, and
   SBANK_LIMIT, "calliope's values" from a third reading (the Galaxy Tab Active3's). No public source names those bit
   positions (Samsung's 9810 and S10 headers name only ENABLE and BURST_LEN). Both DMAs stalling fits a missing
   BUF_UPDATE / FORMAT. **Diagnosed on the phone:** the level-1 sweep above finds the bit(s) if one or a format value
   plus one bit is enough, and the dump shows everything else.

Whether bypass can work: an earlier port's CHANGELOG lists the S9 speakers as seen working through exactly this path (RDMA0 ->
SIFS1 -> UAIF1, SysMMU off, CPU off) at 192000 B/s, so the hardware allows it; what we lack is the CTRL0 / SBANK_LIMIT
values. If the sweep finds nothing, media goes through Calliope (RDMA0 as a Calliope PCM: PCM_SET_BUFFER / HW_PARAMS /
TRIGGER, the RDMA0 IOVA window mapped by abox.c), which is also what media needs while Cellular is on (section 5).

`apcm_dump` lines ("apcm: dump <tag>: ..."): PMU AUD / CPU / pads, SYSPOWER ctrl/status, SYSTEM_CONFIG0, REMAP,
DYN_CLOCK_OFF, QCHANNEL_DISABLE; CMU_AUD PLL_CON0/3, lock, muxes, every divider; the Q-channel words 0x3000-0x302C and
0x3060; the SysMMU's CTRL/CFG/STATUS/INT/fault address; ROUTE_CTRL0-2, SPUS_CTRL0-3, the RDMA0 and MIXP sample banks,
SPUM_CTRL0-3, NSRC0's bank; UAIF0/1 CTRL0/CTRL1/STATUS, the gpb0/gpb1 pins; RDMA0 and WDMA1 0x00-0x30. Read only,
nothing if the domain is off. Also logged once a boot when the running engine finds RDMA0 stalled. `apcm_up` now logs
one "found" line (RDMA0 / WDMA1 CTRL0, UAIF1 CTRL0, DYN_CLOCK_OFF, QCHANNEL_DISABLE, SYSPOWER) before it changes them;
RDMA0 CTRL0/CTRL1/SBANK_LIMIT, WDMA1 CTRL0/SBANK_LIMIT and DYN_CLOCK_OFF are put back by `apcm_down` too.

- **RDMA0's CTRL0 format** is not in the 9810's source: CTRL0 is left as after reset (plus BURST_LEN 8 and ENABLE).
  Level 1 measures the rate; if it is not 192000 B/s, `apcm_cfg.rdma_ctrl0_or` takes the format bits (the S10's
  layout, which an earlier port used, would be the next thing to try).
- BUF_END = one past the end and VOL_FACTOR 0x800000 = unity are inferred (an earlier port's reading too); the zero guard after
  each ring keeps a different BUF_END meaning silent.
- UAIF0 as master with the codec in reset (level 1's capture) is assumed harmless (the codec's pins are inputs then).
- Headphones need `codec_hp(on, level)` from the codec agent: `snd_set_hp(codec_hp)` at init and `snd_jack(plugged)`
  from its jack detection. Until then AUTO always picks the loudspeaker.
- Kernel integration (not done here): `snd_init_libsb()` once at boot; `snd_poll(sb_us())` from a 5 ms repeating
  timer while `snd_busy()` (a new KT owner, e.g. 14); the syscalls of section 6.
- Media while Cellular is on (section 5).
- Speaker protection: the guard is ours, not Maxim's DSM; with amp.c's 15 dB cap under it the speaker is quieter than
  One UI.

## 9. Host test

`sh starbite/sbos/kern/host/snd-test.sh` (202 checks): bring-up and stop order, no write outside the audio blocks and
no access to the domain while it is off, the mixer (sum, volume, clipping, mono / stereo, mute, jack), tones by zero
crossings, the guard on a full-scale square, call suspend / resume and a missed suspend, eleven injected failures each
leaving AUD off and the pins as found, capture, and each probe level (with its FAILs, abort and time-out), plus the
phone's case: DMAs that move only with an unknown CTRL0 bit (the dump, the sweep finding and adopting it, WDMA1
moving with it, all back as found, the dump writing nothing) and a sweep that finds nothing.
`kern/host/audio-test.sh` and the abox host test still pass.

## 10. Calliope media

Four probe runs on 2026-11-12 showed that bypass does not work on this phone. Power, PLL_AUD, AUDIF, UAIF BCLK,
pins, the SysMMU and the DTZPC SMC were all fine, and UAIF0 even clocked, but RDMA0 / WDMA1 never moved
(STATUS 0x80000000), whatever the 1004 trial settings were. So media goes the way One UI sends it: through Calliope.

Files: `kern/abox_media.c/.h` (the stream and the probe `calliope`), additive hooks in `kern/abox.c/.h`, and the
Calliope engine in `kern/snd.c`. Host test: `kern/host/calliope-test.c` + `calliope-test.sh`.

### The path

```
ring in abox_rmem (0xFB200000 + 0x1580000, IOVA 0x91000000 in ABOX's SysMMU)
  -> RDMA0, run by Calliope -> SPUS chain 0 (ASRC off) -> SIFS0 (the mixer, MIXP) -> UAIF1 SPK -> the amps
                                                                                  or UAIF0 SPK -> the codec
```

One UI on this phone, from build/test-boots/20261108-091233-android-logged/dmesg-final.txt:
- Calliope says "ready to sing (version:CRL0)" 64 ms after abox_cpu_enable.
- It plays RDMA0 at 48000 Hz, 16 bit, 2 channels, as 4 periods of 3840 bytes (15360 bytes), into SIFS0 -> UAIF0.
- The order is: uaif startup, OPEN, uaif hw_params, SET_BUFFER + HW_PARAMS, PREPARE, abox_flush_mixp, then the
  uaif trigger, then the rdma trigger.
- It also runs RDMA7 -> SIFS1 -> UAIF1 and WDMA4 from UAIF1 (Maxim's DSM speaker protection on ABOX). We do not
  copy that: our speaker stays capped in amp.c.

### What the AP does (Samsung's abox_rdma.c, abox.c, abox_if.c)

1. Before `abox_boot()`, `cal_map()` asks abox.c (`abox_map_extra`) to add the ring to our SysMMU tables. It gets
   its own L2 table at abox_rmem + 0x1504400, with 4 KiB pages: IOVA 0x91000000 + 128 KiB -> abox_rmem + 0x1580000.
   This is the same window abox_pcm.c uses. The two never run together, because apcm refuses while Calliope owns
   ABOX. The window is counted: snd.c keeps it for every later boot, and the probe adds and removes its own.
2. `abox_boot()` (abox.c, unchanged, steps 1..13).
3. `cal_play_start(uaif, period, periods)`:
   - routes: SPUS_CTRL0 [3:0] = 0x2 (IN RDMA, OUT SIFS0, ASRC off), ROUTE_CTRL0 UAIFn SPK = 1 (SIFS0);
   - clocks: AUDIF /16 = 73.728 MHz, UAIFn /48 = BCLK 1.536 MHz; UAIFn master, CTRL1 0x0984F000;
   - the ring is zeroed;
   - IPC_PCMPLAYBACK channel 0: OPEN, SET_BUFFER (0x91000000, period, periods), HW_PARAMS (48000, 16, 2), PREPARE;
   - MIXP flush, SYSPOWER_CTRL bit 0 (DRAM on), UAIFn SPK_EN, TRIGGER 1.
   - It refuses if the ring is not in the tables the last boot wrote. That happens when someone booted Calliope
     before `cal_map()`; without this check, Calliope would fault on the SysMMU and stop.
4. Position: RDMA0 STATUS (SFR + 0x1030: offset [28:16] x 16 + count [12:0]). Calliope also sends
   PCM_PLTDAI_POINTER once a period; `abox_pcm_pointer()` keeps the last one and a count. `abox_poll()` takes
   Calliope's messages, every 10 ms or less.
5. `cal_play_stop()`:
   - TRIGGER 0, then wait up to 40 ms for RDMA0 ENABLE to clear, then 1 ms for the UAIF FIFO;
   - SPK_EN off, the UAIF, the routes and SYSPOWER as found, then HW_FREE, CLOSE and a MIXP flush.
   - During a call, the call's UAIF1, its route and DRAM on are left alone.

### Changes to abox.c (modem-core's file; additive, reviewed by them)

- `abox_map_extra(iova, pa, size)`: up to 2 extra windows. They are kept until removed with size 0, and step 5
  writes them into the tables. A window that clashes with one of ours, or lies outside abox_rmem's free
  page-table section (0x1510000..0x1600000), is refused or skipped, with a log line.
- `abox_pcm_msg(capture, id, type, a, b, c)`: one PCM task message. It refuses the call's own channels (playback 4,
  capture 2) and ids of 8 or more.
- `abox_pcm_pointer(capture, id, &count)`: `handle()` now stores the POINTER messages it already counted.
- `ABOX_IOVA_RDMA(n)` and `ABOX_PCM_*` in abox.h. Nothing else changes: the call path and its tests are as before.

### The probe `calliope` (in drvprobe.c just before `audio`; budget 12 s a level)

- **Level 0** only reads:
  - the AUD domain and the ABOX CPU (PMU), and IP_INDEX / SysMMU CTRL when the domain is on;
  - Calliope's three files on VENDOR, with their sizes and the time they took to read. They are read into
    abox_rmem, which nothing uses while ABOX's CPU is off. If the CPU is on, they are not read.
- **Level 1** is silent: the amps stay off and the codec stays in reset.
  - It boots Calliope and logs abox.c's 13 step lines, BOOT_DONE's version and up to 60 of Calliope's own
    "abox: fw:" lines. No CP runs, so no VSS "magic" line is expected.
  - It waits 300 ms and takes Calliope's post-boot messages (its log ring, about 30 REPORT_DUMPs, a gear request).
  - It plays zeroes on RDMA0 -> SIFS0 -> UAIF1 for 1 s in One UI's geometry. If that moved, it plays 0.5 s in
    snd's ring (32 x 2048). If it did not, it plays 0.5 s to UAIF0 instead (One UI's own boot route).
  - It measures RDMA0's STATUS (1 ms samples) and Calliope's pointer messages.
  - At each stream start it logs 60 more of Calliope's own lines, every PCM message it sends (with its fields),
    the next 24 messages Calliope sends (raw), and RDMA0's registers 100 ms after TRIGGER and at the end.
  - Then it stops and calls `abox_off()`. The AUD domain ends off, as Android's runtime suspend leaves it
    (SUSPEND / WFI, CPU off, SysMMU off, pad retention, TZPC save, aud_off). That is by design: the probe does not
    power it again and reads nothing inside it once it is off. modem-core owns the power sequence.
- **Level 2** is attended: the same, with a 1 kHz tone at -24 dBFS on the earpiece amp at level 0 (its lowest)
  for 2 s.
- Last line, one of:
  - `PASS Calliope booted (CRL0 in N ms), RDMA0 moved N B/s (STATUS) through it to UAIF1 (192000 expected); snd's 32 x 2048 ring N B/s (STATUS)`
  - `FAIL 3/4 measure: RDMA0 did not move through Calliope to UAIF1; to UAIF0 (One UI's boot route)[ it DID]: N B/s (...)`
  - `FAIL <step>: <why>; everything off`
  The steps are 1/4 boot, 2/4 stream, 3/4 measure and 4/4 off. The probe passes at 144000..240000 B/s, by
  STATUS or by pointers.
- `abox_boot()` is one call of up to about 3.5 s: the VENDOR reads plus up to 3 s waiting for BOOT_DONE. The
  runner's per-call limit (~100 ms) cannot be kept there.

### snd.c's Calliope engine (`snd_ops.use_calliope`; the kernel's `snd_init_libsb` sets it)

- snd's API is unchanged; `snd_ops` gains the optional `use_calliope` field (0 keeps bypass only, as the snd host
  test does).
- When something must play and apcm is off, snd uses the Calliope engine:
  - If Calliope does not run, snd boots it itself (one blocking `abox_boot()`, about 0.1..0.5 s).
  - It plays its usual 32 x 2048 ring through RDMA0 and calls `abox_poll()` from `snd_poll()`.
  - The stream stops after 2 s of silence. Calliope (if snd booted it) goes off 30 s later, so the next sound
    starts without a boot.
- If Calliope fails to boot twice, snd uses the bypass engine from then on.
- Capture still uses the bypass engine. It is refused while Calliope runs, and playback uses bypass while capture
  is open.
- How it works with modem.c:
  - **If the modem booted Calliope** (Cellular is on), snd plays through it and never turns it off.
  - **`snd_suspend_for_call()`** stops the stream, leaves Calliope running and hands it to the modem. The modem's
    `abox_boot()` then says "already runs", and its `audio_stop()` turns it off.
  - **`snd_resume_after_call()`**: media plays through the modem's Calliope again.
  - **A call without the suspend** (`abox_state()` CALL): snd stops its stream within one poll.
  - **The modem's `abox_off()` under a playing stream**: snd drops the stream and, 2 s later, boots its own.
  - In-call media mixing is possible (SIFS0 is the mixer), but calls still win: snd does not play during
    ABOX_ST_CALL.
- Proposed modem.c hooks (not edited):
  - `snd_suspend_for_call()` right before `abox_boot()` in `audio_prepare()` (as in section 5);
  - `snd_resume_after_call()` after `abox_call(0)` in `audio_route(0)`. That way media comes back after each call,
    not only after Cellular stops;
  - in `audio_stop()`, skip `abox_off()` while `snd_busy()` (a playing stream keeps Calliope), or call a new
    `snd_calliope_adopt()` that makes snd the owner.

### Risks

- None of this has run on the phone. abox.c's boot itself never ran on the phone either: the level 1 probe is its
  first run.
- No ACPM AUD DVFS hook, so the CA7 runs at the idle 294.9 MHz. One UI raised it to 1180 MHz while booting.
- `abox_boot()` blocks the single-threaded kernel loop while it reads VENDOR and waits for BOOT_DONE.
- Calliope may want more than One UI's RDMA0 messages before it moves a normal (non-call) RDMA, for example
  ABOX_CONFIG for UAIF1 or a gear request. If RDMA0 does not move, the probe's STATUS / pointer lines and
  Calliope's own log lines show where it stopped.
- The level 1 probe leaves the AUD domain off (abox_off). The bypass "audio" probe runs before it, so nothing in
  the probe image needs the domain afterwards.
- The bypass probe runs first and leaves its own values in ABOX's registers (RDMA0 BUF_STR = the ring's PA, the
  sample banks) while the domain stays on. Calliope then boots on that state, not on a reset one as on Android.

### Run 5 (1f1f277, 2026-11-13) and what run 6 adds

What run 5 showed:
- Calliope booted (CRL0 after 115 ms), and steps 1..13 went through.
- RDMA0 never started: CTRL0 stayed 0x400000, STATUS 0x80000000, no pointer messages.
- BUF_STR stayed at the bypass probe's 0xfc780000. Calliope never programmed RDMA0, so it did not act on our
  TRIGGER, or it refused an earlier message.
- Its log copy stopped at the 40-line cap during its boot lines.
- Its ~30 post-boot messages (REPORT_DUMP and the log ring) were still waiting in the RX slot while we sent the
  PCM messages. They were only taken during the measurement.

Our sequence, field by field, against Samsung's abox_rdma.c / abox.c / abox_gic.c and One UI's dmesg (both logged
boots: 20261108-091233 and 20261108-110840):

| | Samsung / One UI | ours |
|---|---|---|
| IPC id, SGI | IPC_PCMPLAYBACK 2; GICD SOFTINT (1 << 16) \| 2 | the same |
| task_id, channel_id | both = the RDMA id (0) | 0, 0 |
| order | OPEN (pcm open) -> uaif hw_params -> SET_BUFFER, HW_PARAMS (rdma hw_params) -> PREPARE -> flush MIXP -> uaif trigger -> TRIGGER 1 | the same |
| SET_BUFFER | phyaddr IOVA_RDMA_BUFFER(0) = 0x91000000, size = period bytes (3840), count = periods (4) | the same |
| HW_PARAMS | rate 48000, width 16, channels 2 | the same |
| TRIGGER | param.trigger 1 / 0; DRAM on (SYSPOWER) around it | the same |
| RDMA0 registers | the AP only reads CTRL0 (the disable barrier) and STATUS (the pointer) | the same: Calliope programs BUF_STR / END / CTRL0 |
| ERAP, mixer | ABOX_CONFIG rates / formats and ERAP close at boot (abox_restore_data) | the same, at boot (Calliope logged "Changing MIXER sample rate") |
| routes | SFR registers through regmap (SPUS_CTRL0, ROUTE_CTRL0): there is no SET_ROUTE IPC | the same registers |
| UAIF | abox_if_config_put is AP-side only; UAIF CTRL0 / CTRL1 written by the AP: no UAIF IPC | the same |
| CPU gear | gear 4 (394 MHz) requested at rdma open / uaif startup | none: no AUD DVFS hook, CA7 at 294.9 MHz |
| timing | RDMA0 opened ~1.3 s after BOOT_DONE | run 5: right after; run 6: 300 ms after |

Run 6 changes:
- 300 ms of post-boot messages are taken before the stream.
- At the stream start, abox.c gets 60 more fw lines (`abox_fw_log_more`) and 24 raw rx messages
  (`abox_trace_rx`). Both are new additive diagnostics hooks.
- Every PCM message is logged with its fields.
- RDMA0's registers are dumped after 100 ms and at the end.
- If UAIF1 does not move, UAIF0 (One UI's own route) is tried for 0.5 s.
- The domain is no longer powered again after `abox_off`.

Host test: `sh starbite/sbos/kern/host/calliope-test.sh` (234 checks). It runs against abox-test's fake plus a fake
RDMA0 that Calliope starts only after the full IPC sequence, and only if IOVA 0x91000000 is a page table onto the
ring (otherwise a SysMMU fault). It covers:
- each probe level;
- Samsung's order, the page tables, and the routes, UAIF and AUD domain as found;
- failures at every step leaving ABOX off: no BOOT_DONE, missing firmware, PCM IPC dead, RDMA0 still, no free
  window, ABOX busy, abort;
- the abox.c hooks' refusals;
- snd's Calliope engine: boot, play, idle stop, off after 30 s, call suspend / resume, the modem's `abox_off`, and
  a missed suspend.
