/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Exynos 9810 ACPM IPC: the AP's side of the APM mailbox (0x14100000) and the APM's SRAM (0x02039000), plain C,
 * no IOKit. Shared by StarbiteACPM.cpp and the host tests (test/). Register facts and sources: ../NOTES.md.
 *
 * All MMIO goes through struct acpm_io, so the same code runs against a mock APM on the Mac. Every SRAM access is
 * 32 bits wide and at or above sram_first (the pages below it fault on the phone).
 */
#ifndef STARBITE_ACPM_IPC_H
#define STARBITE_ACPM_IPC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mailbox registers (offsets in the 0x14100000 block) */
#define ACPM_MB_INTGR0          0x08u   /* AP -> APM doorbell: bit 16 + channel id */
#define ACPM_MB_INTCR1          0x20u   /* APM -> AP clear: bit = channel id */
#define ACPM_MB_INTMR1          0x24u   /* APM -> AP mask (read only here) */
#define ACPM_MB_INTSR1          0x28u   /* APM -> AP status: bit = channel id */

/* firmware "initdata" (offsets from initdata-base) */
#define ACPM_ID_CHANNELS        0x08u   /* SRAM offset of the channel table */
#define ACPM_ID_NCHANNELS       0x0cu
#define ACPM_ID_AP_MAX          0x18u   /* how many of them are the AP's */
#define ACPM_ID_STRINGS         0x60u   /* version / build text (24 bytes logged) */

/* one channel descriptor, 0x48 bytes */
#define ACPM_CH_SIZE            0x48u
#define ACPM_CH_ID              0x00u   /* low byte */
#define ACPM_CH_OWNER           0x08u
#define ACPM_CH_TYPE            0x0cu
#define ACPM_CH_IN_REAR         0x10u   /* AP -> APM: where the APM reads (SRAM offset of an index word) */
#define ACPM_CH_IN_FRONT        0x14u   /* AP -> APM: where we write */
#define ACPM_CH_IN_BASE         0x18u   /* AP -> APM: queue base */
#define ACPM_CH_OUT_REAR        0x28u   /* APM -> AP: where we read */
#define ACPM_CH_OUT_FRONT       0x2cu   /* APM -> AP: where the APM writes */
#define ACPM_CH_OUT_BASE        0x30u
#define ACPM_CH_QLEN            0x34u
#define ACPM_CH_ELEM            0x38u
#define ACPM_CH_AP_POLL         0x44u

#define ACPM_SEQ_SHIFT          16u
#define ACPM_SEQ_MASK           (0x3fu << ACPM_SEQ_SHIFT)
#define ACPM_MAX_ID             16u     /* channel ids we keep (the S9 uses 0..11) */
#define ACPM_AP_MAX_LIMIT       32u

/* S9 values (device/dtb/starlte.dts acpm_ipc, s2mps18mfd; an earlier port exynos9810_soc.h) */
#define ACPM_S9_SRAM_SIZE       0x30000u
#define ACPM_S9_SRAM_FIRST      0x3000u
#define ACPM_S9_INITDATA        0x7f00u
#define ACPM_CH_PMIC            2u      /* s2mps18mfd acpm-ipc-channel */

/* timing (us) */
#define ACPM_ROOM_US            20000u  /* a place in the request queue */
#define ACPM_ANSWER_US          600000u /* an answer, per send */
#define ACPM_BUFFER_US          1000000u
#define ACPM_POLL_US            50u
#define ACPM_SENDS              3u

/* results */
#define ACPM_OK                 0
#define ACPM_E_NODEV            (-1)    /* no such channel, or not set up */
#define ACPM_E_TIMEOUT          (-2)    /* no answer / no room */
#define ACPM_E_STATUS           (-3)    /* the APM answered with a non-zero status (SPEEDY failed) */
#define ACPM_E_REFUSED          (-4)    /* the write policy said no */
#define ACPM_E_TABLE            (-5)    /* the firmware's table makes no sense */
#define ACPM_E_ARG              (-6)

struct acpm_io {
	void *ctx;
	uint32_t (*sram_rd)(void *ctx, uint32_t off);           /* off: from the SRAM's start, 4-aligned */
	void (*sram_wr)(void *ctx, uint32_t off, uint32_t v);
	uint32_t (*mbox_rd)(void *ctx, uint32_t off);
	void (*mbox_wr)(void *ctx, uint32_t off, uint32_t v);
	void (*barrier)(void *ctx);                             /* DSB SY */
	void (*delay_us)(void *ctx, uint32_t us);
	uint64_t (*now_us)(void *ctx);                          /* monotonic */
	void (*log)(void *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

struct acpm_chan {
	bool usable, buffer;
	uint32_t desc;                                          /* SRAM offset of the descriptor */
	uint32_t owner, type, ap_poll;
	uint32_t in_rear, in_front, in_base, out_rear, out_front, out_base, len, elem;
	uint32_t seq;
};

struct acpm_stats {
	uint32_t requests, resends, timeouts, status_errors, stale_drained, refused;
};

struct acpm {
	const struct acpm_io *io;
	uint32_t sram_size, sram_first, initdata;
	uint32_t table, nchannels, ap_max;
	uint32_t index0_id;                                     /* id of the channel at table index 0 (DVFS) */
	bool ready;
	char version[25];
	struct acpm_chan ch[ACPM_MAX_ID];
	struct acpm_stats st;
	/* timing, settable by the tests */
	uint32_t room_us, answer_us, buffer_us, poll_us, sends;
};

/* Parse the firmware's table. Returns ACPM_OK when the PMIC channel (2) is usable. Reads, plus a drain of stale
 * answers (the answer queue's read index set to its write index, INTCR1) on each queue channel. */
int acpm_setup(struct acpm *a, const struct acpm_io *io, uint32_t sram_size, uint32_t sram_first, uint32_t initdata);

/* One request on channel id; cmd[4] in, the answer's four words out. Caller serialises (one at a time). */
int acpm_request(struct acpm *a, uint32_t id, uint32_t cmd[4]);

/* PMIC framing (channel 2): w0 = type << 8 | reg; w1 = func | value << 8 | mask << 16; answer w1 [15:8] value,
 * [31:24] status (0 = good). type: 0 TOP, 1 PMIC, 2 RTC (the S2MPS18's blocks). */
#define ACPM_PMIC_TOP           0u
#define ACPM_PMIC_PMIC          1u
#define ACPM_PMIC_RTC           2u
#define ACPM_FUNC_READ          0u
#define ACPM_FUNC_WRITE         1u
#define ACPM_FUNC_UPDATE        2u

static inline uint32_t
acpm_pmic_w0(uint32_t type, uint32_t reg)
{
	return (type & 0xfu) << 8 | (reg & 0xffu);
}

static inline uint32_t
acpm_pmic_w1(uint32_t func, uint8_t value, uint8_t mask)
{
	return (func & 0xffu) | (uint32_t)value << 8 | (uint32_t)mask << 16;
}

/* Unchecked PMIC access (no policy): StarbiteACPM.cpp applies s2mps18_policy first. */
int acpm_pmic_read(struct acpm *a, uint32_t type, uint32_t reg, uint8_t *value);
int acpm_pmic_write_raw(struct acpm *a, uint32_t type, uint32_t reg, uint8_t value);

/* DVFS framing (channel at table index 0, as Linux): { domain, kHz, 0 = set | 1 = get, 0 }. */
#define ACPM_DVFS_CPUCL0        2u      /* Cortex-A55 */
#define ACPM_DVFS_CPUCL1        3u      /* Exynos M3 */
#define ACPM_DVFS_G3D           4u
int acpm_dvfs_request(struct acpm *a, uint32_t domain, uint32_t khz, bool get, uint32_t *answer_khz);

const char *acpm_strerror(int e);

#ifdef __cplusplus
}
#endif
#endif /* STARBITE_ACPM_IPC_H */
