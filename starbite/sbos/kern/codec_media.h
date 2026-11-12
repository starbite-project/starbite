/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the CS47L93 codec for media (codec_media.c): the 3.5 mm jack (plug type and headset buttons), the
 * headphone output and microphone capture for apps. The in-call microphone stays codec.c's (codec_mic) and wins.
 *
 * Wiring (the phone's DTB device/dtb/starlte.dts, cs47l93@0 and its cirrus,accdet acc@1; the sound node's routing):
 *   the jack is accessory detect 1 for OUT2 (acc@1 reg 2): HEADPHONE = HPOUT2L/R, HEADSETMIC = IN1BL with MICBIAS2A
 *   (2.8 V, external cap, soft start, active discharge). Jack presence uses JD2 (cirrus,jd-use-jd2) through the mic
 *   clamp (cirrus,micd-clamp-mode 8 = JD1L_JD2L): present = MICD_CLAMP_RISE_STS1 clear in IRQ1_RAW_STATUS_7.
 *   Mic detect: micd-configs <0 1 4 0 1> (sense MICDET1, ground 1, bias MICBIAS2A, HP2 ground 1), software compare
 *   (the MICDET ADC), micd-rate 7, dbtime 1, bias start time 1, force micbias, detect debounce 300 ms, timeout 1 s,
 *   an open circuit counts as a headset. Buttons (micd-ranges, ohms -> key): <= 116 play/pause, <= 173 voice command,
 *   <= 321 volume up, <= 752 volume down, <= 1257 "search"; >= 1258 ohms is the mic (no button).
 *
 * Polled, no interrupt: call codec_jack_poll() every CODEC_JACK_POLL_MS (50 ms); every debounce below counts calls.
 * Each call is a few 8-byte SPI transfers (well under 1 ms); the first call (or the first after a reset) brings the
 * codec out of reset (codec_up, ~30 ms).
 *
 * Calls win: codec_mic(1) (and codec_probe / codec_off / codec_hal_set) first makes this file switch the headphones
 * and media capture off and release the clocks; while a call holds the codec, codec_hp(1) and codec_mic_media(1)
 * return AUD_EBUSY. Jack detection and the buttons keep working during a call. Nothing is switched back on when the
 * call ends: the caller (snd.c) asks again. */
#pragma once
#include "codec.h"

#define CODEC_JACK_NONE 0
#define CODEC_JACK_HEADPHONES 1     /* 3-pole: no mic */
#define CODEC_JACK_HEADSET 2        /* 4-pole with a mic (or an open mic contact: cirrus,micd-open-circuit-declare) */
#define CODEC_JACK_POLL_MS 50U

/* button events from codec_jack_event(): +n pressed, -n released */
#define CODEC_BTN_PLAY 1            /* KEY_MEDIA (play/pause, the hook) */
#define CODEC_BTN_VOICE 2           /* KEY_VOICECOMMAND */
#define CODEC_BTN_VOLUP 3
#define CODEC_BTN_VOLDOWN 4
#define CODEC_BTN_OTHER 5           /* the DT's fifth range (KEY_SEARCH) */

/* Headphone volume: "HPOUT2L/R Impedance Volume" = DAC_DIGITAL_VOLUME_2L/2R OUT2x_VOL, 0x80 = 0 dB, 0.5 dB a step.
 * One UI's media headphone gain (mixer_gains.xml gain-media-headset / -headphone) is 107 (-10.5 dB) plus an offset
 * from its impedance measurement (star_madera.c imp_table, 0..+20 steps). This file does not measure the impedance,
 * so its hard cap is 107 itself: never above One UI's. "HPOUT2x Input 1 Volume" (the mixer) stays One UI's 32 = 0 dB. */
#define CODEC_HP_LEVELS 16          /* 0 = muted (the output stays on), 1..15 = -38.5 .. -10.5 dB in 2 dB steps */
#define CODEC_HP_VOL_MAX 107U
extern const uint8_t codec_hp_vol[CODEC_HP_LEVELS];

/* Media mic gains (mixer_gains.xml gain-recording-mic / gain-recording-headset-mic) */
#define CODEC_MEDIA_MAIN_VOL 129U   /* IN3L Digital Volume: +0.5 dB */
#define CODEC_MEDIA_HS_PGA 8U       /* IN1L Volume (analogue PGA): +8 dB */
#define CODEC_MEDIA_HS_VOL 128U     /* IN1L Digital Volume: 0 dB */

/* Optional: the HAL (also passed to codec.c, which then forgets its state). In the kernel it is not needed: the first
 * call uses codec.c's HAL, or audio_hal_libsb() when codec.c has none yet. */
void codec_media_hal_set(const struct audio_hal *hal);

int codec_jack_poll(void);           /* CODEC_JACK_*; call every CODEC_JACK_POLL_MS */
int codec_jack_event(void);          /* the next button event (+n pressed, -n released), 0 = none; 8 are queued */
unsigned codec_jack_ohms(void);      /* the last MICDET reading in ohms (0xFFFFFFFF = open), for diagnostics */
/* Headphones: AIF1RX1/2 (an I2S slave of ABOX UAIF0, 48 kHz 2x16) -> OUT2L/R mixers (0 dB) -> HPOUT2L/R. on = 1
 * switches the output on (muted, enabled, then unmuted through the 1 ms/6 dB ramp) or changes the level; level is
 * clamped to 0..15 and mapped through codec_hp_vol (capped at CODEC_HP_VOL_MAX on every write). Refused (AUD_EPERM)
 * with no plug in, AUD_EBUSY during a call. The output goes off by itself when the plug is pulled out. */
int codec_hp(int on, int level);
/* Capture for apps to AIF1TX1/2 (both the same mono signal): which 0 = the main mic (DMIC1 on IN3L, MICBIAS1A),
 * 1 = the headset mic (IN1BL differential, MICBIAS2A; only with a headset in). AUD_EBUSY during a call. */
int codec_mic_media(int on, int which);
/* Everything of this file off (headphones, capture, mic detect) and, unless a call holds it, the codec back in
 * reset. The next codec_jack_poll() brings it up again. */
void codec_media_off(void);
const char *codec_media_why(void);   /* plain words: the state or the last failure */

/* The drivers-track probe (drvprobe.h), name "codec". Level 0: reads only (the reset pin, LDO33/LDO34, the ID only
 * if the codec is already out of reset with its bus set up). Level 1: up, ID / revision / BOOT_DONE, jack detect for
 * 2 s (outputs off), back in reset. Level 2: the same for 10 s while a person plugs, unplugs and presses buttons. */
int drv_codec_probe(int level, unsigned *wait_ms);
void drv_codec_abort(void);
