/* SPDX-License-Identifier: APSL-2.0 */
/* GIC-400 register logic (see sb_gicv2.h). Plain C: builds into the kernel and into the host tests. */
#include "sb_gicv2.h"

static inline uint32_t
drd(struct sb_gic *g, uint32_t off)
{
	return g->d.rd(g->d.ctx, off);
}

static inline void
dwr(struct sb_gic *g, uint32_t off, uint32_t v)
{
	g->d.wr(g->d.ctx, off, v);
}

static inline uint32_t
crd(struct sb_gic *g, uint32_t off)
{
	return g->c.rd(g->c.ctx, off);
}

static inline void
cwr(struct sb_gic *g, uint32_t off, uint32_t v)
{
	g->c.wr(g->c.ctx, off, v);
}

int
sb_gic_probe(struct sb_gic *g)
{
	g->typer = drd(g, GICD_TYPER);
	g->iidr = drd(g, GICD_IIDR);
	if (g->typer == 0 || g->typer == 0xffffffffu) {
		return SB_GIC_E_HW;
	}
	uint32_t n = 32u * (GICD_TYPER_ITLINES(g->typer) + 1u);
	g->nlines = n > GIC_MAX_LINES ? GIC_MAX_LINES : n;
	g->ncpu_ifaces = GICD_TYPER_CPUS(g->typer);
	return SB_GIC_OK;
}

enum sb_gic_kind
sb_gic_kind(const struct sb_gic *g, uint32_t intid)
{
	if (intid < 16u) {
		return SB_GIC_KIND_SGI;
	}
	if (intid < GIC_FIRST_SPI) {
		return SB_GIC_KIND_PPI;
	}
	if (intid < g->nlines) {
		return SB_GIC_KIND_SPI;
	}
	return SB_GIC_KIND_INVALID;
}

static int
spi_check(const struct sb_gic *g, uint32_t intid)
{
	switch (sb_gic_kind(g, intid)) {
	case SB_GIC_KIND_SPI:
		return SB_GIC_OK;
	case SB_GIC_KIND_INVALID:
		return SB_GIC_E_INVAL;
	default:
		return SB_GIC_E_NOTSPI;
	}
}

/* one bit per INTID: the set/clear register banks (ISENABLER, ICENABLER, ISPENDR, ICPENDR) need no read-modify-write */
static inline void
bit_write(struct sb_gic *g, uint32_t base, uint32_t intid)
{
	dwr(g, base + 4u * (intid / 32u), 1u << (intid % 32u));
}

/* one byte per INTID (IPRIORITYR, ITARGETSR): 32-bit read-modify-write; the caller serialises distributor writers */
static void
byte_write(struct sb_gic *g, uint32_t base, uint32_t intid, uint8_t v)
{
	uint32_t off = base + (intid & ~3u), sh = 8u * (intid & 3u);
	uint32_t w = drd(g, off);
	w = (w & ~(0xffu << sh)) | ((uint32_t)v << sh);
	dwr(g, off, w);
}

static inline uint8_t
byte_read(struct sb_gic *g, uint32_t base, uint32_t intid)
{
	return (uint8_t)(drd(g, base + (intid & ~3u)) >> (8u * (intid & 3u)));
}

int
sb_gic_dist_init_spis(struct sb_gic *g, uint8_t boot_mask)
{
	if (g->nlines <= GIC_FIRST_SPI || boot_mask == 0 || (boot_mask & (boot_mask - 1u)) != 0) {
		return SB_GIC_E_INVAL;
	}
	uint32_t prio4 = SB_GIC_DEFAULT_PRIO * 0x01010101u;
	uint32_t tgt4 = (uint32_t)boot_mask * 0x01010101u;
	for (uint32_t i = GIC_FIRST_SPI; i < g->nlines; i += 32u) {
		dwr(g, GICD_ICENABLER + i / 8u, 0xffffffffu);
		dwr(g, GICD_ICPENDR + i / 8u, 0xffffffffu);
	}
	for (uint32_t i = GIC_FIRST_SPI; i < g->nlines; i += 4u) {
		dwr(g, GICD_IPRIORITYR + i, prio4);
		dwr(g, GICD_ITARGETSR + i, tgt4);
	}
	for (uint32_t i = GIC_FIRST_SPI; i < g->nlines; i += 16u) {
		dwr(g, GICD_ICFGR + i / 4u, 0);        /* level-sensitive (bit 2n+1 = 0) */
	}
	dwr(g, GICD_CTLR, drd(g, GICD_CTLR) | GICD_CTLR_ENABLE);
	return SB_GIC_OK;
}

int
sb_gic_spi_enable(struct sb_gic *g, uint32_t intid)
{
	int e = spi_check(g, intid);
	if (e == SB_GIC_OK) {
		bit_write(g, GICD_ISENABLER, intid);
	}
	return e;
}

int
sb_gic_spi_disable(struct sb_gic *g, uint32_t intid)
{
	int e = spi_check(g, intid);
	if (e == SB_GIC_OK) {
		bit_write(g, GICD_ICENABLER, intid);
	}
	return e;
}

int
sb_gic_spi_set_pending(struct sb_gic *g, uint32_t intid)
{
	int e = spi_check(g, intid);
	if (e == SB_GIC_OK) {
		bit_write(g, GICD_ISPENDR, intid);
	}
	return e;
}

int
sb_gic_spi_clear_pending(struct sb_gic *g, uint32_t intid)
{
	int e = spi_check(g, intid);
	if (e == SB_GIC_OK) {
		bit_write(g, GICD_ICPENDR, intid);
	}
	return e;
}

int
sb_gic_spi_set_priority(struct sb_gic *g, uint32_t intid, uint8_t prio)
{
	int e = spi_check(g, intid);
	if (e == SB_GIC_OK) {
		byte_write(g, GICD_IPRIORITYR, intid, prio);
	}
	return e;
}

int
sb_gic_spi_set_target(struct sb_gic *g, uint32_t intid, uint8_t mask)
{
	int e = spi_check(g, intid);
	if (e != SB_GIC_OK) {
		return e;
	}
	if (mask == 0) {
		return SB_GIC_E_INVAL;          /* an SPI targeted at nobody is never delivered */
	}
	byte_write(g, GICD_ITARGETSR, intid, mask);
	return SB_GIC_OK;
}

int
sb_gic_spi_get_target(struct sb_gic *g, uint32_t intid, uint8_t *mask)
{
	int e = spi_check(g, intid);
	if (e == SB_GIC_OK) {
		*mask = byte_read(g, GICD_ITARGETSR, intid);
	}
	return e;
}

int
sb_gic_spi_set_edge(struct sb_gic *g, uint32_t intid, bool edge)
{
	int e = spi_check(g, intid);
	if (e != SB_GIC_OK) {
		return e;
	}
	uint32_t off = GICD_ICFGR + 4u * (intid / 16u), bit = 1u << (2u * (intid % 16u) + 1u);
	uint32_t w = drd(g, off);
	dwr(g, off, edge ? (w | bit) : (w & ~bit));
	return SB_GIC_OK;
}

bool
sb_gic_is_edge(struct sb_gic *g, uint32_t intid)
{
	if (sb_gic_kind(g, intid) == SB_GIC_KIND_INVALID) {
		return false;
	}
	if (intid < 16u) {
		return true;                    /* SGIs are always edge-triggered */
	}
	return (drd(g, GICD_ICFGR + 4u * (intid / 16u)) >> (2u * (intid % 16u) + 1u)) & 1u;
}

bool
sb_gic_is_enabled(struct sb_gic *g, uint32_t intid)
{
	if (sb_gic_kind(g, intid) == SB_GIC_KIND_INVALID) {
		return false;
	}
	return (drd(g, GICD_ISENABLER + 4u * (intid / 32u)) >> (intid % 32u)) & 1u;
}

int
sb_gic_record_own_mask(struct sb_gic *g, unsigned logical, uint8_t *mask_out)
{
	if (logical >= SB_GIC_MAX_CPUS) {
		return SB_GIC_E_INVAL;
	}
	uint8_t m = (uint8_t)drd(g, GICD_ITARGETSR);
	if (mask_out != NULL) {
		*mask_out = m;
	}
	if (m == 0 || (m & (m - 1u)) != 0) {
		return SB_GIC_E_BADMASK;
	}
	for (unsigned i = 0; i < SB_GIC_MAX_CPUS; i++) {
		if (i != logical && g->cpu_mask[i] == m) {
			return SB_GIC_E_DUPMASK;
		}
	}
	g->cpu_mask[logical] = m;
	return SB_GIC_OK;
}

uint32_t
sb_gic_sgir(uint32_t sgi, uint32_t filter, uint8_t target_mask)
{
	return ((filter & 3u) << 24) | ((uint32_t)target_mask << 16) | (sgi & 0xfu);
}

int
sb_gic_ipi_word(const struct sb_gic *g, unsigned target, unsigned self, uint32_t sgi, uint32_t *word)
{
	if (sgi > 15u || target >= SB_GIC_MAX_CPUS) {
		return SB_GIC_E_INVAL;
	}
	if (target == self) {
		*word = sb_gic_sgir(sgi, GICD_SGIR_FILTER_SELF, 0);
		return SB_GIC_OK;
	}
	if (g->cpu_mask[target] == 0) {
		return SB_GIC_E_NOMASK;
	}
	*word = sb_gic_sgir(sgi, GICD_SGIR_FILTER_LIST, g->cpu_mask[target]);
	return SB_GIC_OK;
}

int
sb_gic_send_ipi(struct sb_gic *g, unsigned target, unsigned self, uint32_t sgi)
{
	uint32_t w;
	int e = sb_gic_ipi_word(g, target, self, sgi, &w);
	if (e == SB_GIC_OK) {
		dwr(g, GICD_SGIR, w);
	}
	return e;
}

enum sb_gic_policy
sb_gic_register_policy(const struct sb_gic *g, uint32_t intid)
{
	switch (sb_gic_kind(g, intid)) {
	case SB_GIC_KIND_SGI:
		return SB_GIC_POL_PEXPERT;
	case SB_GIC_KIND_SPI:
		return SB_GIC_POL_VECTOR;
	default:
		return SB_GIC_POL_REFUSE;
	}
}

enum sb_gic_action
sb_gic_handle_action(const struct sb_gic *g, uint32_t intid, bool registered, bool disabled_soft)
{
	if (sb_gic_kind(g, intid) != SB_GIC_KIND_SPI || !registered) {
		return SB_GIC_ACT_STRAY;
	}
	return disabled_soft ? SB_GIC_ACT_MASK : SB_GIC_ACT_CALL;
}

void
sb_gic_cpu_init_pexpert(struct sb_gic *g, uint32_t timer_intid, uint32_t ipi_sgi)
{
	uint32_t bits = (1u << timer_intid) | (1u << ipi_sgi);
	dwr(g, GICD_IGROUPR, drd(g, GICD_IGROUPR) | bits);     /* RAZ/WI non-secure; kept as pe_fiq.c does */
	for (uint32_t i = 0; i < 32u; i += 4u) {
		dwr(g, GICD_IPRIORITYR + i, SB_GIC_DEFAULT_PRIO * 0x01010101u);
	}
	dwr(g, GICD_ISENABLER, bits);
	cwr(g, GICC_PMR, 0xff);
	cwr(g, GICC_BPR, 0);
	cwr(g, GICC_CTLR, crd(g, GICC_CTLR) | GICC_CTLR_ENABLE);
	dwr(g, GICD_CTLR, drd(g, GICD_CTLR) | GICD_CTLR_ENABLE);
}

uint32_t
sb_gic_ack(struct sb_gic *g)
{
	return crd(g, GICC_IAR);
}

void
sb_gic_eoi(struct sb_gic *g, uint32_t iar)
{
	cwr(g, GICC_EOIR, iar);
}

enum sb_gic_route
sb_gic_classify(uint32_t iar, uint32_t timer_intid)
{
	uint32_t intid = GIC_IAR_INTID(iar);
	if (intid >= GIC_MAX_LINES) {
		return SB_GIC_ROUTE_SPURIOUS;
	}
	if (intid == timer_intid) {
		return SB_GIC_ROUTE_TIMER;
	}
	if (intid < 16u) {
		return SB_GIC_ROUTE_IPI;
	}
	return SB_GIC_ROUTE_OTHER;
}
