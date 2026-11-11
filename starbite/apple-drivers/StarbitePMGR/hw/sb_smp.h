/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Exynos 9810 CPU bring-up: the per-core facts and the order of operations, plain C, no IOKit. Shared by
 * StarbitePMGR.cpp and the host tests. Sources: ../NOTES.md section 1 (SweetCPU.cpp patches 0018/0065/0304/0308,
 * osfmk/arm64/start.s, machine_routines.c ml_get_cpu_number, tools/board/starlte.board, tools/mkadt_starlte.py).
 *
 * The eight cores:
 *   logical 0..3  cluster 0 'E'  Cortex-A55     MPIDR 0x81000n00 (MT set, core in Aff1)  ADT reg n<<8     PSCI n
 *   logical 4..7  cluster 1 'P'  Exynos M3      MPIDR 0x8000010n (no MT)                ADT reg 0x10100+n PSCI 0x100+n
 * The M3's Aff1:Aff0 (1:n) collides with A55 core 1 (0x100), so XNU's reset vector and ml_get_cpu_number add
 * bit 16 for a core whose MPIDR has no MT bit: that is the "reg" XNU knows the M3s by.
 *
 * Order for one core (sb_smp_cpu_on): the descriptor is consistent -> (M3 only) the PMU CAL values are written
 * AND read back -> PSCI CPU_ON(psci target, entry, logical). Without the CAL values the firmware's CPU_ON returns
 * 0 but the M3 never runs (Linux S9+ port; SweetCPU.cpp), so a failed read-back stops before CPU_ON.
 */
#ifndef STARBITE_SMP_H
#define STARBITE_SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sb_psci.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SB_SMP_MAX_CPUS         8u

/* errors (negative) */
#define SB_SMP_OK               0
#define SB_SMP_E_INVAL          (-1)    /* no such logical CPU */
#define SB_SMP_E_STATE          (-2)    /* the boot CPU, or a CPU already started / failed */
#define SB_SMP_E_DESC           (-3)    /* ADT facts disagree (reg / mpidr / psci target / kind) */
#define SB_SMP_E_NOPMU          (-4)    /* an M3 but no PMU mapping */
#define SB_SMP_E_PMU            (-5)    /* a PMU CAL value did not read back */
#define SB_SMP_E_PSCI           (-6)    /* CPU_ON returned non-zero (see *rc) */
#define SB_SMP_E_IDENT          (-7)    /* the core that booted is not the one described (MPIDR / MIDR) */

enum sb_cpu_state {
	SB_CPU_ABSENT = 0,
	SB_CPU_OFF,                     /* described, not started */
	SB_CPU_ON_SENT,                 /* CPU_ON returned 0, the core has not checked in yet */
	SB_CPU_UP,                      /* checked in on itself (CPU_BOOTED) with the expected MPIDR/MIDR */
	SB_CPU_FAILED,
};

struct sb_cpu_desc {
	uint32_t reg;                   /* ADT "reg" = ml_topology_cpu.phys_id */
	uint64_t mpidr;                 /* ADT "starbite,mpidr": MPIDR_EL1 as the core reads it (low 32 bits) */
	uint64_t psci_target;           /* ADT "starbite,psci-target" */
	char kind;                      /* 'E' (A55) or 'P' (M3): ADT "starbite,cluster-kind" */
	bool is_boot;
};

struct sb_pmu_io {
	void *ctx;
	uint32_t (*rd)(void *ctx, uint32_t off);
	void (*wr)(void *ctx, uint32_t off, uint32_t value);
};

/* the M3 cluster's PMU CAL values (offsets in the PMU block, 0x14060000 on the S9) */
struct sb_pmu_cal {
	uint32_t offset, mask, value;
	const char *name;
};
#define SB_M3_PMU_CAL_N 6u
extern const struct sb_pmu_cal sb_m3_pmu_cal_table[SB_M3_PMU_CAL_N];

struct sb_smp {
	unsigned ncpus;                 /* logical CPUs XNU selected (cpus=N) */
	struct sb_cpu_desc cpu[SB_SMP_MAX_CPUS];
	uint8_t state[SB_SMP_MAX_CPUS];
	int32_t last_rc[SB_SMP_MAX_CPUS];
	uint64_t seen_mpidr[SB_SMP_MAX_CPUS], seen_midr[SB_SMP_MAX_CPUS];
	const struct sb_psci *psci;
	const struct sb_pmu_io *pmu;    /* NULL: the M3s cannot be started */
	unsigned pmu_cal_runs;
	uint32_t pmu_bad_offset;        /* the first offset that did not read back */
};

/* XNU's "reg" for a core from its MPIDR (start.s reset_vector / ml_get_cpu_number, STARLTE) */
uint32_t sb_smp_reg_from_mpidr(uint64_t mpidr);
/* the PSCI target for a reg: A55 n<<8 -> n, M3 0x10100+n -> 0x100+n (SweetCPU::startCPU) */
uint64_t sb_smp_psci_from_reg(uint32_t reg);
/* 'E' for a Cortex-A55 MIDR, 'P' for a Samsung (implementer 0x53) MIDR, '?' otherwise */
char sb_midr_kind(uint64_t midr);
const char *sb_midr_name(uint64_t midr);

/* every fact about one core agrees with the others */
int sb_smp_check_desc(const struct sb_cpu_desc *d);

/* adds one core; the boot core starts SB_CPU_UP, the rest SB_CPU_OFF. SB_SMP_E_DESC if inconsistent. */
int sb_smp_add(struct sb_smp *s, unsigned logical, const struct sb_cpu_desc *d);

/* writes the CAL table (read-modify-write under each mask), then reads every entry back; returns failures */
int sb_m3_pmu_cal(const struct sb_pmu_io *io, uint32_t *first_bad_offset);

/*
 * Starts one secondary: PMU CAL first for a 'P' core, then CPU_ON. *rc = the PSCI result (0 when CPU_ON was not
 * reached). On success the state is SB_CPU_ON_SENT; on any failure SB_CPU_FAILED.
 */
int sb_smp_cpu_on(struct sb_smp *s, unsigned logical, uint64_t entry_pa, int32_t *rc);

/* runs on the core itself when XNU reports it booted (CPU_BOOTED): checks MPIDR and MIDR, state -> UP */
int sb_smp_booted(struct sb_smp *s, unsigned logical, uint64_t mpidr_read, uint64_t midr_read);

/*
 * A 64-bit entry the firmware may truncate (the S7's did: patch 0223): a 16-byte trampoline in a page below
 * 4 GiB, `ldr x16, #8; br x16; .quad entry`. sb_smp_entry() picks what CPU_ON gets.
 */
void sb_smp_trampoline(uint32_t words[4], uint64_t entry_pa);
uint64_t sb_smp_entry(uint64_t entry_pa, uint64_t trampoline_pa);

/* the M2 verdict */
struct sb_smp_tally {
	unsigned selected[2], up[2];    /* [0] = cluster E, [1] = cluster P */
	bool ipi_ok, timer_ok;
	uint64_t timebase_hz;
};
bool sb_smp_tally_pass(const struct sb_smp_tally *t);
/* "cluster E: 4 up, cluster P: 4 up, IPI ok, timer 26 MHz" (bad parts say so: "IPI FAILED", "timer FAILED") */
size_t sb_smp_pass_line(char *buf, size_t n, const struct sb_smp_tally *t);

#ifdef __cplusplus
}
#endif

#endif /* STARBITE_SMP_H */
