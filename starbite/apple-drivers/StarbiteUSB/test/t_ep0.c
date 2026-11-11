/* SPDX-License-Identifier: APSL-2.0
 * Host test: endpoint 0. First the request decoder alone, then whole control transfers against the mock
 * controller: the stages, the TRB types, where SET_ADDRESS lands, what SET_CONFIGURATION programs.
 */
#include "mock.h"

static struct sbusb_func F;
static int link_up, link_calls;
static void on_link(void *c, int up, int fs) { (void)c; (void)fs; link_up = up; ++link_calls; }

static void
decoder(void)
{
	struct sbusb_state s;
	struct sbusb_ep0_res r;
#define SETUP(t, q, v, i, l) do { uint8_t p_[8] = { t, q, (v) & 0xff, (v) >> 8, (i) & 0xff, (i) >> 8, (l) & 0xff, (l) >> 8 }; \
	sbusb_ep0_setup(&s, p_, &r); } while (0)
	sbusb_state_init(&s, "SER");
	SETUP(0x80, 6, 0x0100, 0, 64);  CHECK_EQ(r.action, SBUSB_EP0_IN); CHECK_EQ(r.len, 18);
	SETUP(0x80, 6, 0x0100, 0, 8);   CHECK_EQ(r.len, 8);                     /* cut to wLength */
	SETUP(0x80, 6, 0x0200, 0, 9);   CHECK_EQ(r.len, 9); CHECK_EQ(r.data[1], 2);
	SETUP(0x80, 6, 0x0200, 0, 255); CHECK_EQ(r.len, 94); CHECK_EQ(sbusb_get16(r.data + 87 + 4), 512);
	SETUP(0x80, 6, 0x0700, 0, 255); CHECK_EQ(r.len, 94); CHECK_EQ(r.data[1], 7); CHECK_EQ(sbusb_get16(r.data + 87 + 4), 64);
	SETUP(0x80, 6, 0x0201, 0, 255); CHECK_EQ(r.action, SBUSB_EP0_STALL);    /* no second configuration */
	SETUP(0x80, 6, 0x0600, 0, 10);  CHECK_EQ(r.len, 10);
	SETUP(0x80, 6, 0x0f00, 0, 5);   CHECK_EQ(r.action, SBUSB_EP0_STALL);    /* BOS: a USB 2.0 device has none */
	SETUP(0x80, 6, 0x0300, 0, 255); CHECK_EQ(r.len, 4);
	SETUP(0x80, 6, 0x0303, 0x409, 255); CHECK_EQ(r.len, 8);
	SETUP(0x80, 6, 0x0309, 0x409, 255); CHECK_EQ(r.action, SBUSB_EP0_STALL);
	SETUP(0x80, 6, 0x0100, 0, 0);   CHECK_EQ(r.action, SBUSB_EP0_STALL);
	SETUP(0x00, 5, 37, 0, 0);       CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(r.set_address, 37);
	SETUP(0x00, 5, 128, 0, 0);      CHECK_EQ(r.action, SBUSB_EP0_STALL);
	SETUP(0x80, 8, 0, 0, 1);        CHECK_EQ(r.data[0], 0);
	SETUP(0x81, 10, 0, 1, 1);       CHECK_EQ(r.action, SBUSB_EP0_STALL);    /* GET_INTERFACE before configured */
	SETUP(0x01, 11, 1, 1, 0);       CHECK_EQ(r.action, SBUSB_EP0_STALL);
	SETUP(0x00, 9, 2, 0, 0);        CHECK_EQ(r.action, SBUSB_EP0_STALL);
	SETUP(0x00, 9, 1, 0, 0);        CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(r.set_config, 1); CHECK_EQ(s.config, 1);
	SETUP(0x80, 8, 0, 0, 1);        CHECK_EQ(r.data[0], 1);
	SETUP(0x01, 11, 1, 1, 0);       CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(r.set_alt, 1);
	SETUP(0x81, 10, 0, 1, 1);       CHECK_EQ(r.data[0], 1);
	SETUP(0x81, 10, 0, 0, 1);       CHECK_EQ(r.data[0], 0);
	SETUP(0x01, 11, 2, 1, 0);       CHECK_EQ(r.action, SBUSB_EP0_STALL);    /* no alt 2 */
	SETUP(0x01, 11, 1, 0, 0);       CHECK_EQ(r.action, SBUSB_EP0_STALL);    /* control interface has only alt 0 */
	SETUP(0x01, 11, 0, 0, 0);       CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(r.set_alt, -1);
	SETUP(0x80, 0, 0, 0, 2);        CHECK_EQ(r.len, 2); CHECK_EQ(r.data[0], 1);     /* self-powered */
	SETUP(0x82, 0, 0, 0x82, 2);     CHECK_EQ(r.len, 2); CHECK_EQ(r.data[0], 0);
	SETUP(0x82, 0, 0, 0x83, 2);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* no such endpoint */
	SETUP(0x02, 1, 0, 0x02, 0);     CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(r.halt_ep, 0x02); CHECK_EQ(r.halt_set, 0);
	SETUP(0x02, 3, 0, 0x82, 0);     CHECK_EQ(r.halt_ep, 0x82); CHECK_EQ(r.halt_set, 1);
	SETUP(0x02, 1, 0, 0x00, 0);     CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(r.halt_ep, -1);
	SETUP(0x00, 3, 2, 0x0400, 0);   CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* test mode: not offered */
	/* class */
	SETUP(0xa1, 0x80, 0, 0, 28);    CHECK_EQ(r.action, SBUSB_EP0_IN); CHECK_EQ(r.len, 28); CHECK_EQ(sbusb_get16(r.data), 28);
	SETUP(0xa1, 0x80, 0, 0, 64);    CHECK_EQ(r.len, 28);
	SETUP(0xa1, 0x80, 0, 1, 28);    CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* wrong interface */
	SETUP(0xa1, 0x85, 0, 0, 4);     CHECK_EQ(sbusb_get32(r.data), 16384);
	SETUP(0xa1, 0x83, 0, 0, 2);     CHECK_EQ(r.len, 2); CHECK_EQ(sbusb_get16(r.data), 0);
	SETUP(0x21, 0x84, 0, 0, 0);     CHECK_EQ(r.action, SBUSB_EP0_ACK);
	SETUP(0x21, 0x84, 1, 0, 0);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* 32-bit NTBs: not supported */
	SETUP(0x21, 0x43, 0x000e, 0, 0); CHECK_EQ(r.action, SBUSB_EP0_ACK); CHECK_EQ(s.packet_filter, 0x000e);
	SETUP(0x21, 0x86, 0, 0, 4);     CHECK_EQ(r.action, SBUSB_EP0_OUT); CHECK_EQ(r.len, 4);
	{
		uint8_t p[8] = { 0x21, 0x86, 0, 0, 0, 0, 4, 0 }, v[4];
		sbusb_put32(v, 4096);  CHECK_EQ(sbusb_ep0_out(&s, p, v, 4), 1); CHECK_EQ(s.ntb_in_max, 4096);
		sbusb_put32(v, 2047);  CHECK_EQ(sbusb_ep0_out(&s, p, v, 4), 0); CHECK_EQ(s.ntb_in_max, 4096);
		sbusb_put32(v, 16385); CHECK_EQ(sbusb_ep0_out(&s, p, v, 4), 0);
		sbusb_put32(v, 4096);  CHECK_EQ(sbusb_ep0_out(&s, p, v, 3), 0);
	}
	SETUP(0x21, 0x86, 0, 0, 8);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* the 8-byte form needs a capability we lack */
	SETUP(0xa1, 0x89, 0, 0, 2);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* CRC mode: not offered */
	SETUP(0xa1, 0x87, 0, 0, 2);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* max datagram size: not offered */
	SETUP(0xa1, 0x81, 0, 0, 6);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* net address: not offered */
	SETUP(0x21, 0x20, 0, 0, 7);     CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* ACM's SET_LINE_CODING: not this device */
	SETUP(0x40, 1, 0, 0, 0);        CHECK_EQ(r.action, SBUSB_EP0_STALL);            /* vendor */
	sbusb_state_reset(&s);
	CHECK_EQ(s.config, 0); CHECK_EQ(s.data_alt, 0); CHECK_EQ(s.ntb_in_max, 16384);
#undef SETUP
}

static void
controller(unsigned flags)
{
	struct sbusb_io io;
	struct sbusb_dma dma;
	struct sbusb_func_ops ops = { 0, 0, on_link, 0 };
	uint8_t b[512];
	unsigned c0;
	int n;

	mock_reset();
	io = mock_io();
	dma = mock_dma();
	link_up = link_calls = 0;
	sbusb_func_init(&F, &io, &dma, "0123456789abcdef0123456789abcde", flags, &ops);        /* 31 characters */
	CHECK_EQ(sbusb_func_start(&F), 0);
	/* ep0 after start: both directions configured as control at 512, resources set, SETUP TRB armed */
	CHECK_EQ(mock_count(0, 0, D3_CMD_STARTCFG), 1);
	CHECK_EQ(mock_count(0, 0, D3_CMD_SETEPCONFIG), 1); CHECK_EQ(mock_count(0, 1, D3_CMD_SETEPCONFIG), 1);
	CHECK_EQ(mock_count(0, 0, D3_CMD_SETXFERRES), 1); CHECK_EQ(mock_count(0, 1, D3_CMD_SETXFERRES), 1);
	CHECK_EQ(M.dwc3[D3_DALEPENA / 4], 3);
	CHECK(mock_trb(0) != 0);
	CHECK_EQ(sbusb_get32(mock_trb(0) + 12), D3_TRB_TYPE_SETUP | D3_TRB_HWO | D3_TRB_LST | D3_TRB_IOC | D3_TRB_ISP);
	CHECK_EQ(sbusb_get32(mock_trb(0) + 8), 8);

	/* reset + connect done at high speed: ep0 becomes 64 bytes (MODIFY) */
	c0 = M.ncmd;
	mock_host_reset(&F, 0);
	CHECK_EQ(mock_count(c0, 0, D3_CMD_SETEPCONFIG), 1);
	CHECK_EQ(M.cmd[M.ncmd - 1].p0 >> 30, 2);
	CHECK_EQ((M.cmd[M.ncmd - 1].p0 >> 3) & 0x7ff, 64);
	CHECK_EQ(F.d.connected, 1);

	/* GET_DESCRIPTOR(device), the first thing a host asks */
	n = mock_control(&F, 0x80, 6, 0x0100, 0, 64, b);
	CHECK_EQ(n, 18); CHECK_EQ(b[1], 1); CHECK_EQ(sbusb_get16(b + 8), 0x18d1);
	CHECK(mock_trb(0) != 0);                                /* SETUP armed again */
	/* SET_ADDRESS: in DCFG before the status stage */
	CHECK_EQ(mock_control(&F, 0x00, 5, 21, 0, 0, 0), 0);
	CHECK_EQ((M.dwc3[D3_DCFG / 4] >> 3) & 0x7f, 21);
	CHECK_EQ(M.dwc3[D3_DCFG / 4] & 7, 0);                   /* still high speed */
	n = mock_control(&F, 0x80, 6, 0x0200, 0, 9, b);
	CHECK_EQ(n, 9); CHECK_EQ(sbusb_get16(b + 2), 94);
	n = mock_control(&F, 0x80, 6, 0x0200, 0, 94, b);
	CHECK_EQ(n, 94);
	n = mock_control(&F, 0x80, 6, 0x0200, 0, 255, b);       /* 94 < 255, not a multiple of 64: no zero-length packet */
	CHECK_EQ(n, 94);
	/* a 64-byte answer to a longer request ends with a zero-length packet: the 31-character serial string */
	n = mock_control(&F, 0x80, 6, 0x0303, 0x409, 255, b);
	CHECK_EQ(n, 0x10000 | 64);
	n = mock_control(&F, 0x80, 6, 0x0303, 0x409, 64, b);    /* exactly what was asked: none */
	CHECK_EQ(n, 64);
	/* a request we do not know: STALL, and the next SETUP is taken */
	c0 = M.ncmd;
	CHECK_EQ(mock_control(&F, 0x80, 6, 0x0f00, 0, 5, b), -1);
	CHECK_EQ(mock_count(c0, 0, D3_CMD_SETSTALL), 1);
	CHECK(mock_trb(0) != 0);
	CHECK_EQ(F.d.n.stalls, 1);

	/* SET_CONFIGURATION(1): the three data endpoints */
	c0 = M.ncmd;
	CHECK_EQ(mock_control(&F, 0x00, 9, 1, 0, 0, 0), 0);
	CHECK_EQ(mock_count(c0, 0, D3_CMD_STARTCFG), 1);
	for (unsigned i = c0; i < M.ncmd; ++i) {
		struct mock_cmd *k = &M.cmd[i];
		if ((k->cmd & 0xf) == D3_CMD_STARTCFG) {
			CHECK_EQ(k->cmd >> 16, 2);              /* resources for the data endpoints start at 2 */
		}
		if ((k->cmd & 0xf) != D3_CMD_SETEPCONFIG) {
			continue;
		}
		CHECK(k->phys == 3 || k->phys == 4 || k->phys == 5);
		CHECK_EQ((k->p1 >> 25) & 0x1f, k->phys);
		CHECK(k->p1 & D3_CFG1_XFERCOMPLETE);
		CHECK_EQ(!!(k->p1 & D3_CFG1_XFERINPROGRESS), !!(flags & SBUSB_F_RING));
		CHECK_EQ(k->p0 >> 30, 0);
		if (k->phys == 3) {
			CHECK_EQ((k->p0 >> 1) & 3, 3); CHECK_EQ((k->p0 >> 3) & 0x7ff, 16); CHECK_EQ((k->p0 >> 17) & 0x1f, 1);
			CHECK_EQ((k->p1 >> 16) & 0xff, 8);
		} else {
			CHECK_EQ((k->p0 >> 1) & 3, 2); CHECK_EQ((k->p0 >> 3) & 0x7ff, 512);
			CHECK_EQ((k->p0 >> 17) & 0x1f, k->phys == 5 ? 2 : 0);
		}
	}
	CHECK_EQ(mock_count(c0, 3, D3_CMD_SETEPCONFIG) + mock_count(c0, 4, D3_CMD_SETEPCONFIG) + mock_count(c0, 5, D3_CMD_SETEPCONFIG), 3);
	CHECK_EQ(mock_count(c0, 3, D3_CMD_SETXFERRES) + mock_count(c0, 4, D3_CMD_SETXFERRES) + mock_count(c0, 5, D3_CMD_SETXFERRES), 3);
	CHECK_EQ(M.dwc3[D3_DALEPENA / 4], 3 | 1 << 3 | 1 << 4 | 1 << 5);
	CHECK_EQ(link_up, 0);                                   /* alt 0: no data yet */
	CHECK(mock_trb(4) == 0);

	/* the class requests a host's NCM driver sends before it opens the data interface */
	n = mock_control(&F, 0xa1, 0x80, 0, 0, 28, b);
	CHECK_EQ(n, 28); CHECK_EQ(sbusb_get32(b + 4), 16384); CHECK_EQ(sbusb_get32(b + 16), 16384);
	{
		uint8_t v[4];
		sbusb_put32(v, 8192);
		CHECK_EQ(mock_control(&F, 0x21, 0x86, 0, 0, 4, v), 0);          /* OUT data stage, then STATUS3 on IN */
		CHECK_EQ(F.d.st.ntb_in_max, 8192);
		n = mock_control(&F, 0xa1, 0x85, 0, 0, 4, b);
		CHECK_EQ(n, 4); CHECK_EQ(sbusb_get32(b), 8192);
		sbusb_put32(v, 100);                                            /* below the minimum: refused */
		CHECK_EQ(mock_control(&F, 0x21, 0x86, 0, 0, 4, v), -1);
		CHECK_EQ(F.d.st.ntb_in_max, 8192);
	}
	CHECK_EQ(mock_control(&F, 0x21, 0x43, 0x000e, 0, 0, 0), 0);
	CHECK_EQ(F.d.st.packet_filter, 0x000e);

	/* SET_INTERFACE(data, alt 1): receive buffers armed, both notifications queued, link up */
	CHECK_EQ(mock_control(&F, 0x01, 11, 1, 1, 0, 0), 0);
	CHECK_EQ(link_up, 1);
	CHECK(mock_trb(4) != 0);
	CHECK_EQ(sbusb_get32(mock_trb(4) + 8), SBUSB_NTB_MAX);
	CHECK_EQ(F.d.ring[SBUSB_RING_OUT].inflight, SBUSB_RX_SLOTS);
	CHECK(mock_trb(3) != 0);
	CHECK_EQ(sbusb_get32(mock_trb(3) + 8), 16);
	CHECK(memcmp(mock_trb_buf(mock_trb(3)), "\xa1\x2a\x00\x00\x00\x00\x08\x00", 8) == 0);    /* speed change first */
	CHECK_EQ(sbusb_get32(mock_trb_buf(mock_trb(3)) + 8), 480000000);
	CHECK_EQ(mock_complete(3, 0, 16), 1);
	sbusb_func_poll(&F);
	CHECK(mock_trb(3) != 0);
	CHECK(memcmp(mock_trb_buf(mock_trb(3)), "\xa1\x00\x01\x00\x00\x00\x00\x00", 8) == 0);    /* then "connected" */
	CHECK_EQ(mock_complete(3, 0, 8), 1);
	sbusb_func_poll(&F);
	CHECK(mock_trb(3) == 0);

	/* CLEAR_FEATURE(ENDPOINT_HALT) on bulk OUT (macOS does this when it opens a pipe): transfer ended,
	 * stall cleared, receive armed again */
	c0 = M.ncmd;
	CHECK_EQ(mock_control(&F, 0x02, 1, 0, 0x02, 0, 0), 0);
	CHECK_EQ(mock_count(c0, 4, D3_CMD_ENDXFER), 1);
	CHECK_EQ(mock_count(c0, 4, D3_CMD_CLEARSTALL), 1);
	CHECK(mock_trb(4) != 0);
	CHECK_EQ(F.d.ring[SBUSB_RING_OUT].inflight, SBUSB_RX_SLOTS);
	CHECK_EQ(link_up, 1);

	/* back to alt 0: bulk transfers ended, link down; alt 1 again: armed again */
	c0 = M.ncmd;
	CHECK_EQ(mock_control(&F, 0x01, 11, 0, 1, 0, 0), 0);
	CHECK_EQ(link_up, 0);
	CHECK_EQ(mock_count(c0, 4, D3_CMD_ENDXFER), 1);
	CHECK(mock_trb(4) == 0);
	CHECK_EQ(mock_control(&F, 0x01, 11, 1, 1, 0, 0), 0);
	CHECK_EQ(link_up, 1);
	CHECK_EQ(F.d.ring[SBUSB_RING_OUT].inflight, SBUSB_RX_SLOTS);

	/* a host that abandons a control transfer in its data stage and sends a new SETUP */
	{
		uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 }, *t;
		mock_complete(0, setup, 8);
		sbusb_func_poll(&F);
		t = mock_trb(1);
		CHECK(t != 0);
		sbusb_put32(t + 8, sbusb_get32(t + 8) | D3_TRBSTS_SETUP_PENDING << 28);
		sbusb_put32(t + 12, sbusb_get32(t + 12) & ~D3_TRB_HWO);
		M.cur_trb[1] = 0;
		mock_ep_event(1, D3_EVT_XFERCOMPLETE, 0);
		sbusb_func_poll(&F);
		CHECK(mock_trb(0) != 0);                        /* SETUP armed, no stall */
		n = mock_control(&F, 0x80, 6, 0x0100, 0, 18, b);
		CHECK_EQ(n, 18);
	}
	/* a control transfer whose status stage never comes: after one second ep0 starts over */
	{
		uint8_t setup[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };
		unsigned stalls = F.d.n.stalls;
		mock_complete(0, setup, 8);
		sbusb_func_poll(&F);
		CHECK(mock_trb(0) == 0);
		M.now_us += 900000;
		sbusb_func_poll(&F);
		CHECK_EQ(F.d.n.e0_timeouts, 0);
		M.now_us += 200000;
		sbusb_func_poll(&F);
		CHECK_EQ(F.d.n.e0_timeouts, 1);
		CHECK_EQ(F.d.n.stalls, stalls + 1);
		CHECK(mock_trb(0) != 0);
	}
	/* the status stage's XferNotReady arriving before the data stage completes is remembered */
	{
		uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
		mock_complete(0, setup, 8);
		sbusb_func_poll(&F);
		mock_ep_event(0, D3_EVT_XFERNOTREADY, 2);
		sbusb_func_poll(&F);
		CHECK(mock_trb(0) == 0);                        /* not yet: the data stage is still out */
		mock_complete(1, 0, 18);
		sbusb_func_poll(&F);
		CHECK(mock_trb(0) != 0);
		CHECK_EQ(sbusb_get32(mock_trb(0) + 12) & 0x3f0, D3_TRB_TYPE_STATUS3);
		mock_complete(0, 0, 0);
		sbusb_func_poll(&F);
		CHECK_EQ(sbusb_get32(mock_trb(0) + 12) & 0x3f0, D3_TRB_TYPE_SETUP);
	}
	/* a bus reset: address 0, endpoints off, class defaults, link down */
	mock_host_reset(&F, 1);
	CHECK_EQ((M.dwc3[D3_DCFG / 4] >> 3) & 0x7f, 0);
	CHECK_EQ(M.dwc3[D3_DALEPENA / 4], 3);
	CHECK_EQ(link_up, 0);
	CHECK_EQ(F.d.st.ntb_in_max, 16384);
	CHECK_EQ(F.d.st.full_speed, 1);
	n = mock_control(&F, 0x80, 6, 0x0200, 0, 255, b);       /* full speed now: 64-byte bulk endpoints */
	CHECK_EQ(n, 94); CHECK_EQ(sbusb_get16(b + 87 + 4), 64);
	c0 = M.ncmd;
	CHECK_EQ(mock_control(&F, 0x00, 9, 1, 0, 0, 0), 0);
	for (unsigned i = c0; i < M.ncmd; ++i) {
		if ((M.cmd[i].cmd & 0xf) == D3_CMD_SETEPCONFIG && M.cmd[i].phys >= 4) {
			CHECK_EQ((M.cmd[i].p0 >> 3) & 0x7ff, 64);
		}
	}
	CHECK_EQ(F.d.n.cmd_fail, 0);
}

int
main(void)
{
	decoder();
	controller(0);
	controller(SBUSB_F_RING);
	return mock_summary("t_ep0");
}
