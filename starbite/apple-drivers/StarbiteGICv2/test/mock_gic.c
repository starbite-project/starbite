/* SPDX-License-Identifier: APSL-2.0 */
/* GIC-400 model (see mock_gic.h). Written from the GICv2 architecture specification, not from any driver. */
#include <string.h>
#include "mock_gic.h"

void
mock_gic_init(struct mock_gic *m, unsigned ncpus)
{
	memset(m, 0, sizeof(*m));
	m->ncpus = ncpus;
	for (unsigned c = 0; c < MOCK_GIC_CPUS; c++) {
		/* not the identity on purpose: the driver must read the bit, not assume logical n = interface n */
		m->iface_of_cpu[c] = (uint8_t)(1u << ((c + 3u) % MOCK_GIC_CPUS));
		m->dview[c].g = m;
		m->dview[c].cpu = c;
		m->cview[c].g = m;
		m->cview[c].cpu = c;
	}
}

void
mock_gic_bind(struct mock_gic *m, unsigned cpu, struct sb_gic *g)
{
	g->d.ctx = &m->dview[cpu];
	g->d.rd = mock_gicd_rd;
	g->d.wr = mock_gicd_wr;
	g->c.ctx = &m->cview[cpu];
	g->c.rd = mock_gicc_rd;
	g->c.wr = mock_gicc_wr;
}

/* one-bit-per-INTID registers */
static uint32_t
bits_rd(struct mock_gic *m, unsigned cpu, uint32_t word, int which)
{
	uint32_t v = 0;
	if (word == 0) {
		switch (which) {
		case 0: return m->en_banked[cpu];
		case 1: {
			v = m->ppi_pending[cpu];
			for (unsigned s = 0; s < 16; s++) {
				if (m->sgi_pending[cpu][s]) {
					v |= 1u << s;
				}
			}
			return v;
		}
		default: return m->active_banked[cpu];
		}
	}
	for (unsigned b = 0; b < 32; b++) {
		uint32_t i = word * 32 + b;
		if (i >= MOCK_GIC_LINES) {
			break;
		}
		uint8_t s = which == 0 ? m->en[i] : which == 1 ? m->pend[i] : m->act[i];
		if (s) {
			v |= 1u << b;
		}
	}
	return v;
}

static void
bits_wr(struct mock_gic *m, unsigned cpu, uint32_t word, int which, bool set, uint32_t v)
{
	for (unsigned b = 0; b < 32; b++) {
		if (!(v & (1u << b))) {
			continue;
		}
		uint32_t i = word * 32 + b;
		if (word == 0) {
			uint32_t *r = which == 0 ? &m->en_banked[cpu] : which == 1 ? &m->ppi_pending[cpu] :
			    &m->active_banked[cpu];
			if (which == 1 && i < 16) {
				if (!set) {
					m->sgi_pending[cpu][i] = 0;
				}
				continue;               /* SGIs are made pending through GICD_SGIR (or SPENDSGIR) */
			}
			*r = set ? (*r | (1u << b)) : (*r & ~(1u << b));
			continue;
		}
		if (i >= MOCK_GIC_LINES) {
			m->bad_accesses++;
			continue;
		}
		uint8_t *s = which == 0 ? &m->en[i] : which == 1 ? &m->pend[i] : &m->act[i];
		*s = set ? 1 : 0;
		if (which == 1 && !set && !m->edge[i] && m->line[i]) {
			*s = 1;                 /* a level SPI stays pending while its line is high */
		}
	}
}

uint32_t
mock_gicd_rd(void *ctx, uint32_t off)
{
	struct mock_gic_view *vw = ctx;
	struct mock_gic *m = vw->g;
	unsigned cpu = vw->cpu;
	if (off == GICD_CTLR) {
		return m->ctlr;
	}
	if (off == GICD_TYPER) {
		return ((MOCK_GIC_LINES / 32u) - 1u) | ((m->ncpus - 1u) << 5) | GICD_TYPER_SECEXT;
	}
	if (off == GICD_IIDR) {
		return 0x0200143bu;     /* GIC-400 r0p? (ARM, product 0x02) */
	}
	if (off >= GICD_IGROUPR && off < GICD_ISENABLER) {
		return 0;               /* secure only */
	}
	if (off >= GICD_ISENABLER && off < GICD_ICENABLER) {
		return bits_rd(m, cpu, (off - GICD_ISENABLER) / 4, 0);
	}
	if (off >= GICD_ICENABLER && off < GICD_ISPENDR) {
		return bits_rd(m, cpu, (off - GICD_ICENABLER) / 4, 0);
	}
	if (off >= GICD_ISPENDR && off < GICD_ISACTIVER) {
		return bits_rd(m, cpu, (off - (off < GICD_ICPENDR ? GICD_ISPENDR : GICD_ICPENDR)) / 4, 1);
	}
	if (off >= GICD_ISACTIVER && off < GICD_IPRIORITYR) {
		return bits_rd(m, cpu, (off - (off < GICD_ICACTIVER ? GICD_ISACTIVER : GICD_ICACTIVER)) / 4, 2);
	}
	if (off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + MOCK_GIC_LINES) {
		uint32_t i = off - GICD_IPRIORITYR, v = 0;
		for (unsigned k = 0; k < 4; k++) {
			v |= (uint32_t)(i + k < 32 ? m->prio_banked[cpu][i + k] : m->prio[i + k]) << (8 * k);
		}
		return v;
	}
	if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + MOCK_GIC_LINES) {
		uint32_t i = off - GICD_ITARGETSR, v = 0;
		for (unsigned k = 0; k < 4; k++) {
			v |= (uint32_t)(i + k < 32 ? m->iface_of_cpu[cpu] : m->target[i + k]) << (8 * k);
		}
		return v;
	}
	if (off >= GICD_ICFGR && off < GICD_ICFGR + MOCK_GIC_LINES / 4) {
		uint32_t first = (off - GICD_ICFGR) * 4, v = 0;
		for (unsigned k = 0; k < 16; k++) {
			uint32_t i = first + k;
			bool e = i < 16 ? true : i < 32 ? false : m->edge[i];
			v |= (uint32_t)e << (2 * k + 1);
		}
		return v;
	}
	if (off == GICD_SGIR) {
		m->bad_accesses++;      /* write only */
		return 0;
	}
	m->bad_accesses++;
	return 0;
}

void
mock_gicd_wr(void *ctx, uint32_t off, uint32_t v)
{
	struct mock_gic_view *vw = ctx;
	struct mock_gic *m = vw->g;
	unsigned cpu = vw->cpu;
	if (off == GICD_CTLR) {
		m->ctlr = v & 1u;
		return;
	}
	if (off >= GICD_IGROUPR && off < GICD_ISENABLER) {
		return;                 /* WI */
	}
	if (off >= GICD_ISENABLER && off < GICD_ICENABLER) {
		bits_wr(m, cpu, (off - GICD_ISENABLER) / 4, 0, true, v);
		return;
	}
	if (off >= GICD_ICENABLER && off < GICD_ISPENDR) {
		bits_wr(m, cpu, (off - GICD_ICENABLER) / 4, 0, false, v);
		return;
	}
	if (off >= GICD_ISPENDR && off < GICD_ICPENDR) {
		bits_wr(m, cpu, (off - GICD_ISPENDR) / 4, 1, true, v);
		return;
	}
	if (off >= GICD_ICPENDR && off < GICD_ISACTIVER) {
		bits_wr(m, cpu, (off - GICD_ICPENDR) / 4, 1, false, v);
		return;
	}
	if (off >= GICD_ISACTIVER && off < GICD_ICACTIVER) {
		bits_wr(m, cpu, (off - GICD_ISACTIVER) / 4, 2, true, v);
		return;
	}
	if (off >= GICD_ICACTIVER && off < GICD_IPRIORITYR) {
		bits_wr(m, cpu, (off - GICD_ICACTIVER) / 4, 2, false, v);
		return;
	}
	if (off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + MOCK_GIC_LINES) {
		uint32_t i = off - GICD_IPRIORITYR;
		for (unsigned k = 0; k < 4; k++) {
			uint8_t p = (uint8_t)(v >> (8 * k));
			if (i + k < 32) {
				m->prio_banked[cpu][i + k] = p;
			} else {
				m->prio[i + k] = p;
			}
		}
		return;
	}
	if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + MOCK_GIC_LINES) {
		uint32_t i = off - GICD_ITARGETSR;
		for (unsigned k = 0; k < 4; k++) {
			if (i + k >= 32) {
				m->target[i + k] = (uint8_t)(v >> (8 * k));
			}               /* 0..31: read only */
		}
		return;
	}
	if (off >= GICD_ICFGR && off < GICD_ICFGR + MOCK_GIC_LINES / 4) {
		uint32_t first = (off - GICD_ICFGR) * 4;
		for (unsigned k = 0; k < 16; k++) {
			if (first + k >= 32) {
				m->edge[first + k] = (v >> (2 * k + 1)) & 1u;
			}
		}
		return;
	}
	if (off == GICD_SGIR) {
		m->sgir_writes++;
		m->last_sgir = v;
		uint32_t sgi = v & 0xfu, filter = (v >> 24) & 3u;
		uint8_t list = (uint8_t)(v >> 16);
		unsigned src = 0;       /* GICC_IAR[12:10] = the requester's interface number, not its logical id */
		while (src < 7u && !(m->iface_of_cpu[cpu] & (1u << src))) {
			src++;
		}
		for (unsigned c = 0; c < m->ncpus; c++) {
			bool hit = filter == GICD_SGIR_FILTER_SELF ? c == cpu :
			    filter == GICD_SGIR_FILTER_OTHERS ? c != cpu : (list & m->iface_of_cpu[c]) != 0;
			if (hit) {
				m->sgi_pending[c][sgi] |= (uint8_t)(1u << src);
			}
		}
		return;
	}
	m->bad_accesses++;
}

/* the best interrupt for `cpu`: lowest priority value among pending, enabled, not active, targeted */
static bool
best(struct mock_gic *m, unsigned cpu, uint32_t *intid_out, unsigned *src_out)
{
	int bestp = 0x100;
	uint32_t bi = 0;
	unsigned bs = 0;
	if (!(m->ctlr & 1u) || !(m->cctlr[cpu] & 1u)) {
		return false;
	}
	for (uint32_t i = 0; i < 32; i++) {
		if (!(m->en_banked[cpu] & (1u << i)) || (m->active_banked[cpu] & (1u << i))) {
			continue;
		}
		int p = m->prio_banked[cpu][i];
		if (i < 16) {
			for (unsigned s = 0; s < 8; s++) {
				if ((m->sgi_pending[cpu][i] & (1u << s)) && p < bestp) {
					bestp = p;
					bi = i;
					bs = s;
				}
			}
		} else if ((m->ppi_pending[cpu] & (1u << i)) && p < bestp) {
			bestp = p;
			bi = i;
		}
	}
	for (uint32_t i = 32; i < MOCK_GIC_LINES; i++) {
		if (m->en[i] && m->pend[i] && !m->act[i] && (m->target[i] & m->iface_of_cpu[cpu]) && m->prio[i] < bestp) {
			bestp = m->prio[i];
			bi = i;
		}
	}
	if (bestp >= 0x100 || bestp >= (int)m->pmr[cpu]) {
		return false;
	}
	*intid_out = bi;
	*src_out = bs;
	return true;
}

bool
mock_gic_irq_line(struct mock_gic *m, unsigned cpu)
{
	uint32_t i;
	unsigned s;
	return best(m, cpu, &i, &s);
}

uint32_t
mock_gicc_rd(void *ctx, uint32_t off)
{
	struct mock_gic_view *vw = ctx;
	struct mock_gic *m = vw->g;
	unsigned cpu = vw->cpu;
	switch (off) {
	case GICC_CTLR: return m->cctlr[cpu];
	case GICC_PMR: return m->pmr[cpu];
	case GICC_BPR: return m->bpr[cpu];
	case GICC_IIDR: return 0x0202143bu;
	case GICC_IAR: {
		uint32_t i;
		unsigned s;
		if (!best(m, cpu, &i, &s)) {
			return GIC_INTID_SPURIOUS;
		}
		if (i < 16) {
			m->sgi_pending[cpu][i] &= (uint8_t)~(1u << s);
			m->active_banked[cpu] |= 1u << i;
			return i | (s << 10);
		}
		if (i < 32) {
			m->ppi_pending[cpu] &= ~(1u << i);
			m->active_banked[cpu] |= 1u << i;
			return i;
		}
		m->act[i] = 1;
		m->act_cpu[i] = (uint8_t)cpu;
		m->pend[i] = 0;         /* level: comes back at EOI if the line is still high */
		return i;
	}
	default:
		m->bad_accesses++;
		return 0;
	}
}

void
mock_gicc_wr(void *ctx, uint32_t off, uint32_t v)
{
	struct mock_gic_view *vw = ctx;
	struct mock_gic *m = vw->g;
	unsigned cpu = vw->cpu;
	switch (off) {
	case GICC_CTLR: m->cctlr[cpu] = v & 1u; return;
	case GICC_PMR: m->pmr[cpu] = v & 0xffu; return;
	case GICC_BPR: m->bpr[cpu] = v & 7u; return;
	case GICC_EOIR: {
		uint32_t i = GIC_IAR_INTID(v);
		if (i < 32) {
			if (!(m->active_banked[cpu] & (1u << i))) {
				m->eoi_mismatch++;
			}
			m->active_banked[cpu] &= ~(1u << i);
			return;
		}
		if (i >= MOCK_GIC_LINES || !m->act[i] || m->act_cpu[i] != cpu) {
			m->eoi_mismatch++;
			return;
		}
		m->act[i] = 0;
		if (!m->edge[i] && m->line[i]) {
			m->pend[i] = 1;
		}
		return;
	}
	default:
		m->bad_accesses++;
	}
}

void
mock_gic_set_line(struct mock_gic *m, uint32_t intid, bool high)
{
	if (intid < 32 || intid >= MOCK_GIC_LINES) {
		m->bad_accesses++;
		return;
	}
	bool was = m->line[intid];
	m->line[intid] = high;
	if (m->edge[intid]) {
		if (high && !was) {
			m->pend[intid] = 1;
		}
	} else if (high) {
		m->pend[intid] = 1;
	} else if (!m->act[intid]) {
		m->pend[intid] = 0;
	}
}

void
mock_gic_ppi_raise(struct mock_gic *m, unsigned cpu, uint32_t intid)
{
	if (intid < 16 || intid >= 32) {
		m->bad_accesses++;
		return;
	}
	m->ppi_pending[cpu] |= 1u << intid;
}
