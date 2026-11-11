/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Exynos 9810 ACPM IPC, AP side (see acpm_ipc.h and ../NOTES.md). Written from our own
 * build/xnu-12377/bsd/dev/starlte_touch.c (0302, run on the phone) and the facts in an earlier port's
 * drivers/soc/exynos/exynos_acpm.c (APSL-2.0, an earlier port; credited in ../NOTES.md): the faulting first SRAM
 * pages, 32-bit-only access, stale answers drained before each request, 600 ms per answer, three sends.
 */
#include "acpm_ipc.h"

#define LOG(a, ...) do { if ((a)->io->log) (a)->io->log((a)->io->ctx, __VA_ARGS__); } while (0)

static bool
sram_ok(const struct acpm *a, uint32_t off, uint32_t bytes)
{
	return off >= a->sram_first && off <= a->sram_size && bytes <= a->sram_size - off && (off & 3u) == 0;
}

/* Reads outside the usable SRAM read as 0 (never touched); the setup checks every offset first. */
static uint32_t
srd(const struct acpm *a, uint32_t off)
{
	if (!sram_ok(a, off, 4)) {
		return 0;
	}
	return a->io->sram_rd(a->io->ctx, off);
}

static void
swr(const struct acpm *a, uint32_t off, uint32_t v)
{
	if (sram_ok(a, off, 4)) {
		a->io->sram_wr(a->io->ctx, off, v);
	}
}

static void
barrier(const struct acpm *a)
{
	if (a->io->barrier) {
		a->io->barrier(a->io->ctx);
	}
}

const char *
acpm_strerror(int e)
{
	switch (e) {
	case ACPM_OK: return "ok";
	case ACPM_E_NODEV: return "no such channel";
	case ACPM_E_TIMEOUT: return "no answer";
	case ACPM_E_STATUS: return "APM status error (SPEEDY)";
	case ACPM_E_REFUSED: return "refused by the write policy";
	case ACPM_E_TABLE: return "firmware table unusable";
	case ACPM_E_ARG: return "bad argument";
	default: return "?";
	}
}

/* Every answer in a queue channel's answer queue taken out of the way, and its interrupt cleared. */
static void
drain(struct acpm *a, struct acpm_chan *c, uint32_t id)
{
	uint32_t front = srd(a, c->out_front), rear = srd(a, c->out_rear);
	if (front < c->len && rear != front) {
		swr(a, c->out_rear, front);
		a->st.stale_drained++;
	}
	a->io->mbox_wr(a->io->ctx, ACPM_MB_INTCR1, 1u << id);
}

int
acpm_setup(struct acpm *a, const struct acpm_io *io, uint32_t sram_size, uint32_t sram_first, uint32_t initdata)
{
	uint32_t i;

	for (i = 0; i < sizeof(*a); i++) {
		((uint8_t *)a)[i] = 0;
	}
	a->io = io;
	a->sram_size = sram_size;
	a->sram_first = sram_first;
	a->initdata = initdata;
	a->room_us = ACPM_ROOM_US;
	a->answer_us = ACPM_ANSWER_US;
	a->buffer_us = ACPM_BUFFER_US;
	a->poll_us = ACPM_POLL_US;
	a->sends = ACPM_SENDS;
	if (!sram_ok(a, initdata, 0x80)) {
		LOG(a, "acpm: initdata-base %#x not in usable SRAM (%#x..%#x)\n", initdata, sram_first, sram_size);
		return ACPM_E_TABLE;
	}
	for (i = 0; i < 24; i += 4) {
		uint32_t w = srd(a, initdata + ACPM_ID_STRINGS + i);
		for (uint32_t b = 0; b < 4; b++) {
			char ch = (char)(w >> (8 * b));
			a->version[i + b] = (ch >= 32 && ch < 127) ? ch : '.';
		}
	}
	a->version[24] = 0;
	a->table = srd(a, initdata + ACPM_ID_CHANNELS);
	a->nchannels = srd(a, initdata + ACPM_ID_NCHANNELS);
	a->ap_max = srd(a, initdata + ACPM_ID_AP_MAX);
	LOG(a, "acpm: firmware \"%s\", channel table at %#x, %u channels, %u the AP's\n", a->version, a->table,
	    a->nchannels, a->ap_max);
	if (a->ap_max == 0 || a->ap_max > ACPM_AP_MAX_LIMIT || !sram_ok(a, a->table, a->ap_max * ACPM_CH_SIZE)) {
		LOG(a, "acpm: the channel table is not where it may be read: not used\n");
		return ACPM_E_TABLE;
	}
	for (i = 0; i < a->ap_max; i++) {
		uint32_t d = a->table + i * ACPM_CH_SIZE;
		uint32_t id = srd(a, d + ACPM_CH_ID) & 0xffu;
		struct acpm_chan n;
		for (uint32_t k = 0; k < sizeof(n); k++) {
			((uint8_t *)&n)[k] = 0;
		}
		n.desc = d;
		n.owner = srd(a, d + ACPM_CH_OWNER);
		n.type = srd(a, d + ACPM_CH_TYPE);
		n.ap_poll = srd(a, d + ACPM_CH_AP_POLL);
		n.in_rear = srd(a, d + ACPM_CH_IN_REAR);
		n.in_front = srd(a, d + ACPM_CH_IN_FRONT);
		n.in_base = srd(a, d + ACPM_CH_IN_BASE);
		n.out_rear = srd(a, d + ACPM_CH_OUT_REAR);
		n.out_front = srd(a, d + ACPM_CH_OUT_FRONT);
		n.out_base = srd(a, d + ACPM_CH_OUT_BASE);
		n.len = srd(a, d + ACPM_CH_QLEN);
		n.elem = srd(a, d + ACPM_CH_ELEM);
		n.buffer = n.len <= 1;
		if (i == 0) {
			a->index0_id = id;
		}
		if (n.buffer) {
			n.usable = sram_ok(a, n.in_base, 16) && sram_ok(a, n.out_base, 16);
		} else {
			n.usable = n.len <= 64 && n.elem >= 16 && n.elem <= 256 && (n.elem & 3u) == 0 &&
			    sram_ok(a, n.in_rear, 4) && sram_ok(a, n.in_front, 4) && sram_ok(a, n.out_rear, 4) &&
			    sram_ok(a, n.out_front, 4) && sram_ok(a, n.in_base, n.len * n.elem) &&
			    sram_ok(a, n.out_base, n.len * n.elem);
		}
		LOG(a, "acpm: channel %u (index %u): owner %u type %u, %s %u x %u, %s, %s\n", id, i, n.owner, n.type,
		    n.buffer ? "buffer" : "queue", n.len, n.elem, n.ap_poll ? "polled" : "irq",
		    n.usable ? "usable" : "NOT usable (offsets outside the SRAM)");
		if (id >= ACPM_MAX_ID || a->ch[id].usable || !n.usable) {
			continue;
		}
		a->ch[id] = n;
		if (!n.buffer) {
			drain(a, &a->ch[id], id);
		}
	}
	a->ready = a->ch[ACPM_CH_PMIC].usable;
	if (!a->ready) {
		LOG(a, "acpm: no usable channel %u (PMIC)\n", ACPM_CH_PMIC);
		return ACPM_E_NODEV;
	}
	return ACPM_OK;
}

/* The answer with our sequence number, if it has come: copied out, and the answer queue emptied. */
static bool
take_answer(struct acpm *a, struct acpm_chan *c, uint32_t id, uint32_t seq, uint32_t cmd[4])
{
	uint32_t rear = srd(a, c->out_rear), front = srd(a, c->out_front);
	bool found = false;

	if (rear >= c->len || front >= c->len || rear == front) {
		return false;
	}
	for (uint32_t i = rear; i != front; i = (i + 1) % c->len) {
		uint32_t slot = c->out_base + i * c->elem;
		if (((srd(a, slot) & ACPM_SEQ_MASK) >> ACPM_SEQ_SHIFT) == seq) {
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = srd(a, slot + 4 * w);
			}
			found = true;
		}
	}
	drain(a, c, id);
	return found;
}

static int
request_queue(struct acpm *a, struct acpm_chan *c, uint32_t id, uint32_t cmd[4])
{
	uint32_t sent[4];

	for (uint32_t w = 0; w < 4; w++) {
		sent[w] = cmd[w];
	}
	for (uint32_t send = 0; send < a->sends; send++) {
		if (send) {
			a->st.resends++;
		}
		drain(a, c, id);                /* never into a queue with stale answers in it */
		uint32_t front = srd(a, c->in_front);
		if (front >= c->len) {
			return ACPM_E_TABLE;
		}
		uint32_t next = (front + 1) % c->len;
		uint64_t t0 = a->io->now_us(a->io->ctx);
		while (srd(a, c->in_rear) == next && a->io->now_us(a->io->ctx) - t0 < a->room_us) {
			a->io->delay_us(a->io->ctx, a->poll_us);
		}
		if (srd(a, c->in_rear) == next) {
			continue;               /* the APM has not taken our earlier requests */
		}
		c->seq = c->seq % 63u + 1u;
		sent[0] = (sent[0] & ~ACPM_SEQ_MASK) | c->seq << ACPM_SEQ_SHIFT;
		uint32_t slot = c->in_base + front * c->elem;
		for (uint32_t w = 0; w < c->elem / 4; w++) {
			swr(a, slot + 4 * w, w < 4 ? sent[w] : 0);
		}
		barrier(a);                     /* the words before the index, the index before the doorbell */
		swr(a, c->in_front, next);
		barrier(a);
		a->io->mbox_wr(a->io->ctx, ACPM_MB_INTGR0, 1u << (16 + id));
		t0 = a->io->now_us(a->io->ctx);
		for (;;) {
			if (take_answer(a, c, id, c->seq, cmd)) {
				return ACPM_OK;
			}
			if (a->io->now_us(a->io->ctx) - t0 >= a->answer_us) {
				break;
			}
			a->io->delay_us(a->io->ctx, a->poll_us);
		}
	}
	a->st.timeouts++;
	LOG(a, "acpm: channel %u: no answer after %u sends (w0 %#x)\n", id, a->sends, cmd[0]);
	return ACPM_E_TIMEOUT;
}

static int
request_buffer(struct acpm *a, struct acpm_chan *c, uint32_t id, uint32_t cmd[4])
{
	c->seq = c->seq % 63u + 1u;
	cmd[0] = (cmd[0] & ~ACPM_SEQ_MASK) | c->seq << ACPM_SEQ_SHIFT;
	a->io->mbox_wr(a->io->ctx, ACPM_MB_INTCR1, 1u << id);
	for (uint32_t w = 0; w < 4; w++) {
		swr(a, c->in_base + 4 * w, cmd[w]);
	}
	barrier(a);
	a->io->mbox_wr(a->io->ctx, ACPM_MB_INTGR0, 1u << (16 + id));
	uint64_t t0 = a->io->now_us(a->io->ctx);
	while (!(a->io->mbox_rd(a->io->ctx, ACPM_MB_INTSR1) & (1u << id))) {
		if (a->io->now_us(a->io->ctx) - t0 >= a->buffer_us) {
			a->st.timeouts++;
			return ACPM_E_TIMEOUT;
		}
		a->io->delay_us(a->io->ctx, a->poll_us);
	}
	for (uint32_t w = 0; w < 4; w++) {
		cmd[w] = srd(a, c->out_base + 4 * w);
	}
	a->io->mbox_wr(a->io->ctx, ACPM_MB_INTCR1, 1u << id);
	return ACPM_OK;
}

int
acpm_request(struct acpm *a, uint32_t id, uint32_t cmd[4])
{
	if (a == 0 || !a->ready || id >= ACPM_MAX_ID || !a->ch[id].usable) {
		return ACPM_E_NODEV;
	}
	a->st.requests++;
	struct acpm_chan *c = &a->ch[id];
	return c->buffer ? request_buffer(a, c, id, cmd) : request_queue(a, c, id, cmd);
}

int
acpm_pmic_read(struct acpm *a, uint32_t type, uint32_t reg, uint8_t *value)
{
	uint32_t cmd[4] = { acpm_pmic_w0(type, reg), acpm_pmic_w1(ACPM_FUNC_READ, 0, 0), 0, 0 };
	if (type > 0xfu || reg > 0xffu || value == 0) {
		return ACPM_E_ARG;
	}
	int e = acpm_request(a, ACPM_CH_PMIC, cmd);
	if (e != ACPM_OK) {
		return e;
	}
	if (cmd[1] >> 24) {
		a->st.status_errors++;
		return ACPM_E_STATUS;
	}
	*value = (uint8_t)(cmd[1] >> 8);
	return ACPM_OK;
}

int
acpm_pmic_write_raw(struct acpm *a, uint32_t type, uint32_t reg, uint8_t value)
{
	uint32_t cmd[4] = { acpm_pmic_w0(type, reg), acpm_pmic_w1(ACPM_FUNC_WRITE, value, 0), 0, 0 };
	if (type > 0xfu || reg > 0xffu) {
		return ACPM_E_ARG;
	}
	int e = acpm_request(a, ACPM_CH_PMIC, cmd);
	if (e != ACPM_OK) {
		return e;
	}
	if (cmd[1] >> 24) {
		a->st.status_errors++;
		return ACPM_E_STATUS;
	}
	return ACPM_OK;
}

int
acpm_dvfs_request(struct acpm *a, uint32_t domain, uint32_t khz, bool get, uint32_t *answer_khz)
{
	uint32_t cmd[4] = { domain, get ? 0u : khz, get ? 1u : 0u, 0 };
	if (a == 0 || !a->ready) {
		return ACPM_E_NODEV;
	}
	int e = acpm_request(a, a->index0_id, cmd);
	if (e == ACPM_OK && answer_khz) {
		*answer_khz = cmd[1];
	}
	return e;
}
