/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the Cirrus Logic CS47L93 codec ("Madera", the CS47L92's driver) for the in-call microphone (codec.c).
 *
 * Where it is (Samsung's starlte DT, exynos9810-starlte_sound.dtsi spi_2 / cs47l93@0, exynos9810.dtsi spi_2 / usi_1,
 * exynos9810-pinctrl.dtsi spi2-bus / spi2-cs-func, exynos9810-starlte_common.dtsi codec-reset; the phone's own DTB
 * device/dtb/starlte.dts for the supplies):
 *   SPI 0x10470000, USI01 of PERIC0 (mode word 0x1041100C = 2, SPI), chip select 0 by hand, mode 0, at most 25 MHz,
 *   feedback delay 1; pins gpp0-4/5/6 (CLK, MISO, MOSI: function 2, no pull) and gpp0-7 (nCS: function 2, pull-up);
 *   reset gpg1-0 (output, low = held in reset); AVDD / DBVDD1 / CPVDD1 = LDO33 (vdd_pmic_codec_1p8, always on; S2MPS18
 *   L33CTRL 0x6A), CPVDD2 / DCVDD = LDO34 (vdd_codec_1p2, 1.2 V, enable ramp 5 ms; L34CTRL 0x6B; this phone's
 *   Starbite probes read it on, 0xE8); MCLK1 = the SoC's CLKOUT0 (PMU 0x14060A00, the 26 MHz TCXO).
 *
 * The microphone path (One UI's mixer_paths.xml incall_nb-handset-mic = route-cp-tx + dev-dual-mic; mixer_gains.xml
 * gain-incall_nb-handset-mic): the main microphone DMIC1 on IN3L (MICBIAS1A) to AIF1TX1 at "IN3L Digital Volume" 148
 * (+10 dB), the second microphone DMIC4 on IN4L (MICBIAS1B) to AIF1TX2 at 143 (+7.5 dB); IN3/IN4 OSR 3.072 MHz, their
 * HPFs on at 10 Hz, input ramp 8 ms/6 dB; AIF1 an I2S slave (CBS_CFS, NB_NF) of ABOX's UAIF0, 48 kHz, 2 x 16 bits.
 * Clocks as One UI's sound node: SYSCLK 98.304 MHz from FLL1, FLL1 from MCLK1 at 26 MHz.
 * Two differences from One UI, on purpose (codec.c says why): AIF1 runs on SYSCLK (One UI: ASYNCCLK from FLL2 locked to
 * AIF1's BCLK), and the two microphones go straight to AIF1TX1/2 (One UI: through LHPF1/2 and ASRC1, whose filter
 * coefficients are not in the vendor files).
 *
 * Every call logs one line per step ("codec: ...") and returns 0 or a negative AUD_E* code (amp.h); a failure leaves
 * the codec in reset with LDO34 as it was found. */
#pragma once
#include "amp.h"                    /* struct audio_hal, AUD_E* */

#define CODEC_SPI 0x10470000UL
#define CODEC_ID 0x6371U            /* CS47L92_SILICON_ID (madera-core.c): CS47L92 and CS47L93 */
#define CODEC_MIC_VOL_MAIN 148U     /* IN3L Digital Volume: 0x80 = 0 dB, 0.5 dB a step */
#define CODEC_MIC_VOL_SUB 143U      /* IN4L */

void codec_hal_set(const struct audio_hal *hal);
int codec_probe(void);               /* power/LDO and SPI mode on USI01 as the source says, release reset, read the ID
                                      * (0x6371 expected), wait BOOT_DONE; 0 or <0 */
int codec_mic(int on);               /* the main mic (DMIC per One UI's handset route) to AIF1, an I2S slave on ABOX
                                      * UAIF0, 48 kHz 2x16; off = mic path off */
void codec_off(void);                /* back in reset / low power */
const char *codec_why(void);         /* the last failure, or the state */

/* One 16-bit register, only once probed (for diagnostics): 0 or <0. */
int codec_read(uint32_t reg, uint16_t *v);
/* The first attended check: probe (ID, revision, BOOT_DONE), a few status registers read, then back in reset. */
int codec_selftest(void);

/* ---- for codec_media.c only (the jack, the headphones, media capture; codec_media.h is the API): additive ----
 * Calls win: codec_probe, codec_mic(1), codec_off and codec_hal_set first call the yield hook (reset = 1 when the
 * codec is about to be reset), so media lets go of the outputs, AIF1 and the clocks before the call path takes them. */
void codec_set_yield(void (*fn)(int reset));
const struct audio_hal *codec_hal(void);   /* the HAL set with codec_hal_set, or NULL */
unsigned codec_gen(void);            /* 0: not probed (in reset); else a count that changes on every new power-up */
int codec_call_active(void);         /* 1 while codec_mic(1) holds the codec */
int codec_up(unsigned *gen);         /* probed + the after-boot setup (nothing if done); 0 or <0 (then in reset) */
/* The media clocks: on = CLKOUT0, SYSCLK from FLL1 (locked), AIF1's format, SYSCLK_ENA; off = the reverse, CLKOUT0
 * back as found. AUD_EBUSY (on) / nothing (off) while the call path holds the codec; <0 leaves them off. */
int codec_clocks(int on);
int codec_write(uint32_t reg, uint16_t v);  /* one register, only once probed; the caller keeps its own register list */
/* Level-0 read: only once probed, or when the codec is out of reset and the bus was set up earlier in this boot;
 * nothing is set up or written to get there (AUD_ENOSYS otherwise). */
int codec_peek(uint32_t reg, uint16_t *v);
