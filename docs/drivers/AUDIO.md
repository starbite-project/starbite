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
abox.c): a later step for modem-core and this track.

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
int drv_audio_probe(int level, unsigned *wait_ms);   /* budget_ms 10000 (each level enforces its own: 2 / 4 / 10 s) */
void drv_audio_abort(void);
```

- Level 0 (read only, ~0 ms): PMU AUD status and the ABOX CPU bit; CMU_AUD PLL_AUD, SysMMU CTRL and IP_INDEX only if
  the domain is on (CMU_AUD is inside it); the amps' REV_ID through `amp_probe()` only if their HSI2C is already set up
  (USI07 in I2C mode, gpp4-4/5 in function, gates open: then amp.c writes nothing but the I2C transfers); the codec's
  ID through `audio_probe()` (reads it only if the codec is powered and out of reset). Lines:
  `drvprobe: audio: PASS read only: AUD domain off (nothing in it read), amps/codec as logged` (or `... on, PLL_AUD
  read ...`); `FAIL the amplifiers did not answer (0x39 / 0x3A on HSI2C 0x108C0000)`.
- Level 1 (silent, ~1.6 s, budget 4 s): apcm_up; RDMA0 plays zeroes to UAIF1 for 1 s with the amps OFF, measuring the
  bytes per second from STATUS; then WDMA1 from UAIF0 for 0.5 s with the codec untouched (expected: a moving pointer at
  192000 B/s and near-zero data; the codec's TX pin is idle); everything down, the domain back as found. Polls every
  50 ms (must be < 341 ms). Lines: `PASS RDMA0 -> UAIF1 and UAIF0 -> WDMA1 ran at 48 kHz 16-bit stereo; all back as
  found`; FAILs: `RDMA0 did not move ...`, `RDMA0 moved at the wrong rate: its CTRL0 format bits are needed ...`,
  `... WDMA1 did not move ...`, `ABOX did not come up in bypass ...`, `the AUD domain is not back as found`,
  `out of time: everything put back`.
- Level 2 (attended, ~4.1 s, budget 10 s): 1 kHz at -20 dBFS through the real mixer: 1 s earpiece at amp level 0, 1 s
  loudspeaker at amp level 0 (the quietest), then 2 s of the main mic with its RMS and peak logged. Line:
  `PASS 1 kHz sent to the earpiece and the loudspeaker (ask: both heard, quietly?); mic recorded` with
  `drvprobe: audio: mic: N samples in 2 s, RMS r, peak p (of 32767)` before it.

## 8. Risks and what is left

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

`sh starbite/sbos/kern/host/snd-test.sh` (160 checks): bring-up and stop order, no write outside the audio blocks and
no access to the domain while it is off, the mixer (sum, volume, clipping, mono / stereo, mute, jack), tones by zero
crossings, the guard on a full-scale square, call suspend / resume and a missed suspend, eleven injected failures each
leaving AUD off and the pins as found, capture, and each probe level (with its FAILs, abort and time-out).
`kern/host/audio-test.sh` and the abox host test still pass.
