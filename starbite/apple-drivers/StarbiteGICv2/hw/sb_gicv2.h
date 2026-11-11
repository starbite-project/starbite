/* SPDX-License-Identifier: APSL-2.0 */
/*
 * GIC-400 (ARM GICv2) register logic for the Exynos 9810, plain C, no IOKit. Shared by StarbiteGICv2.cpp and the
 * host tests (test/). Register facts and sources: ../NOTES.md (ARM IHI 0048B, GICv2 architecture specification;
 * pexpert/arm/pe_fiq.c A5XELTE branch, patches 0086/0114).
 *
 * Every MMIO access goes through struct sb_gic_io (32-bit only), so the same code runs against the mock GIC on the
 * Mac (test/mock_gic.c). The distributor's per-CPU registers (SGI/PPI enables, priorities, ITARGETSR0-7) are
 * BANKED: what a read returns depends on the CPU that reads, so the io of a struct sb_gic is the calling CPU's
 * view. In the kernel every CPU sees its own bank through the same mapping; the mock gives each CPU its own io.
 *
 * Ownership split with pexpert (the "who touches what" contract, ../NOTES.md section 2):
 *   pexpert (pe_fiq.c, sleh.c), before and outside IOKit: per-CPU CPU-interface set-up (GICC_CTLR/PMR/BPR),
 *     the banked SGI 1 + PPI 27 enables and priorities, GICC_IAR / GICC_EOIR on every IRQ, the timer (PPI 27)
 *     and SGI dispatch. sb_gic_cpu_init_pexpert() and sb_gic_classify() mirror that code for the tests.
 *   StarbiteGICv2: everything about SPIs (32..nlines-1): enable/disable, pending, priority, target, trigger;
 *     the SGIR writes for IPIs; reading each CPU's interface mask (banked ITARGETSR0).
 */
#ifndef STARBITE_GICV2_H
#define STARBITE_GICV2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* distributor (GICD, 0x10101000 on the S9) */
#define GICD_CTLR               0x000u
#define GICD_TYPER              0x004u
#define GICD_IIDR               0x008u
#define GICD_IGROUPR            0x080u  /* secure only: RAZ/WI from the non-secure side (all we are) */
#define GICD_ISENABLER          0x100u
#define GICD_ICENABLER          0x180u
#define GICD_ISPENDR            0x200u
#define GICD_ICPENDR            0x280u
#define GICD_ISACTIVER          0x300u
#define GICD_ICACTIVER          0x380u
#define GICD_IPRIORITYR         0x400u
#define GICD_ITARGETSR          0x800u
#define GICD_ICFGR              0xc00u
#define GICD_SGIR               0xf00u

#define GICD_CTLR_ENABLE        0x1u    /* non-secure view: EnableGrp1 */
#define GICD_TYPER_ITLINES(t)   ((t) & 0x1fu)
#define GICD_TYPER_CPUS(t)      ((((t) >> 5) & 0x7u) + 1u)
#define GICD_TYPER_SECEXT       (1u << 10)

/* GICD_SGIR fields */
#define GICD_SGIR_FILTER_LIST   0u      /* the CPUTargetList */
#define GICD_SGIR_FILTER_OTHERS 1u      /* every CPU but the requester */
#define GICD_SGIR_FILTER_SELF   2u      /* the requester only */

/* CPU interface (GICC, 0x10102000 on the S9) */
#define GICC_CTLR               0x00u
#define GICC_PMR                0x04u
#define GICC_BPR                0x08u
#define GICC_IAR                0x0cu
#define GICC_EOIR               0x10u
#define GICC_RPR                0x14u
#define GICC_HPPIR              0x18u
#define GICC_IIDR               0xfcu
#define GICC_CTLR_ENABLE        0x1u    /* non-secure view: EnableGrp1 */

#define GIC_IAR_INTID(iar)      ((iar) & 0x3ffu)        /* [12:10] = the SGI's source CPU */
#define GIC_IAR_SRC(iar)        (((iar) >> 10) & 0x7u)
#define GIC_INTID_SPURIOUS      1023u
#define GIC_MAX_LINES           1020u   /* INTIDs 1020..1023 are special */
#define GIC_FIRST_SPI           32u
#define GIC_MAX_CPU_IFACES      8u

/* what this platform uses (the same numbers pexpert hard-codes: A5XELTE_VTIMER_INTID, SGI 1) */
#define SB_GIC_TIMER_INTID      27u     /* virtual timer PPI 11 (dts timer { <1 0x0b ...> }) */
#define SB_GIC_IPI_SGI          1u      /* every XNU cross-CPU signal */
#define SB_GIC_DEFAULT_PRIO     0xa0u   /* pexpert's priority for everything */

#define SB_GIC_MAX_CPUS         8u      /* logical CPUs (MAX_CPUS of STARLTE) */

/* errors (negative) */
#define SB_GIC_OK               0
#define SB_GIC_E_INVAL          (-1)    /* bad INTID / argument */
#define SB_GIC_E_NOTSPI         (-2)    /* an SGI/PPI where only an SPI is allowed */
#define SB_GIC_E_NOMASK         (-3)    /* the target CPU's interface mask is not known yet */
#define SB_GIC_E_BADMASK        (-4)    /* ITARGETSR0 did not read as exactly one bit */
#define SB_GIC_E_DUPMASK        (-5)    /* two logical CPUs claim the same interface */
#define SB_GIC_E_HW             (-6)    /* the hardware does not look like a GIC-400 we can drive */

struct sb_gic_io {
	void *ctx;
	uint32_t (*rd)(void *ctx, uint32_t off);
	void (*wr)(void *ctx, uint32_t off, uint32_t value);
};

struct sb_gic {
	struct sb_gic_io d;             /* distributor, as the calling CPU sees it */
	struct sb_gic_io c;             /* CPU interface, as the calling CPU sees it */
	uint32_t typer, iidr;
	uint32_t nlines;                /* INTIDs 0..nlines-1 exist */
	uint32_t ncpu_ifaces;
	uint8_t cpu_mask[SB_GIC_MAX_CPUS];      /* logical CPU -> its interface bit (0 = not seen yet) */
};

enum sb_gic_kind {
	SB_GIC_KIND_SGI = 0,            /* 0..15 */
	SB_GIC_KIND_PPI,                /* 16..31 */
	SB_GIC_KIND_SPI,                /* 32..nlines-1 */
	SB_GIC_KIND_INVALID,
};

/* how sleh_irq treats an acknowledged interrupt (osfmk/arm64/sleh.c, A5XELTE branch) */
enum sb_gic_route {
	SB_GIC_ROUTE_SPURIOUS = 0,      /* 1020..1023: no EOI */
	SB_GIC_ROUTE_TIMER,             /* PPI 27 -> rtclock_intr, then EOI */
	SB_GIC_ROUTE_IPI,               /* SGI 0..15 -> EOI first, then cpu_signal_handler */
	SB_GIC_ROUTE_OTHER,             /* -> cpu_data interrupt_handler (StarbiteGICv2::handleInterrupt), then EOI */
};

/* probe: reads TYPER/IIDR, fills nlines / ncpu_ifaces. SB_GIC_E_HW if TYPER is 0 or all-ones. */
int sb_gic_probe(struct sb_gic *g);
enum sb_gic_kind sb_gic_kind(const struct sb_gic *g, uint32_t intid);

/*
 * Distributor SPI set-up (once, by the driver, on the boot CPU): every SPI disabled and not pending, priority
 * SB_GIC_DEFAULT_PRIO, target = boot_mask, level-sensitive; then GICD_CTLR.Enable set (it already is: pexpert).
 * Non-secure writes to Group 0 (TrustZone) interrupts are ignored by the hardware, so this cannot take them.
 */
int sb_gic_dist_init_spis(struct sb_gic *g, uint8_t boot_mask);

int sb_gic_spi_enable(struct sb_gic *g, uint32_t intid);
int sb_gic_spi_disable(struct sb_gic *g, uint32_t intid);
int sb_gic_spi_set_pending(struct sb_gic *g, uint32_t intid);  /* causeInterrupt */
int sb_gic_spi_clear_pending(struct sb_gic *g, uint32_t intid);
int sb_gic_spi_set_priority(struct sb_gic *g, uint32_t intid, uint8_t prio);
int sb_gic_spi_set_target(struct sb_gic *g, uint32_t intid, uint8_t mask);
int sb_gic_spi_get_target(struct sb_gic *g, uint32_t intid, uint8_t *mask);
int sb_gic_spi_set_edge(struct sb_gic *g, uint32_t intid, bool edge);
bool sb_gic_is_edge(struct sb_gic *g, uint32_t intid);
bool sb_gic_is_enabled(struct sb_gic *g, uint32_t intid);       /* any INTID, banked view for < 32 */

/*
 * The calling CPU's interface bit: GICD_ITARGETSR0 byte 0 is banked and read-only, it reads as the reader's own
 * interface (GICv2 4.3.12). Records it for `logical`. SB_GIC_E_BADMASK unless exactly one bit;
 * SB_GIC_E_DUPMASK if another logical CPU already has it.
 */
int sb_gic_record_own_mask(struct sb_gic *g, unsigned logical, uint8_t *mask_out);

/* GICD_SGIR word: filter [25:24], CPUTargetList [23:16], SGI [3:0]. NSATT [15] is 0 (only secure writes use it). */
uint32_t sb_gic_sgir(uint32_t sgi, uint32_t filter, uint8_t target_mask);

/*
 * The SGIR word that signals logical CPU `target` from logical CPU `self`: the requester itself by the SELF filter
 * (needs no mask: this is how a booting CPU clears its own SIGPdisabled), any other CPU by its recorded mask.
 */
int sb_gic_ipi_word(const struct sb_gic *g, unsigned target, unsigned self, uint32_t sgi, uint32_t *word);
/* writes it (the caller has done a DSB ISHST so the target sees the signal bits first) */
int sb_gic_send_ipi(struct sb_gic *g, unsigned target, unsigned self, uint32_t sgi);

/*
 * Driver policy, kept here so the host tests check it (StarbiteGICv2::registerInterrupt / handleInterrupt):
 *   SGI  (CPU nubs' IPI entries): accepted as bookkeeping; sleh_irq dispatches SGIs to cpu_signal_handler itself.
 *   PPI: refused; the only PPI in use (27, the timer) belongs to pexpert, and no ADT node names another.
 *   SPI: a normal IOInterruptController vector.
 */
enum sb_gic_policy {
	SB_GIC_POL_REFUSE = 0,
	SB_GIC_POL_PEXPERT,
	SB_GIC_POL_VECTOR,
};
enum sb_gic_policy sb_gic_register_policy(const struct sb_gic *g, uint32_t intid);

/* what handleInterrupt does with an INTID sleh_irq handed it */
enum sb_gic_action {
	SB_GIC_ACT_CALL = 0,            /* registered and soft-enabled: call the client's handler */
	SB_GIC_ACT_MASK,                /* registered but soft-disabled: disable it in the distributor (lazy disable) */
	SB_GIC_ACT_STRAY,               /* nobody registered it, or not an SPI: disable it if it is an SPI, count it */
};
enum sb_gic_action sb_gic_handle_action(const struct sb_gic *g, uint32_t intid, bool registered, bool disabled_soft);

/* --- pexpert's half, mirrored for the host tests (the kernel runs pe_fiq.c, not this) --- */
/* pe_init_fiq(), A5XELTE branch, on the calling CPU */
void sb_gic_cpu_init_pexpert(struct sb_gic *g, uint32_t timer_intid, uint32_t ipi_sgi);
uint32_t sb_gic_ack(struct sb_gic *g);                          /* GICC_IAR */
void sb_gic_eoi(struct sb_gic *g, uint32_t iar);                /* GICC_EOIR */
enum sb_gic_route sb_gic_classify(uint32_t iar, uint32_t timer_intid);

#ifdef __cplusplus
}
#endif

#endif /* STARBITE_GICV2_H */
