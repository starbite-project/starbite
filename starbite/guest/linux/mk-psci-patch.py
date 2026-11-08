#!/usr/bin/env python3
"""Apply Starbite's CPU_ON trampoline change to a copy of arch/arm64/kernel/psci.c (Linux 7.1 + the S9 port).

    mk-psci-patch.py PSCI.C       edits the file in place; fails loudly if the source drifted
The patch (0002-arm64-psci-starbite-cpu-on-trampoline.patch) is `diff -u` of before/after, made by
build-patched.sh against the port's psci.c (it carries S9P-VENDOR-DEFAULTS). Why: docs/STARBITE-OS-LINUX-M3.md 2.
P2 on the phone (2026-11-08 18:32): the guest's own CPU_ON of 0x101..0x103 at secondary_entry (in the
0x8_8000_0000 bank) returned 0 and the cores never ran, like P1's high entry.
"""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()


def sub(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit(f'mk-psci-patch: expected exactly one match for: {old[:60]!r}')
    s = s.replace(old, new)


sub('''static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);''', '''/*
 * STARBITE-TRAMPOLINE (starbite/guest, docs/STARBITE-OS-LINUX-M3.md 2): on the Galaxy S9 (Exynos 9810) PSCI
 * CPU_ON accepts an entry point above 4 GiB, powers the core and the core never runs. A kernel living in the
 * 0x8_8000_0000 bank (the Starbite OS guest on the M3 cluster) names a 64-byte slot below 4 GiB in
 * /psci "starbite,cpu-on-trampoline"; CPU_ON then enters there and the trampoline branches to the real entry
 * (x0 = 0: secondary_entry clears it anyway). "starbite,cpu-on-trampoline-always" uses it for any entry (QEMU
 * test). CPU bring-up and hotplug are serialised by the hotplug lock, so one slot is enough.
 */
static const u32 starbite_tramp_code[8] = {
	0x10000185,	/* adr x5, #48     the breadcrumb (word 12) */
	0xf90000a5,	/* str x5, [x5] */
	0x58000100,	/* ldr x0, #32     word 10 (0) */
	0xaa1f03e1,	/* mov x1, xzr */
	0xaa1f03e2,	/* mov x2, xzr */
	0xaa1f03e3,	/* mov x3, xzr */
	0x58000044,	/* ldr x4, #8      word 8: the entry */
	0xd61f0080,	/* br  x4 */
};

static void __iomem *starbite_tramp, *starbite_pmu, *starbite_pmu_intr;
static phys_addr_t starbite_tramp_pa;
static bool starbite_probed, starbite_always;
static DEFINE_RAW_SPINLOCK(starbite_pmu_lock);

static void starbite_probe(void)
{
	struct device_node *np;
	u32 fp[3];
	u64 pa = 0;

	if (starbite_probed)
		return;
	starbite_probed = true;
	np = of_find_node_by_path("/psci");
	if (!np)
		return;
	if (!of_property_read_u64(np, "starbite,cpu-on-trampoline", &pa) && pa && pa + 64 <= SZ_4G) {
		starbite_tramp = ioremap(pa, 64);
		if (starbite_tramp)
			starbite_tramp_pa = pa;
		starbite_always = of_property_read_bool(np, "starbite,cpu-on-trampoline-always");
		pr_info("CPU_ON through the trampoline at %pa (below 4 GiB)%s\\n", &starbite_tramp_pa,
			starbite_always ? ", always" : "");
	}
	/* <PMU, the PMU's interrupt block, the APM mailbox> (the mailbox is sbguest.ko's, for the cluster) */
	if (!of_property_read_u32_array(np, "starbite,flexpmu", fp, 3)) {
		starbite_pmu = ioremap(fp[0], 0x8000);
		starbite_pmu_intr = ioremap(fp[1], 0x1000);
		if (!starbite_pmu || !starbite_pmu_intr)
			starbite_pmu = NULL;
		pr_info("FlexPMU core power sequences around CPU_ON / CPU_OFF (PMU %#x)\\n", fp[0]);
	}
	of_node_put(np);
}

/*
 * STARBITE-FLEXPMU: Samsung's per-core FlexPMU bookkeeping around CPU_ON / CPU_OFF (the ACPM build's
 * flexpmu_cal_cpu_exynos9810.h core1N_on / core1N_off + pmucal_powermode_hint, run by the vendor kernel in its
 * hotplug callbacks). Without the "off" half the PMU never powers a core down after CPU_OFF (P2b on the phone:
 * the guest's CPUs in CPU_OFF, the PMU still on; the boot menu found the same for the A55s). The core's bit in
 * the GRP1 / GRP2 interrupt blocks: cluster 1 core n = bit n, cluster 0 core n = bit 4 + n; CPU_INFORM(phycpu)
 * at PMU + 0x860 (0 running, 1 = C2). "on" is written for the target before its CPU_ON, "off" by the dying core
 * itself right before its CPU_OFF (interrupts off, the last thing it does).
 */
static void starbite_flexpmu_core(u64 mpidr, bool on)
{
	unsigned int lvl = (mpidr & MPIDR_MT_BITMASK) ? 1 : 0;
	unsigned int phy = (MPIDR_AFFINITY_LEVEL(mpidr, 1 + lvl) << 2) | MPIDR_AFFINITY_LEVEL(mpidr, lvl);
	u32 b = phy >= 4 ? BIT(phy - 4) : BIT(phy + 4);
	void __iomem *pmu = starbite_pmu, *intr = starbite_pmu_intr;
	unsigned long flags;

	if (!pmu || phy > 7)
		return;
	raw_spin_lock_irqsave(&starbite_pmu_lock, flags);
	if (on) {
		writel_relaxed(0, pmu + 0x860 + 4 * phy);				/* the hint cleared */
		writel_relaxed(readl_relaxed(intr + 0x200) & ~b, intr + 0x200);		/* GRP2_INTR_BID_ENABLE */
		if ((readl_relaxed(intr + 0x208) & b) == b)				/* GRP2 pending: clear */
			writel_relaxed(readl_relaxed(intr + 0x20c) | b, intr + 0x20c);
		writel_relaxed(readl_relaxed(pmu + 0x7f08) & ~b, pmu + 0x7f08);		/* EVENT_INTERRUPT_ENABLE_GRP2 */
	} else {
		writel_relaxed(1, pmu + 0x860 + 4 * phy);				/* CPU_INFORM_C2 */
		writel_relaxed(readl_relaxed(intr + 0x200) | b, intr + 0x200);
		if ((readl_relaxed(intr + 0x108) & b) == b)				/* GRP1 pending: clear */
			writel_relaxed(readl_relaxed(intr + 0x10c) | b, intr + 0x10c);
		writel_relaxed(readl_relaxed(pmu + 0x7f08) | b, pmu + 0x7f08);
	}
	dsb(sy);
	raw_spin_unlock_irqrestore(&starbite_pmu_lock, flags);
}

static phys_addr_t starbite_cpu_on_entry(phys_addr_t entry)
{
	void __iomem *tramp = starbite_tramp;
	int i;

	if (!tramp || (entry < SZ_4G && !starbite_always))
		return entry;
	for (i = 0; i < 8; i++)
		writel_relaxed(starbite_tramp_code[i], tramp + 4 * i);
	writel_relaxed(lower_32_bits(entry), tramp + 32);
	writel_relaxed(upper_32_bits(entry), tramp + 36);
	for (i = 40; i < 56; i += 4)
		writel_relaxed(0, tramp + i);
	dsb(sy);
	return starbite_tramp_pa;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);''')
sub('''	err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);''',
    '''	starbite_probe();
	starbite_flexpmu_core(cpu_logical_map(cpu), true);
	err = psci_ops.cpu_on(cpu_logical_map(cpu), starbite_cpu_on_entry(pa_secondary_entry));''')
sub('''	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	psci_ops.cpu_off(state);''', '''	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	starbite_flexpmu_core(read_cpuid_mpidr(), false);
	psci_ops.cpu_off(state);''')
if '#include <linux/sizes.h>' not in s:
    sub('#include <linux/psci.h>\n', '#include <linux/psci.h>\n#include <linux/sizes.h>\n')
p.write_text(s)
print(f'mk-psci-patch: {p} patched')
