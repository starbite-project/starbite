// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbp9: the 9p transport "sbshm" for Starbite OS's files (contract v3, ../sbshm-p9.h; the server is
 * host/sbg_p9.c in Starbite OS). Only when the host serves it (hdr.host_services & SBSHM_SVC_P9):
 *     mount -t 9p -o trans=sbshm,version=9p2000.L,msize=131072 starbite /home/user/Starbite
 * (add -o ro when /sys/module/sbguest/parameters/p9 says "ro": the host's files are read-only; rootfs/sbp9-mount
 * does all that). The host runs ONE 9P session, so one mount at a time (a second gets EBUSY).
 *
 * The channel has SBSHM_P9_SLOTS slots of SBSHM_P9_SLOT bytes (= the transport's maxsize, the largest msize). A
 * request takes a free slot (or waits, killable, for one), copies its T-message in, writes the request entry, dmb,
 * req_head, and pends the host's doorbell. The host answers in the same slot. Completions are reaped by one work
 * item (kicked by the doorbell interrupt and by a timer: every jiffy while requests are in flight when there is no
 * doorbell, every 50 ms as a safety net when there is one): read done_head, dmb, the entry, copy the R-message into
 * the request's buffer, free the slot, dmb, done_tail, then p9_client_cb. The transport keeps the request's second
 * reference while it is in flight, as virtio does (p9_client_cb drops it). A request the host never answers keeps
 * its slot; cancel says "flush it" (the host answers in order, so the answer comes first).
 */
#include <linux/bitmap.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>
#include <net/9p/transport.h>

#include "../sbshm-p9.h"
#include "sbg_core.h"


#define SBP9_SLOT_MASK	0xffU

static struct sbshm_p9 *sbp9_ch;		/* write-combine vmap: config + rings */
static u8 *sbp9_slots;				/* write-combine vmap: the slots */
static struct p9_client *sbp9_client;		/* the one mount */
static DEFINE_SPINLOCK(sbp9_lock);		/* the slots, the request ring (producer), sbp9_client */
static DECLARE_WAIT_QUEUE_HEAD(sbp9_free_wq);	/* requests waiting for a free slot */
static DECLARE_BITMAP(sbp9_busy, SBSHM_P9_SLOTS);
static struct p9_req_t *sbp9_req[SBSHM_P9_SLOTS];	/* NULL in a busy slot: detached (closed), still the host's */
static struct p9_client *sbp9_cl[SBSHM_P9_SLOTS];
static u32 sbp9_id[SBSHM_P9_SLOTS], sbp9_gen[SBSHM_P9_SLOTS];
static u32 sbp9_req_head, sbp9_done_tail;	/* this side's own indices (never read back from the region) */
static struct workqueue_struct *sbp9_wq;
static struct timer_list sbp9_timer;
static unsigned long sbp9_period = 1;
static bool sbp9_ready, sbp9_polled, sbp9_registered;

/* what user space sees (rootfs/sbp9-mount): "off" (no service), "ro" (read-only files), "rw" */
static char sbp9_state[8] = "off";
module_param_string(p9, sbp9_state, sizeof(sbp9_state), 0444);
MODULE_PARM_DESC(p9, "sbp9 (Starbite's files over 9P): off, ro or rw (read-only)");

/* /sys/module/sbguest/parameters/p9_ring: the channel's state, for a hang's post-mortem */
static int sbp9_ring_get(char *buf, const struct kernel_param *kp)
{
	if (!READ_ONCE(sbp9_ready))
		return sysfs_emit(buf, "off\n");
	return sysfs_emit(buf, "busy %*pb req %u/%u done %u/%u client %d\n", SBSHM_P9_SLOTS, sbp9_busy,
			  sbp9_req_head, READ_ONCE(sbp9_ch->req_tail), READ_ONCE(sbp9_ch->done_head), sbp9_done_tail,
			  sbp9_client != NULL);
}
static const struct kernel_param_ops sbp9_ring_ops = { .get = sbp9_ring_get };
module_param_cb(p9_ring, &sbp9_ring_ops, NULL, 0444);
MODULE_PARM_DESC(p9_ring, "sbp9's channel: busy slots, request / completion indices (read only)");

static void sbp9_reap(struct work_struct *w);
static DECLARE_WORK(sbp9_work, sbp9_reap);

void sbp9_kick(void)
{
	if (READ_ONCE(sbp9_ready))
		queue_work(sbp9_wq, &sbp9_work);
}

static void sbp9_timer_fn(struct timer_list *t)
{
	sbp9_kick();
	if (!bitmap_empty(sbp9_busy, SBSHM_P9_SLOTS))
		mod_timer(&sbp9_timer, jiffies + sbp9_period);
}

static int sbp9_take_slot(void)
{
	unsigned long flags;
	unsigned int s;

	spin_lock_irqsave(&sbp9_lock, flags);
	s = find_first_zero_bit(sbp9_busy, SBSHM_P9_SLOTS);
	if (s < SBSHM_P9_SLOTS)
		__set_bit(s, sbp9_busy);
	spin_unlock_irqrestore(&sbp9_lock, flags);
	return s < SBSHM_P9_SLOTS ? (int)s : -1;
}

static void sbp9_free_slot(unsigned int slot)
{
	unsigned long flags;

	spin_lock_irqsave(&sbp9_lock, flags);
	sbp9_req[slot] = NULL;
	sbp9_cl[slot] = NULL;
	__clear_bit(slot, sbp9_busy);
	spin_unlock_irqrestore(&sbp9_lock, flags);
	wake_up(&sbp9_free_wq);
}

static int sbp9_request(struct p9_client *client, struct p9_req_t *req)
{
	struct sbshm_p9_ent e;
	unsigned long flags;
	int slot = -1, err;
	bool full;

	if (req->tc.size < 7 || req->tc.size > SBSHM_P9_SLOT)	/* msize <= maxsize: cannot happen */
		return -EINVAL;
	err = wait_event_killable(sbp9_free_wq, (slot = sbp9_take_slot()) >= 0);
	if (err)
		return err;				/* -ERESTARTSYS: nothing was sent */
	memcpy(sbp9_slots + (size_t)slot * SBSHM_P9_SLOT, req->tc.sdata, req->tc.size);
	WRITE_ONCE(req->status, REQ_STATUS_SENT);

	spin_lock_irqsave(&sbp9_lock, flags);
	sbp9_req[slot] = req;
	sbp9_cl[slot] = client;
	e.id = sbp9_id[slot] = slot | (++sbp9_gen[slot] << 8);
	e.slot = slot;
	e.len = req->tc.size;
	e.status = 0;
	full = sbp9_req_head - READ_ONCE(sbp9_ch->req_tail) >= SBSHM_P9_RING;
	if (!full) {				/* the slot's payload, then the entry, then the index */
		sbshm_dmb();
		memcpy(&sbp9_ch->req[sbp9_req_head & (SBSHM_P9_RING - 1)], &e, sizeof(e));
		sbshm_dmb();
		WRITE_ONCE(sbp9_ch->req_head, ++sbp9_req_head);
		sbshm_dmb();
	}
	spin_unlock_irqrestore(&sbp9_lock, flags);
	if (full) {
		/* SBSHM_P9_SLOTS slots, SBSHM_P9_RING entries: only a host that stopped advancing req_tail gets here */
		pr_err_ratelimited("sbp9: the request ring is full (the host's req_tail stands still)\n");
		sbp9_free_slot(slot);
		return -EIO;
	}
	if (!timer_pending(&sbp9_timer))
		mod_timer(&sbp9_timer, jiffies + sbp9_period);
	sbg_ring_host();
	return 0;
}

static void sbp9_reap(struct work_struct *w)
{
	for (;;) {
		struct sbshm_p9_ent c;
		struct p9_req_t *req = NULL;
		struct p9_client *cl = NULL;
		unsigned long flags;
		unsigned int slot;
		int status = REQ_STATUS_RCVD;
		bool ours = false;

		if (READ_ONCE(sbp9_ch->done_head) == sbp9_done_tail)
			break;
		sbshm_dmb();
		memcpy(&c, &sbp9_ch->done[sbp9_done_tail & (SBSHM_P9_RING - 1)], sizeof(c));
		slot = c.id & SBP9_SLOT_MASK;
		spin_lock_irqsave(&sbp9_lock, flags);
		if (slot < SBSHM_P9_SLOTS && test_bit(slot, sbp9_busy) && sbp9_id[slot] == c.id) {
			ours = true;
			req = sbp9_req[slot];		/* taken: close cannot complete it too */
			cl = sbp9_cl[slot];
			sbp9_req[slot] = NULL;
		}
		spin_unlock_irqrestore(&sbp9_lock, flags);
		if (!ours) {
			pr_err_ratelimited("sbp9: completion %#x for a request that is not in flight\n", c.id);
		} else {
			if (req && !c.status && c.len >= 7 && c.len <= SBSHM_P9_SLOT && c.len <= req->rc.capacity) {
				sbshm_dmb();
				memcpy(req->rc.sdata, sbp9_slots + (size_t)slot * SBSHM_P9_SLOT, c.len);
				req->rc.size = c.len;
			} else if (req) {
				pr_err_ratelimited("sbp9: tag %u: the host answered status %d, %u bytes (room %zu)\n",
						   req->tc.tag, c.status, c.len, req->rc.capacity);
				req->t_err = c.status < 0 && c.status >= -MAX_ERRNO ? c.status : -EIO;
				status = REQ_STATUS_ERROR;
			}
			sbp9_free_slot(slot);
		}
		sbshm_dmb();
		WRITE_ONCE(sbp9_ch->done_tail, ++sbp9_done_tail);
		if (req)
			p9_client_cb(cl, req, status);
		cond_resched();
	}
}

static int sbp9_create(struct p9_client *client, struct fs_context *fc)
{
	unsigned long flags;
	int ret = 0;

	if (!READ_ONCE(sbp9_ready))
		return -ENODEV;
	spin_lock_irqsave(&sbp9_lock, flags);
	if (sbp9_client)
		ret = -EBUSY;
	else
		sbp9_client = client;
	spin_unlock_irqrestore(&sbp9_lock, flags);
	if (ret) {
		pr_warn("sbp9: already mounted (the host serves one 9P session)\n");
		return ret;
	}
	client->trans = sbp9_ch;
	client->status = Connected;
	return 0;
}

static void sbp9_close(struct p9_client *client)
{
	struct p9_req_t *gone[SBSHM_P9_SLOTS];
	unsigned long flags;
	unsigned int i, n = 0;

	spin_lock_irqsave(&sbp9_lock, flags);
	for (i = 0; i < SBSHM_P9_SLOTS; i++)
		if (sbp9_req[i] && sbp9_cl[i] == client) {	/* the slot stays busy until the host answers */
			gone[n++] = sbp9_req[i];
			sbp9_req[i] = NULL;
		}
	if (sbp9_client == client)
		sbp9_client = NULL;
	spin_unlock_irqrestore(&sbp9_lock, flags);
	for (i = 0; i < n; i++) {
		gone[i]->t_err = -EIO;
		p9_client_cb(client, gone[i], REQ_STATUS_ERROR);
	}
	if (n)
		pr_warn("sbp9: %u requests still at the host at unmount\n", n);
	flush_work(&sbp9_work);			/* a reap in progress has finished with this client */
}

/* sent already: the client sends Tflush, and the host answers the request before it (in order) */
static int sbp9_cancel(struct p9_client *client, struct p9_req_t *req)
{
	return 1;
}

static int sbp9_show_options(struct seq_file *m, struct p9_client *client)
{
	return 0;
}

static struct p9_trans_module sbp9_trans = {
	.name = "sbshm",
	.maxsize = SBSHM_P9_SLOT,
	.pooled_rbuffers = false,
	.def = true,
	.supports_vmalloc = true,
	.create = sbp9_create,
	.close = sbp9_close,
	.request = sbp9_request,
	.cancel = sbp9_cancel,
	.show_options = sbp9_show_options,
	.owner = THIS_MODULE,
};

void sbp9_init(struct device *dev, bool irq_ok)
{
	char name[sizeof(sbp9_ch->name)];
	bool ro;

	BUILD_BUG_ON(sizeof(struct sbshm_p9) > SBSHM_P9_SLOTS_OFF);
	BUILD_BUG_ON(SBSHM_P9_SLOTS_OFF + SBSHM_P9_SLOTS * SBSHM_P9_SLOT > SBSHM_P9_SIZE);
	BUILD_BUG_ON(SBSHM_P9_SLOTS > SBP9_SLOT_MASK + 1 || SBSHM_P9_SLOTS > SBSHM_P9_RING);
	if (!sbg_svc(SBSHM_SVC_P9))
		return;
	if (shm_size < SBSHM_P9_OFF + (u64)SBSHM_P9_SIZE) {
		dev_warn(dev, "sbp9: the shared region is too small for the file channel\n");
		return;
	}
	sbp9_ch = sbg_vmap_wc(SBSHM_P9_OFF, sizeof(*sbp9_ch));
	if (!sbp9_ch)
		return;
	if (READ_ONCE(sbp9_ch->magic) != SBSHM_P9_MAGIC || READ_ONCE(sbp9_ch->msize) != SBSHM_P9_SLOT ||
	    READ_ONCE(sbp9_ch->slots) != SBSHM_P9_SLOTS) {
		dev_warn(dev, "sbp9: the host's channel is not this layout (magic %#x, msize %u, %u slots)\n",
			 READ_ONCE(sbp9_ch->magic), READ_ONCE(sbp9_ch->msize), READ_ONCE(sbp9_ch->slots));
		return;
	}
	sbp9_slots = sbg_vmap_wc(SBSHM_P9_OFF + SBSHM_P9_SLOTS_OFF, (size_t)SBSHM_P9_SLOTS * SBSHM_P9_SLOT);
	sbp9_wq = alloc_workqueue("sbp9", WQ_MEM_RECLAIM | WQ_HIGHPRI, 1);
	if (!sbp9_slots || !sbp9_wq)
		return;
	/* this side's indices start where the host's are (0 after its load) */
	sbp9_req_head = READ_ONCE(sbp9_ch->req_head);
	sbp9_done_tail = READ_ONCE(sbp9_ch->done_tail);
	sbp9_polled = !irq_ok || !READ_ONCE(hdr->db_to_guest) || !READ_ONCE(hdr->db_to_host);
	sbp9_period = sbp9_polled ? 1 : max(1, HZ / 20);
	timer_setup(&sbp9_timer, sbp9_timer_fn, 0);
	ro = READ_ONCE(sbp9_ch->flags) & SBSHM_P9_RO;
	memcpy(name, sbp9_ch->name, sizeof(name));
	name[sizeof(name) - 1] = 0;
	strscpy(sbp9_state, ro ? "ro" : "rw", sizeof(sbp9_state));
	WRITE_ONCE(sbp9_ready, true);
	v9fs_register_trans(&sbp9_trans);
	sbp9_registered = true;
	dev_info(dev, "sbp9: \"%s\", %s, msize %u, %u slots, completions by %s; mount -t 9p -o trans=sbshm starbite DIR\n",
		 name, ro ? "read-only" : "read-write", SBSHM_P9_SLOT, SBSHM_P9_SLOTS,
		 sbp9_polled ? "timer poll" : "doorbell");
}

void sbp9_exit(void)
{
	if (!sbp9_registered)
		return;
	v9fs_unregister_trans(&sbp9_trans);	/* no client can hold it: the trans owns a module reference */
	sbp9_registered = false;
	WRITE_ONCE(sbp9_ready, false);
	timer_delete_sync(&sbp9_timer);
	destroy_workqueue(sbp9_wq);
}
