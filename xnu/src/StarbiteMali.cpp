/*
 * Project Starbite: the Mali-G72 for iOS apps (stage 6 of
 * bsd/dev/starlte_gpu.c). The IOService "StarbiteMali" and its user client
 * speak Linux Panfrost's DRM interface (tools/gpu/panfrost_drm.h, MIT), one
 * IOConnect selector per ioctl, so Mesa's Panfrost driver can run on top
 * through a small libdrm shim (the HackiOS approach with msm):
 *
 *   0 SUBMIT, 1 WAIT_BO, 2 CREATE_BO, 3 MMAP_BO, 4 GET_PARAM,
 *   5 GET_BO_OFFSET, 8 MADVISE   structs of panfrost_drm.h, in and out
 *   0x100 GEM_CLOSE             struct drm_gem_close (handle, pad)
 *   0x101 VERSION               out: u32 major, minor, patch (1.3.0)
 *   0x102 LOG                   in: text for the kernel log (test tools)
 *
 * Buffers are XNU memory in 4 KiB pages, cleaned out of the CPU caches and
 * mapped write-combined into the app (the GPU is not IO-coherent), at GPU
 * VAs from 0x04000000 in the apps' address space (AS1). Jobs run to
 * completion inside SUBMIT, so WAIT_BO has nothing to wait for. Heap
 * buffers (PANFROST_BO_HEAP) are allocated in full: nothing handles GPU
 * page faults yet.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <sys/errno.h>

extern "C" {
int starlte_gpu_user_ready(void);
bool starlte_gpu_user_param(uint32_t param, uint64_t *value);
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n);
void starlte_gpu_user_unmap(uint64_t va, unsigned n);
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements);
void starbite_mali_publish(void);
}

/* panfrost_drm.h layouts. */
struct sbm_submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements, jm_ctx_handle, pad;
};
struct sbm_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};
struct sbm_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};
struct sbm_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct sbm_get_param {
	uint32_t param, pad;
	uint64_t value;
};
struct sbm_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};
struct sbm_madvise {
	uint32_t handle, madv, retained;
};
struct sbm_gem_close {
	uint32_t handle, pad;
};
struct sbm_version {
	uint32_t major, minor, patch;
};

enum {
	SBM_SUBMIT = 0, SBM_WAIT_BO = 1, SBM_CREATE_BO = 2, SBM_MMAP_BO = 3, SBM_GET_PARAM = 4,
	SBM_GET_BO_OFFSET = 5, SBM_MADVISE = 8, SBM_GEM_CLOSE = 0x100, SBM_VERSION = 0x101,
	SBM_LOG = 0x102,
};

#define SBM_MAX_BOS     8192u
#define SBM_VA_FIRST    0x04000000ull
#define SBM_VA_END      0x100000000ull
#define SBM_MAX_BO      (256u << 20)

struct sbm_bo {
	IOBufferMemoryDescriptor *mem;
	IOMemoryMap *map;
	uint64_t va;
	uint32_t pages;
};

/* GPU VAs: one bump allocator for all clients (they share AS1), never reused. */
static IOLock *sbm_va_lock;
static uint64_t sbm_next_va = SBM_VA_FIRST;

class StarbiteMali : public IOService {
	OSDeclareDefaultStructors(StarbiteMali);
};

OSDefineMetaClassAndStructors(StarbiteMali, IOService);

class StarbiteMaliUserClient : public IOUserClient {
	OSDeclareDefaultStructors(StarbiteMaliUserClient);
	task_t fTask = NULL;
	IOLock *fLock = NULL;
	sbm_bo *fBos = NULL;            /* handle h is fBos[h - 1] */
	uint32_t fCount = 0;

	IOReturn createBO(sbm_create_bo *a);
	IOReturn mmapBO(sbm_mmap_bo *a);
	sbm_bo *lookup(uint32_t handle);
	void closeBO(sbm_bo *bo);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(StarbiteMaliUserClient, IOUserClient);

bool
StarbiteMaliUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
	if (!starlte_gpu_user_ready() || !IOUserClient::initWithTask(owningTask, securityID, type,
	    properties)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fBos = (sbm_bo *)IOMallocZeroData(SBM_MAX_BOS * sizeof(sbm_bo));
	if (fLock == NULL || fBos == NULL) {
		return false;
	}
	/* No entitlement needed; externalMethod calls are serialized by IOKit. */
	setProperty("IOUserClientEntitlements", kOSBooleanFalse);
	setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
	return true;
}

sbm_bo *
StarbiteMaliUserClient::lookup(uint32_t handle)
{
	return (handle >= 1 && handle <= fCount && fBos[handle - 1].mem != NULL) ? &fBos[handle - 1] :
	       NULL;
}

void
StarbiteMaliUserClient::closeBO(sbm_bo *bo)
{
	starlte_gpu_user_unmap(bo->va, bo->pages);
	OSSafeReleaseNULL(bo->map);
	OSSafeReleaseNULL(bo->mem);
	bo->va = 0;
	bo->pages = 0;
}

IOReturn
StarbiteMaliUserClient::createBO(sbm_create_bo *a)
{
	if (a->size == 0 || a->size > SBM_MAX_BO) {
		return kIOReturnBadArgument;
	}
	uint32_t handle = 0;
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem == NULL) {
			handle = i + 1;
			break;
		}
	}
	if (handle == 0) {
		if (fCount >= SBM_MAX_BOS) {
			return kIOReturnNoResources;
		}
		handle = ++fCount;
	}
	uint32_t pages = (a->size + 0xfff) / 0x1000;
	uint64_t bytes = (uint64_t)pages * 0x1000;
	IOLockLock(sbm_va_lock);
	uint64_t va = sbm_next_va;
	if (va + bytes > SBM_VA_END) {
		IOLockUnlock(sbm_va_lock);
		return kIOReturnNoSpace;
	}
	sbm_next_va = (va + bytes + 0xffff) & ~0xffffull;       /* a gap between buffers */
	IOLockUnlock(sbm_va_lock);

	IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
	    (vm_size_t)round_page(bytes), page_size);
	uint64_t *pa = (uint64_t *)IOMallocData((vm_size_t)pages * sizeof(uint64_t));
	if (mem == NULL || pa == NULL) {
		OSSafeReleaseNULL(mem);
		if (pa) {
			IOFreeData(pa, (vm_size_t)pages * sizeof(uint64_t));
		}
		return kIOReturnNoMemory;
	}
	/* Out of the CPU caches by physical address, then zeroed write-combined:
	 * no dirty line can later be evicted over what the GPU wrote. */
	mem->performOperation(kIOMemoryIncoherentIOFlush, 0, mem->getLength());
	bzero(mem->getBytesNoCopy(), (size_t)bytes);
	bool ok = true;
	for (uint32_t i = 0; i < pages && ok; i++) {
		IOByteCount len = 0;
		pa[i] = mem->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);
		ok = pa[i] != 0;
	}
	ok = ok && starlte_gpu_user_map(va, pa, pages);
	IOFreeData(pa, (vm_size_t)pages * sizeof(uint64_t));
	if (!ok) {
		mem->release();
		return kIOReturnVMError;
	}
	fBos[handle - 1] = (sbm_bo){ .mem = mem, .map = NULL, .va = va, .pages = pages };
	a->handle = handle;
	a->offset = va;
	return kIOReturnSuccess;
}

IOReturn
StarbiteMaliUserClient::mmapBO(sbm_mmap_bo *a)
{
	sbm_bo *bo = lookup(a->handle);
	if (bo == NULL) {
		return kIOReturnBadArgument;
	}
	if (bo->map == NULL) {
		bo->map = bo->mem->createMappingInTask(fTask, 0,
		    kIOMapAnywhere | kIOMapWriteCombineCache);
		if (bo->map == NULL) {
			return kIOReturnVMError;
		}
	}
	/* With this shim the "mmap offset" is the mapped address itself. */
	a->offset = bo->map->getAddress();
	return kIOReturnSuccess;
}

IOReturn
StarbiteMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	union {
		sbm_submit submit;
		sbm_wait_bo wait;
		sbm_create_bo create;
		sbm_mmap_bo mmap;
		sbm_get_param param;
		sbm_get_bo_offset offset;
		sbm_madvise madvise;
		sbm_gem_close close;
		sbm_version version;
		char text[256];
	} u;
	size_t want;
	switch (selector) {
	case SBM_SUBMIT: want = sizeof(u.submit); break;
	case SBM_WAIT_BO: want = sizeof(u.wait); break;
	case SBM_CREATE_BO: want = sizeof(u.create); break;
	case SBM_MMAP_BO: want = sizeof(u.mmap); break;
	case SBM_GET_PARAM: want = sizeof(u.param); break;
	case SBM_GET_BO_OFFSET: want = sizeof(u.offset); break;
	case SBM_MADVISE: want = sizeof(u.madvise); break;
	case SBM_GEM_CLOSE: want = sizeof(u.close); break;
	case SBM_VERSION: want = 0; break;
	case SBM_LOG: want = args->structureInputSize; break;
	default:
		return kIOReturnUnsupported;
	}
	bzero(&u, sizeof(u));
	if (selector == SBM_LOG) {
		if (want == 0 || want >= sizeof(u.text)) {
			return kIOReturnBadArgument;
		}
	} else if (args->structureInputSize != want) {
		return kIOReturnBadArgument;
	}
	if (want) {
		bcopy(args->structureInput, &u, want);
	}

	IOReturn ret = kIOReturnSuccess;
	IOLockLock(fLock);
	switch (selector) {
	case SBM_SUBMIT: {
		int err = starlte_gpu_user_submit(u.submit.jc, u.submit.requirements);
		ret = err == 0 ? kIOReturnSuccess : err == ETIMEDOUT ? kIOReturnTimeout : kIOReturnIOError;
		break;
	}
	case SBM_WAIT_BO:
		ret = lookup(u.wait.handle) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case SBM_CREATE_BO:
		ret = createBO(&u.create);
		break;
	case SBM_MMAP_BO:
		ret = mmapBO(&u.mmap);
		break;
	case SBM_GET_PARAM:
		ret = starlte_gpu_user_param(u.param.param, &u.param.value) ? kIOReturnSuccess :
		    kIOReturnBadArgument;
		break;
	case SBM_GET_BO_OFFSET: {
		sbm_bo *bo = lookup(u.offset.handle);
		u.offset.offset = bo ? bo->va : 0;
		ret = bo ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	}
	case SBM_MADVISE:
		u.madvise.retained = 1;                 /* never purged */
		ret = lookup(u.madvise.handle) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case SBM_GEM_CLOSE: {
		sbm_bo *bo = lookup(u.close.handle);
		if (bo) {
			closeBO(bo);
		}
		ret = bo ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	}
	case SBM_VERSION:
		u.version = (sbm_version){ 1, 3, 0 };
		want = sizeof(u.version);
		break;
	case SBM_LOG:
		u.text[want] = 0;
		IOLog("STARLTE GPU APP: %s\n", u.text);
		want = 0;
		break;
	}
	IOLockUnlock(fLock);
	if (ret == kIOReturnSuccess && want && args->structureOutput != NULL) {
		if (args->structureOutputSize < want) {
			return kIOReturnBadArgument;
		}
		bcopy(&u, args->structureOutput, want);
		args->structureOutputSize = (uint32_t)want;
	}
	return ret;
}

IOReturn
StarbiteMaliUserClient::clientClose(void)
{
	IOLockLock(fLock);
	for (uint32_t i = 0; i < fCount; i++) {
		if (fBos[i].mem != NULL) {
			closeBO(&fBos[i]);
		}
	}
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

void
StarbiteMaliUserClient::free(void)
{
	if (fBos != NULL) {
		IOFreeData(fBos, SBM_MAX_BOS * sizeof(sbm_bo));
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
	}
	IOUserClient::free();
}

/* Called once by starlte_gpu.c when the GPU is ready for apps. */
void
starbite_mali_publish(void)
{
	sbm_va_lock = IOLockAlloc();
	StarbiteMali *service = OSTypeAlloc(StarbiteMali);
	IOService *provider = IOService::getResourceService();
	if (sbm_va_lock == NULL || service == NULL || !service->init() || provider == NULL ||
	    !service->attach(provider)) {
		IOLog("STARLTE GPU: StarbiteMali not published\n");
		OSSafeReleaseNULL(service);
		return;
	}
	service->setProperty(kIOUserClientClassKey, "StarbiteMaliUserClient");
	service->setProperty("GPU", "Mali-G72 MP18 (Bifrost), Panfrost DRM interface 1.3");
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return;
	}
	service->registerService();
	IOLog("STARLTE GPU: StarbiteMali published for apps\n");
	service->release();
}
