/* SPDX-License-Identifier: APSL-2.0 */
/*
 * What StarbiteACPM lets its clients do to the S2MPS18 through ACPM. The rule (docs/APPLE-S9-DRIVERS.md row 7,
 * task brief): no regulator write beyond what today's kernel already does on the phone. Every allowed write is
 * listed here with the patch that does it today; anything else is refused before it reaches the APM.
 *
 * Reads: everything except the PMIC block's clear-on-read interrupt registers INT1..INT7 (type 1, 0x00..0x06).
 * Writes (type 1 = PMIC block):
 *   0x6c L35CTRL tsp_io      0xec (on, 1.8 V)              starlte_touch.c st_power_on (0302); only if the
 *                                                          programmed vsel is 0x2c or 0
 *   0x74 L43CTRL tsp_avdd    0xf0 (on, 3.0 V)              same; only if vsel is 0 or 0x28..0x3c
 *   0x2b B6OUT   vdd_g3d     0x68 (950 mV), or the value read first this boot when that was in 0x20..0x70
 *                                                          starlte_gpu.c stage 2 (0301 / 0412)
 *   0x68 L31CTRL vmmc (SD)   0x28 (off, 2.8 V), 0xe8 (on, 2.8 V); only if vsel is 0 or 0x24..0x3c
 *                                                          starlte_sd.c rails / park (0345, 0400, 4aeec8f)
 *   0x4a L2CTRL1 vqmmc (SD)  0xc0 (on 1.8 V), 0xfc (on 3.3 V), 0x3c (off 3.3 V), or off at the vsel it has
 *                                                          starlte_sd.c (0345, 0353)
 * Writes (type 2 = RTC block), for StarbiteRTC only:
 *   0x02 UPDATE              the value read + RUDR (bit 0) or + WUDR (bit 1), nothing else changed
 *   0x05..0x0b SEC..YEAR     a value in its field's range
 * Never: type 0 (TOP), any other PMIC register (bucks, LDOs, OCP, SMPL/WTSR, ramp), RTC CTRL 0x00, WTSR_SMPL 0x01,
 * CAPSEL 0x03 (the 32 kHz oscillator's load), MSEC 0x04, the alarms 0x0c.. and OSCCTRL 0x1a.
 */
#ifndef STARBITE_S2MPS18_POLICY_H
#define STARBITE_S2MPS18_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2MPS18_B6OUT           0x2bu
#define S2MPS18_L2CTRL1         0x4au
#define S2MPS18_L31CTRL         0x68u
#define S2MPS18_L35CTRL         0x6cu
#define S2MPS18_L43CTRL         0x74u

#define S2MPS18_RTC_CTRL        0x00u
#define S2MPS18_RTC_WTSR_SMPL   0x01u
#define S2MPS18_RTC_UPDATE      0x02u
#define S2MPS18_RTC_CAPSEL      0x03u
#define S2MPS18_RTC_MSEC        0x04u
#define S2MPS18_RTC_SEC         0x05u
#define S2MPS18_RTC_YEAR        0x0bu
#define S2MPS18_RTC_RUDR        0x01u
#define S2MPS18_RTC_WUDR        0x02u

/* LDO control: [7:6] enable (3 = on), [5:0] vsel */
#define S2MPS18_LDO_ON          0xc0u
#define S2MPS18_LDO_VSEL        0x3fu

struct s2mps18_policy {
	bool b6_seen;                   /* B6OUT read once: its boot value kept for the restore rule */
	uint8_t b6_boot;
};

/* May (type, reg) be read? (false: a clear-on-read interrupt register) */
bool s2mps18_read_allowed(uint32_t type, uint32_t reg);

/* Record a value read (the policy remembers B6OUT's first value). */
void s2mps18_note_read(struct s2mps18_policy *p, uint32_t type, uint32_t reg, uint8_t value);

/* May value be written to (type, reg) whose current content is cur? why: the reason when not. */
bool s2mps18_write_allowed(const struct s2mps18_policy *p, uint32_t type, uint32_t reg, uint8_t value, uint8_t cur,
    const char **why);

/*
 * DVFS limits (the clocks today's kernel asks for): cpucl0 (2) and cpucl1 (3) at most 1794000 kHz
 * (starlte_touch.c st_cpu_dvfs, "not without thermal control"); cpucl1 up to 2704000 kHz only with a thermal
 * governor running (0411 starcpugov); g3d (4) only 572000 546000 455000 338000 299000 260000 (starlte_gpu.c);
 * every other domain refused (MIF 2093000 / INT 667000 froze the phone at once, boot69). 0 kHz is refused.
 */
bool acpm_dvfs_allowed(uint32_t domain, uint32_t khz, bool governor, const char **why);

#ifdef __cplusplus
}
#endif
#endif
