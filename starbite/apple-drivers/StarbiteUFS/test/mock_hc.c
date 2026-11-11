/* SPDX-License-Identifier: APSL-2.0 */
/* The mock UFS host (mock_hc.h). Its descriptor decoding is written separately from hw/ on purpose. */
#include "mock_hc.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t b32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t b64(const uint8_t *p) { return (uint64_t)b32(p) << 32 | b32(p + 4); }

static uint8_t *
pa2va(struct mock *m, uint64_t pa, size_t len)
{
	if (pa >= MOCK_DMA_PA && pa + len <= MOCK_DMA_PA + m->dma_size) {
		return m->dma + (pa - MOCK_DMA_PA);
	}
	for (unsigned i = 0; i < m->nmap; i++) {
		if (pa >= m->map[i].pa && pa + len <= m->map[i].pa + m->map[i].len) {
			return m->map[i].va + (pa - m->map[i].pa);
		}
	}
	return NULL;
}

static uint32_t m_rd(void *ctx, unsigned bank, uint32_t off)
{
	struct mock *m = ctx;
	assert(off < 0x200 && (off & 3) == 0);
	if (bank == SBUFS_BANK_HCI) {
		return m->hci[off / 4];
	}
	if (bank == SBUFS_BANK_VS) {
		return m->vs[off / 4];
	}
	assert(!"mock: bank never mapped by the driver");
	return 0;
}

static void execute(struct mock *m, unsigned slot);

static void m_wr(void *ctx, unsigned bank, uint32_t off, uint32_t v)
{
	struct mock *m = ctx;
	assert(off < 0x200 && (off & 3) == 0);
	if (bank == SBUFS_BANK_VS) {
		m->vs[off / 4] = v;
		return;
	}
	assert(bank == SBUFS_BANK_HCI);
	switch (off) {
	case SBUFS_IS:
		m->hci[off / 4] &= ~v;          /* write 1 to clear */
		return;
	case SBUFS_UTRLBA:
		m->hci[off / 4] = m->refuse_list ? 0 : v;
		return;
	case SBUFS_UTRLCLR:
		/* Exynos: a 1 clears that slot */
		m->hci[SBUFS_UTRLDBR / 4] &= ~v;
		m->pending &= ~v;
		m->hung &= ~v;
		return;
	case SBUFS_UTRLDBR: {
		uint32_t newbits = v & ~m->hci[off / 4];
		m->hci[off / 4] |= v;
		if (m->inject_fatal) {
			m->inject_fatal = false;
			m->hci[SBUFS_IS / 4] |= SBUFS_IS_HCFES;
			m->hung |= newbits;
			return;
		}
		for (unsigned s = 0; s < 32; s++) {
			if (!(newbits & (1u << s))) {
				continue;
			}
			if (m->inject_hang) {
				m->inject_hang--;
				m->hung |= 1u << s;
			} else if (m->deferred) {
				m->pending |= 1u << s;
			} else {
				execute(m, s);
			}
		}
		return;
	}
	default:
		m->hci[off / 4] = v;
	}
}

static void m_barrier(void *ctx) { (void)ctx; }
static uint64_t m_now(void *ctx) { struct mock *m = ctx; return m->now++; }
static void m_delay(void *ctx, uint32_t us) { struct mock *m = ctx; m->now += us; if (m->deferred) mock_tick(m); }
static void m_log(void *ctx, const char *line)
{
	struct mock *m = ctx;
	snprintf(m->lastlog, sizeof(m->lastlog), "%s", line);
	m->nlog++;
	if (m->echo) {
		printf("    | %s\n", line);
	}
}

void
mock_init(struct mock *m, size_t dma_size)
{
	memset(m, 0, sizeof(*m));
	m->dma_size = dma_size;
	m->dma = calloc(1, dma_size);
	/* the phone after S-Boot (NOTES.md): cap 0x1303ff0f, ver 0x210, hcs 0x10f, HCE 0, S-Boot's list, rsr 1, nexus 2 */
	m->hci[SBUFS_CAP / 4] = 0x1303ff0f;
	m->hci[SBUFS_VER / 4] = 0x210;
	m->hci[SBUFS_HCS / 4] = 0x10f;
	m->hci[SBUFS_HCE / 4] = 0;
	m->hci[SBUFS_UTRLBA / 4] = 0xef002000;
	m->hci[SBUFS_UTRLRSR / 4] = 1;
	m->vs[SBUFS_VS_UTRL_NEXUS_TYPE / 4] = 2;
	m->ops.ctx = m;
	m->ops.rd = m_rd;
	m->ops.wr = m_wr;
	m->ops.barrier = m_barrier;
	m->ops.now_us = m_now;
	m->ops.delay_us = m_delay;
	m->ops.log = m_log;
	m->device_asleep = true;
}

void
mock_free(struct mock *m)
{
	free(m->dma);
	for (unsigned i = 0; i < 8; i++) {
		free(m->lu[i].data);
		free(m->lu[i].touched);
	}
}

void
mock_add_lu(struct mock *m, unsigned lun, uint64_t blocks)
{
	m->lu[lun].blocks = blocks;
	m->lu[lun].data = calloc(blocks, SBUFS_BLOCK);
	m->lu[lun].touched = calloc(blocks, 1);
	m->lu[lun].ua_pending = true;
	for (uint64_t b = 0; b < blocks; b++) {         /* each block says where it is */
		uint8_t *p = m->lu[lun].data + b * SBUFS_BLOCK;
		for (unsigned i = 0; i < SBUFS_BLOCK; i += 16) {
			p[i] = (uint8_t)lun;
			memcpy(p + i + 1, &b, 8);
			p[i + 9] = (uint8_t)(i >> 4);
		}
	}
}

void
mock_map_buffer(struct mock *m, uint64_t pa, uint8_t *va, size_t len)
{
	assert(m->nmap < MOCK_MAX_MAP);
	m->map[m->nmap].pa = pa;
	m->map[m->nmap].va = va;
	m->map[m->nmap].len = len;
	m->nmap++;
}

void
mock_tick(struct mock *m)
{
	mock_tick_mask(m, 0xffffffffu);
}

void
mock_tick_mask(struct mock *m, uint32_t mask)
{
	uint32_t p = m->pending & mask;
	m->pending &= ~mask;
	for (unsigned s = 0; s < 32; s++) {
		if (p & (1u << s)) {
			execute(m, s);
		}
	}
}

uint64_t
mock_count_touched(const struct mock *m, unsigned lun, uint64_t first, uint64_t count, int how)
{
	uint64_t n = 0;
	for (uint64_t b = first; b < first + count && b < m->lu[lun].blocks; b++) {
		n += how ? (m->lu[lun].touched[b] & how) != 0 : m->lu[lun].touched[b] != 0;
	}
	return n;
}

uint64_t
mock_count_touched_outside(const struct mock *m, unsigned lun, uint64_t first, uint64_t count)
{
	uint64_t n = 0;
	for (uint64_t b = 0; b < m->lu[lun].blocks; b++) {
		if ((b < first || b >= first + count) && m->lu[lun].touched[b]) {
			n++;
		}
	}
	return n;
}

/* ---- the device ---- */

static void
respond(uint8_t *utrd, uint8_t *rsp, uint8_t tag, uint8_t ocs, uint8_t status, uint8_t key, uint8_t asc, uint8_t ascq)
{
	memset(rsp, 0, 512);
	rsp[0] = 0x21;
	rsp[3] = tag;
	rsp[7] = status;
	if (status == SBUFS_STATUS_CHECK) {
		rsp[32] = 0;
		rsp[33] = 18;
		rsp[34] = 0x70;
		rsp[34 + 2] = key;
		rsp[34 + 12] = asc;
		rsp[34 + 13] = ascq;
	}
	p32(utrd + 8, (g32(utrd + 8) & ~0xffu) | ocs);
}

/* move len bytes between the device buffer and the PRDT's memory; false if the PRDT is unusable */
static bool
prdt_xfer(struct mock *m, uint8_t *utrd, uint8_t *ucd, uint8_t *buf, uint32_t len, bool to_mem, bool garble)
{
	uint32_t dw7 = g32(utrd + 28);
	uint32_t off = dw7 >> 16, bytes = dw7 & 0xffff;   /* Exynos: bytes */
	if (bytes % 16 != 0) {
		m->bad_prdt++;
		return false;
	}
	uint32_t n = bytes / 16, done = 0;
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = ucd + off + i * 16;
		uint64_t pa = g32(e) | (uint64_t)g32(e + 4) << 32;
		uint32_t elen = (g32(e + 12) & 0x3ffff) + 1;
		if (elen > 4096 || (g32(e + 12) >> 28) != 0 || done + elen > len) {
			m->bad_prdt++;
			return false;
		}
		uint8_t *va = pa2va(m, pa, elen);
		if (va == NULL) {
			m->bad_prdt++;
			return false;
		}
		if (to_mem) {
			memcpy(va, buf + done, elen);
			if (garble) {
				va[0] ^= 0x5a;
			}
		} else {
			memcpy(buf + done, va, elen);
		}
		done += elen;
	}
	if (done != len) {
		m->bad_prdt++;
		return false;
	}
	return true;
}

static void
execute(struct mock *m, unsigned slot)
{
	uint64_t list = m->hci[SBUFS_UTRLBA / 4] | (uint64_t)m->hci[SBUFS_UTRLBAU / 4] << 32;
	uint8_t *utrd = pa2va(m, list + slot * 32, 32);
	assert(utrd != NULL);
	uint64_t ucd_pa = g32(utrd + 16) | (uint64_t)g32(utrd + 20) << 32;
	uint8_t *ucd = pa2va(m, ucd_pa, 0x800);
	assert(ucd != NULL);
	uint32_t dw6 = g32(utrd + 24);
	uint8_t *rsp = ucd + (dw6 >> 16);
	uint8_t tag = ucd[3], lun = ucd[2];
	const uint8_t *cdb = ucd + 16;
	uint32_t xfer = b32(ucd + 12);
	static uint8_t buf[1 << 20];

	m->ncmds++;
	m->hci[SBUFS_UTRLDBR / 4] &= ~(1u << slot);
	m->hci[SBUFS_IS / 4] |= SBUFS_IS_UTRCS;
	if (!(m->vs[SBUFS_VS_UTRL_NEXUS_TYPE / 4] & (1u << slot))) {
		m->nexus_missing++;
		respond(utrd, rsp, tag, 0x8, 0, 0, 0, 0);
		return;
	}
	if (m->inject_ocs) {
		m->inject_ocs--;
		respond(utrd, rsp, tag, 0x7, 0, 0, 0, 0);
		return;
	}
	if (m->inject_check) {
		m->inject_check--;
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 3, 0x11, 0);
		return;
	}
	if (lun == SBUFS_WLUN_DEVICE) {
		if (cdb[0] == SBUFS_SCSI_SSU && cdb[4] == 0x10) {
			m->device_asleep = false;
			respond(utrd, rsp, tag, 0, 0, 0, 0, 0);
		} else if (cdb[0] == SBUFS_SCSI_TUR) {
			respond(utrd, rsp, tag, m->device_asleep ? 7 : 0, m->device_asleep ? SBUFS_STATUS_CHECK : 0, 2, 4, 2);
		} else {
			respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 5, 0x20, 0);
		}
		return;
	}
	if (lun >= 8 || m->lu[lun].blocks == 0) {
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 5, 0x25, 0);   /* LOGICAL UNIT NOT SUPPORTED */
		return;
	}
	struct mock_lu *L = &m->lu[lun];
	if (m->device_asleep) {
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 2, 4, 2);
		return;
	}
	if (L->ua_pending) {
		L->ua_pending = false;
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 6, 0x29, 0);
		return;
	}
	uint64_t lba = 0, n = 0;
	switch (cdb[0]) {
	case SBUFS_SCSI_TUR:
		respond(utrd, rsp, tag, 0, 0, 0, 0, 0);
		return;
	case SBUFS_SCSI_INQUIRY:
		memset(buf, 0, 36);
		memcpy(buf + 8, "SAMSUNG ", 8);
		memcpy(buf + 16, "KLUCG4J1ED-B0C1 ", 16);
		memcpy(buf + 32, "0200", 4);
		respond(utrd, rsp, tag, prdt_xfer(m, utrd, ucd, buf, xfer, true, false) ? 0 : 6, 0, 0, 0, 0);
		return;
	case SBUFS_SCSI_READ_CAP10: {
		uint64_t last = L->blocks - 1;
		memset(buf, 0, 8);
		uint32_t l32 = (last > 0xffffffffu || m->rc10_ffff) ? 0xffffffffu : (uint32_t)last;
		buf[0] = l32 >> 24; buf[1] = l32 >> 16; buf[2] = l32 >> 8; buf[3] = l32;
		buf[6] = 0x10;                          /* 4096 */
		respond(utrd, rsp, tag, prdt_xfer(m, utrd, ucd, buf, xfer, true, false) ? 0 : 6, 0, 0, 0, 0);
		return;
	}
	case SBUFS_SCSI_SAI16: {
		uint64_t last = L->blocks - 1;
		memset(buf, 0, 32);
		for (int i = 0; i < 8; i++) {
			buf[i] = (uint8_t)(last >> (56 - 8 * i));
		}
		buf[10] = 0x10;
		respond(utrd, rsp, tag, prdt_xfer(m, utrd, ucd, buf, xfer, true, false) ? 0 : 6, 0, 0, 0, 0);
		return;
	}
	case SBUFS_SCSI_READ10: case SBUFS_SCSI_WRITE10: case SBUFS_SCSI_SYNC10:
		lba = b32(cdb + 2);
		n = (uint64_t)cdb[7] << 8 | cdb[8];
		break;
	case SBUFS_SCSI_READ16: case SBUFS_SCSI_WRITE16: case SBUFS_SCSI_SYNC16:
		lba = b64(cdb + 2);
		n = b32(cdb + 10);
		break;
	default:
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 5, 0x20, 0);
		return;
	}
	if (lba >= L->blocks || n > L->blocks - lba) {
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 5, 0x21, 0);   /* LBA OUT OF RANGE */
		return;
	}
	m->last_lba[lun] = lba;
	bool is_sync = cdb[0] == SBUFS_SCSI_SYNC10 || cdb[0] == SBUFS_SCSI_SYNC16;
	bool is_read = cdb[0] == SBUFS_SCSI_READ10 || cdb[0] == SBUFS_SCSI_READ16;
	if (is_sync) {
		respond(utrd, rsp, tag, 0, 0, 0, 0, 0);
		return;
	}
	if (xfer != n * SBUFS_BLOCK || xfer > sizeof(buf)) {
		respond(utrd, rsp, tag, 0, SBUFS_STATUS_CHECK, 5, 0x24, 0);
		return;
	}
	uint32_t entries = (g32(utrd + 28) & 0xffff) / 16;
	bool garble = entries > 1 && (m->multi_broken ||
	    (m->multi_needs_vs && m->vs[SBUFS_VS_TXPRDT_ENTRY_SIZE / 4] != SBUFS_VS_PRDT_PREFETCH_4K));
	for (uint64_t b = lba; b < lba + n; b++) {
		L->touched[b] |= is_read ? 1 : 2;
	}
	if (is_read) {
		memcpy(buf, L->data + lba * SBUFS_BLOCK, xfer);
		respond(utrd, rsp, tag, prdt_xfer(m, utrd, ucd, buf, xfer, true, garble) ? 0 : 6, 0, 0, 0, 0);
	} else {
		bool ok = prdt_xfer(m, utrd, ucd, buf, xfer, false, false);
		if (ok) {
			memcpy(L->data + lba * SBUFS_BLOCK, buf, xfer);
		}
		respond(utrd, rsp, tag, ok ? 0 : 6, 0, 0, 0, 0);
	}
}
