# CS47L93 codec: the jack, the headphones, media capture (Starbite OS)

Files: `starbite/sbos/kern/codec_media.c` / `codec_media.h` (this part), on top of `kern/codec.c` / `codec.h`
(the bus, the reset, the boot, the clocks and the in-call microphone). Host test: `kern/host/codec-media-test.sh`.
State: written and host-tested on 2026-11-12. **Nothing of it has run on a phone yet.**

The earpiece and the loudspeaker are not on the codec. They are on the MAX98512 amplifiers (`kern/amp.c`).

## Wiring

Sources: the phone's own DTB (`device/dtb/starlte.dts`, `cs47l93@0` and its `cirrus,accdet` / `acc@1`, the
`sound` node's `samsung,routing`), Samsung's GPL kernel (extcon-madera.c, madera.c, cs47l92.c, star_madera.c,
`include/linux/mfd/madera/registers.h`) and One UI's `mixer_paths.xml` / `mixer_gains.xml` from VENDOR.

| What | Where | Source |
|---|---|---|
| Codec bus, reset, supplies, MCLK1 | as `codec.h` says: SPI on USI01, reset gpg1-0, LDO33 / LDO34, CLKOUT0 | codec.h |
| Headphones | **HPOUT2L/R** (OUT2). The brief said HPOUT1; the DT routes `HEADPHONE` to HPOUT2 and acc@1 is for output 2 | routing, `acc@1 reg = 2` |
| Headset mic | IN1BL ("IN1L Mux" B), differential (`cirrus,inmode` IN1B = 0), MICBIAS2A | routing, mixer_paths |
| MICBIAS2 | 2.8 V, external cap, soft start; 2A with active discharge | `MICBIAS2`, `MICBIAS2A` nodes |
| MICVDD | 3.0 V (the mic charge pump, LDO2) | `micvdd` |
| Jack presence | JD2 through the mic clamp (`cirrus,jd-use-jd2`, `micd-clamp-mode = 8` = JD1L_JD2L). Present = MICD_CLAMP_RISE_STS1 (0x1886 bit 4) **clear** | extcon-madera.c `madera_jack_present` |
| Mic detect | `micd-configs <0 1 4 0 1>`: sense MICDET1, ground 1, bias MICBIAS2A, HP2 ground 1. `micd-software-compare` (ADC mode), rate 7, dbtime 1, bias start time 1, force micbias | acc@1 |
| Detect timing | detect debounce 300 ms, timeout 1000 ms, init delay 30 ms, open circuit = headset | acc@1 |
| Buttons | `micd-ranges`: <= 116 ohm play/pause, <= 173 voice, <= 321 vol up, <= 752 vol down, <= 1257 "search". At or above 1258 ohm = the mic (no button) | acc@1, extcon-madera.c |

## Gains (never above One UI's)

- Headphones: One UI writes "HPOUT2L/R Impedance Volume" 107 for media. This is DAC_DIGITAL_VOLUME_2L/2R; 0x80 is 0 dB,
  so 107 is -10.5 dB. star_madera.c then adds 0..20 steps from its headphone impedance measurement (HPDET). We do
  not measure the impedance. So our hard cap is **107 itself**, checked on every write of the two registers.
  Levels 1..15 run from 51 to 107 (-38.5 to -10.5 dB, 2 dB a step). Level 0 mutes but keeps the output on.
- The OUT2 mixers stay at One UI's "Input 1 Volume" 32 = 0 dB (register 0x80).
- Main mic for apps: IN3L Digital Volume 129 (+0.5 dB), gain-recording-mic.
- Headset mic: IN1L Volume 8 (+8 dB analogue PGA), IN1L Digital Volume 128 (0 dB), gain-recording-headset-mic.

## The sequences

**Jack set-up** (once per codec power-up, first poll): MICBIAS2 config, MICBIAS2A discharge, MICVDD level, MICD
control 1 (start time, rate, bias source, dbtime), MICD control 0 (ADC mode, sense, ground), HPD_GND_SEL and
HP2_GND_SEL, the five button levels (madera_micd_set_level), the clamp mode 8, the clamp / JD debounces, JD1_ENA and
JD2_ENA. The clamp status is only read after this.

**Polling**: `codec_jack_poll()` every 50 ms. No interrupt is used.
- Plug in: present for 3 polls (150 ms). Then MICD starts (MICVDD, clamp override off, ADC mode, MICBIAS2 + 2A,
  MICD_ENA).
- After 300 ms, each poll reads the MICDET ADC: MICD_ENA off, MICDET_ADCVAL read, MICD_ENA on again. The value
  indexes madera's level table to give ohms. Two equal results decide: >= 1258 ohm (or open) is a headset, less is
  headphones. Still undecided after 1 s means headphones.
- Headphones: MICD stops (MICD off, MICBIAS2 off, MICVDD off if nobody needs it, clamp override on).
- Headset: MICD keeps running and every poll reads the buttons. A button must read the same twice (100 ms). Events
  come from `codec_jack_event()` (+n press, -n release).
- Plug out: absent for 2 polls. The headphones and headset capture go off by themselves, a held button is released,
  and MICD stops.

**Headphones on** (`codec_hp(1, level)`): media clocks (`codec_clocks(1)`: CLKOUT0, SYSCLK from FLL1, AIF1's format,
SYSCLK_ENA) -> output ramps 1 ms/6 dB -> DAC volume set **muted** -> OUT2L/R mixers from AIF1RX1/2 at 0 dB ->
AIF1RX1/2 on -> OUT2L/R_ENA (the codec runs its own HP sequence) -> 12 ms (madera_out_ev: 6 ms per HP output on
CS47L92/93) -> HP2 ENABLE_DONE checked -> unmute, ramping up.

**Headphones off**: mute (ramps down) -> 10 ms -> OUT2 off -> 2 ms -> DISABLE_DONE checked -> AIF1RX off -> mixers
cleared -> clocks off if capture is off too. The output is never on while SYSCLK, the FLL or the reset go away (the
host test watches every write for this).

**Capture** (`codec_mic_media(1, which)`): MICVDD; main mic = IN3 OSR 3.072 MHz, HPF 10 Hz, IN3L at 129 muted,
AIF1TX1/2 <- IN3L, MICBIAS1 + 1A; headset = IN1L mux B differential, PGA +8 dB, 128 muted, AIF1TX1/2 <- IN1L,
MICBIAS2/2A. Then AIF1TX on, the input on, 2 ms, unmute, 2 ms, volume update (madera_in_ev).

## Calls win

`codec.c` calls this file's yield hook at the start of `codec_mic(1)`, `codec_probe`, `codec_off` and
`codec_hal_set`. The hook switches the headphones and capture off (in the order above) and releases the clocks, before
the call path writes anything. During a call, `codec_hp(1)` and `codec_mic_media(1)` return AUD_EBUSY. The jack and
the headset buttons keep working in a call. When a call ends, `codec.c` switches MICVDD off. The next jack poll puts
it back for the buttons. Nothing is switched back on after a call: snd.c asks again.

The additions to `codec.c` / `codec.h` are additive and leave the call path's behaviour unchanged: `codec_set_yield`,
`codec_hal`, `codec_gen`, `codec_call_active`, `codec_up`, `codec_clocks`, `codec_write` (no register list there;
`codec_media.c` keeps its own) and `codec_peek`. `kern/host/audio-test.sh` still passes.

## Probe (`drv_codec_probe`, name "codec")

| Level | Touches | Pass line |
|---|---|---|
| 0 | Reads only: GPG1 CON/DAT/PUD, LDO33/LDO34 through the PMIC read, the ID only if the codec is already out of reset with its bus set up earlier in the boot | `drvprobe: codec: PASS level 0: reset held (low), LDO33 on, LDO34 on 1200 mV; held in reset: ID not read, the codec untouched` |
| 1 | Bus up, reset released, ID / revision / BOOT_DONE, the patch, jack detect (JD, MICBIAS2, MICD) for 2 s. No output, no CLKOUT0, no PMIC write. Then back in reset | `drvprobe: codec: PASS level 1: ID 6371, revision N, BOOT_DONE; jack: no plug / headphones / headset (...); outputs off, back in reset` |
| 2 | As level 1 for 10 s, logging plug changes, button presses and the MICDET reading each second | `... PASS level 2: ...` |

Budgets: level 0 1000 ms, level 1 5000 ms, level 2 14000 ms. FAIL lines name the step that failed. The codec is
always back in reset after a FAIL. `drv_codec_abort()` switches everything off and puts the codec in reset.

## Risks and what is left

- Nothing has run on a phone. The clamp polarity (present = clamp status clear), the ADC-mode reads and the
  ENABLE_DONE bits come from Samsung's code and the register map. They have not been seen working.
- `MICD_CLAMP_OVD` is cleared during MICD and set after it, as Linux does. Its reset value and its effect on the
  clamp status are unverified.
- The patch (`cs47l92_patch`) writes HP2_GND_SEL = 5. The extcon code (and so this file) sets it to 1 when the jack
  is set up. On the phone, check for hum or a lower level on the headphones.
- No HPDET, so no impedance-based gain step: the headphones are 0..10 dB quieter at the top than One UI. No
  short-circuit or moisture detection.
- A headset button held while the plug goes in reads as headphones (as in Linux with one mic config).
- Pulling a headset out can short the mic contact first. That may give one spurious button press before the plug-out.
- The headset mic is not muted while a button is held.
- AIF1's RX slots are the codec's reset defaults (RX1 slot 0, RX2 slot 1). ABOX must clock UAIF0 (48 kHz, 2 x 16, I2S)
  before anything is heard.
- `codec_hal_set` from the modem path forgets codec.c's state. This file then re-detects the jack (about 1 s).
