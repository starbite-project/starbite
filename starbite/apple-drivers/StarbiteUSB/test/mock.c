/* SPDX-License-Identifier: APSL-2.0
 * Host-test hardware model (mock.h).
 */
#include "mock.h"

struct mock M;
int checks, failures;
static uint8_t dma_mem[SBUSB_DMA_BYTES];

static int m_has(void *c, unsigned blk) { (void)c; return !(M.missing & (1u << blk)); }

static uint32_t
m_rd(void *c, unsigned blk, uint32_t off)
{
	(void)c;
	if (blk == SBUSB_BLK_DWC3) {
		return M.dwc3[off / 4];
	}
	if (blk == SBUSB_BLK_PHY) {
		return M.phy[off / 4];
	}
	return M.single[blk];
}

static void
m_wr(void *c, unsigned blk, uint32_t off, uint32_t v)
{
	(void)c;
	++M.writes[blk];
	if (blk == SBUSB_BLK_PHY) {
		M.phy[off / 4] = v;
		return;
	}
	if (blk != SBUSB_BLK_DWC3) {
		M.single[blk] = v;
		return;
	}
	if (off == D3_DCTL) {
		if ((v & D3_DCTL_CSFTRST) && !M.csftrst_stuck) {
			v &= ~D3_DCTL_CSFTRST;          /* self-clearing; the core is back at its reset state */
			M.dwc3[D3_DSTS / 4] |= D3_DSTS_DEVCTRLHLT;
			M.dwc3[D3_GEVNTCOUNT / 4] = 0;
			M.evt_w = 0;
			memset(M.cur_trb, 0, sizeof(M.cur_trb));
		}
		if (v & D3_DCTL_RUN_STOP) {
			M.dwc3[D3_DSTS / 4] &= ~D3_DSTS_DEVCTRLHLT;
		} else if (M.dwc3[D3_GEVNTCOUNT / 4] == 0) {
			M.dwc3[D3_DSTS / 4] |= D3_DSTS_DEVCTRLHLT;
		}
		M.dwc3[off / 4] = v;
	} else if (off == D3_GEVNTCOUNT) {
		uint32_t c0 = M.dwc3[off / 4];
		M.dwc3[off / 4] = v > c0 ? 0 : c0 - v;
		if (!(M.dwc3[D3_DCTL / 4] & D3_DCTL_RUN_STOP) && M.dwc3[off / 4] == 0) {
			M.dwc3[D3_DSTS / 4] |= D3_DSTS_DEVCTRLHLT;
		}
	} else if (off >= 0xc800u && off < 0xca00u && (off & 0xfu) == 0xcu) {
		unsigned phys = (off - 0xc800u) / 0x10u, code = v & 0xfu;
		struct mock_cmd *k = &M.cmd[M.ncmd % MOCK_CMDS];

		k->phys = phys;
		k->cmd = v & ~D3_CMD_ACT;
		k->p0 = M.dwc3[D3_DEPCMDPAR0(phys) / 4];
		k->p1 = M.dwc3[D3_DEPCMDPAR1(phys) / 4];
		k->p2 = M.dwc3[D3_DEPCMDPAR2(phys) / 4];
		++M.ncmd;
		v &= ~(D3_CMD_ACT | (0xfu << 12));
		if (M.fail_cmd && (unsigned)M.fail_cmd == code && (unsigned)M.fail_phys == phys) {
			v |= 1u << 12;
		} else if (code == D3_CMD_STARTXFER) {
			M.cur_trb[phys] = (uint64_t)k->p0 << 32 | k->p1;
			v = (v & ~(0x7fu << 16)) | (phys + 1u) << 16;   /* the transfer resource index */
		} else if (code == D3_CMD_ENDXFER) {
			M.cur_trb[phys] = 0;
		}
		M.dwc3[off / 4] = v;
	} else {
		M.dwc3[off / 4] = v;
	}
}

static void m_delay(void *c, unsigned us) { (void)c; M.now_us += us; }
static uint64_t m_now(void *c) { (void)c; return M.now_us; }
static void m_sync(void *c, uint32_t off, uint32_t len, int to_dev) { (void)c; (void)to_dev; ++M.syncs; if (off + len > SBUSB_DMA_BYTES) { abort(); } }

void
mock_reset(void)
{
	memset(&M, 0, sizeof(M));
	memset(dma_mem, 0xa5, sizeof(dma_mem));
	M.dma = dma_mem;
	M.now_us = 1000000;
	M.dwc3[D3_GSNPSID / 4] = 0x5533300au;
	M.dwc3[D3_DSTS / 4] = D3_DSTS_DEVCTRLHLT;
	M.dwc3[D3_GUSB2PHYCFG / 4] = D3_U2_SUSPHY | D3_U2_ENBLSLPM | D3_U2_FREECLK_EXISTS | 0x2400u;
	M.dwc3[D3_GUSB3PIPECTL / 4] = D3_U3_SUSPHY | 0x2u;
	M.dwc3[D3_GCTL / 4] = 0x00102000u | D3_GCTL_SCALEDOWN_MASK;      /* some other mode, scaled down */
	M.dwc3[D3_DCFG / 4] = 0x00080804u;                               /* SuperSpeed, a stale address */
	M.phy[UC_HSP_TEST / 4] = UC_HSP_TEST_SIDDQ | (1u << 20);
	M.phy[UC_UTMI / 4] = UC_UTMI_FORCE_SUSPEND | UC_UTMI_FORCE_SLEEP | UC_UTMI_DP_PULLDOWN | UC_UTMI_DM_PULLDOWN;
	M.phy[UC_LINK_CTRL / 4] = UC_LC_PIPE3_FORCE_PHY_STATUS;
	M.phy[UC_REWA_ENABLE / 4] = 1;
	M.phy[UC_HSREWA_CTRL / 4] = 1u << 24;
}

struct sbusb_io
mock_io(void)
{
	struct sbusb_io io = { 0, m_has, m_rd, m_wr, m_delay, m_now, m_sync };
	return io;
}

struct sbusb_dma
mock_dma(void)
{
	struct sbusb_dma d = { dma_mem, MOCK_DMA_PA, SBUSB_DMA_BYTES };
	return d;
}

static uint8_t *
pa_to_va(uint64_t pa)
{
	if (pa < MOCK_DMA_PA || pa >= MOCK_DMA_PA + SBUSB_DMA_BYTES) {
		printf("  mock: the controller was given an address outside the DMA block: 0x%llx\n", (unsigned long long)pa);
		abort();
	}
	return dma_mem + (pa - MOCK_DMA_PA);
}

void
mock_event(uint32_t ev)
{
	uint64_t base = (uint64_t)M.dwc3[D3_GEVNTADRHI / 4] << 32 | M.dwc3[D3_GEVNTADRLO / 4];
	uint32_t size = M.dwc3[D3_GEVNTSIZ / 4] & 0xfffcu;

	if (size == 0) {
		printf("  mock: an event with no event buffer\n");
		abort();
	}
	sbusb_put32(pa_to_va(base + M.evt_w), ev);
	M.evt_w = (M.evt_w + 4) % size;
	M.dwc3[D3_GEVNTCOUNT / 4] += 4;
}

void mock_dev_event(unsigned type, unsigned info) { mock_event(1u | type << 8 | info << 16); }
void mock_ep_event(unsigned phys, unsigned type, unsigned status) { mock_event(phys << 1 | type << 6 | status << 12); }

unsigned
mock_count(unsigned from, unsigned phys, uint32_t cmd)
{
	unsigned n = 0;
	for (unsigned i = from; i < M.ncmd && i < MOCK_CMDS; ++i) {
		n += M.cmd[i].phys == phys && (M.cmd[i].cmd & 0xfu) == cmd;
	}
	return n;
}

uint8_t *
mock_trb(unsigned phys)
{
	uint8_t *t;

	if (M.cur_trb[phys] == 0) {
		return 0;
	}
	t = pa_to_va(M.cur_trb[phys]);
	for (unsigned hops = 0; hops < 2 && (sbusb_get32(t + 12) & 0x3f0u) == D3_TRB_TYPE_LINK; ++hops) {
		if (!(sbusb_get32(t + 12) & D3_TRB_HWO)) {
			return 0;
		}
		M.cur_trb[phys] = (uint64_t)sbusb_get32(t + 4) << 32 | sbusb_get32(t);
		t = pa_to_va(M.cur_trb[phys]);
	}
	return (sbusb_get32(t + 12) & D3_TRB_HWO) ? t : 0;
}

uint8_t *
mock_trb_buf(const uint8_t *trb)
{
	return pa_to_va((uint64_t)sbusb_get32(trb + 4) << 32 | sbusb_get32(trb));
}

int
mock_complete(unsigned phys, const uint8_t *data, uint32_t moved)
{
	uint8_t *t = mock_trb(phys);
	uint32_t size, ctl;

	if (t == 0) {
		return 0;
	}
	size = sbusb_get32(t + 8) & D3_TRB_SIZE_MASK;
	ctl = sbusb_get32(t + 12);
	if (moved > size) {
		printf("  mock: %u bytes into a TRB of %u\n", moved, size);
		abort();
	}
	if (data != 0 && moved != 0) {
		memcpy(mock_trb_buf(t), data, moved);
	}
	sbusb_put32(t + 8, size - moved);
	sbusb_put32(t + 12, ctl & ~D3_TRB_HWO);
	if (ctl & D3_TRB_LST) {
		M.cur_trb[phys] = 0;
		mock_ep_event(phys, D3_EVT_XFERCOMPLETE, 0);
	} else {
		M.cur_trb[phys] += D3_TRB_BYTES;
		mock_ep_event(phys, D3_EVT_XFERINPROGRESS, 0);
	}
	return 1;
}

void
mock_host_reset(struct sbusb_func *f, int full_speed)
{
	mock_dev_event(1, 0);
	M.dwc3[D3_DSTS / 4] = (M.dwc3[D3_DSTS / 4] & ~D3_DSTS_SPEED_MASK) | (full_speed ? 1u : 0u);
	mock_dev_event(2, 0);
	sbusb_func_poll(f);
}

int
mock_control(struct sbusb_func *f, unsigned type, unsigned req, unsigned value, unsigned index, unsigned length,
    uint8_t *data)
{
	uint8_t setup[8] = { (uint8_t)type, (uint8_t)req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index,
		             (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8) };
	unsigned c0 = M.ncmd, status_phys;
	int got = 0;
	uint8_t *t = mock_trb(0);

	if (t == 0 || (sbusb_get32(t + 12) & 0x3f0u) != D3_TRB_TYPE_SETUP) {
		printf("  mock: no SETUP TRB armed on ep0\n");
		++failures;
		return -1;
	}
	mock_complete(0, setup, 8);
	sbusb_func_poll(f);
	if (mock_count(c0, 0, D3_CMD_SETSTALL)) {
		return -1;
	}
	if (length != 0 && (type & 0x80u)) {
		for (;;) {                              /* the data stage, and a zero-length packet if one follows */
			uint32_t n;

			t = mock_trb(1);
			if (t == 0 || (sbusb_get32(t + 12) & 0x3f0u) != D3_TRB_TYPE_DATA) {
				break;
			}
			n = sbusb_get32(t + 8) & D3_TRB_SIZE_MASK;
			memcpy(data + got, mock_trb_buf(t), n);
			got += (int)n;
			mock_complete(1, 0, n);
			sbusb_func_poll(f);
			if (n != 0 && n % 64 != 0) {
				break;
			}
			if (n == 0) {
				got |= 0x10000;                 /* flag: ended by a zero-length packet */
				break;
			}
		}
		status_phys = 0;
	} else if (length != 0) {
		t = mock_trb(0);
		if (t == 0 || (sbusb_get32(t + 12) & 0x3f0u) != D3_TRB_TYPE_DATA) {
			printf("  mock: no OUT data TRB on ep0\n");
			++failures;
			return -1;
		}
		mock_complete(0, data, length);
		sbusb_func_poll(f);
		if (mock_count(c0, 0, D3_CMD_SETSTALL)) {
			return -1;
		}
		status_phys = 1;
	} else {
		status_phys = 1;
	}
	mock_ep_event(status_phys, D3_EVT_XFERNOTREADY, 2);
	sbusb_func_poll(f);
	t = mock_trb(status_phys);
	if (t == 0 || ((sbusb_get32(t + 12) & 0x3f0u) != D3_TRB_TYPE_STATUS2 &&
	    (sbusb_get32(t + 12) & 0x3f0u) != D3_TRB_TYPE_STATUS3)) {
		printf("  mock: no status TRB on physical endpoint %u\n", status_phys);
		++failures;
		return -1;
	}
	CHECK_EQ(sbusb_get32(t + 12) & 0x3f0u, length != 0 ? D3_TRB_TYPE_STATUS3 : D3_TRB_TYPE_STATUS2);
	mock_complete(status_phys, 0, 0);
	sbusb_func_poll(f);
	return got;
}

void
mock_enumerate(struct sbusb_func *f, int full_speed)
{
	mock_host_reset(f, full_speed);
	CHECK_EQ(mock_control(f, 0x00, 5, 5, 0, 0, 0), 0);
	CHECK_EQ(mock_control(f, 0x00, 9, 1, 0, 0, 0), 0);
	CHECK_EQ(mock_control(f, 0x01, 11, 1, 1, 0, 0), 0);
}

int
mock_summary(const char *name)
{
	printf("%s: %d checks, %d failed\n", name, checks, failures);
	return failures != 0;
}
