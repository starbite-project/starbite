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
 *   4. sbblk (contract v2, sbshm.h "v2: sbblk"): the block devices the host serves (/dev/sbblk0 the base image,
 *      sbblk1 the overlay, sbblk2 the repo), a blk-mq driver over the request / completion rings and the data
 *      window. Doorbells only; the message rings belong to user space (sbwl).
 *
 * DT: compatible "starbite,guest-shm", reg = the header page, interrupts = the host->guest doorbell SPI
 * (optional); /chosen/starbite,shm = <base size> of the whole region.
 */
#include <asm/cputype.h>
#include <linux/arm-smccc.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <uapi/linux/psci.h>

#include "../sbshm.h"
#include "sbg_core.h"

phys_addr_t shm_base;
u64 shm_size;
struct sbshm_hdr *hdr;           /* write-combine vmap of the header page */
static void __iomem *gicd;
static bool use_hvc;
static u32 fn_cpu_off = PSCI_0_2_FN_CPU_OFF;
static atomic_t db_count = ATOMIC_INIT(0);
static atomic_t sbg_db_on_cpu[8];		/* 8e: the doorbell's interrupts by CPU (sbblk_report) */
static int sbg_db_irq_nr;
static bool sbg_db_bad;				/* 8e: the doorbell found lost (sbg_db_lost_fn) */
static atomic_t sbblk_found_by_timer = ATOMIC_INIT(0), sbblk_completions = ATOMIC_INIT(0);	/* 8e: sbblk_timer_fn */
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

void sbg_ring_host(void)
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
	unsigned int phy = (MPIDR_AFFINITY_LEVEL(mpidr, (1 + lvl)) << 2) | MPIDR_AFFINITY_LEVEL(mpidr, lvl);
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

static void sbblk_freeze_overlay(void);
static void sbblk_report(struct work_struct *w);
static int sbg_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned int cpu, self = raw_smp_processor_id();

	sbblk_freeze_overlay();			/* the card's overlay clean before the cluster goes off */
	sbblk_report(NULL);			/* 8e: the doorbell's counts once more (a self test stops at ~25 s) */

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

/*
 * A stop the host asked for (hdr.host_state STOPPING, MSG_STOP) that user space does not carry out (no sbwl / guestd
 * yet, or one that hangs: cp3 2026-11-12, a start stuck before sbwl): 10 s after the doorbell that brought it, the
 * kernel powers off by itself (orderly_poweroff(true): with no /sbin/poweroff in the base, an emergency sync and
 * kernel_power_off, so sbg_power_off takes the M3 cluster down). The host then sees the cluster off and may start
 * the guest again without a reset.
 */
/* hdr.guest_load every 5 s: the CPU time of all guest CPUs (user, nice, system, irq, softirq) in % of one core */
#define SBG_LOAD_PERIOD_S 5
static u64 sbg_load_last;
static struct timer_list sbg_load_timer;
static u64 sbg_busy_ns(void)
{
	u64 t = 0;
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		const u64 *c = kcpustat_cpu(cpu).cpustat;

		t += c[CPUTIME_USER] + c[CPUTIME_NICE] + c[CPUTIME_SYSTEM] + c[CPUTIME_IRQ] + c[CPUTIME_SOFTIRQ];
	}
	return t;
}
static void sbg_load_tick(struct timer_list *t)
{
	u64 now = sbg_busy_ns(), d = now - sbg_load_last;

	sbg_load_last = now;
	WRITE_ONCE(hdr->guest_load, (u32)min_t(u64, div_u64(d, SBG_LOAD_PERIOD_S * 10000000ULL), 400));
	mod_timer(&sbg_load_timer, jiffies + SBG_LOAD_PERIOD_S * HZ);
}

#define SBG_STOP_GRACE_S 10
static void sbg_stop_fallback(struct work_struct *w)
{
	if (READ_ONCE(hdr->host_state) != SBG_HOST_STOPPING)
		return;
	pr_warn("sbguest: the host asked to stop %d s ago and user space has not powered off: powering off now\n",
		SBG_STOP_GRACE_S);
	orderly_poweroff(true);
}
static DECLARE_DELAYED_WORK(sbg_stop_work, sbg_stop_fallback);
static atomic_t sbg_stop_armed = ATOMIC_INIT(0);

/* ---- instant start (2026-11-13): freeze / thaw on the host's hdr.host_freeze --------------------------------
 * Starbite OS stopped Linux 60 s after it was hidden, so nearly every open was a cold boot (~20-30 s on the phone).
 * Now the host freezes it instead: this work takes CPUs 1..n offline (PSCI CPU_OFF, as at power off), says
 * guest_frozen and suspends to idle (tasks frozen, the file systems synced first, the ticks stopped, CPU 0 in WFI);
 * the doorbell (IRQF_NO_SUSPEND) wakes it when host_freeze goes 0 or the host stops it; then the CPUs come back
 * (PSCI CPU_ON through the trampoline, as at boot) and guest_frozen goes 0. The carve-out stays as it is. */
#ifdef CONFIG_SUSPEND
static bool freeze_offline = true;
module_param(freeze_offline, bool, 0644);
MODULE_PARM_DESC(freeze_offline, "take CPUs 1..n offline while frozen (default 1)");
static atomic_t sbg_freeze_busy = ATOMIC_INIT(0);
static bool sbg_in_suspend;
static bool sbg_freeze_wanted(void)
{
	return READ_ONCE(hdr->host_freeze) && READ_ONCE(hdr->host_state) != SBG_HOST_STOPPING;
}
static void sbg_freeze_fn(struct work_struct *w)
{
	unsigned long offl = 0;
	unsigned int cpu, n = 0, fails = 0;
	u64 t0;
	int r;

	do {
		if (freeze_offline) {
			unsigned long want = 0;

			for_each_online_cpu(cpu)
				if (cpu && cpu < BITS_PER_LONG)
					want |= BIT(cpu);
			for_each_set_bit(cpu, &want, BITS_PER_LONG)
				if (!remove_cpu(cpu))
					offl |= BIT(cpu);
		}
		pr_info("sbguest: freezing (the host asked; CPUs offline %#lx)\n", offl);
		WRITE_ONCE(hdr->guest_frozen, 1);
		sbg_ring_host();
		while (sbg_freeze_wanted()) {
			WRITE_ONCE(sbg_in_suspend, true);
			r = pm_suspend(PM_SUSPEND_TO_IDLE);
			WRITE_ONCE(sbg_in_suspend, false);
			n++;
			if (r) {	/* a task that would not freeze, a wakeup during the freeze: again soon */
				if (fails++ < 4)
					pr_warn("sbguest: suspend to idle: %d (again)\n", r);
				msleep(200);
			}
		}
		t0 = ktime_get_ns();
		for_each_set_bit(cpu, &offl, BITS_PER_LONG)
			if (add_cpu(cpu))
				pr_warn("sbguest: CPU %u did not come back\n", cpu);
		offl = 0;
		WRITE_ONCE(hdr->guest_frozen, 0);
		sbg_ring_host();
		pr_info("sbguest: thawed after %u suspend(s); CPUs back in %llu us\n", n,
			div_u64(ktime_get_ns() - t0, 1000));
		atomic_set(&sbg_freeze_busy, 0);
		/* the host may have asked again while the CPUs came back */
	} while (sbg_freeze_wanted() && !atomic_xchg(&sbg_freeze_busy, 1));
}
static DECLARE_WORK(sbg_freeze_work, sbg_freeze_fn);
static void sbg_freeze_db(void)
{
	if (READ_ONCE(sbg_db_bad))
		return;				/* 8e: no freeze without a doorbell to thaw it (sbg_db_lost_fn) */
	if (sbg_freeze_wanted()) {
		if (!atomic_xchg(&sbg_freeze_busy, 1))
			queue_work(system_unbound_wq, &sbg_freeze_work);
	} else if (READ_ONCE(sbg_in_suspend)) {
		pm_system_wakeup();		/* thaw (or the host stops it): out of suspend to idle */
	}
}
#else
static void sbg_freeze_db(void) { }
#endif

/* ---- /dev/sbshm ------------------------------------------------------------------------------------ */
static void sbblk_kick(void);

/* what a doorbell does (the interrupt, or the soft doorbell below when the interrupt does not come) */
static void sbg_db_serve(bool wake_user)
{
	if (wake_user) {
		atomic_inc(&db_count);
		wake_up_interruptible(&db_wait);
	}
	sbblk_kick();				/* the doorbell is shared: completions may be waiting */
	sbnet_kick();				/* v3: each service checks its own rings */
	sbp9_kick();
	sbsnd_kick();
	sbgpu_kick();
	if (READ_ONCE(hdr->host_state) == SBG_HOST_STOPPING && !atomic_xchg(&sbg_stop_armed, 1))
		schedule_delayed_work(&sbg_stop_work, SBG_STOP_GRACE_S * HZ);
	sbg_freeze_db();
}

static irqreturn_t sbg_db_irq(int irq, void *dev)
{
	atomic_inc(&sbg_db_on_cpu[raw_smp_processor_id() & 7]);
	sbg_db_serve(true);
	return IRQ_HANDLED;
}

/* 8e (Track E, Track A's ask): in some phone boots the host's doorbell never reaches Linux (sbblk_timer_fn). Once
 * sbblk's counts say so, Linux (a) stops offering the freeze: a frozen Linux wakes only by that doorbell, and one
 * that cannot thaw is worse than one that stays awake (the host's sbg_freeze then returns -1 and Starbite stops an
 * unused Linux as before the instant start), and (b) serves itself a soft doorbell every 20 ms: the host's
 * messages (a new entry in the host -> guest ring wakes /dev/sbshm's readers, sbwl), its stop, and the services'
 * rings; user space is also woken every 250 ms in case the host changed something else. */
#define SBG_SOFT_DB_MS 20
#define SBG_SOFT_WAKE_MS 250
static struct timer_list sbg_soft_db;
static const volatile u32 *sbg_h2g_head;
static u32 sbg_soft_head;
static unsigned long sbg_soft_woke;
static void sbg_soft_db_fn(struct timer_list *t)
{
	u32 h = sbg_h2g_head ? READ_ONCE(*sbg_h2g_head) : 0;
	bool wake = h != sbg_soft_head || READ_ONCE(hdr->host_state) == SBG_HOST_STOPPING ||
		    time_after(jiffies, sbg_soft_woke + msecs_to_jiffies(SBG_SOFT_WAKE_MS));

	sbg_soft_head = h;
	if (wake)
		sbg_soft_woke = jiffies;
	sbg_db_serve(wake);
	mod_timer(&sbg_soft_db, jiffies + max(1UL, msecs_to_jiffies(SBG_SOFT_DB_MS)));
}
static void sbg_db_lost_fn(struct work_struct *w)
{
	pr_warn("sbguest: the host's doorbell does not reach Linux (sbblk: %d of %d completions found by the timer): no freeze offered (an unused Linux is stopped instead), a soft doorbell every %d ms\n",
		atomic_read(&sbblk_found_by_timer), atomic_read(&sbblk_completions), SBG_SOFT_DB_MS);
	WRITE_ONCE(hdr->guest_caps, READ_ONCE(hdr->guest_caps) & ~SBSHM_CAP_FREEZE);
	sbg_h2g_head = sbg_vmap_wc(SBSHM_H2G_OFF, PAGE_SIZE);
	timer_setup(&sbg_soft_db, sbg_soft_db_fn, 0);
	sbg_soft_woke = jiffies;
	mod_timer(&sbg_soft_db, jiffies + 1);
}
static DECLARE_WORK(sbg_db_lost_work, sbg_db_lost_fn);

static int sbg_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;	/* a window of the region (sbstore maps the store's part) */

	if (off >= shm_size || len > shm_size - off)
		return -EINVAL;
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, PHYS_PFN(shm_base + off), len, vma->vm_page_prot);
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

/* ---- sbblk: the host's block devices (sbshm.h "v2: sbblk") -------------------------------------------
 * One gendisk per PRESENT device, one tag set (64 tags) shared by all of them: at most SBSHM_BLK_SLOTS
 * requests are in flight, so every request gets a data slot and the 64-entry rings can never overflow.
 * A request is at most one slot (max_hw_sectors = SBSHM_BLK_SLOT / 512): the block layer does the splitting.
 * The logical block size is 4096, so every request is whole 4 KiB blocks (file systems on these devices need
 * 4 KiB blocks: EROFS's default, mkfs.ext4 -b 4096).
 *
 * Submit (any context, sbblk_lock): take a free slot from the bitmap, copy a write's payload into it, write the
 * request entry, dmb, req_head (sbshm.h's order: payload, barrier, index), then pend the host's doorbell.
 * Reap (one work item, so the completion ring has one consumer; kicked by the doorbell interrupt and by a
 * timer): read done_head, dmb, copy the entry, copy a read's payload out of the slot, free the slot, dmb,
 * done_tail, end the request. The timer is the only wake-up when there is no doorbell (every jiffy while
 * requests are in flight) and a safety net when there is one (every jiffy too since 8e: sbblk_timer_fn).
 * The id of a request is its slot plus a per-slot generation in the high bits: a completion for a slot that is
 * free, or with an old generation, is logged and dropped.
 */
#define SBBLK_SLOT_MASK	0xffU

struct sbblk_dev {
	struct gendisk *disk;
	u32 idx;
};

struct sbblk_cmd {
	u32 id;
};

static struct sbshm_blk *sbblk;			/* write-combine vmap: devices + rings */
static u8 *sbblk_data;				/* write-combine vmap: the data window */
static struct blk_mq_tag_set sbblk_set;
static struct sbblk_dev sbblk_devs[SBSHM_BLK_DEVS];
static DEFINE_SPINLOCK(sbblk_lock);		/* the slots and the request ring (producer) */
static DECLARE_BITMAP(sbblk_busy, SBSHM_BLK_SLOTS);
static struct request *sbblk_rq[SBSHM_BLK_SLOTS];
static u32 sbblk_gen[SBSHM_BLK_SLOTS];
static u32 sbblk_req_head, sbblk_done_tail;	/* this side's own indices (never read back from the region) */
static struct workqueue_struct *sbblk_wq;
static struct timer_list sbblk_timer;
static unsigned long sbblk_period = 1;		/* jiffies between timer polls */
static bool sbblk_ready, sbblk_polled;
static int sbblk_major;

static void sbblk_reap(struct work_struct *w);

/*
 * Every power off or restart (sbwl's MSG_STOP, the stop fallback, a reboot from user space) ends with the overlay's
 * ext4 frozen: written out and its journal clean, as at an unmount (Track F, cp3 2026-11-12: after an unclean stop a
 * file of apt's came back unreadable). Nothing writes after it: the cluster goes off next.
 */
static void sbblk_freeze_overlay(void)
{
	struct gendisk *d = sbblk_ready ? sbblk_devs[SBSHM_BLK_OVERLAY].disk : NULL;
	int r;

	if (!d)
		return;
	r = bdev_freeze(d->part0);
	pr_info("sbguest: the overlay (%s) %s\n", d->disk_name, r ? "could not be frozen" : "written out and frozen");
}
static DECLARE_WORK(sbblk_work, sbblk_reap);

static void sbblk_kick(void)
{
	if (READ_ONCE(sbblk_ready))
		queue_work(sbblk_wq, &sbblk_work);
}

/* 8e (Track E): on the phone some boots take ~50 ms per block read (the steps of the initramfs in multiples of
 * ~52 ms: 106, 262, 314 ms; the same steps 11-57 ms in other boots of the same build), as if the completions were
 * found by this timer (then every HZ / 20) and not by the doorbell. The timer now polls every jiffy while requests
 * are in flight, doorbell or not, and counts how often it finds completions waiting (sbblk_report at 15 s): */

static void sbblk_timer_fn(struct timer_list *t)
{
	static int win_done, win_found;

	if (READ_ONCE(sbblk->done_head) != READ_ONCE(sbblk_done_tail)) {
		int done = atomic_read(&sbblk_completions), found = atomic_inc_return(&sbblk_found_by_timer);

		/* the doorbell lost: over the last 64+ completions more than half were found here (QEMU: ~8 % with
		 * the doorbell, ~93 % without); a window, so a doorbell lost later is seen too */
		if (done - win_done >= 64) {
			if (!sbblk_polled && !sbg_db_bad && 2 * (found - win_found) > done - win_done) {
				WRITE_ONCE(sbg_db_bad, true);
				schedule_work(&sbg_db_lost_work);
			}
			win_done = done;
			win_found = found;
		}
	}
	sbblk_kick();
	if (!bitmap_empty(sbblk_busy, SBSHM_BLK_SLOTS))
		mod_timer(&sbblk_timer, jiffies + sbblk_period);
}

static void sbblk_slot_free(unsigned int slot)
{
	unsigned long flags;

	spin_lock_irqsave(&sbblk_lock, flags);
	sbblk_rq[slot] = NULL;
	__clear_bit(slot, sbblk_busy);
	spin_unlock_irqrestore(&sbblk_lock, flags);
}

static blk_status_t sbblk_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct sbblk_dev *d = rq->q->queuedata;
	struct sbblk_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct sbshm_blkreq r = { .dev = d->idx };
	struct req_iterator it;
	struct bio_vec bv;
	unsigned long flags;
	unsigned int slot, off = 0;
	bool full;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		r.op = SBG_BLK_READ;
		break;
	case REQ_OP_WRITE:
		r.op = SBG_BLK_WRITE;
		break;
	case REQ_OP_FLUSH:
		r.op = SBG_BLK_FLUSH;
		break;
	default:
		return BLK_STS_NOTSUPP;
	}
	if (r.op != SBG_BLK_FLUSH) {
		if (blk_rq_bytes(rq) > SBSHM_BLK_SLOT)		/* the queue limits forbid it */
			return BLK_STS_IOERR;
		r.sector = blk_rq_pos(rq);
		r.count = blk_rq_sectors(rq);
	}
	spin_lock_irqsave(&sbblk_lock, flags);
	slot = find_first_zero_bit(sbblk_busy, SBSHM_BLK_SLOTS);
	if (slot >= SBSHM_BLK_SLOTS) {
		spin_unlock_irqrestore(&sbblk_lock, flags);
		return BLK_STS_RESOURCE;			/* blk-mq runs the queue again */
	}
	__set_bit(slot, sbblk_busy);
	sbblk_rq[slot] = rq;
	cmd->id = r.id = slot | (++sbblk_gen[slot] << 8);
	spin_unlock_irqrestore(&sbblk_lock, flags);
	r.data_slot = slot;

	blk_mq_start_request(rq);
	if (r.op == SBG_BLK_WRITE)				/* the payload first (the slot is ours alone) */
		rq_for_each_segment(bv, rq, it) {
			memcpy_from_bvec(sbblk_data + (size_t)slot * SBSHM_BLK_SLOT + off, &bv);
			off += bv.bv_len;
		}

	spin_lock_irqsave(&sbblk_lock, flags);
	full = sbblk_req_head - READ_ONCE(sbblk->req_tail) >= SBSHM_BLK_RING;
	if (!full) {
		memcpy(&sbblk->req[sbblk_req_head & (SBSHM_BLK_RING - 1)], &r, sizeof(r));
		sbshm_dmb();
		WRITE_ONCE(sbblk->req_head, ++sbblk_req_head);
		sbshm_dmb();
	}
	spin_unlock_irqrestore(&sbblk_lock, flags);
	if (full) {
		/* 64 slots, 64 entries: only a host that completes without advancing req_tail gets here */
		pr_err_ratelimited("sbblk: the request ring is full (the host's req_tail stands still)\n");
		sbblk_slot_free(slot);
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}
	if (!timer_pending(&sbblk_timer))
		mod_timer(&sbblk_timer, jiffies + sbblk_period);
	if (bd->last)
		sbg_ring_host();
	return BLK_STS_OK;
}

static void sbblk_commit_rqs(struct blk_mq_hw_ctx *hctx)
{
	sbg_ring_host();
}

static enum blk_eh_timer_return sbblk_timeout(struct request *rq)
{
	struct sbblk_dev *d = rq->q->queuedata;
	struct sbblk_cmd *cmd = blk_mq_rq_to_pdu(rq);

	/* the slot stays the host's until it completes the request: nothing to take back, keep waiting */
	pr_warn_ratelimited("sbblk%u: request %#x (%s sector %llu) waits on the host: req %u/%u done %u/%u\n",
			    d->idx, cmd->id, blk_op_str(req_op(rq)), (u64)blk_rq_pos(rq), sbblk_req_head,
			    READ_ONCE(sbblk->req_tail), READ_ONCE(sbblk->done_head), sbblk_done_tail);
	sbg_ring_host();
	sbblk_kick();
	return BLK_EH_RESET_TIMER;
}

static void sbblk_reap(struct work_struct *w)
{
	for (;;) {
		struct sbshm_blkreq c;
		struct request *rq = NULL;
		struct req_iterator it;
		struct bio_vec bv;
		unsigned int slot, off = 0;
		blk_status_t sts = BLK_STS_OK;

		if (READ_ONCE(sbblk->done_head) == sbblk_done_tail)
			break;
		sbshm_dmb();
		memcpy(&c, &sbblk->done[sbblk_done_tail & (SBSHM_BLK_RING - 1)], sizeof(c));
		slot = c.id & SBBLK_SLOT_MASK;
		if (slot < SBSHM_BLK_SLOTS)
			rq = READ_ONCE(sbblk_rq[slot]);
		if (rq && ((struct sbblk_cmd *)blk_mq_rq_to_pdu(rq))->id != c.id)
			rq = NULL;
		if (!rq) {
			pr_err_ratelimited("sbblk: completion for request %#x that is not in flight\n", c.id);
		} else {
			if (c.status > 0 || c.status < -MAX_ERRNO)
				c.status = -EIO;
			sts = errno_to_blk_status(c.status);	/* -EROFS and the like: BLK_STS_IOERR */
			if (c.status)
				pr_err_ratelimited("sbblk: %s sector %llu + %u: the host says %d\n",
						   blk_op_str(req_op(rq)), (u64)blk_rq_pos(rq), blk_rq_sectors(rq),
						   c.status);
			else if (req_op(rq) == REQ_OP_READ)	/* the host filled the slot before the completion */
				rq_for_each_segment(bv, rq, it) {
					memcpy_to_bvec(&bv, sbblk_data + (size_t)slot * SBSHM_BLK_SLOT + off);
					off += bv.bv_len;
				}
			sbblk_slot_free(slot);
		}
		sbshm_dmb();
		WRITE_ONCE(sbblk->done_tail, ++sbblk_done_tail);
		atomic_inc(&sbblk_completions);
		if (rq)
			blk_mq_end_request(rq, sts);
		cond_resched();
	}
}

static const struct blk_mq_ops sbblk_mq_ops = {
	.queue_rq = sbblk_queue_rq,
	.commit_rqs = sbblk_commit_rqs,
	.timeout = sbblk_timeout,
};

static const struct block_device_operations sbblk_fops = {
	.owner = THIS_MODULE,
};

/* a write-combine mapping of part of the shared region (RAM reserved without no-map: ioremap refuses it) */
void *sbg_vmap_wc(u64 off, size_t len)
{
	unsigned int i, n = PAGE_ALIGN(len) >> PAGE_SHIFT;
	struct page **pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	void *p;

	if (!pages)
		return NULL;
	for (i = 0; i < n; i++)
		pages[i] = pfn_to_page(PHYS_PFN(shm_base + off) + i);
	p = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kvfree(pages);
	return p;
}

/* 8e: one line 15 s after sbblk starts (a slow cold start's HELLO was at ~12 s) and again at the power off: the completions, how many the timer
 * found before the doorbell's work did, the doorbell's interrupts per CPU and its line as the distributor has it */
static void sbblk_report(struct work_struct *w)
{
	char cpus[64];
	unsigned int i, n = 0, hw = 0;
	u8 tgt = 0;
	int en = -1, pend = -1;
	struct irq_data *d = sbg_db_irq_nr > 0 ? irq_get_irq_data(sbg_db_irq_nr) : NULL;

	if (!READ_ONCE(sbblk_ready))
		return;
	cpus[0] = 0;
	for (i = 0; i < 8 && n < sizeof(cpus) - 12; i++)
		if (atomic_read(&sbg_db_on_cpu[i]))
			n += scnprintf(cpus + n, sizeof(cpus) - n, "%s%u:%d", n ? " " : "", i,
				       atomic_read(&sbg_db_on_cpu[i]));
	if (d)
		hw = (unsigned int)irqd_to_hwirq(d);
	if (gicd && hw >= 32 && hw < 1020) {
		tgt = readb(gicd + 0x800 + hw);
		en = (readl(gicd + 0x100 + 4 * (hw / 32)) >> (hw % 32)) & 1;
		pend = (readl(gicd + 0x200 + 4 * (hw / 32)) >> (hw % 32)) & 1;
	}
	pr_info("sbguest: sbblk: %d completions, %d found by the timer first; doorbell INTID %u: interrupts by CPU %s, target %#x, enabled %d, pending %d\n",
		atomic_read(&sbblk_completions), atomic_read(&sbblk_found_by_timer), hw, n ? cpus : "none",
		tgt, en, pend);
}
static DECLARE_DELAYED_WORK(sbblk_report_work, sbblk_report);

static void sbblk_init(struct device *dev, bool irq_ok)
{
	struct sbshm_blkdev bd[SBSHM_BLK_DEVS];
	unsigned int i, n = 0;
	int ret;

	if (READ_ONCE(hdr->version) < SBSHM_VERSION_V2)
		return;
	BUILD_BUG_ON(SBSHM_BLK_SLOTS > SBBLK_SLOT_MASK + 1 || SBSHM_BLK_SLOTS > SBSHM_BLK_RING);
	BUILD_BUG_ON(SBSHM_BLK_OFF + sizeof(struct sbshm_blk) > SBSHM_BLKDATA_OFF);
	if (shm_size < SBSHM_BLKDATA_OFF + (u64)SBSHM_BLK_SLOTS * SBSHM_BLK_SLOT) {
		dev_warn(dev, "sbblk: the shared region is too small for the block window\n");
		return;
	}
	sbblk = sbg_vmap_wc(SBSHM_BLK_OFF, sizeof(*sbblk));
	if (!sbblk)
		return;
	memcpy(bd, sbblk->dev, sizeof(bd));
	for (i = 0; i < SBSHM_BLK_DEVS; i++)
		n += (bd[i].flags & SBSHM_BLKDEV_PRESENT) && bd[i].sectors;
	if (!n) {
		dev_info(dev, "sbblk: the host offers no block device\n");
		return;
	}
	sbblk_data = sbg_vmap_wc(SBSHM_BLKDATA_OFF, (size_t)SBSHM_BLK_SLOTS * SBSHM_BLK_SLOT);
	sbblk_wq = alloc_workqueue("sbblk", WQ_MEM_RECLAIM | WQ_HIGHPRI, 1);
	if (!sbblk_data || !sbblk_wq)
		return;
	sbblk_major = register_blkdev(0, "sbblk");
	if (sbblk_major < 0)
		return;
	sbblk_set.ops = &sbblk_mq_ops;
	sbblk_set.nr_hw_queues = 1;
	sbblk_set.queue_depth = SBSHM_BLK_SLOTS;
	sbblk_set.numa_node = NUMA_NO_NODE;
	sbblk_set.cmd_size = sizeof(struct sbblk_cmd);
	ret = blk_mq_alloc_tag_set(&sbblk_set);
	if (ret) {
		dev_warn(dev, "sbblk: tag set: %d\n", ret);
		return;
	}
	/* completions: the doorbell interrupt when both sides have one, else the timer alone */
	sbblk_polled = !irq_ok || !READ_ONCE(hdr->db_to_guest) || !READ_ONCE(hdr->db_to_host);
	sbblk_period = 1;			/* 8e: every jiffy (was HZ / 20 with a doorbell): see sbblk_timer_fn */
	timer_setup(&sbblk_timer, sbblk_timer_fn, 0);
	WRITE_ONCE(sbblk_ready, true);
	schedule_delayed_work(&sbblk_report_work, 15 * HZ);
	for (i = 0; i < SBSHM_BLK_DEVS; i++) {
		bool ro = bd[i].flags & SBSHM_BLKDEV_RO;
		struct queue_limits lim = {
			/* whole 4 KiB blocks only: the host's storage (the store, UFS, the card) is in 4 KiB
			 * blocks and must never read-modify-write for us; sector / count stay in 512s */
			.logical_block_size = 4096,
			.physical_block_size = 4096,
			.io_min = 4096,
			/* read-ahead of 1 MiB (2 x io_opt): a sequential reader keeps about 16 requests in
			 * flight, so the host's poll (8 requests by default) always finds a full batch */
			.io_opt = 8 * SBSHM_BLK_SLOT,
			.max_hw_sectors = SBSHM_BLK_SLOT >> SECTOR_SHIFT,
			.features = ro ? 0 : BLK_FEAT_WRITE_CACHE,	/* REQ_OP_FLUSH -> SBG_BLK_FLUSH */
		};
		struct sbblk_dev *d = &sbblk_devs[i];
		struct gendisk *disk;

		if (!(bd[i].flags & SBSHM_BLKDEV_PRESENT) || !bd[i].sectors)
			continue;
		d->idx = i;
		disk = blk_mq_alloc_disk(&sbblk_set, &lim, d);
		if (IS_ERR(disk)) {
			dev_warn(dev, "sbblk%u: %ld\n", i, PTR_ERR(disk));
			continue;
		}
		disk->major = sbblk_major;
		disk->first_minor = i;
		disk->minors = 1;
		disk->flags |= GENHD_FL_NO_PART;
		disk->fops = &sbblk_fops;
		disk->private_data = d;
		snprintf(disk->disk_name, sizeof(disk->disk_name), "sbblk%u", i);
		set_capacity(disk, bd[i].sectors & ~7ULL);
		set_disk_ro(disk, ro);
		ret = add_disk(disk);
		if (ret) {
			dev_warn(dev, "sbblk%u: add_disk: %d\n", i, ret);
			put_disk(disk);
			continue;
		}
		d->disk = disk;
		bd[i].name[sizeof(bd[i].name) - 1] = 0;
		dev_info(dev, "sbblk%u: \"%s\", %llu MiB, %s, completions by %s\n", i, bd[i].name,
			 bd[i].sectors >> 11, ro ? "read-only" : "read-write",
			 sbblk_polled ? "timer poll" : "doorbell");
	}
}

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
	bool irq_ok = false;

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
		/* IRQF_NO_SUSPEND: the doorbell also thaws a guest suspended to idle (instant start) */
		ret = devm_request_irq(&pdev->dev, irq, sbg_db_irq, IRQF_NO_SUSPEND, "sbshm-doorbell", NULL);
		if (ret)
			dev_warn(&pdev->dev, "doorbell irq %d: %d (polling)\n", irq, ret);
		irq_ok = !ret;
		if (irq_ok)
			sbg_db_irq_nr = irq;
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
	sbblk_init(&pdev->dev, irq_ok);		/* contract v2; the module works without it */
	/* contract v3: each runs only when the host says it serves it (hdr.host_services) */
	sbnet_init(&pdev->dev, irq_ok);
	sbp9_init(&pdev->dev, irq_ok);
	sbsnd_init(&pdev->dev, irq_ok);
	sbgpu_init(&pdev->dev, irq_ok);
#ifdef CONFIG_SUSPEND
	if (irq_ok)
		WRITE_ONCE(hdr->guest_caps, READ_ONCE(hdr->guest_caps) | SBSHM_CAP_FREEZE);
#endif
	sbg_load_last = sbg_busy_ns();
	timer_setup(&sbg_load_timer, sbg_load_tick, 0);
	mod_timer(&sbg_load_timer, jiffies + SBG_LOAD_PERIOD_S * HZ);
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
static int __init sbg_init(void)
{
	return platform_driver_register(&sbg_driver);
}
module_init(sbg_init);

static void __exit sbg_exit(void)
{
	if (sbg_load_timer.function)
		timer_shutdown_sync(&sbg_load_timer);
	cancel_delayed_work_sync(&sbg_stop_work);
	cancel_delayed_work_sync(&sbblk_report_work);
	cancel_work_sync(&sbg_db_lost_work);
	if (sbg_soft_db.function)
		timer_shutdown_sync(&sbg_soft_db);
#ifdef CONFIG_SUSPEND
	cancel_work_sync(&sbg_freeze_work);
#endif
	sbgpu_exit();
	sbp9_exit();				/* the 9p transport is registered with 9pnet: take it back */
	platform_driver_unregister(&sbg_driver);
}
module_exit(sbg_exit);

MODULE_DESCRIPTION("Starbite OS guest: shared region, doorbells, power off without SYSTEM_OFF, sbblk, sbnet, sbp9, sbsnd");
MODULE_LICENSE("GPL");
