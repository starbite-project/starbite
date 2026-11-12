/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the MAX98512 amplifier pair for in-call audio (amp.c), and the small hardware HAL that amp.c and
 * codec.c share.
 *
 * The pair (Samsung's starlte DT, exynos9810-starlte_common.dtsi hsi2c_18 / max98512@39): HSI2C 0x108C0000 (USI07 of
 * PERIC1, pins gpp4-4/5 function 2), the main die at 0x39 and the sub die at 0x3A (maxim,sub_reg, mono_stereo 1).
 * One UI's mixer_paths.xml: the handset (earpiece) is "One Stop Mode" = "Receiver Left" (OSM_RCV_L: the 0x39 die
 * alone), the in-call speaker is "Mono Right" (OSM_MONO_R: the 0x3A die alone). Both dies are I2S slaves of ABOX's
 * UAIF1 (CBS_CFS, I2S, NB_NF), 48 kHz, 2 x 16 bits.
 *
 * Every call logs one line per step ("amp: ...") and returns 0 or a negative AUD_E* code; on any failure both dies are
 * left shut down (GLOBAL_SHDN 0, AMP_EN 0). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- the shared HAL (amp.c, codec.c) ---------------------------------------------------------------------------
 * Every hardware access of amp.c and codec.c goes through these hooks, so the host test can trace them all and the
 * kernel can pass libsb's (audio_hal_libsb() below). Physical addresses; return 0 or a negative AUD_E* code.
 * Each hook must be bounded in time (libsb's HSI2C message gives up after 50 ms). */
struct audio_hal {
    uint32_t (*rd32)(uintptr_t pa);
    void (*wr32)(uintptr_t pa, uint32_t v);
    /* wn bytes written to addr (7-bit), then, when rn > 0, a repeated start and rn bytes read; a stop at the end */
    int (*i2c_xfer)(uintptr_t bus, unsigned addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
    /* n bytes full duplex with the chip select held for the whole transfer (rx may be NULL); NULL: codec.c's own
     * polled driver of the Exynos SPI (s3c64xx) over rd32/wr32 */
    int (*spi_xfer)(uintptr_t bus, const uint8_t *tx, uint8_t *rx, unsigned n);
    /* bank = the physical address of the bank's CON register (DAT +4, PUD +8): the pin made an output at level
     * (the level first, then the function), no pull; gpio_get reads DAT (0/1, <0 error) */
    int (*gpio_set)(uintptr_t bank, unsigned pin, int level);
    int (*gpio_get)(uintptr_t bank, unsigned pin);
    /* S2MPS18's PMIC block (ACPM type 1): only L33CTRL 0x6A is read and L34CTRL 0x6B read / written */
    int (*pmic_read)(unsigned reg, uint8_t *v);
    int (*pmic_write)(unsigned reg, uint8_t v);         /* may be NULL: LDO34 is then never switched */
    void (*delay_us)(unsigned us);
    void (*log)(const char *line);
};

#define AUD_OK 0
#define AUD_EPERM (-1)              /* refused: a cap, a closed clock gate, an unexpected state */
#define AUD_EIO (-5)                /* a bus transaction failed (after the retries) */
#define AUD_EBUSY (-16)
#define AUD_ENODEV (-19)            /* the chip did not identify as expected */
#define AUD_EINVAL (-22)            /* a bad argument */
#define AUD_ENOSYS (-38)            /* no HAL, or a HAL hook missing */
#define AUD_ETIMEDOUT (-110)        /* a status never came (reset, boot, lock) */
#define AUD_EVERIFY (-121)          /* a register did not read back as written */

#ifdef SBOS_KERNEL
/* libsb's: MMIO with barriers, sb_i2c_msg on the amp's HSI2C (brought up on first use with S-Boot's timing, else
 * Android's 200 MHz / 400 kHz one), sb_udelay, sb_log, sb_pmic_read. pmic_write is NULL (LDO34 is only read: the phone
 * leaves it on at 1.2 V; a gated writer may be filled in by the caller on a copy). spi_xfer is NULL (codec.c's). */
const struct audio_hal *audio_hal_libsb(void);
#endif

/* ---- the amplifiers ------------------------------------------------------------------------------------------- */

#define AMP_BUS 0x108C0000UL        /* hsi2c_18: USI07 of PERIC1 */
#define AMP_ADDR_RCV 0x39U          /* the main die: the earpiece ("Receiver Left") */
#define AMP_ADDR_SPK 0x3AU          /* the sub die: the loudspeaker ("Mono Right") */
#define AMP_LEVELS 6                /* 0..5 */

/* The levels (SPK_GAIN [2:0]: 0 mute, n = +3n dB; AMP_VOL_CTRL [6:0]: -16 dB + 0.25 dB a step, 0x40 = 0 dB).
 * Earpiece: One UI's in-call handset gain (mixer_gains.xml gain-incall_nb-handset: "Rcv Digital Gain" 32,
 * "SPK Analog Left Gain" 3dB) is level 3; 4 and 5 stay there: nothing above it is ever written.
 * Speaker: One UI's in-call speaker is +15 dB analog at 0 dB digital (SPK Analog Right Gain 15dB, digital-gain 0x40),
 * with Maxim's DSM protecting the speaker on ABOX. There is no DSM here, so the speaker is capped at +3 dB analog and
 * -3 dB digital (the receiver's own DT values, maxim,spk-gain-rcv 1 / digital-gain-rcv 0x34): 0 dB in all, 15 dB
 * (1/32 of the power) under One UI's. The caps below are enforced on every write of these two registers. */
#define AMP_RCV_GAIN 1U             /* +3 dB: One UI's handset */
#define AMP_RCV_VOL_MAX 0x20U       /* -8 dB: One UI's handset ("Rcv Digital Gain" 32) */
#define AMP_SPK_GAIN_MAX 1U         /* +3 dB (One UI: 5, +15 dB) */
#define AMP_SPK_VOL_MAX 0x34U       /* -3 dB (One UI: 0x40, 0 dB) */
extern const uint8_t amp_rcv_vol[AMP_LEVELS];   /* 0x08 0x10 0x18 0x20 0x20 0x20: -14 .. -8 dB */
extern const uint8_t amp_spk_vol[AMP_LEVELS];   /* 0x00 0x10 0x1C 0x28 0x2E 0x34: -16 .. -3 dB */

void amp_hal_set(const struct audio_hal *hal);
int amp_probe(void);                 /* read the revision / ID registers of both dies; 0 or <0; logs what it found */
int amp_receiver(int on, int level); /* earpiece: receiver mode, I2S from ABOX UAIF1 48 kHz 2x16, level 0..5 mapped to
                                      * safe gains (level 3 = One UI's handset default, never above it) */
int amp_speaker(int on, int level);  /* loudspeaker for speakerphone: capped well below One UI's default because there
                                      * is no speaker protection (DSM) here; level 0..5 */
void amp_off(void);                  /* both dies shut down */
const char *amp_why(void);           /* the last failure, or the state */

/* What amp_probe found for a die (0x39 / 0x3A): 0 not found, 'A' MAX98512 (REV_ID 0x0402), 'B' MAX98512B (REV_ID at
 * 0x0600, its registers from 0x20 moved: max98512_remap_reg); *rev the REV_ID value. */
int amp_die_kind(unsigned addr, uint8_t *rev);
/* The first attended check: probe, both dies' status, the earpiece on at level 0 for hold_ms (the bus clocks from
 * UAIF1 are not needed: CLK_MON is off), its status read back, then everything off. 0 or <0. */
int amp_selftest(unsigned hold_ms);
