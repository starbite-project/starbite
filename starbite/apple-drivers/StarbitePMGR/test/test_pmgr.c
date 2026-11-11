/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host tests of StarbitePMGR's hw/ (PSCI wrappers, CPU bring-up order, M3 PMU CAL) plus an end-to-end bring-up
 * simulation of milestone M2: a mock Samsung EL3 (PSCI 0.1 over SMC), a mock PMU, and the GIC-400 model from
 * ../../StarbiteGICv2/test that delivers the IPIs and timer interrupts.
 *
 * The CPU descriptors come from tools/board/starlte.board (test/run.sh turns it into board_cpus.h), so a wrong
 * MPIDR / PSCI target in the board file fails here first.
 */
#include <stdio.h>
#include <string.h>
#include "../hw/sb_smp.h"
#include "../../StarbiteGICv2/test/mock_gic.h"
#include "board_cpus.h"         /* BOARD_CPUS[] = { logical, mpidr, psci, cluster, kind }, BOARD_PSCI_* */

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ event log (ordering checks) */
static unsigned seq;

/* ------------------------------------------------------------------ mock PMU */
#define PMU_SIZE 0x10000u
static struct {
	uint32_t reg[PMU_SIZE / 4];
	uint32_t stuck_off;             /* a register that ignores writes (0 = none) */
	unsigned writes;
	unsigned last_write_seq;
} PMU;

static uint32_t
pmu_rd(void *ctx, uint32_t off)
{
	(void)ctx;
	return PMU.reg[off / 4];
}

static void
pmu_wr(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	PMU.writes++;
	PMU.last_write_seq = ++seq;
	if (off != PMU.stuck_off || off == 0) {
		PMU.reg[off / 4] = v;
	}
}

static const struct sb_pmu_io pmu_io = { NULL, pmu_rd, pmu_wr };

static bool
pmu_cal_in_place(void)
{
	for (unsigned i = 0; i < SB_M3_PMU_CAL_N; i++) {
		const struct sb_pmu_cal *c = &sb_m3_pmu_cal_table[i];
		if ((PMU.reg[c->offset / 4] & c->mask) != c->value) {
			return false;
		}
	}
	return true;
}

/* ------------------------------------------------------------------ the simulated SoC */
static struct mock_gic M;
static struct sb_gic GV[MOCK_GIC_CPUS];         /* each CPU's view; driver state shared in DRV */
static struct sb_gic DRV;                       /* the driver's struct (masks, nlines) */
static uint64_t EXPECTED_ENTRY;                 /* sweet_secondary_entry's PA */
static uint32_t RAM_TRAMPOLINE[4];              /* the page below 4 GiB, if used */
static uint64_t RAM_TRAMPOLINE_PA;

struct core {
	uint64_t mpidr, midr, psci;
	bool on, ran;
	unsigned on_seq;
	unsigned logical;
	bool sigp_enabled;
	unsigned ipis, timers;
};
static struct core CORE[8];

/* firmware knobs */
static bool FW_TRUNCATES_ENTRY;                 /* an S7-style firmware that keeps 32 bits of the entry */
static int32_t FW_FORCE_RC;                     /* != 0: CPU_ON answers this */
static unsigned FW_CALLS, FW_VERSION_ASKS, FW_UNKNOWN;
static struct sb_smp SMP;
static struct sb_psci PSCI;

static void secondary_runs(struct core *c, uint64_t entry);

static int64_t
fw_smc(void *ctx, uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	(void)ctx;
	FW_CALLS++;
	if (fid == PSCI_0_2_FN_PSCI_VERSION) {
		FW_VERSION_ASKS++;
		return PSCI_NOT_SUPPORTED;      /* what the S9 answers */
	}
	if (fid != BOARD_PSCI_CPU_ON) {
		FW_UNKNOWN++;
		return PSCI_NOT_SUPPORTED;
	}
	if (FW_FORCE_RC != 0) {
		return (int64_t)(uint32_t)FW_FORCE_RC;  /* w0 only: the wrapper must sign-extend it */
	}
	for (unsigned i = 0; i < 8; i++) {
		struct core *c = &CORE[i];
		if (c->psci != a1) {
			continue;
		}
		if (c->on) {
			return PSCI_ALREADY_ON;
		}
		c->on = true;
		c->on_seq = ++seq;
		c->logical = (unsigned)a3;
		uint64_t entry = FW_TRUNCATES_ENTRY ? (a2 & 0xffffffffu) : a2;
		/* an M3 started without its CAL values: CPU_ON says 0, the core never runs */
		if (sb_midr_kind(c->midr) == 'P' && !pmu_cal_in_place()) {
			return PSCI_SUCCESS;
		}
		secondary_runs(c, entry);
		return PSCI_SUCCESS;
	}
	return PSCI_INVALID_PARAMS;
}

/* sleh_irq (A5XELTE branch) for one CPU */
static void
sim_irq(unsigned cpu)
{
	struct sb_gic *g = &GV[cpu];
	uint32_t iar = sb_gic_ack(g);
	switch (sb_gic_classify(iar, SB_GIC_TIMER_INTID)) {
	case SB_GIC_ROUTE_SPURIOUS:
		return;
	case SB_GIC_ROUTE_TIMER:
		CORE[cpu].timers++;
		sb_gic_eoi(g, iar);
		return;
	case SB_GIC_ROUTE_IPI:
		sb_gic_eoi(g, iar);
		CORE[cpu].ipis++;
		CORE[cpu].sigp_enabled = true;  /* cpu_signal_handler clears SIGPdisabled */
		return;
	case SB_GIC_ROUTE_OTHER:
		sb_gic_eoi(g, iar);
		return;
	}
}

static void
run_irqs(void)
{
	for (unsigned rounds = 0; rounds < 64; rounds++) {
		bool any = false;
		for (unsigned c = 0; c < 8; c++) {
			if (CORE[c].ran && mock_gic_irq_line(&M, c)) {
				sim_irq(c);
				any = true;
			}
		}
		if (!any) {
			return;
		}
	}
}

/* the driver's CPU_BOOTED callbacks, on the booted core: GIC mask, then the PMGR identity check */
static int booted_errors;
static void
cpu_booted(unsigned logical)
{
	struct sb_gic *g = &GV[logical];
	memcpy(g->cpu_mask, DRV.cpu_mask, sizeof(DRV.cpu_mask));
	if (sb_gic_record_own_mask(g, logical, NULL) != SB_GIC_OK) {
		booted_errors++;
	}
	memcpy(DRV.cpu_mask, g->cpu_mask, sizeof(DRV.cpu_mask));
	if (sb_smp_booted(&SMP, logical, CORE[logical].mpidr, CORE[logical].midr) != SB_SMP_OK) {
		booted_errors++;
	}
	/* PE_cpu_machine_init: the self-IPI that clears SIGPdisabled */
	if (sb_gic_send_ipi(g, logical, logical, SB_GIC_IPI_SGI) != SB_GIC_OK) {
		booted_errors++;
	}
}

static void
secondary_runs(struct core *c, uint64_t entry)
{
	/* the trampoline is followed if the firmware jumped there */
	if (RAM_TRAMPOLINE_PA != 0 && entry == RAM_TRAMPOLINE_PA) {
		entry = (uint64_t)RAM_TRAMPOLINE[2] | ((uint64_t)RAM_TRAMPOLINE[3] << 32);
	}
	if (entry != EXPECTED_ENTRY) {
		return;                         /* jumped somewhere else: never checks in */
	}
	unsigned cpu = (unsigned)(c - CORE);
	c->ran = true;
	/* start.s -> arm_init_cpu -> pe_init_fiq: pexpert's per-CPU GIC init */
	sb_gic_cpu_init_pexpert(&GV[cpu], SB_GIC_TIMER_INTID, SB_GIC_IPI_SGI);
	cpu_booted(c->logical);
	run_irqs();
}

static void
soc_reset(unsigned ncpus_selected)
{
	memset(&PMU, 0, sizeof(PMU));
	/* plausible reset values: other bits set, so the RMW must keep them */
	PMU.reg[0x1088 / 4] = 0x00000011u;
	PMU.reg[0x2114 / 4] = 0x00010001u;
	PMU.reg[0x2180 / 4] = 0x00010001u;
	PMU.reg[0x21ec / 4] = 0x00010001u;
	PMU.reg[0x2258 / 4] = 0x00010001u;
	PMU.reg[0x2384 / 4] = 0x00010000u;
	mock_gic_init(&M, 8);
	memset(&DRV, 0, sizeof(DRV));
	for (unsigned c = 0; c < 8; c++) {
		memset(&GV[c], 0, sizeof(GV[c]));
		mock_gic_bind(&M, c, &GV[c]);
	}
	mock_gic_bind(&M, 0, &DRV);
	memset(CORE, 0, sizeof(CORE));
	for (unsigned i = 0; i < BOARD_NCPUS; i++) {
		CORE[i].mpidr = BOARD_CPUS[i].mpidr;
		CORE[i].psci = BOARD_CPUS[i].psci;
		CORE[i].midr = BOARD_CPUS[i].kind == 'E' ? 0x412fd050u : 0x531f0020u;
	}
	FW_TRUNCATES_ENTRY = false;
	FW_FORCE_RC = 0;
	FW_CALLS = FW_VERSION_ASKS = FW_UNKNOWN = 0;
	booted_errors = 0;
	RAM_TRAMPOLINE_PA = 0;
	memset(RAM_TRAMPOLINE, 0, sizeof(RAM_TRAMPOLINE));
	seq = 0;

	memset(&SMP, 0, sizeof(SMP));
	CHECK(sb_psci_init(&PSCI, "smc", 4, BOARD_PSCI_CPU_ON, BOARD_PSCI_CPU_OFF, BOARD_PSCI_CPU_SUSPEND,
	    fw_smc, NULL) == PSCI_SUCCESS);
	SMP.psci = &PSCI;
	SMP.pmu = &pmu_io;
	for (unsigned i = 0; i < ncpus_selected; i++) {
		struct sb_cpu_desc d = {
			.reg = sb_smp_reg_from_mpidr(BOARD_CPUS[i].mpidr), .mpidr = BOARD_CPUS[i].mpidr,
			.psci_target = BOARD_CPUS[i].psci, .kind = BOARD_CPUS[i].kind, .is_boot = i == 0,
		};
		CHECK(sb_smp_add(&SMP, i, &d) == SB_SMP_OK);
	}
	/* the boot CPU: pexpert init at boot, then the GIC driver's start() */
	CORE[0].on = CORE[0].ran = true;
	CORE[0].logical = 0;
	sb_gic_cpu_init_pexpert(&GV[0], SB_GIC_TIMER_INTID, SB_GIC_IPI_SGI);
	CHECK(sb_gic_probe(&DRV) == SB_GIC_OK);
	CHECK(sb_gic_record_own_mask(&DRV, 0, NULL) == SB_GIC_OK);
	CHECK(sb_gic_dist_init_spis(&DRV, DRV.cpu_mask[0]) == SB_GIC_OK);
	for (unsigned c = 0; c < 8; c++) {
		GV[c].nlines = DRV.nlines;
	}
	cpu_booted(0);                  /* AppleARMSMP: processor_boot(boot cpu) -> PE_cpu_machine_init */
	run_irqs();                     /* ml_wait_for_cpu_signal_to_enable: the boot CPU takes its self-IPI */
	CHECK(CORE[0].sigp_enabled);
}

/* AppleARMSMP's loop: processor_boot -> PE_cpu_start_internal -> enableCPUCore for each secondary */
static int
boot_all(unsigned ncpus, int *first_err, int32_t *first_rc)
{
	int bad = 0;
	*first_err = 0;
	*first_rc = 0;
	for (unsigned i = 1; i < ncpus; i++) {
		int32_t rc;
		uint64_t entry = sb_smp_entry(EXPECTED_ENTRY, RAM_TRAMPOLINE_PA);
		int e = sb_smp_cpu_on(&SMP, i, entry, &rc);
		if (e != SB_SMP_OK) {
			if (bad++ == 0) {
				*first_err = e;
				*first_rc = rc;
			}
		}
	}
	return bad;
}

/* the M2 self-test of StarbitePMGR: count, IPIs from CPU 0 to every other CPU, a timer tick on each */
static struct sb_smp_tally
self_test(unsigned ncpus)
{
	struct sb_smp_tally t = { .timebase_hz = 26000000u };
	t.ipi_ok = t.timer_ok = true;
	for (unsigned i = 0; i < ncpus; i++) {
		unsigned k = SMP.cpu[i].kind == 'P';
		t.selected[k]++;
		if (SMP.state[i] == SB_CPU_UP && CORE[i].sigp_enabled) {
			t.up[k]++;
		}
	}
	for (unsigned i = 1; i < ncpus; i++) {
		unsigned before = CORE[i].ipis;
		if (!CORE[i].ran || sb_gic_send_ipi(&DRV, i, 0, SB_GIC_IPI_SGI) != SB_GIC_OK) {
			t.ipi_ok = false;
			continue;
		}
		run_irqs();
		t.ipi_ok &= CORE[i].ipis == before + 1;
	}
	for (unsigned i = 0; i < ncpus; i++) {
		if (!CORE[i].ran) {
			t.timer_ok = false;
			continue;
		}
		unsigned before = CORE[i].timers;
		mock_gic_ppi_raise(&M, i, SB_GIC_TIMER_INTID);
		run_irqs();
		t.timer_ok &= CORE[i].timers == before + 1;
	}
	return t;
}

int
main(void)
{
	char line[128];
	int err;
	int32_t rc;
	EXPECTED_ENTRY = 0x92001234u;   /* below 4 GiB: today's iOS 26 kernel window */

	/* ---- the board file's facts */
	CHECK(BOARD_NCPUS == 8);
	for (unsigned i = 0; i < BOARD_NCPUS; i++) {
		struct sb_cpu_desc d = {
			.reg = sb_smp_reg_from_mpidr(BOARD_CPUS[i].mpidr), .mpidr = BOARD_CPUS[i].mpidr,
			.psci_target = BOARD_CPUS[i].psci, .kind = BOARD_CPUS[i].kind,
		};
		CHECK(sb_smp_check_desc(&d) == SB_SMP_OK);
		CHECK(BOARD_CPUS[i].logical == i);
		CHECK((BOARD_CPUS[i].kind == 'E') == (BOARD_CPUS[i].cluster == 0));
	}
	/* the regs today's ADT (mkadt_starlte.py) gives the cores */
	CHECK(sb_smp_reg_from_mpidr(0x81000000u) == 0x000);
	CHECK(sb_smp_reg_from_mpidr(0x81000300u) == 0x300);
	CHECK(sb_smp_reg_from_mpidr(0x80000100u) == 0x10100);
	CHECK(sb_smp_reg_from_mpidr(0x80000103u) == 0x10103);
	CHECK(sb_smp_psci_from_reg(0x200) == 2 && sb_smp_psci_from_reg(0x10102) == 0x102);
	{
		struct sb_cpu_desc d = { .reg = 0x10100, .mpidr = 0x80000100u, .psci_target = 0x100, .kind = 'P' };
		CHECK(sb_smp_check_desc(&d) == SB_SMP_OK);
		d.kind = 'E';
		CHECK(sb_smp_check_desc(&d) == SB_SMP_E_DESC);         /* an A55 with the M3 bit */
		d.kind = 'P';
		d.psci_target = 1;
		CHECK(sb_smp_check_desc(&d) == SB_SMP_E_DESC);         /* the collision the bit-16 quirk avoids */
		d.psci_target = 0x100;
		d.reg = 0x100;
		CHECK(sb_smp_check_desc(&d) == SB_SMP_E_DESC);         /* reg without bit 16 = A55 core 1's */
		d.reg = 0x10100;
		d.kind = 'X';
		CHECK(sb_smp_check_desc(&d) == SB_SMP_E_DESC);
	}
	CHECK(sb_midr_kind(0x412fd050u) == 'E' && sb_midr_kind(0x531f0020u) == 'P' && sb_midr_kind(0x410fd034u) == '?');

	/* ---- PSCI wrappers */
	{
		struct sb_psci p;
		CHECK(sb_psci_init(&p, "hvc", 4, 0xc4000003u, 0, 0, fw_smc, NULL) == SB_PSCI_E_METHOD);
		CHECK(!p.ready);
		CHECK(sb_psci_init(&p, "smcx", 4, 0xc4000003u, 0, 0, fw_smc, NULL) == SB_PSCI_E_METHOD);
		CHECK(sb_psci_init(&p, NULL, 0, 0xc4000003u, 0, 0, fw_smc, NULL) == SB_PSCI_E_METHOD);
		CHECK(sb_psci_init(&p, "smc", 3, 0, 0, 0, fw_smc, NULL) == SB_PSCI_E_FNID);
		CHECK(sb_psci_init(&p, "smc", 3, 0x44000003u, 0, 0, fw_smc, NULL) == SB_PSCI_E_FNID);
		CHECK(sb_psci_init(&p, "smc", 3, 0xc4000003u, 0, 0, NULL, NULL) == SB_PSCI_E_FNID);
		CHECK(sb_psci_cpu_on(&p, 1, 0, 0) == PSCI_NOT_SUPPORTED);      /* not ready: no SMC */
		CHECK(sb_psci_init(&p, "smc", 3, 0xc4000003u, 0, 0, fw_smc, NULL) == PSCI_SUCCESS);
		CHECK(BOARD_PSCI_CPU_ON == 0xc4000003u);
		CHECK(strcmp(sb_psci_strerror(PSCI_ALREADY_ON), "ALREADY_ON") == 0);
		CHECK(strcmp(sb_psci_strerror(-42), "UNKNOWN") == 0);
	}

	/* ---- M3 PMU CAL alone: values set, other bits kept */
	soc_reset(8);
	{
		uint32_t bad = 0;
		CHECK(sb_m3_pmu_cal(&pmu_io, &bad) == 0);
		CHECK(PMU.reg[0x1088 / 4] == 0x8d000011u);
		CHECK(PMU.reg[0x2114 / 4] == 0x00010002u && PMU.reg[0x2384 / 4] == 0x00010002u);
		CHECK(PMU.writes == SB_M3_PMU_CAL_N);
		PMU.stuck_off = 0x21ec;
		PMU.reg[0x21ec / 4] = 0;
		CHECK(sb_m3_pmu_cal(&pmu_io, &bad) == 1 && bad == 0x21ec);
	}

	/* ---- M2, cpus=8: the whole bring-up */
	soc_reset(8);
	CHECK(boot_all(8, &err, &rc) == 0);
	CHECK(booted_errors == 0);
	CHECK(FW_CALLS == 7 && FW_VERSION_ASKS == 0 && FW_UNKNOWN == 0);
	CHECK(SMP.pmu_cal_runs == 4);                   /* before every M3 CPU_ON, never for an A55 */
	for (unsigned i = 0; i < 8; i++) {
		CHECK(SMP.state[i] == SB_CPU_UP && CORE[i].sigp_enabled && CORE[i].ipis == 1);
		CHECK(DRV.cpu_mask[i] == M.iface_of_cpu[i]);
	}
	struct sb_smp_tally t = self_test(8);
	CHECK(sb_smp_tally_pass(&t));
	sb_smp_pass_line(line, sizeof(line), &t);
	CHECK(strcmp(line, "cluster E: 4 up, cluster P: 4 up, IPI ok, timer 26 MHz") == 0);
	printf("  cpus=8: %s\n", line);
	CHECK(M.eoi_mismatch == 0 && M.bad_accesses == 0);
	/* starting a core twice, or the boot core, is refused without an SMC */
	{
		unsigned calls = FW_CALLS;
		CHECK(sb_smp_cpu_on(&SMP, 3, EXPECTED_ENTRY, &rc) == SB_SMP_E_STATE);
		CHECK(sb_smp_cpu_on(&SMP, 0, EXPECTED_ENTRY, &rc) == SB_SMP_E_STATE);
		CHECK(sb_smp_cpu_on(&SMP, 9, EXPECTED_ENTRY, &rc) == SB_SMP_E_INVAL);
		CHECK(FW_CALLS == calls);
	}

	/* ---- the order matters: CPU_ON for every M3 came after all six CAL writes of its own run */
	soc_reset(8);
	{
		unsigned last_cal_before[8] = { 0 };
		for (unsigned i = 1; i < 8; i++) {
			int32_t r;
			unsigned w0 = PMU.writes;
			CHECK(sb_smp_cpu_on(&SMP, i, EXPECTED_ENTRY, &r) == SB_SMP_OK);
			last_cal_before[i] = PMU.last_write_seq;
			if (i >= 4) {
				CHECK(PMU.writes == w0 + SB_M3_PMU_CAL_N);
				CHECK(CORE[i].on_seq > last_cal_before[i]);
			} else {
				CHECK(PMU.writes == w0);
			}
		}
	}

	/* ---- cpus=4 (the first phone run): A55s only, no PMU writes, no M3 SMC */
	soc_reset(4);
	CHECK(boot_all(4, &err, &rc) == 0);
	CHECK(PMU.writes == 0 && FW_CALLS == 3);
	t = self_test(4);
	CHECK(sb_smp_tally_pass(&t));
	sb_smp_pass_line(line, sizeof(line), &t);
	CHECK(strcmp(line, "cluster E: 4 up, cluster P: 0 up, IPI ok, timer 26 MHz") == 0);
	printf("  cpus=4: %s\n", line);

	/* ---- a PMU register that will not take its value: that M3 is NOT started (no SMC), the rest are */
	soc_reset(8);
	PMU.stuck_off = 0x2180;
	CHECK(boot_all(8, &err, &rc) == 4 && err == SB_SMP_E_PMU && rc == 0);
	CHECK(FW_CALLS == 3);
	CHECK(SMP.pmu_bad_offset == 0x2180);
	t = self_test(8);
	CHECK(!sb_smp_tally_pass(&t));
	sb_smp_pass_line(line, sizeof(line), &t);
	CHECK(strcmp(line, "cluster E: 4 up, cluster P: 0 up of 4, IPI FAILED, timer FAILED 26 MHz") == 0);

	/* ---- no PMU mapping: the M3s fail before any SMC */
	soc_reset(8);
	SMP.pmu = NULL;
	CHECK(boot_all(8, &err, &rc) == 4 && err == SB_SMP_E_NOPMU);
	CHECK(FW_CALLS == 3);

	/* ---- what the mock firmware does if the CAL is skipped: rc 0, the M3 never runs (why the order exists) */
	soc_reset(8);
	{
		int64_t r = fw_smc(NULL, BOARD_PSCI_CPU_ON, 0x100, EXPECTED_ENTRY, 4);
		CHECK(r == 0 && CORE[4].on && !CORE[4].ran);
	}

	/* ---- the firmware refuses: the signed result comes through (w0 sign-extended) */
	soc_reset(8);
	FW_FORCE_RC = PSCI_DENIED;
	CHECK(boot_all(2, &err, &rc) == 1 && err == SB_SMP_E_PSCI && rc == PSCI_DENIED);
	CHECK(SMP.state[1] == SB_CPU_FAILED && SMP.last_rc[1] == PSCI_DENIED);

	/* ---- a 64-bit entry on a firmware that truncates it: without a trampoline nothing checks in */
	EXPECTED_ENTRY = 0x881234560ull;        /* the Apple chain loader's bank-2 kernel */
	soc_reset(8);
	FW_TRUNCATES_ENTRY = true;
	CHECK(boot_all(8, &err, &rc) == 0);     /* every CPU_ON says 0 ... */
	t = self_test(8);
	CHECK(t.up[0] == 1 && t.up[1] == 0 && !sb_smp_tally_pass(&t));   /* ... and only the boot CPU is up */
	/* with the trampoline below 4 GiB they all arrive */
	soc_reset(8);
	FW_TRUNCATES_ENTRY = true;
	RAM_TRAMPOLINE_PA = 0x91ffd000u;
	sb_smp_trampoline(RAM_TRAMPOLINE, EXPECTED_ENTRY);
	CHECK(RAM_TRAMPOLINE[0] == 0x58000050u && RAM_TRAMPOLINE[1] == 0xd61f0200u);
	CHECK(RAM_TRAMPOLINE[2] == 0x81234560u && RAM_TRAMPOLINE[3] == 0x8u);
	CHECK(boot_all(8, &err, &rc) == 0);
	t = self_test(8);
	CHECK(sb_smp_tally_pass(&t));
	CHECK(sb_smp_entry(0x92000000u, 0x91ffd000u) == 0x92000000u);    /* low entries go direct */
	CHECK(sb_smp_entry(0x881234560ull, 0) == 0x881234560ull);
	CHECK(sb_smp_entry(0x881234560ull, 0x900000000ull) == 0x881234560ull);    /* a "trampoline" above 4G is useless */
	EXPECTED_ENTRY = 0x92001234u;

	/* ---- identity checks on the booted core */
	soc_reset(8);
	{
		int32_t r;
		CORE[5].midr = 0x412fd050u;     /* an A55 answers where an M3 should */
		CHECK(sb_smp_cpu_on(&SMP, 5, EXPECTED_ENTRY, &r) == SB_SMP_OK);
		CHECK(SMP.state[5] == SB_CPU_FAILED && booted_errors == 1);
		CHECK(sb_smp_booted(&SMP, 6, 0x80000102u, 0x531f0020u) == SB_SMP_E_STATE);   /* never started */
		CHECK(sb_smp_booted(&SMP, 12, 0, 0) == SB_SMP_E_INVAL);
	}

	/* ---- the pass line's other shapes */
	{
		struct sb_smp_tally x = { { 4, 4 }, { 4, 4 }, true, true, 24576000u };
		sb_smp_pass_line(line, sizeof(line), &x);
		CHECK(strcmp(line, "cluster E: 4 up, cluster P: 4 up, IPI ok, timer 24.576 MHz") == 0);
		CHECK(!sb_smp_tally_pass(&x));                  /* only 26 MHz is right on the S9 */
		struct sb_smp_tally z = { { 0, 0 }, { 0, 0 }, true, true, 26000000u };
		CHECK(!sb_smp_tally_pass(&z));
		char tiny[8];
		size_t n = sb_smp_pass_line(tiny, sizeof(tiny), &x);
		CHECK(n > sizeof(tiny) && strlen(tiny) == sizeof(tiny) - 1);
	}

	printf("StarbitePMGR hw: %d/%d checks passed\n", checks - fails, checks);
	return fails != 0;
}
