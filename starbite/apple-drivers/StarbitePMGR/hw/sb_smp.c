/* SPDX-License-Identifier: APSL-2.0 */
/* Exynos 9810 CPU bring-up sequencing (see sb_smp.h). */
#include "sb_smp.h"

/*
 * The Samsung 4.9 kernel's pmucal_lpm_init entries for CPUCL1, applied before every M3 CPU_ON by the Linux S9+
 * port and by SweetCPU.cpp (patch 0304); measured to be what this S9 needs. Register names from the vendor CAL.
 */
const struct sb_pmu_cal sb_m3_pmu_cal_table[SB_M3_PMU_CAL_N] = {
	{ 0x1088, 0xff000000u, 0x8d000000u, "CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG[31:24]" },
	{ 0x2114, 0x3u, 0x2u, "MEMORY_CLUSTER1_CPU0_OPTION[1:0]" },
	{ 0x2180, 0x3u, 0x2u, "MEMORY_CLUSTER1_CPU1_OPTION[1:0]" },
	{ 0x21ec, 0x3u, 0x2u, "MEMORY_CLUSTER1_CPU2_OPTION[1:0]" },
	{ 0x2258, 0x3u, 0x2u, "MEMORY_CLUSTER1_CPU3_OPTION[1:0]" },
	{ 0x2384, 0x3u, 0x2u, "MEMORY_CLUSTER1_NONCPU_OPTION[1:0]" },
};

uint32_t
sb_smp_reg_from_mpidr(uint64_t mpidr)
{
	uint32_t r = (uint32_t)(mpidr & 0xffffu);       /* Aff1:Aff0 */
	if (!(mpidr & (1u << 24))) {
		r |= 0x10000u;                          /* no MT bit: the M3s (start.s, STARLTE) */
	}
	return r;
}

uint64_t
sb_smp_psci_from_reg(uint32_t reg)
{
	if (reg & 0x10000u) {
		return reg & 0xffffu;                   /* M3: 0x100 + n */
	}
	return reg >> 8;                                /* A55: Aff1 = n */
}

char
sb_midr_kind(uint64_t midr)
{
	uint32_t impl = (uint32_t)(midr >> 24) & 0xffu, part = (uint32_t)(midr >> 4) & 0xfffu;
	if (impl == 0x41u && part == 0xd05u) {
		return 'E';
	}
	if (impl == 0x53u) {
		return 'P';
	}
	return '?';
}

const char *
sb_midr_name(uint64_t midr)
{
	switch (sb_midr_kind(midr)) {
	case 'E': return "Cortex-A55";
	case 'P': return "Exynos M3";
	default: return "unknown core";
	}
}

int
sb_smp_check_desc(const struct sb_cpu_desc *d)
{
	if (d->kind != 'E' && d->kind != 'P') {
		return SB_SMP_E_DESC;
	}
	if (sb_smp_reg_from_mpidr(d->mpidr) != d->reg) {
		return SB_SMP_E_DESC;
	}
	if (sb_smp_psci_from_reg(d->reg) != d->psci_target) {
		return SB_SMP_E_DESC;
	}
	/* the M3s, and only they, have the bit-16 reg */
	if ((d->kind == 'P') != ((d->reg & 0x10000u) != 0)) {
		return SB_SMP_E_DESC;
	}
	return SB_SMP_OK;
}

int
sb_smp_add(struct sb_smp *s, unsigned logical, const struct sb_cpu_desc *d)
{
	if (logical >= SB_SMP_MAX_CPUS) {
		return SB_SMP_E_INVAL;
	}
	int e = sb_smp_check_desc(d);
	s->cpu[logical] = *d;
	s->state[logical] = e != SB_SMP_OK ? SB_CPU_FAILED : d->is_boot ? SB_CPU_UP : SB_CPU_OFF;
	if (logical >= s->ncpus) {
		s->ncpus = logical + 1;
	}
	return e;
}

int
sb_m3_pmu_cal(const struct sb_pmu_io *io, uint32_t *first_bad_offset)
{
	int bad = 0;
	for (unsigned i = 0; i < SB_M3_PMU_CAL_N; i++) {
		const struct sb_pmu_cal *c = &sb_m3_pmu_cal_table[i];
		uint32_t v = io->rd(io->ctx, c->offset);
		io->wr(io->ctx, c->offset, (v & ~c->mask) | c->value);
	}
	/* read back after all writes (the kernel's io does a DSB after each write) */
	for (unsigned i = 0; i < SB_M3_PMU_CAL_N; i++) {
		const struct sb_pmu_cal *c = &sb_m3_pmu_cal_table[i];
		if ((io->rd(io->ctx, c->offset) & c->mask) != c->value) {
			if (bad == 0 && first_bad_offset != NULL) {
				*first_bad_offset = c->offset;
			}
			bad++;
		}
	}
	return bad;
}

int
sb_smp_cpu_on(struct sb_smp *s, unsigned logical, uint64_t entry_pa, int32_t *rc)
{
	*rc = 0;
	if (logical >= s->ncpus || s->state[logical] == SB_CPU_ABSENT) {
		return SB_SMP_E_INVAL;
	}
	if (s->state[logical] != SB_CPU_OFF || s->cpu[logical].is_boot) {
		return SB_SMP_E_STATE;
	}
	const struct sb_cpu_desc *d = &s->cpu[logical];
	if (sb_smp_check_desc(d) != SB_SMP_OK) {
		s->state[logical] = SB_CPU_FAILED;
		return SB_SMP_E_DESC;
	}
	if (d->kind == 'P') {
		if (s->pmu == NULL) {
			s->state[logical] = SB_CPU_FAILED;
			return SB_SMP_E_NOPMU;
		}
		s->pmu_cal_runs++;
		if (sb_m3_pmu_cal(s->pmu, &s->pmu_bad_offset) != 0) {
			s->state[logical] = SB_CPU_FAILED;
			return SB_SMP_E_PMU;
		}
	}
	/*
	 * ON_SENT before the SMC: the core can run and report in (sb_smp_booted, on itself) before the firmware
	 * returns here. The kernel's store is followed by the DSB the conduit does before `smc`.
	 */
	s->state[logical] = SB_CPU_ON_SENT;
	*rc = sb_psci_cpu_on(s->psci, d->psci_target, entry_pa, logical);
	s->last_rc[logical] = *rc;
	if (*rc != PSCI_SUCCESS) {
		s->state[logical] = SB_CPU_FAILED;
		return SB_SMP_E_PSCI;
	}
	return SB_SMP_OK;
}

int
sb_smp_booted(struct sb_smp *s, unsigned logical, uint64_t mpidr_read, uint64_t midr_read)
{
	if (logical >= s->ncpus || s->state[logical] == SB_CPU_ABSENT) {
		return SB_SMP_E_INVAL;
	}
	s->seen_mpidr[logical] = mpidr_read;
	s->seen_midr[logical] = midr_read;
	const struct sb_cpu_desc *d = &s->cpu[logical];
	if ((mpidr_read & 0xffffffffu) != (d->mpidr & 0xffffffffu) || sb_midr_kind(midr_read) != d->kind) {
		s->state[logical] = SB_CPU_FAILED;
		return SB_SMP_E_IDENT;
	}
	if (!d->is_boot && s->state[logical] != SB_CPU_ON_SENT) {
		s->state[logical] = SB_CPU_FAILED;
		return SB_SMP_E_STATE;          /* a core we never started reports in: something else started it */
	}
	s->state[logical] = SB_CPU_UP;
	return SB_SMP_OK;
}

void
sb_smp_trampoline(uint32_t words[4], uint64_t entry_pa)
{
	words[0] = 0x58000050u;                         /* ldr x16, #8 */
	words[1] = 0xd61f0200u;                         /* br x16 */
	words[2] = (uint32_t)entry_pa;
	words[3] = (uint32_t)(entry_pa >> 32);
}

uint64_t
sb_smp_entry(uint64_t entry_pa, uint64_t trampoline_pa)
{
	if ((entry_pa >> 32) != 0 && trampoline_pa != 0 && (trampoline_pa >> 32) == 0) {
		return trampoline_pa;
	}
	return entry_pa;
}

bool
sb_smp_tally_pass(const struct sb_smp_tally *t)
{
	return t->selected[0] + t->selected[1] > 0 && t->up[0] == t->selected[0] && t->up[1] == t->selected[1] &&
	       t->ipi_ok && t->timer_ok && t->timebase_hz == 26000000u;
}

/* a tiny formatter: the kernel and the host both have snprintf, but not under one header */
static size_t
put_s(char *b, size_t n, size_t at, const char *s)
{
	while (*s != '\0') {
		if (at + 1 < n) {
			b[at] = *s;
		}
		at++;
		s++;
	}
	if (n != 0) {
		b[at < n ? at : n - 1] = '\0';
	}
	return at;
}

static size_t
put_u(char *b, size_t n, size_t at, uint64_t v, unsigned min_digits)
{
	char t[24];
	unsigned k = 0;
	do {
		t[k++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0 || k < min_digits);
	char r[24];
	for (unsigned i = 0; i < k; i++) {
		r[i] = t[k - 1 - i];
	}
	r[k] = '\0';
	return put_s(b, n, at, r);
}

size_t
sb_smp_pass_line(char *buf, size_t n, const struct sb_smp_tally *t)
{
	size_t at = 0;
	at = put_s(buf, n, at, "cluster E: ");
	at = put_u(buf, n, at, t->up[0], 1);
	at = put_s(buf, n, at, " up");
	if (t->up[0] != t->selected[0]) {
		at = put_s(buf, n, at, " of ");
		at = put_u(buf, n, at, t->selected[0], 1);
	}
	at = put_s(buf, n, at, ", cluster P: ");
	at = put_u(buf, n, at, t->up[1], 1);
	at = put_s(buf, n, at, " up");
	if (t->up[1] != t->selected[1]) {
		at = put_s(buf, n, at, " of ");
		at = put_u(buf, n, at, t->selected[1], 1);
	}
	at = put_s(buf, n, at, t->ipi_ok ? ", IPI ok" : ", IPI FAILED");
	at = put_s(buf, n, at, t->timer_ok ? ", timer " : ", timer FAILED ");
	at = put_u(buf, n, at, t->timebase_hz / 1000000u, 1);
	if (t->timebase_hz % 1000000u != 0) {
		at = put_s(buf, n, at, ".");
		at = put_u(buf, n, at, (t->timebase_hz % 1000000u) / 1000u, 3);
	}
	at = put_s(buf, n, at, " MHz");
	return at;
}
