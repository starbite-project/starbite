// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbgpu: the Mali through Starbite OS (contract v3 service SBSHM_SVC_GPU, ../sbshm-gpu.h; Track E 2026-11-14).
 *
 * A DRM render node named "panfrost" with Panfrost's UAPI (1.2: SUBMIT, WAIT_BO, CREATE_BO, MMAP_BO, GET_PARAM,
 * GET_BO_OFFSET, MADVISE, SET_LABEL_BO), so Mesa's Panfrost / PanVK run in the guest unchanged. The GPU itself is
 * Starbite's: every buffer's pages are mapped into the guest's GPU address space by the host (MAP / UNMAP requests,
 * the PFNs staged in the region), and a SUBMIT hands the host the job chain's address; the host runs it when its own
 * compositing leaves the GPU free and reports job_done.
 *   - buffers: shmem GEM objects, write-combine for user space (the GPU is not coherent with the guest's caches);
 *     their pages pinned and DMA-mapped (cache maintenance) at CREATE_BO, mapped for the GPU at once (no heap growth
 *     on faults: a HEAP buffer is mapped whole). One GPU VA space for the guest, [32 MiB, 4 GiB), Panfrost's rules
 *     (an executable buffer aligned to its size, never across a 4 GiB boundary; 2 MiB alignment from 2 MiB).
 *   - jobs: in submission order, seq 1, 2, ...; a SUBMIT first waits for its in-syncs, then posts the chain; its
 *     fence (seqno = seq) signals when the host's job_done reaches it; WAIT_BO waits for the buffer's last job.
 *   - wake-ups: the shared doorbell (sbgpu_kick), else a 1-jiffy timer while jobs or requests are outstanding.
 */
#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mm.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_utils.h>
#include <drm/panfrost_drm.h>

#include "sbg_core.h"
#include "../sbshm-gpu.h"

struct sbgpu_bo {
	struct drm_gem_shmem_object base;
	struct drm_mm_node node;		/* its GPU VA (pages) */
	bool mapped, noexec, heap;
	u64 last_seq;				/* the last job that referenced it */
};

static struct drm_device *sbgpu_ddev;
static struct sbshm_gpu *sg;			/* the region's sbgpu part (write-combine) */
static u64 *sg_pfn;				/* its MAP staging area */
static DEFINE_MUTEX(sg_req_lock);		/* one request in flight, the staging area with it */
static DEFINE_MUTEX(sg_mm_lock);
static struct drm_mm sg_mm;
static u64 sg_req_seq, sg_job_seq;
static DECLARE_WAIT_QUEUE_HEAD(sg_wq);
static struct timer_list sg_timer;
static bool sg_ready;
static u64 sg_fence_ctx;
static DEFINE_SPINLOCK(sg_fence_lock);
static LIST_HEAD(sg_pending);			/* fences of jobs not finished yet, in seq order */
static atomic_t sg_waiting = ATOMIC_INIT(0);	/* requests or jobs outstanding: the timer runs */

struct sbgpu_fence {
	struct dma_fence base;
	struct list_head link;
};

static const char *sbgpu_fence_driver(struct dma_fence *f) { return "sbgpu"; }
static const char *sbgpu_fence_timeline(struct dma_fence *f) { return "sbgpu-jobs"; }
static const struct dma_fence_ops sbgpu_fence_ops = {
	.get_driver_name = sbgpu_fence_driver,
	.get_timeline_name = sbgpu_fence_timeline,
};

static u64 sg_job_done(void)
{
	return READ_ONCE(sg->job_done);
}

/* the finished jobs' fences signalled, the waiters woken (doorbell, timer) */
static void sbgpu_reap(void)
{
	struct sbgpu_fence *f, *n;
	unsigned long flags;
	u64 done;

	if (!sg_ready)
		return;
	done = sg_job_done();
	spin_lock_irqsave(&sg_fence_lock, flags);
	list_for_each_entry_safe(f, n, &sg_pending, link) {
		if (f->base.seqno > done)
			break;
		list_del(&f->link);
		dma_fence_signal_locked(&f->base);	/* (sg_fence_lock is the fences' own lock, held here) */
		dma_fence_put(&f->base);
	}
	spin_unlock_irqrestore(&sg_fence_lock, flags);
	wake_up_all(&sg_wq);
}

void sbgpu_kick(void)
{
	sbgpu_reap();
}

static void sbgpu_timer_fn(struct timer_list *t)
{
	sbgpu_reap();
	if (atomic_read(&sg_waiting) || !list_empty(&sg_pending))
		mod_timer(&sg_timer, jiffies + 1);
}

static void sbgpu_arm(void)
{
	if (!timer_pending(&sg_timer))
		mod_timer(&sg_timer, jiffies + 1);
}

/* one request to the host, answered in order: its status (0, -errno), -ETIMEDOUT after 5 s, -EINTR */
static int sbgpu_request(u32 op, u32 flags, u64 a, u64 b)
{
	struct sbgpu_req *r;
	u64 seq;
	long left;
	u32 head;

	lockdep_assert_held(&sg_req_lock);
	seq = ++sg_req_seq;
	head = READ_ONCE(sg->req_head);
	r = &sg->req[head & (SBGPU_RING - 1)];
	WRITE_ONCE(r->op, op);
	WRITE_ONCE(r->flags, flags);
	WRITE_ONCE(r->seq, seq);
	WRITE_ONCE(r->a, a);
	WRITE_ONCE(r->b, b);
	dma_wmb();
	WRITE_ONCE(sg->req_head, head + 1);
	atomic_inc(&sg_waiting);
	sbgpu_arm();
	sbg_ring_host();
	left = wait_event_interruptible_timeout(sg_wq, (s64)(READ_ONCE(sg->done_seq) - seq) >= 0, 5 * HZ);
	atomic_dec(&sg_waiting);
	if (left < 0)
		return -EINTR;		/* (the host still answers it: the ring stays in order) */
	if (!left)
		return -ETIMEDOUT;
	dma_rmb();
	return READ_ONCE(sg->status);
}

/* the buffer's pages into the guest's GPU address space, in staging-area sized pieces */
static int sbgpu_map(struct sbgpu_bo *bo)
{
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(&bo->base);
	struct sg_page_iter it;
	u64 va = bo->node.start << PAGE_SHIFT, piece_va = va;
	u32 n = 0, flags = bo->noexec ? SBGPU_MAP_NOEXEC : 0;
	int ret = 0;

	if (IS_ERR(sgt))
		return PTR_ERR(sgt);
	mutex_lock(&sg_req_lock);
	for_each_sgtable_page(sgt, &it, 0) {
		WRITE_ONCE(sg_pfn[n++], page_to_pfn(sg_page_iter_page(&it)));
		if (n == SBGPU_PFN_MAX) {
			ret = sbgpu_request(SBGPU_OP_MAP, flags, piece_va, n);
			if (ret)
				break;
			piece_va += (u64)n << PAGE_SHIFT;
			n = 0;
		}
	}
	if (!ret && n)
		ret = sbgpu_request(SBGPU_OP_MAP, flags, piece_va, n);
	mutex_unlock(&sg_req_lock);
	if (!ret)
		bo->mapped = true;
	return ret;
}

static void sbgpu_wait_seq(u64 seq)
{
	if (seq && sg_ready)
		wait_event_timeout(sg_wq, (s64)(sg_job_done() - seq) >= 0, 5 * HZ);
}

static void sbgpu_gem_free(struct drm_gem_object *obj)
{
	struct sbgpu_bo *bo = container_of(to_drm_gem_shmem_obj(obj), struct sbgpu_bo, base);

	if (bo->mapped) {
		sbgpu_wait_seq(bo->last_seq);	/* the GPU done with it before its pages go */
		mutex_lock(&sg_req_lock);
		sbgpu_request(SBGPU_OP_UNMAP, 0, bo->node.start << PAGE_SHIFT, bo->base.base.size >> PAGE_SHIFT);
		mutex_unlock(&sg_req_lock);
	}
	if (drm_mm_node_allocated(&bo->node)) {
		mutex_lock(&sg_mm_lock);
		drm_mm_remove_node(&bo->node);
		mutex_unlock(&sg_mm_lock);
	}
	drm_gem_shmem_free(&bo->base);
}

static const struct drm_gem_object_funcs sbgpu_gem_funcs = {
	.free = sbgpu_gem_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static struct drm_gem_object *sbgpu_gem_create_object(struct drm_device *dev, size_t size)
{
	struct sbgpu_bo *bo = kzalloc(sizeof(*bo), GFP_KERNEL);

	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.base.funcs = &sbgpu_gem_funcs;
	bo->base.map_wc = true;
	return &bo->base.base;
}

static struct sbgpu_bo *sbgpu_lookup(struct drm_file *file, u32 handle)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);

	return obj ? container_of(to_drm_gem_shmem_obj(obj), struct sbgpu_bo, base) : NULL;
}

/* Panfrost's VA rules (panfrost_gem_open, panfrost_drm_mm_color_adjust) */
static void sbgpu_color_adjust(const struct drm_mm_node *node, unsigned long color, u64 *start, u64 *end)
{
	const u64 pfn_4g = SZ_4G >> PAGE_SHIFT, mask = pfn_4g - 1, pfn_16m = SZ_16M >> PAGE_SHIFT;

	if (!(color & PANFROST_BO_NOEXEC)) {
		u64 next;

		if ((*start & mask) == 0)
			(*start)++;
		if ((*end & mask) == 0)
			(*end)--;
		next = ALIGN(*start, pfn_4g);
		if (next - *start <= pfn_16m)
			*start = next + 1;
		*end = min(*end, ALIGN(*start, pfn_4g) - 1);
	}
}

static int sbgpu_ioctl_create_bo(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_create_bo *args = data;
	struct drm_gem_shmem_object *shmem;
	struct sbgpu_bo *bo;
	size_t size;
	u64 align;
	int ret;

	if (!args->size || args->pad || (args->flags & ~(PANFROST_BO_NOEXEC | PANFROST_BO_HEAP)))
		return -EINVAL;
	if ((args->flags & PANFROST_BO_HEAP) && !(args->flags & PANFROST_BO_NOEXEC))
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	bo = container_of(shmem, struct sbgpu_bo, base);
	bo->noexec = args->flags & PANFROST_BO_NOEXEC;
	bo->heap = args->flags & PANFROST_BO_HEAP;
	align = !bo->noexec ? size >> PAGE_SHIFT : size >= SZ_2M ? SZ_2M >> PAGE_SHIFT : 0;
	mutex_lock(&sg_mm_lock);
	ret = drm_mm_insert_node_generic(&sg_mm, &bo->node, size >> PAGE_SHIFT, align,
					 bo->noexec ? PANFROST_BO_NOEXEC : 0, 0);
	mutex_unlock(&sg_mm_lock);
	if (!ret)
		ret = sbgpu_map(bo);
	if (!ret)
		ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	if (!ret)
		args->offset = bo->node.start << PAGE_SHIFT;
	drm_gem_object_put(&shmem->base);
	return ret;
}

static int sbgpu_ioctl_mmap_bo(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_mmap_bo *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->flags)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	ret = drm_gem_create_mmap_offset(obj);
	if (!ret)
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return ret;
}

static int sbgpu_ioctl_get_param(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_get_param *param = data;

	if (param->pad)
		return -EINVAL;
	if (param->param >= SBGPU_PARAMS || param->param > DRM_PANFROST_PARAM_AFBC_FEATURES)
		return -EINVAL;
	param->value = READ_ONCE(sg->params[param->param]);
	return 0;
}

static int sbgpu_ioctl_get_bo_offset(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_get_bo_offset *args = data;
	struct sbgpu_bo *bo = sbgpu_lookup(file, args->handle);

	if (!bo)
		return -ENOENT;
	args->offset = bo->node.start << PAGE_SHIFT;
	drm_gem_object_put(&bo->base.base);
	return 0;
}

static int sbgpu_ioctl_wait_bo(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_wait_bo *args = data;
	unsigned long timeout = drm_timeout_abs_to_jiffies(args->timeout_ns);
	struct sbgpu_bo *bo;
	long ret;
	u64 seq;

	if (args->pad)
		return -EINVAL;
	bo = sbgpu_lookup(file, args->handle);
	if (!bo)
		return -ENOENT;
	seq = READ_ONCE(bo->last_seq);
	drm_gem_object_put(&bo->base.base);
	if (!seq || (s64)(sg_job_done() - seq) >= 0)
		return 0;
	if (!timeout)
		return -ETIMEDOUT;
	sbgpu_arm();
	ret = wait_event_interruptible_timeout(sg_wq, (s64)(sg_job_done() - seq) >= 0, timeout);
	if (ret < 0)
		return ret;
	return ret ? 0 : -ETIMEDOUT;
}

static int sbgpu_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_submit *args = data;
	struct drm_syncobj *out = NULL;
	struct sbgpu_fence *f;
	unsigned long flags;
	u32 *handles = NULL;
	u64 seq;
	int ret = 0;
	u32 i;

	if (!args->jc || args->pad || args->jm_ctx_handle)
		return -EINVAL;
	if (args->out_sync) {
		out = drm_syncobj_find(file, args->out_sync);
		if (!out)
			return -ENODEV;
	}
	/* the in-syncs first (a sync submission: Mesa's chains already wait for their own dependencies) */
	for (i = 0; i < args->in_sync_count; i++) {
		u32 h;
		struct dma_fence *in;

		if (copy_from_user(&h, u64_to_user_ptr(args->in_syncs + 4ULL * i), 4)) {
			ret = -EFAULT;
			goto out;
		}
		ret = drm_syncobj_find_fence(file, h, 0, 0, &in);
		if (ret)
			goto out;
		ret = dma_fence_wait(in, true);
		dma_fence_put(in);
		if (ret)
			goto out;
	}
	if (args->bo_handle_count) {
		handles = kvmalloc_array(args->bo_handle_count, sizeof(u32), GFP_KERNEL);
		if (!handles) {
			ret = -ENOMEM;
			goto out;
		}
		if (copy_from_user(handles, u64_to_user_ptr(args->bo_handles), args->bo_handle_count * sizeof(u32))) {
			ret = -EFAULT;
			goto out;
		}
	}
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		ret = -ENOMEM;
		goto out;
	}
	mutex_lock(&sg_req_lock);
	seq = ++sg_job_seq;
	dma_fence_init(&f->base, &sbgpu_fence_ops, &sg_fence_lock, sg_fence_ctx, seq);
	for (i = 0; i < args->bo_handle_count; i++) {
		struct sbgpu_bo *bo = sbgpu_lookup(file, handles[i]);

		if (bo) {
			WRITE_ONCE(bo->last_seq, seq);
			drm_gem_object_put(&bo->base.base);
		}
	}
	dma_fence_get(&f->base);		/* the pending list's reference */
	spin_lock_irqsave(&sg_fence_lock, flags);
	list_add_tail(&f->link, &sg_pending);
	spin_unlock_irqrestore(&sg_fence_lock, flags);
	ret = sbgpu_request(SBGPU_OP_SUBMIT, (args->requirements & PANFROST_JD_REQ_FS) ? SBGPU_SUBMIT_FS : 0,
			    args->jc, seq);
	mutex_unlock(&sg_req_lock);
	sbgpu_arm();
	sbgpu_reap();				/* (a refused one is done at once, as failed) */
	if (out)
		drm_syncobj_replace_fence(out, &f->base);
	dma_fence_put(&f->base);
out:
	kvfree(handles);
	if (out)
		drm_syncobj_put(out);
	return ret;
}

static int sbgpu_ioctl_madvise(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_panfrost_madvise *args = data;

	args->retained = 1;			/* nothing is purged: buffers stay as long as they are open */
	return 0;
}

static int sbgpu_ioctl_set_label_bo(struct drm_device *dev, void *data, struct drm_file *file)
{
	return 0;
}

static int sbgpu_ioctl_perfcnt(struct drm_device *dev, void *data, struct drm_file *file)
{
	return -ENODEV;
}

static const struct drm_ioctl_desc sbgpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PANFROST_SUBMIT, sbgpu_ioctl_submit, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_WAIT_BO, sbgpu_ioctl_wait_bo, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_CREATE_BO, sbgpu_ioctl_create_bo, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_MMAP_BO, sbgpu_ioctl_mmap_bo, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_GET_PARAM, sbgpu_ioctl_get_param, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_GET_BO_OFFSET, sbgpu_ioctl_get_bo_offset, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_PERFCNT_ENABLE, sbgpu_ioctl_perfcnt, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_PERFCNT_DUMP, sbgpu_ioctl_perfcnt, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_MADVISE, sbgpu_ioctl_madvise, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANFROST_SET_LABEL_BO, sbgpu_ioctl_set_label_bo, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(sbgpu_fops);

static const struct drm_driver sbgpu_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.gem_create_object = sbgpu_gem_create_object,
	.ioctls = sbgpu_ioctls,
	.num_ioctls = ARRAY_SIZE(sbgpu_ioctls),
	.fops = &sbgpu_fops,
	.name = "panfrost",			/* Mesa's loader picks its Panfrost driver by this name */
	.desc = "Starbite's Mali (forwarded)",
	.major = 1,
	.minor = 2,
	DRM_GEM_SHMEM_DRIVER_OPS,
};

void sbgpu_init(struct device *dev, bool irq_ok)
{
	struct drm_device *ddev;
	int ret;

	if (!sbg_svc(SBSHM_SVC_GPU) || shm_size < SBSHM_GPU_OFF + SBSHM_GPU_SIZE)
		return;
	sg = sbg_vmap_wc(SBSHM_GPU_OFF, SBSHM_GPU_SIZE);
	if (!sg)
		return;
	if (READ_ONCE(sg->magic) != SBGPU_MAGIC || READ_ONCE(sg->version) != SBGPU_VERSION) {
		dev_info(dev, "sbgpu: the host offers no GPU (no magic)\n");
		return;
	}
	sg_pfn = (u64 *)((u8 *)sg + SBGPU_PFN_OFF);
	sg_req_seq = READ_ONCE(sg->done_seq);
	sg_job_seq = READ_ONCE(sg->job_done);
	drm_mm_init(&sg_mm, SZ_32M >> PAGE_SHIFT, (SZ_4G - SZ_32M) >> PAGE_SHIFT);
	sg_mm.color_adjust = sbgpu_color_adjust;
	sg_fence_ctx = dma_fence_context_alloc(1);
	timer_setup(&sg_timer, sbgpu_timer_fn, 0);
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	ddev = drm_dev_alloc(&sbgpu_driver, dev);
	if (IS_ERR(ddev)) {
		dev_warn(dev, "sbgpu: drm_dev_alloc: %ld\n", PTR_ERR(ddev));
		return;
	}
	WRITE_ONCE(sg_ready, true);
	ret = drm_dev_register(ddev, 0);
	if (ret) {
		dev_warn(dev, "sbgpu: drm_dev_register: %d\n", ret);
		WRITE_ONCE(sg_ready, false);
		drm_dev_put(ddev);
		return;
	}
	sbgpu_ddev = ddev;
	dev_info(dev, "sbgpu: Mali id %#llx r%#llx, shader cores %#llx, through Starbite (GPU address space %u)%s\n",
		 sg->params[DRM_PANFROST_PARAM_GPU_PROD_ID], sg->params[DRM_PANFROST_PARAM_GPU_REVISION],
		 sg->params[DRM_PANFROST_PARAM_SHADER_PRESENT], sg->as, irq_ok ? "" : ", polled");
}

void sbgpu_exit(void)
{
	if (!sbgpu_ddev)
		return;
	drm_dev_unregister(sbgpu_ddev);
	drm_dev_put(sbgpu_ddev);
	sbgpu_ddev = NULL;
	WRITE_ONCE(sg_ready, false);
	timer_shutdown_sync(&sg_timer);
}
