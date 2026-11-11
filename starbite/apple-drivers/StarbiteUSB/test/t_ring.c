/* SPDX-License-Identifier: APSL-2.0
 * Host test: the TRB rings (wrap, full, both transfer modes), the NCM data path through them, start and
 * stop, the hand-off states the driver must cope with, the PHY sequence, the link watch.
 */
#include "mock.h"

static struct sbusb_func F;
static int link_up, tx_ready_calls;
static struct { unsigned n; uint32_t len[256]; uint8_t tag[256]; } rx;
static void on_link(void *c, int up, int fs) { (void)c; (void)fs; link_up = up; }
static void on_ready(void *c) { (void)c; ++tx_ready_calls; }
static void on_rx(void *c, const uint8_t *frame, uint32_t len) { (void)c; if (rx.n < 256) { rx.len[rx.n] = len; rx.tag[rx.n] = frame[14]; } ++rx.n; }

static void
bring_up(unsigned flags, int full_speed)
{
	struct sbusb_io io;
	struct sbusb_dma dma;
	struct sbusb_func_ops ops = { 0, on_rx, on_link, on_ready };

	mock_reset();
	io = mock_io();
	dma = mock_dma();
	link_up = tx_ready_calls = 0;
	memset(&rx, 0, sizeof(rx));
	sbusb_func_init(&F, &io, &dma, "S9", flags, &ops);
	CHECK_EQ(sbusb_func_start(&F), 0);
	mock_enumerate(&F, full_speed);
	CHECK_EQ(link_up, 1);
	while (mock_complete(3, 0, sbusb_get32(mock_trb(3) + 8))) {     /* the two notifications */
		sbusb_func_poll(&F);
		if (mock_trb(3) == 0) {
			break;
		}
	}
}

/* raw ring behaviour on the bulk IN endpoint, without the class layer queueing anything */
static void
ring_wrap(unsigned flags)
{
	struct sbusb_dwc3 *d = &F.d;
	struct sbusb_ring *r = &d->ring[SBUSB_RING_IN];
	struct sbusb_ev e;
	unsigned c0, done = 0, starts, updates;
	uint64_t ring_pa = MOCK_DMA_PA + SBUSB_DMA_RING(SBUSB_RING_IN);
	int ring = !!(flags & SBUSB_F_RING);

	bring_up(flags, 0);
	c0 = M.ncmd;
	if (ring) {
		/* the link TRB closes the ring: last slot, points at TRB 0, owned by the controller */
		uint8_t *lt = M.dma + SBUSB_DMA_RING(SBUSB_RING_IN) + 16 * SBUSB_RING_USABLE;
		CHECK_EQ(sbusb_get32(lt + 12), D3_TRB_TYPE_LINK | D3_TRB_HWO);
		CHECK_EQ(sbusb_get32(lt), (uint32_t)ring_pa);
		CHECK_EQ(sbusb_get32(lt + 4), (uint32_t)(ring_pa >> 32));
	}
	/* fill it: 7 fit, the 8th does not */
	for (unsigned i = 0; i < SBUSB_RING_USABLE; ++i) {
		CHECK_EQ(sbusb_dwc3_queue(d, SBUSB_RING_IN, SBUSB_DMA_TX(i % 4), 100 + i, i), 0);
	}
	CHECK_EQ(sbusb_dwc3_queue(d, SBUSB_RING_IN, SBUSB_DMA_TX(0), 100, 99), -1);
	CHECK_EQ(sbusb_dwc3_ring_free(d, SBUSB_RING_IN), 0);
	CHECK_EQ(sbusb_dwc3_queue(d, SBUSB_RING_IN, SBUSB_DMA_BYTES - 8, 16, 0), -1);   /* outside the DMA block */
	CHECK_EQ(sbusb_dwc3_queue(d, 7, 0, 0, 0), -1);
	/* drain and refill for 40 transfers: 5 times around; order and lengths kept, TRB addresses wrap */
	for (unsigned next = SBUSB_RING_USABLE; done < 40;) {
		uint8_t *t = mock_trb(5);
		CHECK(t != 0);
		if (t == 0) {
			break;
		}
		CHECK_EQ((uint64_t)(t - M.dma) + MOCK_DMA_PA, ring_pa + 16 * (done % SBUSB_RING_USABLE));
		CHECK_EQ(sbusb_get32(t + 8), 100 + done);
		CHECK_EQ(!!(sbusb_get32(t + 12) & D3_TRB_LST), !ring);
		CHECK(sbusb_get32(t + 12) & D3_TRB_IOC);
		CHECK(!(sbusb_get32(t + 12) & (D3_TRB_ISP | D3_TRB_CSP)));      /* IN */
		mock_complete(5, 0, 100 + done);
		CHECK_EQ(sbusb_dwc3_poll(d, &e), 1);
		CHECK_EQ(e.type, SBUSB_EV_DONE); CHECK_EQ(e.ring, SBUSB_RING_IN);
		CHECK_EQ(e.cookie, done); CHECK_EQ(e.value, 100 + done);
		CHECK_EQ(sbusb_dwc3_poll(d, &e), 0);
		++done;
		CHECK_EQ(sbusb_dwc3_ring_free(d, SBUSB_RING_IN), 1);
		CHECK_EQ(sbusb_dwc3_queue(d, SBUSB_RING_IN, SBUSB_DMA_TX(next % 4), 100 + next, next), 0);
		++next;
	}
	CHECK_EQ(r->wraps, 40 / SBUSB_RING_USABLE);
	CHECK_EQ(r->inflight, SBUSB_RING_USABLE);
	starts = mock_count(c0, 5, D3_CMD_STARTXFER);
	updates = mock_count(c0, 5, D3_CMD_UPDATEXFER);
	if (ring) {
		CHECK_EQ(starts, 1);                    /* started once, then only told about new TRBs */
		CHECK_EQ(updates, SBUSB_RING_USABLE - 1 + 40);
		CHECK_EQ(M.cmd[M.ncmd - 1].cmd >> 16, 5 + 1);   /* with the transfer resource index Start Transfer gave */
	} else {
		CHECK_EQ(starts, 1 + 40);               /* one TRB at a time */
		CHECK_EQ(updates, 0);
	}
	/* a short IN completion reports what moved */
	mock_complete(5, 0, 10);
	CHECK_EQ(sbusb_dwc3_poll(d, &e), 1);
	CHECK_EQ(e.value, 10);
	/* ring mode: two TRBs finished before we look: both reaped from one event, in order */
	if (ring) {
		mock_complete(5, 0, 0);
		mock_complete(5, 0, 0);
		CHECK_EQ(sbusb_dwc3_poll(d, &e), 1);
		CHECK_EQ(e.cookie, 41);
		CHECK_EQ(sbusb_dwc3_poll(d, &e), 1);
		CHECK_EQ(e.cookie, 42);
		CHECK_EQ(sbusb_dwc3_poll(d, &e), 0);
	}
	/* an event for an endpoint with nothing in flight is ignored */
	mock_ep_event(3, D3_EVT_XFERCOMPLETE, 0);
	CHECK_EQ(sbusb_dwc3_poll(d, &e), 0);
	CHECK_EQ(d->n.cmd_fail, 0);
}

static uint32_t
host_ntb(uint8_t *ntb, unsigned count, uint32_t dglen, unsigned tag0)
{
	struct sbusb_ntb_w w;

	sbusb_ntb_begin(&w, ntb, SBUSB_NTB_MAX, 7);
	for (unsigned i = 0; i < count; ++i) {
		uint8_t *p = sbusb_ntb_add(&w, dglen);
		memset(p, 0, dglen);
		p[14] = (uint8_t)(tag0 + i);
	}
	return sbusb_ntb_end(&w, 512);
}

static void
data_path(unsigned flags)
{
	static uint8_t ntb[SBUSB_NTB_MAX];
	uint32_t n;
	int stall;
	uint8_t *p, *t;
	unsigned sent = 0, c0;

	bring_up(flags, 0);
	/* host to phone: 30 NTBs of 3 frames each through 4 buffers: every frame delivered once, in order */
	for (unsigned k = 0; k < 30; ++k) {
		n = host_ntb(ntb, 3, 200 + k, k * 3);
		CHECK_EQ(mock_complete(4, ntb, n), 1);
		sbusb_func_poll(&F);
		CHECK_EQ(F.d.ring[SBUSB_RING_OUT].inflight, SBUSB_RX_SLOTS);    /* queued again at once */
	}
	CHECK_EQ(rx.n, 90);
	for (unsigned i = 0; i < 90; ++i) {
		CHECK_EQ(rx.tag[i], i);
		CHECK_EQ(rx.len[i], 200 + i / 3);
	}
	CHECK_EQ(F.n.rx_ntb, 30); CHECK_EQ(F.n.rx_bad, 0);
	CHECK(F.d.ring[SBUSB_RING_OUT].wraps >= 4);
	t = mock_trb(4);
	CHECK(sbusb_get32(t + 12) & D3_TRB_ISP);
	CHECK_EQ(!!(sbusb_get32(t + 12) & D3_TRB_CSP), !!(flags & SBUSB_F_RING));
	/* a broken NTB is counted, not delivered, and the buffer goes back */
	memset(ntb, 0x55, 64);
	mock_complete(4, ntb, 64);
	sbusb_func_poll(&F);
	CHECK_EQ(rx.n, 90); CHECK_EQ(F.n.rx_bad, 1); CHECK_EQ(F.n.rx_last_error, 2);
	CHECK_EQ(F.d.ring[SBUSB_RING_OUT].inflight, SBUSB_RX_SLOTS);
	mock_complete(4, ntb, 0);                               /* a zero-length packet: nothing */
	sbusb_func_poll(&F);
	CHECK_EQ(F.n.rx_bad, 1);

	/* phone to host: the first frame goes out at once, alone */
	p = sbusb_func_tx_begin(&F, 60, &stall);
	CHECK(p != 0);
	memset(p, 0, 60); p[14] = 0xc1;
	sbusb_func_tx_commit(&F);
	t = mock_trb(5);
	CHECK(t != 0);
	n = sbusb_get32(t + 8);
	CHECK_EQ(n, 184 + 60);
	memset(&rx, 0, sizeof(rx));
	CHECK_EQ(sbusb_ntb_parse(mock_trb_buf(t), n, on_rx, 0), 1);
	CHECK_EQ(rx.tag[0], 0xc1); CHECK_EQ(rx.len[0], 60);
	/* while it is on the wire, frames collect in the next NTB; nothing more is queued */
	for (unsigned i = 0; i < 5; ++i) {
		p = sbusb_func_tx_begin(&F, 1514, &stall);
		CHECK(p != 0);
		memset(p, 0, 1514); p[14] = (uint8_t)i;
		sbusb_func_tx_commit(&F);
	}
	CHECK_EQ(F.d.ring[SBUSB_RING_IN].inflight, 1);
	mock_complete(5, 0, n);
	sbusb_func_poll(&F);                                    /* the completion sends the collected five */
	t = mock_trb(5);
	n = sbusb_get32(t + 8);
	memset(&rx, 0, sizeof(rx));
	CHECK_EQ(sbusb_ntb_parse(mock_trb_buf(t), n, on_rx, 0), 5);
	CHECK(n % 512 != 0);
	for (unsigned i = 0; i < 5; ++i) { CHECK_EQ(rx.tag[i], i); CHECK_EQ(rx.len[i], 1514); }
	CHECK_EQ(sbusb_get16(mock_trb_buf(t) + 6), 1);          /* wSequence counts NTBs */
	/* flood: with the host taking nothing, 3 more NTBs fill (10 frames each), then stall */
	while ((p = sbusb_func_tx_begin(&F, 1514, &stall)) != 0) {
		memset(p, 0, 1514);
		sbusb_func_tx_commit(&F);
		++sent;
		CHECK(sent < 100);
		if (sent >= 100) {
			break;
		}
	}
	CHECK_EQ(stall, 1);
	CHECK_EQ(sent, 30);
	CHECK_EQ(F.n.tx_stalls, 1);
	CHECK_EQ(tx_ready_calls, 0);
	mock_complete(5, 0, n);
	sbusb_func_poll(&F);
	CHECK_EQ(tx_ready_calls, 1);                            /* a buffer came back: the queue may go on */
	CHECK(sbusb_func_tx_begin(&F, 1514, &stall) != 0);
	sbusb_func_tx_commit(&F);
	/* frames the function cannot carry are refused without a stall */
	CHECK(sbusb_func_tx_begin(&F, 13, &stall) == 0); CHECK_EQ(stall, 0);
	CHECK(sbusb_func_tx_begin(&F, 1515, &stall) == 0); CHECK_EQ(stall, 0);
	/* the host shrinks its NTB input size: NTBs we build respect it */
	mock_enumerate(&F, 0);
	{ uint8_t v[4]; sbusb_put32(v, 2048); CHECK_EQ(mock_control(&F, 0x21, 0x86, 0, 0, 4, v), 0); }
	for (unsigned i = 0; i < 3; ++i) {
		p = sbusb_func_tx_begin(&F, 1000, &stall);
		CHECK(p != 0);
		memset(p, 0, 1000);
		sbusb_func_tx_commit(&F);
	}
	t = mock_trb(5);
	CHECK(sbusb_get32(t + 8) <= 2048);
	mock_complete(5, 0, sbusb_get32(t + 8));
	sbusb_func_poll(&F);
	t = mock_trb(5);
	CHECK(t != 0 && sbusb_get32(t + 8) <= 2048);

	/* suspend: link down, frames dropped not stalled; resume: up again */
	mock_dev_event(3, D3_LINK_U3);
	sbusb_func_poll(&F);
	CHECK_EQ(link_up, 0);
	CHECK(sbusb_func_tx_begin(&F, 60, &stall) == 0); CHECK_EQ(stall, 0);
	mock_dev_event(3, D3_LINK_U0);
	sbusb_func_poll(&F);
	CHECK_EQ(link_up, 1);
	/* disconnect */
	mock_dev_event(0, 0);
	sbusb_func_poll(&F);
	CHECK_EQ(link_up, 0);

	/* stop: every transfer ended, pull-up off, events off, core reset, event buffer taken away */
	mock_enumerate(&F, 0);
	c0 = M.ncmd;
	sbusb_func_stop(&F);
	CHECK(mock_count(c0, 4, D3_CMD_ENDXFER) == 1 && mock_count(c0, 0, D3_CMD_ENDXFER) == 1);
	CHECK_EQ(M.dwc3[D3_DCTL / 4] & D3_DCTL_RUN_STOP, 0);
	CHECK_EQ(M.dwc3[D3_DEVTEN / 4], 0);
	CHECK_EQ(M.dwc3[D3_DALEPENA / 4], 0);
	CHECK_EQ(M.dwc3[D3_GEVNTADRLO / 4], 0);
	CHECK_EQ(M.dwc3[D3_GEVNTSIZ / 4], D3_GEVNTSIZ_INTMASK);
	CHECK_EQ(link_up, 0);
	c0 = M.ncmd;
	sbusb_func_stop(&F);                                    /* twice is safe */
	CHECK_EQ(M.ncmd, c0);
	CHECK(sbusb_func_tx_begin(&F, 60, &stall) == 0);
	CHECK_EQ(sbusb_func_poll(&F), 0);
	CHECK_EQ(F.d.n.cmd_fail, 0);
}

static void
start_states(void)
{
	struct sbusb_io io;
	struct sbusb_dma dma;
	struct sbusb_func_ops ops = { 0, on_rx, on_link, on_ready };
	uint32_t w0;

	/* the normal hand-off (the loader's fbbg_off: core reset and halted, PHY as the loader left it) */
	mock_reset();
	io = mock_io(); dma = mock_dma();
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), 0);
	CHECK(F.d.why == 0);
	CHECK_EQ(M.dwc3[D3_GCTL / 4] & D3_GCTL_PRTCAP_MASK, D3_GCTL_PRTCAP_DEVICE);
	CHECK_EQ(M.dwc3[D3_GCTL / 4] & D3_GCTL_SCALEDOWN_MASK, 0);
	CHECK_EQ(M.dwc3[D3_GCTL / 4] & D3_GCTL_DSBLCLKGTNG, 0);        /* 3.00a: not in the 2.10a..2.50a window */
	CHECK_EQ(M.dwc3[D3_GUSB2PHYCFG / 4] & (D3_U2_SUSPHY | D3_U2_ENBLSLPM | D3_U2_FREECLK_EXISTS | D3_U2_PHYSOFTRST), 0);
	CHECK_EQ(M.dwc3[D3_GUSB2PHYCFG / 4] & 0x2400, 0x2400);          /* the other fields are left alone */
	CHECK_EQ(M.dwc3[D3_GUSB3PIPECTL / 4] & D3_U3_SUSPHY, 0);
	CHECK_EQ(M.dwc3[D3_DCFG / 4] & (D3_DCFG_SPEED_MASK | D3_DCFG_ADDR_MASK | D3_DCFG_LPM_CAP), 0);
	CHECK_EQ(M.dwc3[D3_GEVNTADRLO / 4], (uint32_t)MOCK_DMA_PA);
	CHECK_EQ(M.dwc3[D3_GEVNTSIZ / 4], D3_GEVNTSIZ_INTMASK | 4096);  /* polled: interrupt masked */
	CHECK_EQ(M.dwc3[D3_DEVTEN / 4], 0xf);
	CHECK(M.dwc3[D3_DCTL / 4] & D3_DCTL_RUN_STOP);
	CHECK_EQ(M.dwc3[D3_DCTL / 4] & D3_DCTL_ULSTCHNGREQ_MASK, 0);
	CHECK_EQ(M.dwc3[D3_GSBUSCFG0 / 4] & 0xf, 9);
	CHECK_EQ(M.dwc3[D3_GFLADJ / 4] & 0xbf, 0xa0);
	CHECK(M.now_us - 1000000 < 400000);                             /* bounded: well under half a second */
	/* the PHY: every step written once, the end state right */
	CHECK_EQ(M.writes[SBUSB_BLK_PHY] + M.writes[SBUSB_BLK_PMU_USB2] + M.writes[SBUSB_BLK_PMU_USB3DP] +
	    M.writes[SBUSB_BLK_GATE_TOP] + M.writes[SBUSB_BLK_GATE_REF] + M.writes[SBUSB_BLK_GATE_DPPHY], sbusb_phy_step_count());
	CHECK_EQ(M.single[SBUSB_BLK_PMU_USB2], 1); CHECK_EQ(M.single[SBUSB_BLK_PMU_USB3DP], 1);
	CHECK_EQ(M.single[SBUSB_BLK_GATE_TOP], 3u << 20); CHECK_EQ(M.single[SBUSB_BLK_GATE_REF], 3u << 20);
	CHECK_EQ(M.single[SBUSB_BLK_GATE_DPPHY], 3u << 20);
	CHECK_EQ(M.phy[UC_HSP_TEST / 4] & UC_HSP_TEST_SIDDQ, 0);
	CHECK_EQ(M.phy[UC_UTMI / 4], UC_UTMI_FORCE_BVALID | UC_UTMI_FORCE_VBUSVALID);
	CHECK_EQ(M.phy[UC_CLKRST / 4], UC_CR_PHY_RST_SEL);
	CHECK_EQ(M.phy[UC_HSP / 4], UC_HSP_EN_UTMISUSPEND | UC_HSP_COMMONONN | UC_HSP_VBUSVLDEXTSEL | UC_HSP_VBUSVLDEXT);
	CHECK_EQ(M.phy[UC_LINK_CTRL / 4], UC_LC_FORCE_QACT | UC_LC_DIS_QACT_BVALID | UC_LC_DIS_QACT_VBUS_VALID |
	    UC_LC_DIS_QACT_ID0 | UC_LC_DIS_QACT_LINKGATE | UC_LC_BUS_FILTER_BYPASS | UC_LC_PIPE3_FORCE_EN |
	    UC_LC_PIPE3_FORCE_RX_ELEC_IDLE);
	CHECK_EQ(M.phy[UC_LINK_PORT / 4], 0xc);
	CHECK_EQ(M.phy[UC_COMBO_PMA / 4], UC_PMA_LOW_PWRN);
	CHECK_EQ(M.phy[UC_HSP_TUNE / 4], 0xd16c0403);
	CHECK_EQ(M.phy[UC_REWA_ENABLE / 4], 0);
	CHECK_EQ(M.phy[UC_HSREWA_CTRL / 4], 1u << 28);
	CHECK_EQ(M.phy[UC_HSREWA_HSTK / 4], 1); CHECK_EQ(M.phy[UC_HSREWA_REFTO / 4], 0xff00);
	CHECK_EQ(M.phy[UC_HSREWA_INT1_MASK / 4], 0x70007);
	CHECK_EQ(F.d.phy.missing, 0);
	CHECK(M.syncs > 0);

	/* interrupt mode leaves the event interrupt unmasked */
	mock_reset();
	sbusb_func_init(&F, &io, &dma, "S9", SBUSB_F_IRQ, &ops);
	CHECK_EQ(sbusb_func_start(&F), 0);
	CHECK_EQ(M.dwc3[D3_GEVNTSIZ / 4], 4096);

	/* a controller left running with events pending (S-Boot, or a loader that did not stop it): halted and
	 * drained before the PHY is reset under it, then started as usual */
	mock_reset();
	M.dwc3[D3_DCTL / 4] = D3_DCTL_RUN_STOP;
	M.dwc3[D3_DSTS / 4] = 0;
	M.dwc3[D3_GEVNTCOUNT / 4] = 24;
	M.dwc3[D3_GEVNTSIZ / 4] = 4096;
	M.dwc3[D3_GEVNTADRLO / 4] = 0x91000000;
	M.dwc3[D3_DEVTEN / 4] = 7;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), 0);
	CHECK_EQ(M.dwc3[D3_GEVNTCOUNT / 4], 0);
	CHECK_EQ(F.d.evt_pos, 0);
	mock_enumerate(&F, 0);                                  /* and it enumerates from a clean event buffer */
	CHECK_EQ(F.d.st.config, 1);

	/* the nub carries only USBCON (no PMU / clock-gate entries): those steps are skipped and reported */
	mock_reset();
	M.missing = 1u << SBUSB_BLK_PMU_USB2 | 1u << SBUSB_BLK_PMU_USB3DP | 1u << SBUSB_BLK_GATE_TOP |
	    1u << SBUSB_BLK_GATE_REF | 1u << SBUSB_BLK_GATE_DPPHY;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), 0);
	CHECK_EQ(F.d.phy.missing, M.missing);
	CHECK_EQ(M.writes[SBUSB_BLK_PMU_USB2] + M.writes[SBUSB_BLK_GATE_TOP], 0);

	/* failures: each bounded, each with a reason, nothing left running */
	mock_reset();
	M.missing = 1u << SBUSB_BLK_PHY;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), -1);
	CHECK(F.d.why != 0);
	CHECK_EQ(M.writes[SBUSB_BLK_DWC3], 0);

	mock_reset();
	M.dwc3[D3_GSNPSID / 4] = 0;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), -2);
	CHECK_EQ(M.writes[SBUSB_BLK_DWC3], 0);                  /* not a DWC3: not one write to it */

	mock_reset();
	M.csftrst_stuck = 1;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	w0 = (uint32_t)M.now_us;
	CHECK_EQ(sbusb_func_start(&F), -3);
	CHECK((uint32_t)M.now_us - w0 < 300000);
	CHECK_EQ(M.dwc3[D3_GEVNTADRLO / 4], 0);

	mock_reset();
	M.fail_cmd = D3_CMD_SETEPCONFIG; M.fail_phys = 1;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), -4);
	CHECK_EQ(F.d.n.cmd_fail, 1);
	CHECK_EQ(M.dwc3[D3_DCTL / 4] & D3_DCTL_RUN_STOP, 0);    /* never pulled up */

	mock_reset();
	dma.size = 0x8000;
	sbusb_func_init(&F, &io, &dma, "S9", 0, &ops);
	CHECK_EQ(sbusb_func_start(&F), -6);
	CHECK_EQ(M.writes[SBUSB_BLK_DWC3] + M.writes[SBUSB_BLK_PHY], 0);
}

/* a replug the controller cannot see: resets without a SETUP restart the link from the PHY, with back-off */
static void
link_watch(void)
{
	uint32_t phy0;

	bring_up(0, 0);
	phy0 = M.writes[SBUSB_BLK_PHY];
	for (unsigned i = 0; i < 11; ++i) {
		mock_dev_event(1, 0);
		sbusb_func_poll(&F);
	}
	CHECK_EQ(F.d.n.restarts, 0);
	CHECK_EQ(link_up, 0);
	CHECK_EQ(mock_control(&F, 0x80, 6, 0x0100, 0, 18, (uint8_t[64]){ 0 }), 18);    /* one SETUP: a live host */
	for (unsigned i = 0; i < 11; ++i) {
		mock_dev_event(1, 0);
		sbusb_func_poll(&F);
	}
	CHECK_EQ(F.d.n.restarts, 0);
	mock_dev_event(1, 0);
	sbusb_func_poll(&F);
	CHECK_EQ(F.d.n.restarts, 1);                            /* the 12th in a row */
	CHECK(M.writes[SBUSB_BLK_PHY] > phy0);
	CHECK_EQ(F.d.started, 1);
	CHECK(M.dwc3[D3_DCTL / 4] & D3_DCTL_RUN_STOP);
	CHECK(mock_trb(0) != 0);
	/* another storm right away waits for the pause (2 s) */
	for (unsigned i = 0; i < 20; ++i) {
		mock_dev_event(1, 0);
		sbusb_func_poll(&F);
	}
	CHECK_EQ(F.d.n.restarts, 1);
	M.now_us += 2100000;
	sbusb_func_poll(&F);
	CHECK_EQ(F.d.n.restarts, 2);
	/* and after it the link enumerates */
	mock_enumerate(&F, 0);
	CHECK_EQ(link_up, 1);
	/* a restart that fails is tried again after the pause, and nothing is touched in between */
	M.now_us += 10000000;
	M.csftrst_stuck = 1;
	for (unsigned i = 0; i < 12; ++i) {
		mock_dev_event(1, 0);
		sbusb_func_poll(&F);
	}
	CHECK_EQ(F.d.n.restarts, 3);
	CHECK_EQ(F.d.started, 0);
	CHECK_EQ(link_up, 0);
	{
		uint32_t w = M.writes[SBUSB_BLK_DWC3];
		M.now_us += 1000000;
		sbusb_func_poll(&F);
		CHECK_EQ(M.writes[SBUSB_BLK_DWC3], w);
	}
	M.csftrst_stuck = 0;
	M.dwc3[D3_DCTL / 4] = 0;
	M.now_us += 4000000;
	sbusb_func_poll(&F);
	CHECK_EQ(F.d.n.restarts, 4);
	CHECK_EQ(F.d.started, 1);
	sbusb_func_stop(&F);                                    /* a stopped driver is never restarted */
	M.now_us += 100000000;
	CHECK_EQ(sbusb_func_poll(&F), 0);
	CHECK_EQ(F.d.started, 0);
}

int
main(void)
{
	ring_wrap(0);
	ring_wrap(SBUSB_F_RING);
	data_path(0);
	data_path(SBUSB_F_RING);
	bring_up(0, 1);                                         /* full speed: 64-byte packets, 12 Mbit/s notified */
	start_states();
	link_watch();
	return mock_summary("t_ring");
}
