// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbguest: the guest Linux's kernel side of Starbite OS 0.9 app support (docs/STARBITE-OS-LINUX-M3.md 5, 8).
 * Linux runs alone on the M3 cluster next to Starbite OS (no hypervisor), so three things must never
 * reach the firmware's whole-machine calls, and the shared region wants a proper mapping:
 *
 *   1. power off / reboot / panic: PSCI SYSTEM_OFF / SYSTEM_RESET would switch off or reset the phone under
 *      Starbite OS. A reboot notifier takes every other guest CPU down (remove_cpu: PSCI CPU_OFF), then a
 *      sys-off handler (above PSCI's) and a restart handler (priority 255, above PSCI's 129) mark the header
 *      and call PSCI CPU_OFF on the last CPU: the cluster powers down, Starbite OS sees AFFINITY_INFO OFF.
 *   2. /dev/sbshm: mmap of the shared region (write-combine), poll/read = the host's doorbell SPI count,
 *      write = pend the host's doorbell SPI (GICD_ISPENDR, a write-1-to-set register: safe to share).
 *   3. the header's guest_cpuif (the guest CPUs' GIC CPU-interface bits, ORed), for the host's routing checks.
 *
 * DT: compatible "starbite,guest-shm", reg = the header page, interrupts = the host->guest doorbell SPI
 * (optional); /chosen/starbite,shm = <base size> of the whole region.
 */
#include <asm/cputype.h>
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <uapi/linux/psci.h>

#include "../sbshm.h"

static phys_addr_t shm_base;
static u64 shm_size;
static struct sbshm_hdr *hdr;           /* write-combine vmap of the header page */
static void __iomem *gicd;
static bool use_hvc;
static u32 fn_cpu_off = PSCI_0_2_FN_CPU_OFF;
static atomic_t db_count = ATOMIC_INIT(0);
static void __iomem *fp_pmu, *fp_intr, *fp_mbox;	/* /psci starbite,flexpmu (the S9) */
static DECLARE_WAIT_QUEUE_HEAD(db_wait);

static void sbg_psci_cpu_off(void)
{
	struct arm_smccc_res res;

	if (use_hvc)
		arm_smccc_hvc(fn_cpu_off, 0, 0, 0, 0, 0, 0, 0, &res);
	else
		arm_smccc_smc(fn_cpu_off, 0, 0, 0, 0, 0, 0, 0, &res);
}

static void sbg_ring_host(void)
{
	u32 id = READ_ONCE(hdr->db_to_host);

	if (gicd && id >= 32 && id < 1020)
		writel(BIT(id % 32), gicd + 0x200 + 4 * (id / 32));	/* GICD_ISPENDR */
}

/*
 * The last guest CPU's FlexPMU bookkeeping before its CPU_OFF, as Samsung's hotplug-out does for the last core
 * of a cluster (exynos-powermode.c: cpu_disable, then cluster_disable): the core's "off" sequence with the C2
 * hint, then the CPD hint and the APM mailbox's cluster bit (flexpmu_cal_cpu_exynos9810.h cluster1_off:
 * MAILBOX_APM2AP + 0x10 bit 24). The other cores did their "off" in cpu_psci_cpu_die (linux/0002).
 */
static void sbg_flexpmu_last(void)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int lvl = (mpidr & MPIDR_MT_BITMASK) ? 1 : 0;
	unsigned int phy = (MPIDR_AFFINITY_LEVEL(mpidr, 1 + lvl) << 2) | MPIDR_AFFINITY_LEVEL(mpidr, lvl);
	u32 b = phy >= 4 ? BIT(phy - 4) : BIT(phy + 4);

	if (!fp_pmu || !fp_intr || !fp_mbox || phy > 7)
		return;
	writel_relaxed(1, fp_pmu + 0x860 + 4 * phy);				/* CPU_INFORM_C2 */
	writel_relaxed(readl_relaxed(fp_intr + 0x200) | b, fp_intr + 0x200);	/* GRP2_INTR_BID_ENABLE */
	if ((readl_relaxed(fp_intr + 0x108) & b) == b)
		writel_relaxed(readl_relaxed(fp_intr + 0x10c) | b, fp_intr + 0x10c);
	writel_relaxed(readl_relaxed(fp_pmu + 0x7f08) | b, fp_pmu + 0x7f08);
	writel_relaxed(2, fp_pmu + 0x860 + 4 * phy);				/* CPU_INFORM_CPD */
	writel_relaxed(readl_relaxed(fp_mbox + 0x10) | BIT(24), fp_mbox + 0x10);	/* cluster1_off */
	dsb(sy);
}

/* the last CPU: mark, ring, the FlexPMU's cluster-off bookkeeping, CPU_OFF; never returns */
static void __noreturn sbg_last_cpu_off(u32 state)
{
	local_irq_disable();
	sbg_flexpmu_last();
	WRITE_ONCE(hdr->guest_state, state);
	WRITE_ONCE(hdr->guest_ncpus, num_online_cpus());
	dsb(sy);
	sbg_ring_host();
	dsb(sy);
	sbg_psci_cpu_off();
	for (;;)
		wfi();
}

static int sbg_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned int cpu, self = raw_smp_processor_id();

	/* process context, every CPU still online: take the others down properly (PSCI CPU_OFF each) */
	for_each_online_cpu(cpu)
		if (cpu != self && remove_cpu(cpu))
			pr_warn("sbguest: CPU%u did not go offline\n", cpu);
	pr_info("sbguest: %u CPU online for the %s\n", num_online_cpus(),
		action == SYS_RESTART ? "restart" : "power off");
	return NOTIFY_DONE;
}

static struct notifier_block sbg_reboot_nb = {
	.notifier_call = sbg_reboot_notify,
	.priority = INT_MAX,
};

static int sbg_power_off(struct sys_off_data *data)
{
	sbg_last_cpu_off(SBG_GUEST_OFF);
}

static int sbg_restart(struct notifier_block *nb, unsigned long mode, void *cmd)
{
	/* a reboot asked for from user space (the host may start the guest again), or a panic */
	sbg_last_cpu_off(system_state == SYSTEM_RESTART ? SBG_GUEST_OFF : SBG_GUEST_PANIC);
}

static struct notifier_block sbg_restart_nb = {
	.notifier_call = sbg_restart,
	.priority = 255,
};

/* ---- /dev/sbshm ------------------------------------------------------------------------------------ */
static irqreturn_t sbg_db_irq(int irq, void *dev)
{
	atomic_inc(&db_count);
	wake_up_interruptible(&db_wait);
	return IRQ_HANDLED;
}

static int sbg_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || len > shm_size)
		return -EINVAL;
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, PHYS_PFN(shm_base), len, vma->vm_page_prot);
}

static ssize_t sbg_read(struct file *f, char __user *buf, size_t n, loff_t *off)
{
	u32 c;

	if (n < sizeof(c))
		return -EINVAL;
	if (wait_event_interruptible(db_wait, atomic_read(&db_count)))
		return -ERESTARTSYS;
	c = atomic_xchg(&db_count, 0);
	return copy_to_user(buf, &c, sizeof(c)) ? -EFAULT : sizeof(c);
}

static ssize_t sbg_write(struct file *f, const char __user *buf, size_t n, loff_t *off)
{
	sbg_ring_host();
	return n;
}

static __poll_t sbg_poll(struct file *f, poll_table *wait)
{
	poll_wait(f, &db_wait, wait);
	return atomic_read(&db_count) ? EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations sbg_fops = {
	.owner = THIS_MODULE,
	.mmap = sbg_mmap,
	.read = sbg_read,
	.write = sbg_write,
	.poll = sbg_poll,
	.llseek = noop_llseek,
};

static struct miscdevice sbg_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sbshm",
	.fops = &sbg_fops,
};

static atomic_t cpuif_mask = ATOMIC_INIT(0);

static void sbg_read_cpuif(void *unused)
{
	atomic_or(readb(gicd + 0x800), &cpuif_mask);
}

static int sbg_probe(struct platform_device *pdev)
{
	struct device_node *chosen, *psci;
	const char *method = NULL;
	struct page *page;
	u64 r[2];
	int irq, ret;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen || of_property_read_u64_array(chosen, "starbite,shm", r, 2))
		return dev_err_probe(&pdev->dev, -ENODEV, "no /chosen/starbite,shm\n");
	of_node_put(chosen);
	shm_base = r[0];
	shm_size = r[1];
	/* the region is RAM reserved without no-map: map the header page write-combine (ioremap refuses RAM) */
	page = pfn_to_page(PHYS_PFN(shm_base));
	hdr = vmap(&page, 1, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!hdr)
		return -ENOMEM;
	if (READ_ONCE(hdr->magic) != SBSHM_MAGIC) {
		vunmap(hdr);
		return dev_err_probe(&pdev->dev, -ENODEV, "no Starbite header at %pa\n", &shm_base);
	}
	/* PSCI 0.2+ (QEMU) or 0.1 with listed ids (the S9: "arm,psci", cpu_off = <0x84000002>) */
	psci = of_find_compatible_node(NULL, NULL, "arm,psci-0.2");
	if (!psci)
		psci = of_find_compatible_node(NULL, NULL, "arm,psci");
	if (psci && !of_property_read_string(psci, "method", &method))
		use_hvc = !strcmp(method, "hvc");
	if (psci) {
		u32 fp[3];

		of_property_read_u32(psci, "cpu_off", &fn_cpu_off);
		if (!of_property_read_u32_array(psci, "starbite,flexpmu", fp, 3)) {
			fp_pmu = ioremap(fp[0], 0x8000);
			fp_intr = ioremap(fp[1], 0x1000);
			fp_mbox = ioremap(fp[2], 0x1000);
		}
	}
	of_node_put(psci);
	if (hdr->gicd_base)
		gicd = ioremap(hdr->gicd_base, 0x1000);
	/* every guest CPU's GIC interface bit (GICD_ITARGETSR0 byte 0 is banked per CPU), ORed */
	if (gicd)
		on_each_cpu(sbg_read_cpuif, NULL, 1);
	WRITE_ONCE(hdr->guest_cpuif, atomic_read(&cpuif_mask));

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_irq(&pdev->dev, irq, sbg_db_irq, 0, "sbshm-doorbell", NULL);
		if (ret)
			dev_warn(&pdev->dev, "doorbell irq %d: %d (polling)\n", irq, ret);
	}
	ret = misc_register(&sbg_misc);
	if (ret)
		return ret;
	register_reboot_notifier(&sbg_reboot_nb);
	register_restart_handler(&sbg_restart_nb);
	register_sys_off_handler(SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_HIGH, sbg_power_off, NULL);
	dev_info(&pdev->dev, "shared region %pa + %#llx, PSCI %s, doorbell irq %d, GIC CPU interface %#x%s\n",
		 &shm_base, shm_size, use_hvc ? "hvc" : "smc", irq, hdr->guest_cpuif,
		 fp_mbox ? ", FlexPMU cluster off" : "");
	return 0;
}

static const struct of_device_id sbg_of[] = {
	{ .compatible = "starbite,guest-shm" },
	{ }
};
MODULE_DEVICE_TABLE(of, sbg_of);

static struct platform_driver sbg_driver = {
	.probe = sbg_probe,
	.driver = {
		.name = "sbguest",
		.of_match_table = sbg_of,
		.suppress_bind_attrs = true,	/* no unbind: the handlers stay registered */
	},
};
module_platform_driver(sbg_driver);

MODULE_DESCRIPTION("Starbite OS guest: shared region, doorbells, power off without SYSTEM_OFF");
MODULE_LICENSE("GPL");
