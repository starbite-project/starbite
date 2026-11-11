/* SPDX-License-Identifier: APSL-2.0 */
/*
 * A GIC-400 model for the host tests: one distributor, N CPU interfaces, the banked SGI/PPI registers per CPU,
 * level and edge SPIs with device lines, SGIs with a source CPU, priorities, PMR, active state and EOI.
 * It delivers interrupts: mock_gic_irq_line(cpu) says whether a CPU's IRQ line is asserted, and GICC_IAR / EOIR
 * on a CPU's view behave as GICv2 4.4 describes (highest priority pending + enabled + targeted, below PMR).
 * Non-secure model: GICD_IGROUPR reads as zero and ignores writes, as on the phone.
 * Also used by ../../StarbitePMGR/test (the bring-up simulation).
 */
#ifndef MOCK_GIC_H
#define MOCK_GIC_H

#include <stdint.h>
#include <stdbool.h>
#include "../hw/sb_gicv2.h"

#define MOCK_GIC_LINES  544u            /* ITLinesNumber 16 -> 544, like an Exynos 9810 (SPI 0x1d1 exists) */
#define MOCK_GIC_CPUS   8u

struct mock_gic;

struct mock_gic_view {
	struct mock_gic *g;
	unsigned cpu;                   /* which CPU interface is accessing */
};

struct mock_gic {
	uint32_t ctlr;
	uint32_t ncpus;
	/* banked (per CPU) state for INTIDs 0..31 */
	uint32_t en_banked[MOCK_GIC_CPUS];
	uint8_t prio_banked[MOCK_GIC_CPUS][32];
	uint32_t ppi_pending[MOCK_GIC_CPUS];
	uint8_t sgi_pending[MOCK_GIC_CPUS][16];  /* bit s = pending from source CPU s */
	uint32_t active_banked[MOCK_GIC_CPUS];
	/* SPI state */
	uint8_t en[MOCK_GIC_LINES], pend[MOCK_GIC_LINES], act[MOCK_GIC_LINES], line[MOCK_GIC_LINES];
	uint8_t prio[MOCK_GIC_LINES], target[MOCK_GIC_LINES], edge[MOCK_GIC_LINES];
	uint8_t act_cpu[MOCK_GIC_LINES];
	/* CPU interfaces */
	uint32_t cctlr[MOCK_GIC_CPUS], pmr[MOCK_GIC_CPUS], bpr[MOCK_GIC_CPUS];
	uint8_t iface_of_cpu[MOCK_GIC_CPUS];    /* the interface bit wired to each CPU (what ITARGETSR0 reads) */
	/* bookkeeping for the tests */
	unsigned sgir_writes, bad_accesses, eoi_mismatch;
	uint32_t last_sgir;
	struct mock_gic_view dview[MOCK_GIC_CPUS], cview[MOCK_GIC_CPUS];
};

void mock_gic_init(struct mock_gic *m, unsigned ncpus);
/* an sb_gic whose io is CPU `cpu`'s view of the mock */
void mock_gic_bind(struct mock_gic *m, unsigned cpu, struct sb_gic *g);

uint32_t mock_gicd_rd(void *ctx, uint32_t off);
void mock_gicd_wr(void *ctx, uint32_t off, uint32_t v);
uint32_t mock_gicc_rd(void *ctx, uint32_t off);
void mock_gicc_wr(void *ctx, uint32_t off, uint32_t v);

/* device side */
void mock_gic_set_line(struct mock_gic *m, uint32_t intid, bool high);  /* SPI line (level: follows; edge: rising) */
void mock_gic_ppi_raise(struct mock_gic *m, unsigned cpu, uint32_t intid);  /* e.g. the timer PPI 27 */
bool mock_gic_irq_line(struct mock_gic *m, unsigned cpu);               /* would this CPU take an IRQ now? */

#endif
