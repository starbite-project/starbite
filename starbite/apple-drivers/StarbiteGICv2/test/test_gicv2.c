/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host tests of hw/sb_gicv2.c against the GIC-400 model (mock_gic.c). sim_irq() is sleh_irq's A5XELTE branch
 * (osfmk/arm64/sleh.c) in C: ack, classify, timer / IPI / other, EOI. "other" goes to drv_handle(), which is
 * StarbiteGICv2::handleInterrupt's logic (the vector table + sb_gic_handle_action).
 */
#include <stdio.h>
#include <string.h>
#include "mock_gic.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct mock_gic M;
static struct sb_gic G[MOCK_GIC_CPUS];         /* G[c] = CPU c's view; masks are shared through G[0] below */

/* the driver's vector table (IOInterruptVector reduced to what handleInterrupt reads) */
struct vec {
	bool registered, soft_disabled;
	unsigned calls;
	unsigned last_cpu;
	bool clears_line;               /* the client's handler quiets its device (a level SPI drops) */
};
static struct vec V[MOCK_GIC_LINES];
static unsigned stray;

/* per-CPU outcome counters of the simulated sleh_irq */
static unsigned timer_hits[MOCK_GIC_CPUS], ipi_hits[MOCK_GIC_CPUS], ipi_src[MOCK_GIC_CPUS], other_hits[MOCK_GIC_CPUS];
static unsigned spurious_hits[MOCK_GIC_CPUS];

static void
drv_handle(unsigned cpu, uint32_t intid)
{
	struct sb_gic *g = &G[cpu];
	switch (sb_gic_handle_action(g, intid, V[intid].registered, V[intid].soft_disabled)) {
	case SB_GIC_ACT_CALL:
		V[intid].calls++;
		V[intid].last_cpu = cpu;
		if (V[intid].clears_line) {
			mock_gic_set_line(&M, intid, false);
		}
		break;
	case SB_GIC_ACT_MASK:
		sb_gic_spi_disable(g, intid);
		break;
	case SB_GIC_ACT_STRAY:
		stray++;
		if (sb_gic_kind(g, intid) == SB_GIC_KIND_SPI) {
			sb_gic_spi_disable(g, intid);
		}
		break;
	}
}

/* sleh_irq, A5XELTE branch */
static void
sim_irq(unsigned cpu)
{
	struct sb_gic *g = &G[cpu];
	uint32_t iar = sb_gic_ack(g);
	switch (sb_gic_classify(iar, SB_GIC_TIMER_INTID)) {
	case SB_GIC_ROUTE_SPURIOUS:
		spurious_hits[cpu]++;
		return;
	case SB_GIC_ROUTE_TIMER:
		timer_hits[cpu]++;
		sb_gic_eoi(g, iar);
		return;
	case SB_GIC_ROUTE_IPI:
		sb_gic_eoi(g, iar);     /* EOI first: an IPI raised meanwhile is not lost */
		ipi_hits[cpu]++;
		ipi_src[cpu] = GIC_IAR_SRC(iar);
		return;
	case SB_GIC_ROUTE_OTHER:
		other_hits[cpu]++;
		drv_handle(cpu, GIC_IAR_INTID(iar));
		sb_gic_eoi(g, iar);
		return;
	}
}

/* run every CPU until no IRQ line is up (bounded, so a storm fails the test instead of hanging it) */
static unsigned
run_all(unsigned limit)
{
	unsigned n = 0;
	for (bool any = true; any && n < limit;) {
		any = false;
		for (unsigned c = 0; c < M.ncpus; c++) {
			if (mock_gic_irq_line(&M, c)) {
				sim_irq(c);
				n++;
				any = true;
			}
		}
	}
	return n;
}

static void
reset_counts(void)
{
	memset(timer_hits, 0, sizeof(timer_hits));
	memset(ipi_hits, 0, sizeof(ipi_hits));
	memset(other_hits, 0, sizeof(other_hits));
	memset(spurious_hits, 0, sizeof(spurious_hits));
	stray = 0;
}

static unsigned
iface_num(uint8_t bit)
{
	unsigned n = 0;
	while (n < 7 && !(bit & (1u << n))) {
		n++;
	}
	return n;
}

static uint32_t rd_ones(void *c, uint32_t o) { (void)c; (void)o; return 0xffffffffu; }
static uint32_t rd_zero(void *c, uint32_t o) { (void)c; (void)o; return 0; }

int
main(void)
{
	mock_gic_init(&M, 8);
	for (unsigned c = 0; c < 8; c++) {
		memset(&G[c], 0, sizeof(G[c]));
		mock_gic_bind(&M, c, &G[c]);
	}

	/* probe */
	CHECK(sb_gic_probe(&G[0]) == SB_GIC_OK);
	CHECK(G[0].nlines == MOCK_GIC_LINES);
	CHECK(G[0].ncpu_ifaces == 8);
	CHECK(G[0].typer & GICD_TYPER_SECEXT);
	CHECK(sb_gic_kind(&G[0], 1) == SB_GIC_KIND_SGI);
	CHECK(sb_gic_kind(&G[0], 27) == SB_GIC_KIND_PPI);
	CHECK(sb_gic_kind(&G[0], 32) == SB_GIC_KIND_SPI);
	CHECK(sb_gic_kind(&G[0], 0x1d1 + 32) == SB_GIC_KIND_SPI);  /* wdt-cl1, the highest SPI the board names */
	CHECK(sb_gic_kind(&G[0], MOCK_GIC_LINES) == SB_GIC_KIND_INVALID);
	for (unsigned c = 1; c < 8; c++) {
		G[c].typer = G[0].typer;
		G[c].nlines = G[0].nlines;
		G[c].ncpu_ifaces = G[0].ncpu_ifaces;
	}
	/* a GIC that is not there (unmapped / powered off reads as all ones or zero) */
	{
		struct sb_gic z;
		memset(&z, 0, sizeof(z));
		z.d.rd = rd_ones;
		CHECK(sb_gic_probe(&z) == SB_GIC_E_HW);
		z.d.rd = rd_zero;
		CHECK(sb_gic_probe(&z) == SB_GIC_E_HW);
	}

	/* before any CPU init nothing is delivered */
	CHECK(!mock_gic_irq_line(&M, 0));

	/* pexpert's per-CPU init on the boot CPU, then the driver reads its interface bit */
	sb_gic_cpu_init_pexpert(&G[0], SB_GIC_TIMER_INTID, SB_GIC_IPI_SGI);
	CHECK(M.cctlr[0] == 1 && M.pmr[0] == 0xff && M.bpr[0] == 0 && (M.ctlr & 1));
	CHECK(M.en_banked[0] == ((1u << 27) | (1u << 1)));
	CHECK(M.en_banked[1] == 0);                     /* banked: CPU 1 untouched */
	CHECK(M.prio_banked[0][27] == 0xa0);
	CHECK(sb_gic_is_enabled(&G[0], 27) && !sb_gic_is_enabled(&G[1], 27));
	uint8_t mask = 0;
	CHECK(sb_gic_record_own_mask(&G[0], 0, &mask) == SB_GIC_OK);
	CHECK(mask == M.iface_of_cpu[0] && G[0].cpu_mask[0] == mask);
	CHECK(sb_gic_record_own_mask(&G[0], 0, NULL) == SB_GIC_OK);   /* idempotent */

	/* the distributor: an SPI firmware left enabled and pending is quiet afterwards */
	M.en[100] = 1;
	M.pend[100] = 1;
	M.target[100] = 0xff;
	M.edge[100] = 1;
	CHECK(sb_gic_dist_init_spis(&G[0], 0) == SB_GIC_E_INVAL);
	CHECK(sb_gic_dist_init_spis(&G[0], 0x3) == SB_GIC_E_INVAL);
	CHECK(sb_gic_dist_init_spis(&G[0], G[0].cpu_mask[0]) == SB_GIC_OK);
	{
		bool all = true;
		for (uint32_t i = 32; i < MOCK_GIC_LINES; i++) {
			all &= !M.en[i] && !M.pend[i] && M.prio[i] == 0xa0 && M.target[i] == mask && !M.edge[i];
		}
		CHECK(all);
	}
	CHECK(M.en_banked[0] == ((1u << 27) | (1u << 1)));     /* SGI/PPI enables are pexpert's: untouched */
	CHECK(M.bad_accesses == 0);

	/* secondaries come up one by one: pexpert init, then the CPU_BOOTED callback records the mask */
	for (unsigned c = 1; c < 8; c++) {
		sb_gic_cpu_init_pexpert(&G[c], SB_GIC_TIMER_INTID, SB_GIC_IPI_SGI);
		memcpy(G[c].cpu_mask, G[0].cpu_mask, sizeof(G[0].cpu_mask));
		CHECK(sb_gic_record_own_mask(&G[c], c, NULL) == SB_GIC_OK);
		memcpy(G[0].cpu_mask, G[c].cpu_mask, sizeof(G[0].cpu_mask));
	}
	for (unsigned c = 0; c < 8; c++) {
		CHECK(G[0].cpu_mask[c] == M.iface_of_cpu[c]);
	}
	/* a second CPU claiming CPU 3's interface, and a broken read, are refused */
	{
		struct sb_gic t = G[3];
		CHECK(sb_gic_record_own_mask(&t, 5, NULL) == SB_GIC_E_DUPMASK);
		uint8_t saved = M.iface_of_cpu[3];
		M.iface_of_cpu[3] = 0;
		CHECK(sb_gic_record_own_mask(&t, 3, NULL) == SB_GIC_E_BADMASK);
		M.iface_of_cpu[3] = 0x30;
		CHECK(sb_gic_record_own_mask(&t, 3, NULL) == SB_GIC_E_BADMASK);
		M.iface_of_cpu[3] = saved;
		CHECK(sb_gic_record_own_mask(&t, 8, NULL) == SB_GIC_E_INVAL);
	}

	/* the SGIR encoding */
	CHECK(sb_gic_sgir(1, GICD_SGIR_FILTER_SELF, 0) == 0x02000001u);
	CHECK(sb_gic_sgir(1, GICD_SGIR_FILTER_LIST, 0x10) == 0x00100001u);
	CHECK(sb_gic_sgir(15, GICD_SGIR_FILTER_OTHERS, 0) == 0x0100000fu);
	CHECK(sb_gic_sgir(0x11, GICD_SGIR_FILTER_LIST, 0x80) == 0x00800001u);
	{
		uint32_t w = 0;
		struct sb_gic fresh;
		memset(&fresh, 0, sizeof(fresh));
		CHECK(sb_gic_ipi_word(&fresh, 4, 4, 1, &w) == SB_GIC_OK && w == 0x02000001u);  /* self needs no mask */
		CHECK(sb_gic_ipi_word(&fresh, 4, 0, 1, &w) == SB_GIC_E_NOMASK);
		CHECK(sb_gic_ipi_word(&G[0], 4, 0, 1, &w) == SB_GIC_OK && w == ((uint32_t)M.iface_of_cpu[4] << 16 | 1u));
		CHECK(sb_gic_ipi_word(&G[0], 4, 0, 16, &w) == SB_GIC_E_INVAL);
		CHECK(sb_gic_ipi_word(&G[0], 9, 0, 1, &w) == SB_GIC_E_INVAL);
	}

	/* self-IPIs: each CPU clears its own SIGPdisabled this way */
	reset_counts();
	for (unsigned c = 0; c < 8; c++) {
		CHECK(sb_gic_send_ipi(&G[c], c, c, SB_GIC_IPI_SGI) == SB_GIC_OK);
	}
	CHECK(run_all(100) == 8);
	for (unsigned c = 0; c < 8; c++) {
		CHECK(ipi_hits[c] == 1 && ipi_src[c] == iface_num(M.iface_of_cpu[c]));
	}

	/* every cross pair, one at a time: only the target takes it, and its IAR names the sender's interface */
	{
		bool ok = true;
		for (unsigned s = 0; s < 8; s++) {
			for (unsigned t = 0; t < 8; t++) {
				if (s == t) {
					continue;
				}
				reset_counts();
				memcpy(G[s].cpu_mask, G[0].cpu_mask, sizeof(G[0].cpu_mask));
				ok &= sb_gic_send_ipi(&G[s], t, s, SB_GIC_IPI_SGI) == SB_GIC_OK;
				ok &= run_all(10) == 1;
				for (unsigned c = 0; c < 8; c++) {
					ok &= ipi_hits[c] == (c == t ? 1u : 0u);
				}
				ok &= ipi_src[t] == iface_num(M.iface_of_cpu[s]);
			}
		}
		CHECK(ok);
	}
	/* two IPIs from two senders to one CPU before it runs: both arrive (one pending bit per source) */
	reset_counts();
	sb_gic_send_ipi(&G[1], 6, 1, 1);
	sb_gic_send_ipi(&G[2], 6, 2, 1);
	CHECK(run_all(10) == 2 && ipi_hits[6] == 2);

	/* the timer PPI is per CPU */
	reset_counts();
	mock_gic_ppi_raise(&M, 2, 27);
	CHECK(run_all(10) == 1 && timer_hits[2] == 1 && timer_hits[0] == 0);
	/* a PPI pexpert did not enable stays in the GIC */
	mock_gic_ppi_raise(&M, 2, 30);
	CHECK(!mock_gic_irq_line(&M, 2));
	M.ppi_pending[2] = 0;

	/* SPIs: a level SPI whose handler quiets the device: one delivery, on the boot CPU */
	reset_counts();
	uint32_t uart = 0x1c0;
	V[uart].registered = true;
	V[uart].clears_line = true;
	CHECK(sb_gic_spi_enable(&G[0], uart) == SB_GIC_OK);
	mock_gic_set_line(&M, uart, true);
	CHECK(run_all(10) == 1 && V[uart].calls == 1 && V[uart].last_cpu == 0 && other_hits[0] == 1);
	/* routed to CPU 5 */
	CHECK(sb_gic_spi_set_target(&G[0], uart, G[0].cpu_mask[5]) == SB_GIC_OK);
	{
		uint8_t t = 0;
		CHECK(sb_gic_spi_get_target(&G[0], uart, &t) == SB_GIC_OK && t == M.iface_of_cpu[5]);
	}
	mock_gic_set_line(&M, uart, true);
	CHECK(run_all(10) == 1 && V[uart].calls == 2 && V[uart].last_cpu == 5);
	/* neighbours of a byte-wide write are untouched */
	CHECK(M.target[uart - 1] == mask && M.target[uart + 1] == mask && M.target[uart + 2] == mask);
	CHECK(sb_gic_spi_set_target(&G[0], uart, 0) == SB_GIC_E_INVAL);
	CHECK(sb_gic_spi_set_priority(&G[0], uart + 1, 0x40) == SB_GIC_OK);
	CHECK(M.prio[uart + 1] == 0x40 && M.prio[uart] == 0xa0 && M.prio[uart + 2] == 0xa0 && M.prio[uart + 3] == 0xa0);
	CHECK(sb_gic_spi_set_priority(&G[0], uart + 1, 0xa0) == SB_GIC_OK);

	/* a soft-disabled level SPI that keeps firing: masked in the distributor at the first hit (no storm) */
	reset_counts();
	uint32_t lvl = 0x40 + 32;
	V[lvl].registered = true;
	V[lvl].soft_disabled = true;
	sb_gic_spi_enable(&G[0], lvl);
	mock_gic_set_line(&M, lvl, true);
	CHECK(run_all(50) == 1 && V[lvl].calls == 0 && !sb_gic_is_enabled(&G[0], lvl));
	/* soft-enabled again (enableInterrupt -> enableVector): the client sees it */
	V[lvl].soft_disabled = false;
	V[lvl].clears_line = true;
	sb_gic_spi_enable(&G[0], lvl);
	CHECK(run_all(50) == 1 && V[lvl].calls == 1);

	/* an SPI nobody registered (firmware left it enabled?): disabled at the first hit, counted */
	reset_counts();
	uint32_t orphan = 0x99 + 32;
	sb_gic_spi_enable(&G[0], orphan);
	mock_gic_set_line(&M, orphan, true);
	CHECK(run_all(50) == 1 && stray == 1 && !sb_gic_is_enabled(&G[0], orphan));
	mock_gic_set_line(&M, orphan, false);

	/* edge SPI: one pulse = one delivery; ICFGR bit 2n+1 */
	reset_counts();
	uint32_t ed = 0x1d1 + 32;
	V[ed].registered = true;
	CHECK(!sb_gic_is_edge(&G[0], ed));
	CHECK(sb_gic_spi_set_edge(&G[0], ed, true) == SB_GIC_OK && sb_gic_is_edge(&G[0], ed) && M.edge[ed]);
	CHECK(!M.edge[ed - 1] && !M.edge[ed + 1]);
	sb_gic_spi_enable(&G[0], ed);
	mock_gic_set_line(&M, ed, true);
	CHECK(run_all(10) == 1 && V[ed].calls == 1);
	mock_gic_set_line(&M, ed, false);
	CHECK(run_all(10) == 0);
	CHECK(sb_gic_is_edge(&G[0], 1) && !sb_gic_is_edge(&G[0], 27));
	CHECK(sb_gic_spi_set_edge(&G[0], ed, false) == SB_GIC_OK && !M.edge[ed]);

	/* causeInterrupt: a software-pending SPI is delivered once */
	reset_counts();
	uint32_t sw = 0x50 + 32;
	V[sw].registered = true;
	sb_gic_spi_enable(&G[0], sw);
	CHECK(sb_gic_spi_set_pending(&G[0], sw) == SB_GIC_OK);
	CHECK(run_all(10) == 1 && V[sw].calls == 1);
	/* pending but disabled: not delivered; clear pending works */
	sb_gic_spi_disable(&G[0], sw);
	sb_gic_spi_set_pending(&G[0], sw);
	CHECK(run_all(10) == 0);
	CHECK(sb_gic_spi_clear_pending(&G[0], sw) == SB_GIC_OK && !M.pend[sw]);

	/* priority mask: PMR 0 blocks everything on that CPU, IAR reads spurious */
	reset_counts();
	M.pmr[0] = 0;
	sb_gic_send_ipi(&G[0], 0, 0, 1);
	CHECK(!mock_gic_irq_line(&M, 0));
	CHECK(sb_gic_ack(&G[0]) == GIC_INTID_SPURIOUS);
	M.pmr[0] = 0xff;
	CHECK(run_all(10) == 1 && ipi_hits[0] == 1);

	/* SPI ops refuse SGIs/PPIs and INTIDs past the last line */
	CHECK(sb_gic_spi_enable(&G[0], 1) == SB_GIC_E_NOTSPI);
	CHECK(sb_gic_spi_disable(&G[0], 27) == SB_GIC_E_NOTSPI);
	CHECK(sb_gic_spi_enable(&G[0], MOCK_GIC_LINES) == SB_GIC_E_INVAL);
	CHECK(sb_gic_spi_set_target(&G[0], 27, 1) == SB_GIC_E_NOTSPI);
	CHECK(sb_gic_spi_set_edge(&G[0], 2000, true) == SB_GIC_E_INVAL);

	/* the classifier = sleh_irq's decisions */
	CHECK(sb_gic_classify(1023, 27) == SB_GIC_ROUTE_SPURIOUS);
	CHECK(sb_gic_classify(1020, 27) == SB_GIC_ROUTE_SPURIOUS);
	CHECK(sb_gic_classify(27, 27) == SB_GIC_ROUTE_TIMER);
	CHECK(sb_gic_classify(0x401, 27) == SB_GIC_ROUTE_IPI);         /* CPU 1's SGI 1 */
	CHECK(sb_gic_classify(0x1c00 | 0x1f1, 27) == SB_GIC_ROUTE_OTHER);
	CHECK(sb_gic_classify(30, 27) == SB_GIC_ROUTE_OTHER);

	/* registration policy */
	CHECK(sb_gic_register_policy(&G[0], 1) == SB_GIC_POL_PEXPERT);
	CHECK(sb_gic_register_policy(&G[0], 27) == SB_GIC_POL_REFUSE);
	CHECK(sb_gic_register_policy(&G[0], 23) == SB_GIC_POL_REFUSE);
	CHECK(sb_gic_register_policy(&G[0], 0xb0) == SB_GIC_POL_VECTOR);           /* A55 core 0 PMU */
	CHECK(sb_gic_register_policy(&G[0], MOCK_GIC_LINES) == SB_GIC_POL_REFUSE);
	CHECK(sb_gic_handle_action(&G[0], 0x60, false, false) == SB_GIC_ACT_STRAY);
	CHECK(sb_gic_handle_action(&G[0], 30, true, false) == SB_GIC_ACT_STRAY);
	CHECK(sb_gic_handle_action(&G[0], 0x60, true, true) == SB_GIC_ACT_MASK);
	CHECK(sb_gic_handle_action(&G[0], 0x60, true, false) == SB_GIC_ACT_CALL);

	CHECK(M.eoi_mismatch == 0);
	CHECK(M.bad_accesses == 0);
	printf("StarbiteGICv2 hw: %d/%d checks passed\n", checks - fails, checks);
	return fails != 0;
}
