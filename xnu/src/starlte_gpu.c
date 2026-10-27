/*
 * STARLTE (Galaxy S9 SM-G960F, Exynos 9810) Mali-G72 MP18 (Bifrost), stage 1:
 * read-only probe, the S9 counterpart of herolte_gpu.c (Mali-T880).
 *
 * From the S9+ mainline port (SheroAbi/galaxy-s9plus-mainline-linux, which
 * drives this GPU with Panfrost on the same SoC): gpu@17500000 (0x5000; SPIs
 * job 282, mmu 281, gpu 283), G3D domain PMU EMBEDDED_G3D_CONFIGURATION/STATUS
 * 0x14064040/0x14064044, CMU_G3D 0x17400000 (PLL_G3D CON0 at +0x140),
 * vdd_g3d = S2MPS18 BUCK6 (reg 0x2b, 300 mV + n * 6.25 mV; S-Boot leaves
 * 644 mV, jobs need 950 mV). S-Boot hands the domain over switched on; after
 * a power cycle the domain's TZPC must be restored through EL3
 * (SMC 0x82000410(1, 0x17410204, 0)) or the first GPU access is an SError.
 *
 * The probe writes nothing. CMU_G3D and the GPU are read only while the
 * domain status low nibble is 0xf. Boot-arg stargpu=0 skips everything,
 * 1 probes only, 2 (default) also runs the stage 2 jobs (below).
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
extern bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
void starlte_gpu_probe(void);

#define SG_PMU_G3D_PA   0x14064000u     /* page with CONFIGURATION +0x40, STATUS +0x44 */
#define SG_PMU_G3D_CONF 0x40u
#define SG_PMU_G3D_STAT 0x44u
#define SG_CMU_PA       0x17400000u
#define SG_GPU_PA       0x17500000u

/* Mali Bifrost register offsets (Linux Panfrost panfrost_regs.h). */
#define GPU_ID                  0x000
#define L2_FEATURES             0x004
#define CORE_FEATURES           0x008
#define TILER_FEATURES          0x00c
#define MEM_FEATURES            0x010
#define MMU_FEATURES            0x014
#define AS_PRESENT              0x018
#define JS_PRESENT              0x01c
#define GPU_IRQ_RAWSTAT         0x020
#define GPU_STATUS              0x034
#define THREAD_MAX_THREADS      0x0a0
#define SHADER_PRESENT_LO       0x100
#define TILER_PRESENT_LO        0x110
#define L2_PRESENT_LO           0x120
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160
#define COHERENCY_FEATURES      0x300

static vm_offset_t sg_gpu;

static uint32_t
sg_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__builtin_arm_dmb(0xb);
	return v;
}

static void
sg_msleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	clock_delay_until(deadline);
}

static void
sg_probe(vm_offset_t pmu, const char *when)
{
	uint32_t conf = sg_rd(pmu, SG_PMU_G3D_CONF), stat = sg_rd(pmu, SG_PMU_G3D_STAT);
	printf("STARLTE GPU: %s: PMU G3D conf=%#x status=%#x\n", when, conf, stat);
	if ((stat & 0xf) != 0xf) {
		printf("STARLTE GPU: %s: G3D domain off; CMU and Mali not read\n", when);
		return;
	}
	static vm_offset_t cmu;
	if (!cmu) {
		cmu = ml_io_map(SG_CMU_PA, 0x2000);
	}
	if (!sg_gpu) {
		sg_gpu = ml_io_map(SG_GPU_PA, 0x5000);
	}
	vm_offset_t gpu = sg_gpu;
	if (!cmu || !gpu) {
		printf("STARLTE GPU: MMIO mapping failed\n");
		return;
	}
	uint32_t con0 = sg_rd(cmu, 0x140);
	uint64_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	uint64_t khz = (con0 & 0x10) && p ? 26000ull * m / (p << s) : 26000;
	printf("STARLTE GPU: %s: PLL_G3D CON0=%#x CON2=%#x (%llu kHz%s), USER mux=%#x BUSD mux=%#x div=%#x busp=%#x\n",
	    when, con0, sg_rd(cmu, 0x148), khz, (con0 & (1u << 29)) ? ", locked" : ", not stable",
	    sg_rd(cmu, 0x100), sg_rd(cmu, 0x1000), sg_rd(cmu, 0x1800), sg_rd(cmu, 0x1804));
	if ((sg_rd(pmu, SG_PMU_G3D_STAT) & 0xf) != 0xf) {
		printf("STARLTE GPU: %s: domain went off; Mali not read\n", when);
		return;
	}
	uint32_t id = sg_rd(gpu, GPU_ID);
	printf("STARLTE GPU: %s: GPU_ID=%#x product %#x (0x6221 = Mali-G72) r%up%u\n",
	    when, id, id >> 16, (id >> 12) & 0xf, (id >> 4) & 0xff);
	printf("STARLTE GPU: L2_FEATURES=%#x CORE=%#x TILER=%#x MEM=%#x MMU=%#x AS=%#x JS=%#x THREADS=%u\n",
	    sg_rd(gpu, L2_FEATURES), sg_rd(gpu, CORE_FEATURES), sg_rd(gpu, TILER_FEATURES),
	    sg_rd(gpu, MEM_FEATURES), sg_rd(gpu, MMU_FEATURES), sg_rd(gpu, AS_PRESENT),
	    sg_rd(gpu, JS_PRESENT), sg_rd(gpu, THREAD_MAX_THREADS));
	printf("STARLTE GPU: present shader=%#x tiler=%#x l2=%#x; ready shader=%#x tiler=%#x l2=%#x\n",
	    sg_rd(gpu, SHADER_PRESENT_LO), sg_rd(gpu, TILER_PRESENT_LO), sg_rd(gpu, L2_PRESENT_LO),
	    sg_rd(gpu, SHADER_READY_LO), sg_rd(gpu, TILER_READY_LO), sg_rd(gpu, L2_READY_LO));
	printf("STARLTE GPU: GPU_STATUS=%#x IRQ_RAWSTAT=%#x COHERENCY_FEATURES=%#x\n",
	    sg_rd(gpu, GPU_STATUS), sg_rd(gpu, GPU_IRQ_RAWSTAT), sg_rd(gpu, COHERENCY_FEATURES));
}

/*
 * Stage 2: the first GPU jobs, polled (no interrupts yet), as herolte_gpu.c
 * did on the T880 (herogpu=3). Steps:
 *   1. checks: domain on, GPU_ID 0x6221, PLL_G3D locked (S-Boot's 260 MHz,
 *      the lowest Panfrost OPP on this SoC; the clock is not touched)
 *   2. vdd_g3d: S2MPS18 BUCK6 (reg 0x2b) raised to 950 mV (0x68) through
 *      ACPM, only from a value in 0.5..1.0 V, read back
 *   3. soft reset (GPU_COMMAND 1, wait RESET_COMPLETED), as Panfrost does
 *   4. L2, then all shader cores, then the tiler powered on (bounded polls)
 *   5. AS0 with Mali-LPAE tables (Panfrost runs this G72 in LPAE mode,
 *      TRANSCFG = 0 = legacy), MEMATTR 0x888d88, TRANSTAB | 7
 *   6. WRITE_VALUE "zero" on JS1 (the T880's first job), then WRITE_VALUE
 *      "immediate 64" writing "STARBITE", both on every shader core
 *   7. cores off, BUCK6 back to the value S-Boot left
 * The GPU memory is 64 KiB at 0x8f0000000, in bank 2 above XNU's 1 GiB
 * (0x880000000, tools/mkadt_starlte.py --bank2), mapped uncached on the
 * CPU (COHERENCY_FEATURES = 0: the GPU is not IO-coherent).
 */
#define SG_MEM_PA       0x8f0000000ull
#define SG_MEM_BYTES    0x10000u
#define SG_VA           0x02000000ull   /* the data pages, from +0x4000 */
#define SG_DATA_OFF     0x4000u
#define SG_DATA_PAGES   8u
#define SG_BUCK6        0x2bu
#define SG_BUCK6_950MV  0x68u           /* 300 mV + 0x68 * 6.25 mV */

#define GPU_IRQ_CLEAR           0x024
#define GPU_COMMAND             0x030
#define GPU_FAULTSTATUS         0x03c
#define GPU_FAULTADDR_LO        0x040
#define SHADER_PWRON_LO         0x180
#define TILER_PWRON_LO          0x190
#define L2_PWRON_LO             0x1a0
#define SHADER_PWROFF_LO        0x1c0
#define TILER_PWROFF_LO         0x1d0
#define L2_PWROFF_LO            0x1e0
#define JOB_IRQ_RAWSTAT         0x1000
#define JOB_IRQ_CLEAR           0x1004
#define JS_BASE(n)              (0x1800u + (n) * 0x80u)
#define MMU_IRQ_RAWSTAT         0x2000
#define MMU_IRQ_CLEAR           0x2004
#define AS0                     0x2400

static vm_offset_t sg_mem;

static void
sg_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static bool
sg_power(uint32_t on, uint32_t ready, uint32_t mask, const char *name)
{
	sg_wr(sg_gpu, on, mask);
	uint32_t r = 0;
	unsigned us;
	for (us = 0; us < 20000; us += 10) {
		r = sg_rd(sg_gpu, ready);
		if ((r & mask) == mask) {
			break;
		}
		delay(10);
	}
	printf("STARLTE GPU: power %s mask=%#x ready=%#x after %u us\n", name, mask, r, us);
	return (r & mask) == mask;
}

static void
sg_cores_off(void)
{
	sg_wr(sg_gpu, TILER_PWROFF_LO, sg_rd(sg_gpu, TILER_READY_LO));
	sg_wr(sg_gpu, SHADER_PWROFF_LO, sg_rd(sg_gpu, SHADER_READY_LO));
	delay(100);
	sg_wr(sg_gpu, L2_PWROFF_LO, sg_rd(sg_gpu, L2_READY_LO));
	delay(100);
	printf("STARLTE GPU: cores off: ready shader=%#x tiler=%#x l2=%#x\n",
	    sg_rd(sg_gpu, SHADER_READY_LO), sg_rd(sg_gpu, TILER_READY_LO), sg_rd(sg_gpu, L2_READY_LO));
}

static bool
sg_soft_reset(void)
{
	sg_wr(sg_gpu, GPU_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, GPU_COMMAND, 1);                  /* SOFT_RESET */
	uint32_t raw = 0;
	unsigned us;
	for (us = 0; us < 10000; us += 10) {
		raw = sg_rd(sg_gpu, GPU_IRQ_RAWSTAT);
		if (raw & 0x100) {                      /* RESET_COMPLETED */
			break;
		}
		delay(10);
	}
	sg_wr(sg_gpu, GPU_IRQ_CLEAR, 0xffffffffu);
	printf("STARLTE GPU: soft reset %s after %u us (rawstat %#x), GPU_STATUS=%#x\n",
	    (raw & 0x100) ? "done" : "TIMEOUT", us, raw, sg_rd(sg_gpu, GPU_STATUS));
	return raw & 0x100;
}

/* Page tables: SG_DATA_PAGES pages from +SG_DATA_OFF at GPU VA SG_VA. */
static void
sg_tables(void)
{
	volatile uint64_t *q = (volatile uint64_t *)sg_mem;
	for (unsigned i = 0; i < SG_MEM_BYTES / 8; i++) {
		q[i] = 0;
	}
	volatile uint64_t *l0 = (volatile uint64_t *)sg_mem;
	volatile uint64_t *l1 = (volatile uint64_t *)(sg_mem + 0x1000);
	volatile uint64_t *l2 = (volatile uint64_t *)(sg_mem + 0x2000);
	volatile uint64_t *l3 = (volatile uint64_t *)(sg_mem + 0x3000);
	l0[0] = (SG_MEM_PA + 0x1000) | 3;
	l1[0] = (SG_MEM_PA + 0x2000) | 3;
	l2[(SG_VA >> 21) & 0x1ff] = (SG_MEM_PA + 0x3000) | 3;
	for (unsigned i = 0; i < SG_DATA_PAGES; i++) {
		/* Mali-LPAE leaf: read/write, outer shareable, MEMATTR index 0. */
		l3[i] = (SG_MEM_PA + SG_DATA_OFF + i * 0x1000) | 0x2c1;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
sg_mmu_setup(void)
{
	uint64_t transtab = SG_MEM_PA | 0x7;            /* ADRMODE_TABLE | READ_INNER */
	printf("STARLTE GPU: AS0 before: TRANSCFG=%#x STATUS=%#x\n",
	    sg_rd(sg_gpu, AS0 + 0x30), sg_rd(sg_gpu, AS0 + 0x28));
	sg_wr(sg_gpu, AS0 + 0x00, (uint32_t)transtab);
	sg_wr(sg_gpu, AS0 + 0x04, (uint32_t)(transtab >> 32));
	sg_wr(sg_gpu, AS0 + 0x08, 0x888d88);            /* MEMATTR */
	sg_wr(sg_gpu, AS0 + 0x0c, 0);
	sg_wr(sg_gpu, AS0 + 0x30, 0);                   /* TRANSCFG: legacy (LPAE) */
	sg_wr(sg_gpu, AS0 + 0x34, 0);
	sg_wr(sg_gpu, AS0 + 0x18, 1);                   /* AS_COMMAND_UPDATE */
	for (unsigned i = 0; i < 10000 && (sg_rd(sg_gpu, AS0 + 0x28) & 1); i++) {
		delay(10);
	}
	if (sg_rd(sg_gpu, AS0 + 0x28) & 1) {
		printf("STARLTE GPU: AS0 update timeout\n");
		return false;
	}
	return true;
}

/* Start the chain at GPU VA job_va on slot js, wait (bounded), log. */
static bool
sg_run(unsigned js, uint64_t job_va, uint32_t affinity, const char *what)
{
	uint32_t slot = JS_BASE(js);
	sg_wr(sg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, slot + 0x40, (uint32_t)job_va);           /* HEAD_NEXT */
	sg_wr(sg_gpu, slot + 0x44, (uint32_t)(job_va >> 32));
	sg_wr(sg_gpu, slot + 0x50, affinity);                   /* AFFINITY_NEXT */
	sg_wr(sg_gpu, slot + 0x54, 0);
	/* AS 0, flush clean+invalidate at start and end, thread priority 8 */
	sg_wr(sg_gpu, slot + 0x58, (8u << 16) | (3u << 8) | (3u << 12));
	uint64_t t0 = mach_absolute_time();
	sg_wr(sg_gpu, slot + 0x60, 1);                          /* COMMAND_NEXT START */

	uint32_t jirq = 0, mirq = 0, girq = 0;
	for (unsigned i = 0; i < 20000; i++) {
		jirq = sg_rd(sg_gpu, JOB_IRQ_RAWSTAT);
		mirq = sg_rd(sg_gpu, MMU_IRQ_RAWSTAT);
		girq = sg_rd(sg_gpu, GPU_IRQ_RAWSTAT);
		if ((jirq & ((1u << js) | (1u << (js + 16)))) || mirq || (girq & 0x81)) {
			break;
		}
		delay(10);
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	volatile uint32_t *hdr = (volatile uint32_t *)(sg_mem + SG_DATA_OFF + (job_va - SG_VA));
	bool ok = (jirq & (1u << js)) && !(jirq & (1u << (js + 16))) && !mirq && !(girq & 0x81);
	printf("STARLTE GPU: %s: %s in %llu us: irq=%#x mmu=%#x gpu=%#x js_status=%#x "
	    "as_fault=%#x@%#x exception=%#x fault=%#x:%#x gpu_fault=%#x@%#x\n", what,
	    ok ? "done" : "FAILED", ns / 1000, jirq, mirq, girq,
	    sg_rd(sg_gpu, slot + 0x24), sg_rd(sg_gpu, AS0 + 0x1c), sg_rd(sg_gpu, AS0 + 0x20),
	    hdr[0], hdr[3], hdr[2], sg_rd(sg_gpu, GPU_FAULTSTATUS), sg_rd(sg_gpu, GPU_FAULTADDR_LO));
	sg_wr(sg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	sg_wr(sg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	return ok;
}

/*
 * One WRITE_VALUE job at data offset job_off (Job Header, 32 bytes, then
 * the payload: address, type, immediate; Mesa genxml v6), writing to
 * out_off. Types: 3 = zero, 7 = immediate 64.
 */
static bool
sg_write_value(uint32_t job_off, uint32_t out_off, uint32_t type, uint64_t value,
    uint64_t expect, uint32_t affinity, const char *what)
{
	vm_offset_t d = sg_mem + SG_DATA_OFF;
	volatile uint8_t *job = (volatile uint8_t *)(d + job_off);
	for (unsigned i = 0; i < 64; i++) {
		job[i] = 0;
	}
	job[16] = 1 | (2 << 1);                         /* 64-bit, WRITE_VALUE */
	*(volatile uint16_t *)(job + 18) = 1;           /* index */
	*(volatile uint64_t *)(job + 32) = SG_VA + out_off;
	*(volatile uint32_t *)(job + 40) = type;
	*(volatile uint64_t *)(job + 48) = value;
	volatile uint64_t *out = (volatile uint64_t *)(d + out_off);
	*out = 0xa5a5a5a55a5a5a5aull;
	__asm__ volatile ("dsb sy" ::: "memory");
	bool ok = sg_run(1, SG_VA + job_off, affinity, what) && *out == expect;
	printf("STARLTE GPU: %s: output=%#llx (want %#llx) -> %s\n", what, *out, expect,
	    ok ? "THE MALI-G72 RAN A JOB" : "failed");
	return ok;
}

static void
sg_jobs(vm_offset_t pmu)
{
	if ((sg_rd(pmu, SG_PMU_G3D_STAT) & 0xf) != 0xf || !sg_gpu ||
	    (sg_rd(sg_gpu, GPU_ID) >> 16) != 0x6221) {
		printf("STARLTE GPU: stage 2: domain off or not a G72; no jobs\n");
		return;
	}
	vm_offset_t cmu = ml_io_map(SG_CMU_PA, 0x1000);
	if (!cmu || !(sg_rd(cmu, 0x140) & (1u << 29))) {
		printf("STARLTE GPU: stage 2: PLL_G3D not locked; no jobs\n");
		return;
	}
	uint8_t was = 0, now = 0;
	if (!starlte_pmic_read(1, SG_BUCK6, &was)) {
		printf("STARLTE GPU: stage 2: BUCK6 unreadable; no jobs\n");
		return;
	}
	/* 0x20 = 500 mV .. 0x70 = 1000 mV; anything else is not vdd_g3d as known. */
	if (was < 0x20 || was > 0x70) {
		printf("STARLTE GPU: stage 2: BUCK6 vsel %#x unexpected; no jobs\n", was);
		return;
	}
	bool volt = starlte_pmic_write(1, SG_BUCK6, SG_BUCK6_950MV);
	delay(1000);                                    /* ramp: well under 1 ms */
	(void)starlte_pmic_read(1, SG_BUCK6, &now);
	printf("STARLTE GPU: vdd_g3d BUCK6 %#x (%u uV) -> %#x (%u uV) %s\n", was,
	    300000u + was * 6250u, now, 300000u + now * 6250u,
	    volt && now == SG_BUCK6_950MV ? "ok" : "FAILED");
	if (!volt || now != SG_BUCK6_950MV) {
		goto restore;
	}
	sg_mem = ml_io_map((vm_offset_t)SG_MEM_PA, SG_MEM_BYTES);
	if (!sg_mem) {
		printf("STARLTE GPU: GPU memory mapping failed\n");
		goto restore;
	}
	if (!sg_soft_reset()) {
		goto restore;
	}
	uint32_t shaders = sg_rd(sg_gpu, SHADER_PRESENT_LO);
	bool on = sg_power(L2_PWRON_LO, L2_READY_LO, sg_rd(sg_gpu, L2_PRESENT_LO), "L2") &&
	    sg_power(SHADER_PWRON_LO, SHADER_READY_LO, shaders, "shader cores") &&
	    sg_power(TILER_PWRON_LO, TILER_READY_LO, sg_rd(sg_gpu, TILER_PRESENT_LO), "tiler");
	printf("STARLTE GPU: GPU_STATUS=%#x rawstat=%#x after power-up\n",
	    sg_rd(sg_gpu, GPU_STATUS), sg_rd(sg_gpu, GPU_IRQ_RAWSTAT));
	if (on) {
		sg_tables();
		if (sg_mmu_setup()) {
			bool a = sg_write_value(0x000, 0x100, 3, 0, 0, shaders, "WRITE_VALUE zero");
			bool b = sg_write_value(0x200, 0x300, 7, 0x4554494252415453ull,
			    0x4554494252415453ull, shaders, "WRITE_VALUE STARBITE");
			printf("STARLTE GPU: stage 2 %s\n", a && b ? "PASSED" : "did not pass");
		}
	}
	sg_cores_off();
restore:
	if (starlte_pmic_write(1, SG_BUCK6, was) && starlte_pmic_read(1, SG_BUCK6, &now)) {
		printf("STARLTE GPU: vdd_g3d BUCK6 back to %#x (%u uV)\n", now, 300000u + now * 6250u);
	}
}

static void
sg_thread(void *arg, __unused wait_result_t wr)
{
	int mode = (int)(uintptr_t)arg;
	vm_offset_t pmu = ml_io_map(SG_PMU_G3D_PA, 0x1000);
	if (!pmu) {
		printf("STARLTE GPU: PMU mapping failed\n");
		return;
	}
	/* Once early, once after the UI is up (in case something changed it). */
	sg_msleep(5000);
	sg_probe(pmu, "5s");
	if (mode >= 2) {
		/* After the touch thread's ACPM traffic (2 s) and before the UI. */
		sg_msleep(3000);
		sg_jobs(pmu);
	}
	sg_msleep(52000);
	sg_probe(pmu, "60s");
}

void
starlte_gpu_probe(void)
{
	int enabled = 2;
	thread_t thread;

	PE_parse_boot_argn("stargpu", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("STARLTE GPU: probe disabled\n");
		return;
	}
	if (kernel_thread_start(sg_thread, (void *)(uintptr_t)enabled, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
