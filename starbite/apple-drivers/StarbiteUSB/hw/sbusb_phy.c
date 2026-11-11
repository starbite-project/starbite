/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB PHY bring-up (sbusb_phy.h).
 *
 * The sequence is Samsung's CAL enable for USBCON version 3.0.0, as two files that run on this phone
 * describe it: an earlier port's glue (build/refport27/refport-kernel/drivers/usb/exynos_usb.c, APSL-2.0, its steps
 * a to l) and our loader (shim/starlte/usb_phy.c), which adds the three clock gates, the USB3/DP isolation
 * bit, the parked ReWA block and the stock device tuning, and is the variant that enumerates on the Mac.
 * This file follows the loader's superset as a table of register facts. NOTES.md section 2.
 */
#include "sbusb_phy.h"
#include "sbusb_regs.h"

struct step {
	uint8_t blk;
	uint16_t off;
	uint32_t clear, set;
	uint16_t wait_before_us, wait_after_us;         /* around the write */
};

static const struct step steps[] = {
	/* clocks and power: manual gate control with the clock on; PHYs out of isolation */
	{ SBUSB_BLK_GATE_TOP, 0, 0, GATE_MANUAL_CGVAL, 0, 0 },
	{ SBUSB_BLK_GATE_REF, 0, 0, GATE_MANUAL_CGVAL, 0, 0 },
	{ SBUSB_BLK_GATE_DPPHY, 0, 0, GATE_MANUAL_CGVAL, 0, 0 },
	{ SBUSB_BLK_PMU_USB2, 0, 0, PMU_PHY_ENABLE, 0, 0 },
	{ SBUSB_BLK_PMU_USB3DP, 0, 0, PMU_PHY_ENABLE, 0, 10 },
	/* the Q-channel held active: the four DIS_QACT bits with FORCE_QACT low, then FORCE_QACT high */
	{ SBUSB_BLK_PHY, UC_LINK_CTRL, UC_LC_FORCE_QACT,
	  UC_LC_DIS_QACT_ID0 | UC_LC_DIS_QACT_VBUS_VALID | UC_LC_DIS_QACT_BVALID | UC_LC_DIS_QACT_LINKGATE, 500, 500 },
	{ SBUSB_BLK_PHY, UC_LINK_CTRL, 0, UC_LC_FORCE_QACT, 500, 0 },
	/* link reset pulse */
	{ SBUSB_BLK_PHY, UC_CLKRST, 0, UC_CR_LINK_SW_RST, 0, 10 },
	{ SBUSB_BLK_PHY, UC_CLKRST, UC_CR_LINK_SW_RST, 0, 0, 0 },
	/* PHY in reset, powered, UTMI overrides off, common block kept on in suspend */
	{ SBUSB_BLK_PHY, UC_CLKRST, 0, UC_CR_PHY_SW_RST | UC_CR_PHY_RST_SEL, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSP_TEST, UC_HSP_TEST_SIDDQ, 0, 0, 0 },
	{ SBUSB_BLK_PHY, UC_UTMI, UC_UTMI_FORCE_SUSPEND | UC_UTMI_FORCE_SLEEP | UC_UTMI_DP_PULLDOWN | UC_UTMI_DM_PULLDOWN,
	  0, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSP, 0, UC_HSP_EN_UTMISUSPEND | UC_HSP_COMMONONN, 0, 100 },
	/* PHY out of reset */
	{ SBUSB_BLK_PHY, UC_CLKRST, UC_CR_PHY_SW_RST | UC_CR_PORT_RST, UC_CR_PHY_RST_SEL, 0, 0 },
	/* this board has no VBUS pad on the PHY: VBUS forced valid, the bus filter bypassed */
	{ SBUSB_BLK_PHY, UC_LINK_CTRL, 0, UC_LC_BUS_FILTER_BYPASS, 0, 0 },
	{ SBUSB_BLK_PHY, UC_UTMI, 0, UC_UTMI_FORCE_BVALID | UC_UTMI_FORCE_VBUSVALID, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSP, 0, UC_HSP_VBUSVLDEXTSEL | UC_HSP_VBUSVLDEXT, 0, 0 },
	{ SBUSB_BLK_PHY, UC_LINK_PORT, 0, UC_LP_HUB_PORT_SEL_OCD_U3 | UC_LP_HUB_PORT_SEL_OCD_U2, 0, 0 },
	/* remote wake-up assist parked */
	{ SBUSB_BLK_PHY, UC_REWA_ENABLE, 1u, 0, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSREWA_CTRL, 1u << 24, 1u << 28, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSREWA_HSTK, 0xffffffffu, 0x1u, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSREWA_REFTO, 0xffffffffu, 0xff00u, 0, 0 },
	{ SBUSB_BLK_PHY, UC_HSREWA_INT1_MASK, 0, (1u << 18) | (1u << 17) | (1u << 16) | 7u, 0, 0 },
	/* the SuperSpeed side: PIPE3 forced idle, its PMA in low power */
	{ SBUSB_BLK_PHY, UC_LINK_CTRL, UC_LC_PIPE3_FORCE_PHY_STATUS, UC_LC_PIPE3_FORCE_EN | UC_LC_PIPE3_FORCE_RX_ELEC_IDLE,
	  0, 0 },
	{ SBUSB_BLK_PHY, UC_COMBO_PMA, 0, UC_PMA_LOW_PWRN, 0, 0 },
	/* high-speed transmitter and receiver tuning, the stock device-mode values */
	{ SBUSB_BLK_PHY, UC_HSP_TUNE, UC_TUNE_MASK, UC_TUNE_VALUE, 0, 100 },
};

unsigned
sbusb_phy_step_count(void)
{
	return (unsigned)(sizeof(steps) / sizeof(steps[0]));
}

unsigned
sbusb_phy_line(const struct sbusb_io *io)
{
	return UC_HSP_TEST_LINESTATE(io->rd(io->ctx, SBUSB_BLK_PHY, UC_HSP_TEST));
}

int
sbusb_phy_start(const struct sbusb_io *io, struct sbusb_phy_report *rep)
{
	const uint32_t forced = UC_UTMI_FORCE_BVALID | UC_UTMI_FORCE_VBUSVALID;

	rep->missing = 0;
	rep->utmi = rep->hsp_test = 0;
	rep->line = 0;
	if (!io->has(io->ctx, SBUSB_BLK_PHY)) {
		rep->missing = 1u << SBUSB_BLK_PHY;
		return -2;
	}
	for (unsigned i = 0; i < sbusb_phy_step_count(); ++i) {
		const struct step *s = &steps[i];
		uint32_t v;

		if (!io->has(io->ctx, s->blk)) {
			rep->missing |= 1u << s->blk;
			continue;
		}
		v = (io->rd(io->ctx, s->blk, s->off) & ~s->clear) | s->set;
		if (s->wait_before_us != 0) {
			io->delay_us(io->ctx, s->wait_before_us);
		}
		io->wr(io->ctx, s->blk, s->off, v);
		if (s->wait_after_us != 0) {
			io->delay_us(io->ctx, s->wait_after_us);
		}
	}
	rep->utmi = io->rd(io->ctx, SBUSB_BLK_PHY, UC_UTMI);
	rep->hsp_test = io->rd(io->ctx, SBUSB_BLK_PHY, UC_HSP_TEST);
	rep->line = UC_HSP_TEST_LINESTATE(rep->hsp_test);
	if ((rep->hsp_test & UC_HSP_TEST_SIDDQ) != 0 || (rep->utmi & forced) != forced) {
		return -1;
	}
	return 0;
}
