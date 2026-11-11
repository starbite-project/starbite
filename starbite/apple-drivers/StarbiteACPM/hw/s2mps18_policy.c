/* SPDX-License-Identifier: APSL-2.0 */
/* The S2MPS18 access policy of StarbiteACPM: see s2mps18_policy.h for the table and its sources. */
#include "s2mps18_policy.h"
#include "acpm_ipc.h"

bool
s2mps18_read_allowed(uint32_t type, uint32_t reg)
{
	if (type > ACPM_PMIC_RTC || reg > 0xffu) {
		return false;
	}
	/* INT1..INT7 of the PMIC block clear when read (s2mps18-private.h); the TOP block's INTSRC does not. */
	return !(type == ACPM_PMIC_PMIC && reg <= 0x06u);
}

void
s2mps18_note_read(struct s2mps18_policy *p, uint32_t type, uint32_t reg, uint8_t value)
{
	if (type == ACPM_PMIC_PMIC && reg == S2MPS18_B6OUT && !p->b6_seen) {
		p->b6_seen = true;
		p->b6_boot = value;
	}
}

static bool
deny(const char **why, const char *s)
{
	if (why) {
		*why = s;
	}
	return false;
}

static bool
rtc_field_ok(uint32_t reg, uint8_t v)
{
	/* binary or BCD: the largest legal byte of each field in either coding */
	switch (reg) {
	case 0x05: return v <= 0x59u;          /* SEC  (binary 59 = 0x3b < 0x59) */
	case 0x06: return v <= 0x59u;          /* MIN */
	case 0x07: return (v & 0x3fu) <= 0x23u && !(v & 0x80u);        /* HOUR [5:0], bit 6 PM */
	case 0x08: return v != 0 && v <= 0x40u && !(v & (v - 1u));      /* WEEK: one bit a day */
	case 0x09: return v >= 1u && v <= 0x31u;                        /* DAY */
	case 0x0a: return v >= 1u && v <= 0x12u;                        /* MONTH */
	case 0x0b: return v <= 0x99u;                                   /* YEAR since 2000 */
	default: return false;
	}
}

bool
s2mps18_write_allowed(const struct s2mps18_policy *p, uint32_t type, uint32_t reg, uint8_t value, uint8_t cur,
    const char **why)
{
	uint8_t vs = cur & S2MPS18_LDO_VSEL;

	if (why) {
		*why = "ok";
	}
	if (type == ACPM_PMIC_RTC) {
		if (reg == S2MPS18_RTC_UPDATE) {
			if (value == (cur | S2MPS18_RTC_RUDR) || value == (cur | S2MPS18_RTC_WUDR)) {
				return true;
			}
			return deny(why, "RTC UPDATE: only RUDR or WUDR may be added");
		}
		if (reg >= S2MPS18_RTC_SEC && reg <= S2MPS18_RTC_YEAR) {
			return rtc_field_ok(reg, value) ? true : deny(why, "RTC time field out of range");
		}
		return deny(why, "RTC register not writable (CTRL, WTSR/SMPL, CAPSEL, MSEC, alarms, OSCCTRL)");
	}
	if (type != ACPM_PMIC_PMIC) {
		return deny(why, "only the PMIC and RTC blocks are writable");
	}
	switch (reg) {
	case S2MPS18_L35CTRL:
		if (value != 0xecu) {
			return deny(why, "LDO35: only 0xec (on, 1.8 V)");
		}
		return (vs == 0x2cu || vs == 0) ? true : deny(why, "LDO35: programmed vsel unexpected");
	case S2MPS18_L43CTRL:
		if (value != 0xf0u) {
			return deny(why, "LDO43: only 0xf0 (on, 3.0 V)");
		}
		return (vs == 0 || (vs >= 0x28u && vs <= 0x3cu)) ? true : deny(why, "LDO43: programmed vsel unexpected");
	case S2MPS18_B6OUT:
		if (value == 0x68u) {
			return true;
		}
		if (p && p->b6_seen && value == p->b6_boot && value >= 0x20u && value <= 0x70u) {
			return true;
		}
		return deny(why, "BUCK6: only 0x68 (950 mV) or its boot value");
	case S2MPS18_L31CTRL:
		if (value != 0x28u && value != 0xe8u) {
			return deny(why, "LDO31: only 0x28 / 0xe8 (2.8 V off / on)");
		}
		return (vs == 0 || (vs >= 0x24u && vs <= 0x3cu)) ? true : deny(why, "LDO31: programmed vsel unexpected");
	case S2MPS18_L2CTRL1:
		if (value == 0xc0u || value == 0xfcu || value == 0x3cu || value == vs) {
			return true;
		}
		return deny(why, "LDO2: only 0xc0 / 0xfc / 0x3c or off at its vsel");
	default:
		return deny(why, "PMIC register not on the write list");
	}
}

bool
acpm_dvfs_allowed(uint32_t domain, uint32_t khz, bool governor, const char **why)
{
	static const uint32_t g3d[] = { 572000, 546000, 455000, 338000, 299000, 260000 };

	if (why) {
		*why = "ok";
	}
	if (khz == 0) {
		return deny(why, "0 kHz");
	}
	switch (domain) {
	case ACPM_DVFS_CPUCL0:
		return khz <= 1794000u ? true : deny(why, "cpucl0 above 1794000 kHz");
	case ACPM_DVFS_CPUCL1:
		if (khz <= 1794000u) {
			return true;
		}
		if (!governor) {
			return deny(why, "cpucl1 above 1794000 kHz without a thermal governor");
		}
		return khz <= 2704000u ? true : deny(why, "cpucl1 above 2704000 kHz");
	case ACPM_DVFS_G3D:
		for (unsigned i = 0; i < sizeof(g3d) / sizeof(g3d[0]); i++) {
			if (khz == g3d[i]) {
				return true;
			}
		}
		return deny(why, "g3d: not one of starlte_gpu.c's levels");
	default:
		return deny(why, "domain not allowed (MIF/INT froze the phone)");
	}
}
