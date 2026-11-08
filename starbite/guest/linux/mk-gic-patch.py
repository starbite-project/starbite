#!/usr/bin/env python3
"""Apply Starbite's shared-distributor change to a copy of drivers/irqchip/irq-gic.c (Linux 7.1).

    mk-gic-patch.py IRQ-GIC.C        edits the file in place; fails loudly if the source drifted
The patch file (0001-irqchip-gic-starbite-shared-distributor.patch) is `diff -u` of before/after
(starbite/guest/linux/build-patched.sh). Why: docs/STARBITE-OS-LINUX-M3.md 4.
"""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()


def sub(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit(f'mk-gic-patch: expected exactly one match for: {old[:60]!r}')
    s = s.replace(old, new)


sub('''static void gic_dist_init(struct gic_chip_data *gic)
{
	unsigned int i;
	u32 cpumask;
	unsigned int gic_irqs = gic->gic_irqs;
	void __iomem *base = gic_data_dist_base(gic);

	writel_relaxed(GICD_DISABLE, base + GIC_DIST_CTRL);
''', '''/*
 * "starbite,shared-distributor": this kernel runs on some of the CPUs next to another kernel (Starbite OS,
 * no hypervisor) that owns the distributor. Leave GICD_CTLR and every SPI's target, priority, enable and
 * trigger alone at boot and across cluster power-down; the other kernel configured our SPIs before it
 * started us. Per-CPU state (SGIs, PPIs, the CPU interface) is banked and set up as usual; the SPIs this
 * kernel requests are still enabled and routed (byte writes and write-1-to-set registers only).
 */
static bool gic_dist_shared;

static void gic_dist_init(struct gic_chip_data *gic)
{
	unsigned int i;
	u32 cpumask;
	unsigned int gic_irqs = gic->gic_irqs;
	void __iomem *base = gic_data_dist_base(gic);

	if (gic_dist_shared) {
		pr_info("GIC: shared distributor (starbite,shared-distributor), CTLR %#x left as found\\n",
			readl_relaxed(base + GIC_DIST_CTRL));
		return;
	}

	writel_relaxed(GICD_DISABLE, base + GIC_DIST_CTRL);
''')
sub('''void gic_dist_save(struct gic_chip_data *gic)
{
	unsigned int gic_irqs;
	void __iomem *dist_base;
	int i;

	if (WARN_ON(!gic))
''', '''void gic_dist_save(struct gic_chip_data *gic)
{
	unsigned int gic_irqs;
	void __iomem *dist_base;
	int i;

	if (gic_dist_shared)
		return;
	if (WARN_ON(!gic))
''')
sub('''void gic_dist_restore(struct gic_chip_data *gic)
{
	unsigned int gic_irqs;
	unsigned int i;
	void __iomem *dist_base;

	if (WARN_ON(!gic))
		return;
''', '''void gic_dist_restore(struct gic_chip_data *gic)
{
	unsigned int gic_irqs;
	unsigned int i;
	void __iomem *dist_base;

	if (gic_dist_shared)
		return;
	if (WARN_ON(!gic))
		return;
''')
sub('''	if (gic_cnt == 0 && !gic_check_eoimode(node, &gic->raw_cpu_base))
		static_branch_disable(&supports_deactivate_key);

	ret = __gic_init_bases(gic, &node->fwnode);''', '''	if (gic_cnt == 0 && !gic_check_eoimode(node, &gic->raw_cpu_base))
		static_branch_disable(&supports_deactivate_key);

	if (gic_cnt == 0)
		gic_dist_shared = of_property_read_bool(node, "starbite,shared-distributor");

	ret = __gic_init_bases(gic, &node->fwnode);''')
p.write_text(s)
print(f'mk-gic-patch: {p} patched')
