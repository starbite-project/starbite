// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's command path (docs/M5.2-PLAN.md): Render copies the UMD's
 * MW_COMMANDs into the DMA buffer's private data, SubmitCommand queues them
 * with their fence, and the hardware works through the queue one job chain
 * at a time. When a submission's last chain finishes, its fence is reported
 * DMA_COMPLETED.
 *
 * The queue is shared with the ISR, so every change to it happens at the
 * device's interrupt level: inside DxgkCbSynchronizeExecution or the ISR.
 * Whether dxgkrnl connects the Mali's interrupts on this ACPI device is not
 * yet known, so a timer also polls the job status while work is in flight;
 * with working interrupts it only costs latency.
 */
#include "maliwddm.h"

#define MW_DMA_MAGIC            0x4D57444Du /* 'MWDM' in the private data */
#define MW_DMA_BUFFER_SIZE      (16 * 1024)
#define MW_LIST_SIZE            256     /* Panfrost submits reference many BOs */
#define MW_JOB_TIMEOUT_MS       2000
#define MW_POLL_MS              1

/* ---- devices and contexts ---- */

NTSTATUS APIENTRY MwCreateDevice(IN_CONST_HANDLE hAdapter, DXGKARG_CREATEDEVICE *Arg)
{
    MW_DEVICE *dev = (MW_DEVICE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*dev), MW_TAG);

    if (dev == NULL)
        return STATUS_NO_MEMORY;
    dev->Adapter = (MW_ADAPTER *)hAdapter;
    Arg->hDevice = dev;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyDevice(IN_CONST_HANDLE hDevice)
{
    ExFreePoolWithTag(hDevice, MW_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCreateContext(IN_CONST_HANDLE hDevice, DXGKARG_CREATECONTEXT *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), MW_TAG);

    if (ctx == NULL)
        return STATUS_NO_MEMORY;
    ctx->Device = (MW_DEVICE *)hDevice;
    Arg->hContext = ctx;
    Arg->ContextInfo.DmaBufferSize = MW_DMA_BUFFER_SIZE;
    Arg->ContextInfo.DmaBufferSegmentSet = 0;           /* system memory, as viogpu3d */
    Arg->ContextInfo.DmaBufferPrivateDataSize = sizeof(MW_DMA_PRIVATE);
    Arg->ContextInfo.AllocationListSize = MW_LIST_SIZE;
    Arg->ContextInfo.PatchLocationListSize = MW_LIST_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyContext(IN_CONST_HANDLE hContext)
{
    ExFreePoolWithTag(hContext, MW_TAG);
    return STATUS_SUCCESS;
}

/* ---- building DMA buffers ---- */

/* The DMA buffer itself is never read: SubmitCommand gets only its physical
 * address. It carries one marker word so that no submission is empty. */
static void MarkDmaBuffer(VOID **DmaBuffer)
{
    *(ULONG *)*DmaBuffer = MW_DMA_MAGIC;
    *DmaBuffer = (PUCHAR)*DmaBuffer + sizeof(ULONG);
}

/* One patch location per allocation the buffer references. Nothing in our
 * DMA buffer needs patching (GPU addresses are fixed), but WDDM 1.x VidMm
 * learns from the patch list what to make resident: with the UMD's empty
 * list the job's allocation was never mapped into the aperture (WinPE runs
 * 13-14: no MAP_APERTURE_SEGMENT for it, translation fault at its address). */
static void PatchEveryAllocation(UINT Allocations, D3DDDI_PATCHLOCATIONLIST **Out, UINT OutSize)
{
    for (UINT i = 0; i < Allocations && i < OutSize; i++) {
        RtlZeroMemory(*Out, sizeof(**Out));
        (*Out)->AllocationIndex = i;
        (*Out)->SlotId = i;
        (*Out)++;
    }
}

static BOOLEAN JcInList(const MW_COMMAND *C, const DXGKARG_RENDER *Arg)
{
    for (UINT i = 0; i < Arg->AllocationListSize; i++) {
        const MW_ALLOCATION *a = (const MW_ALLOCATION *)Arg->pAllocationList[i].hDeviceSpecificAllocation;
        if (a && !(a->Flags & MW_ALLOC_PRIMARY) && sbx_head_in(C->Jc, a->GpuVa, a->Size))
            return TRUE;
    }
    return FALSE;
}

NTSTATUS APIENTRY MwRender(IN_CONST_HANDLE hContext, DXGKARG_RENDER *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    ULONG count;

    if (priv == NULL || Arg->DmaBufferPrivateDataSize < sizeof(*priv) || Arg->DmaSize < sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;
    if (Arg->CommandLength % sizeof(MW_COMMAND) != 0)
        return STATUS_INVALID_PARAMETER;
    count = Arg->CommandLength / sizeof(MW_COMMAND);
    if (count > MW_MAX_COMMANDS)
        return STATUS_INVALID_PARAMETER;

    /* pCommand is the UMD's buffer, mapped in the caller's process. */
    __try {
        for (ULONG i = 0; i < count; i++) {
            MW_COMMAND c = ((const MW_COMMAND *)Arg->pCommand)[i];
            if (c.Type != MW_CMD_JOB || c.Slot > 1 || c.Jc == 0)
                return STATUS_INVALID_PARAMETER;
            /* S9 port: the chain must start inside a (non-primary) allocation this render lists, which is also
             * what VidMm makes resident (patched below): never at an address the UMD merely claims. */
            if (!JcInList(&c, Arg)) {
                ctx->Device->Adapter->JcRejected++;
                return STATUS_INVALID_PARAMETER;
            }
            priv->Commands[i] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }
    priv->Count = count;
    priv->Reserved = MW_DMA_MAGIC;

    PatchEveryAllocation(Arg->AllocationListSize, &Arg->pPatchLocationListOut, Arg->PatchLocationListOutSize);
    MarkDmaBuffer(&Arg->pDmaBuffer);
    ctx->Device->Adapter->Renders++;
    return STATUS_SUCCESS;
}

/* Presents are done by the CPU at submission (MwSubmitCommand): a blt from
 * the source allocation's pages, or a colour fill, into the primary (the
 * POST framebuffer) or the destination allocation's pages. Render records
 * what to do in the DMA buffer's private data and patches both allocations
 * so that VidMm makes them resident, which is when their pages are known.
 * Flips are not handled yet. */
NTSTATUS APIENTRY MwPresent(IN_CONST_HANDLE hContext, DXGKARG_PRESENT *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    MW_ADAPTER *a = ctx->Device->Adapter;
    MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;

    if (priv == NULL || Arg->DmaBufferPrivateDataSize < sizeof(*priv))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(priv, sizeof(*priv));
    priv->Reserved = MW_DMA_MAGIC;
    if (Arg->Flags.Blt || Arg->Flags.ColorFill) {
        MW_PRESENT_OP *op = &priv->Present;
        op->Kind = Arg->Flags.ColorFill ? MW_PRESENT_FILL : MW_PRESENT_BLT;
        op->Color = Arg->Color;
        /* A present's allocation list has fixed slots; an unused one has
         * no allocation. */
        op->Src = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
        op->Dst = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
        op->SrcRect = Arg->SrcRect;
        op->DstRect = Arg->DstRect;
        op->NumRects = Arg->SubRectCnt <= MW_MAX_RECTS ? Arg->SubRectCnt : 0;
        for (ULONG i = 0; i < op->NumRects; i++)
            op->Rects[i] = Arg->pDstSubRects[i];
        /* Both allocations must be resident (and their pages known). */
        for (UINT i = DXGK_PRESENT_SOURCE_INDEX;
             i <= DXGK_PRESENT_DESTINATION_INDEX && Arg->PatchLocationListOutSize > 0; i++) {
            if (Arg->pAllocationList[i].hDeviceSpecificAllocation == NULL)
                continue;
            RtlZeroMemory(Arg->pPatchLocationListOut, sizeof(*Arg->pPatchLocationListOut));
            Arg->pPatchLocationListOut->AllocationIndex = i;
            Arg->pPatchLocationListOut->SlotId = i;
            Arg->pPatchLocationListOut++;
            Arg->PatchLocationListOutSize--;
        }
    }
    if (Arg->pDmaBuffer != NULL && Arg->DmaSize >= sizeof(ULONG))
        MarkDmaBuffer(&Arg->pDmaBuffer);
    a->Presents++;
    return STATUS_SUCCESS;
}

/* The CPU view of a resident allocation, or NULL. S9 port (T13e): mapped once with the caching VidMm gave
 * its pages (see MW_ALLOCATION.CpuMap), not the default cached system view. */
static PUCHAR AllocationVa(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    if (Alloc == NULL || Alloc->Mdl == NULL)
        return NULL;
    if (Alloc->CpuMap == NULL) {
        PVOID m = NULL;
        __try {                           /* DISPATCH_LEVEL at most: SubmitCommand and the vsync DPC */
            m = MmMapLockedPagesSpecifyCache(Alloc->Mdl, KernelMode, Alloc->Coherent ? MmCached : MmWriteCombined,
                                             NULL, FALSE, HighPagePriority | MdlMappingNoExecute);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            m = NULL;
        }
        if (m == NULL) {
            A->CpuMapFails++;
            return NULL;
        }
        /* the vsync DPC and a present can race here: one view stays, the other is undone */
        if (InterlockedCompareExchangePointer(&Alloc->CpuMap, m, NULL) != NULL)
            MmUnmapLockedPages(m, Alloc->Mdl);
    }
    return (PUCHAR)Alloc->CpuMap + (SIZE_T)Alloc->MdlPage * PAGE_SIZE;
}

/* S9 port (T13e): did a frame bring pixels? 16 samples of what was copied (alpha ignored). The guard counts
 * only such frames: T13d's 223 copies to the panel were black and still counted. */
static BOOLEAN NotBlack(const UCHAR *Base, LONG Pitch, LONG L, LONG T, LONG R, LONG B)
{
    if (Base == NULL || R <= L || B <= T)
        return FALSE;
    for (LONG i = 0; i < 16; i++) {
        LONG x = L + (R - L) * (i % 4) / 4 + (R - L) / 8, y = T + (B - T) * (i / 4) / 4 + (B - T) / 8;
        if (((const ULONG *)(Base + (SIZE_T)y * Pitch))[x] & 0x00FFFFFFu)
            return TRUE;
    }
    return FALSE;
}

static void ClipRect(RECT *R, LONG W, LONG H)
{
    if (R->left < 0) R->left = 0;
    if (R->top < 0) R->top = 0;
    if (R->right > W) R->right = W;
    if (R->bottom > H) R->bottom = H;
}

/* Carry out a present on the CPU. 32-bit pixels only. */
static void DoPresent(MW_ADAPTER *A, const MW_PRESENT_OP *Op)
{
    PUCHAR dst, src = NULL;
    LONG dstW, dstH, dstPitch, srcPitch = 0, srcW = 0, srcH = 0;
    ULONG n = Op->NumRects ? Op->NumRects : 1;

    if (Op->Dst != NULL && (Op->Dst->Flags & MW_ALLOC_PRIMARY) && A->FbVa != NULL) {
        A->PrimaryBlts++;                 /* S9 port: this one reaches the panel */
        dst = A->FbVa;
        dstW = (LONG)A->Fb.Width;
        dstH = (LONG)A->Fb.Height;
        dstPitch = (LONG)A->Fb.Pitch;
    } else {
        dst = AllocationVa(A, Op->Dst);
        if (dst == NULL) {
            A->PresentSkips++;
            return;
        }
        dstW = (LONG)Op->Dst->Width;
        dstH = (LONG)Op->Dst->Height;
        dstPitch = (LONG)(Op->Dst->Pitch ? Op->Dst->Pitch : Op->Dst->Width * 4);
    }
    if (Op->Kind == MW_PRESENT_BLT) {
        src = AllocationVa(A, Op->Src);
        if (src == NULL) {
            A->PresentSkips++;
            return;
        }
        srcW = (LONG)Op->Src->Width;
        srcH = (LONG)Op->Src->Height;
        srcPitch = (LONG)(Op->Src->Pitch ? Op->Src->Pitch : Op->Src->Width * 4);
    }

    for (ULONG i = 0; i < n; i++) {
        RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;

        ClipRect(&r, dstW, dstH);
        if (Op->Kind == MW_PRESENT_BLT) {
            /* Keep the source side inside the source too. */
            if (r.left + dx < 0) r.left = -dx;
            if (r.top + dy < 0) r.top = -dy;
            if (r.right + dx > srcW) r.right = srcW - dx;
            if (r.bottom + dy > srcH) r.bottom = srcH - dy;
        }
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        for (LONG y = r.top; y < r.bottom; y++) {
            ULONG *d = (ULONG *)(dst + (SIZE_T)y * dstPitch) + r.left;
            if (Op->Kind == MW_PRESENT_BLT) {
                const ULONG *s = (const ULONG *)(src + (SIZE_T)(y + dy) * srcPitch) + r.left + dx;
                RtlCopyMemory(d, s, (SIZE_T)(r.right - r.left) * 4);
            } else {
                for (LONG x = 0; x < r.right - r.left; x++)
                    d[x] = Op->Color;
            }
        }
    }
    if (Op->Kind == MW_PRESENT_BLT)
        A->PresentBlts++;
    else
        A->PresentFills++;
    if (Op->Kind == MW_PRESENT_BLT && Op->Dst != NULL && (Op->Dst->Flags & MW_ALLOC_PRIMARY) && src != NULL) {
        RECT r = Op->SrcRect;
        ClipRect(&r, srcW, srcH);
        if (NotBlack(src, srcPitch, r.left, r.top, r.right, r.bottom))
            A->ContentFrames++;
    }
}

NTSTATUS APIENTRY MwPatch(IN_CONST_HANDLE hAdapter, const DXGKARG_PATCH *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    /* Nothing to patch; record where VidMm put the first allocation. */
    a->Patches++;
    a->PatchAllocations = Arg->AllocationListSize;
    a->PatchPatchLocations = Arg->PatchLocationListSubmissionLength;
    if (Arg->AllocationListSize != 0 && Arg->pAllocationList != NULL) {
        a->PatchSegment0 = Arg->pAllocationList[0].SegmentId;
        a->PatchAddress0 = (ULONG64)Arg->pAllocationList[0].PhysicalAddress.QuadPart;
    }
    return STATUS_SUCCESS;
}

/* ---- the queue, at interrupt level ---- */

static void NotifyCompleted(MW_ADAPTER *A, ULONG Fence)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    n.DmaCompleted.SubmissionFenceId = Fence;
    n.DmaCompleted.NodeOrdinal = 0;
    n.DmaCompleted.EngineOrdinal = 0;
    A->Dxgk.DxgkCbNotifyInterrupt(A->Dxgk.DeviceHandle, &n);
    A->LastCompletedFence = Fence;
    A->NeedDpc = TRUE;
}

/* Start the next job chain, completing every submission that has none left.
 * Called with the queue owned (interrupt level). */
static void Kick(MW_ADAPTER *A)
{
    while (!A->Running && A->QCount != 0) {
        MW_SUBMISSION *s = &A->Queue[A->QHead];

        /* S9 port: while the watchdog says Stop/Critical, or is silent, work completes without running
         * (frames are dropped; the GPU stays idle until it is cool again). */
        if (!A->GpuUp || A->ThermalHold || s->Next >= s->Count) {
            NotifyCompleted(A, s->FenceId);
            A->QHead = (A->QHead + 1) % MW_QUEUE_DEPTH;
            A->QCount--;
            continue;
        }
        const MW_COMMAND *c = &s->Commands[s->Next++];
        A->RunningSlot = c->Slot;
        A->Running = TRUE;
        A->JobStart = KeQueryInterruptTime();
        (void)MkJobTakeStatus(&A->Gpu, c->Slot);
        InterlockedExchange(&A->Gpu.MmuIrqStatus, 0);
        MkJobStart(&A->Gpu, c->Slot, c->Jc, (A->ChainFlag++ & 1) != 0);
    }
}

/* Collect the running chain's outcome. TRUE if it finished, either way. */
static BOOLEAN Reap(MW_ADAPTER *A, BOOLEAN CheckTimeout)
{
    MK_GPU *gpu = &A->Gpu;
    ULONG slot = A->RunningSlot;
    ULONG bits;
    BOOLEAN failed;

    if (!A->Running)
        return FALSE;
    bits = MkJobTakeStatus(gpu, slot);
    if (bits & (1u << (16 + slot)) || gpu->MmuIrqStatus != 0) {
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        failed = TRUE;
    } else if (bits & (1u << slot)) {
        failed = FALSE;
    } else if (CheckTimeout && (KeQueryInterruptTime() - A->JobStart) / 10000 > MW_JOB_TIMEOUT_MS) {
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        MkJobHardStop(gpu, slot);
        A->JobsTimedOut++;
        failed = TRUE;
    } else {
        return FALSE;
    }

    A->Running = FALSE;
    if (!failed) {
        A->JobsDone++;
        return TRUE;
    }
    /* A fault leaves the address space stopped: start the GPU over, and
     * drop the rest of this submission (its later chains depend on it). */
    A->JobsFailed++;
    A->LastFaultStatus = gpu->LastMmuFault;
    A->LastFaultAddress = gpu->LastFaultAddress;
    A->Queue[A->QHead].Next = A->Queue[A->QHead].Count;
    gpu->Resets++;
    if (!MkGpuBringup(gpu))
        A->GpuUp = FALSE;
    return TRUE;
}

/* From the ISR, after MkIsr has collected the status. */
BOOLEAN MwJobInterrupt(MW_ADAPTER *A)
{
    if (Reap(A, FALSE))
        Kick(A);
    if (A->NeedDpc) {
        A->NeedDpc = FALSE;
        A->Dxgk.DxgkCbQueueDpc(A->Dxgk.DeviceHandle);
    }
    return TRUE;
}

typedef struct {
    MW_ADAPTER *A;
    const MW_SUBMISSION *New;
    NTSTATUS Status;
} MW_SYNC;

static BOOLEAN SyncSubmit(PVOID Context)
{
    MW_SYNC *s = (MW_SYNC *)Context;
    MW_ADAPTER *a = s->A;

    if (a->QCount == MW_QUEUE_DEPTH) {
        s->Status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        return FALSE;
    }
    a->Queue[(a->QHead + a->QCount) % MW_QUEUE_DEPTH] = *s->New;
    a->QCount++;
    a->LastSubmittedFence = s->New->FenceId;
    Kick(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    s->Status = STATUS_SUCCESS;
    return TRUE;
}

static BOOLEAN SyncPoll(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (Reap(a, TRUE))
        Kick(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    return a->QCount != 0;
}

static BOOLEAN Synchronize(MW_ADAPTER *A, PKSYNCHRONIZE_ROUTINE Routine, PVOID Context)
{
    BOOLEAN ret = FALSE;
    NTSTATUS st = A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, Routine, Context, 0, &ret);
    return NT_SUCCESS(st) && ret;
}

/* The fallback for interrupts that never arrive, and the job timeout. */
static VOID PollDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (a->Stopping) {
        InterlockedExchange(&a->PollArmed, 0);
        return;
    }
    if (Synchronize(a, SyncPoll, a)) {
        MwArmPoll(a);
        return;
    }
    /* Idle. A submission that raced with this saw PollArmed set and did
     * not arm the timer, so look once more after letting go. */
    InterlockedExchange(&a->PollArmed, 0);
    KeMemoryBarrier();
    if (a->QCount != 0 && InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
}

void MwArmPoll(MW_ADAPTER *A)
{
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * MW_POLL_MS;
    KeSetTimer(&A->PollTimer, due, &A->PollDpc);
}

/* ---- VSync, emulated ---- */

static BOOLEAN SyncVsync(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    n.CrtcVsync.VidPnTargetId = 0;
    n.CrtcVsync.PhysicalAddress = a->ScanAddr;   /* the flip this vsync completes */
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle, &n);
    return TRUE;
}

/* The primary just made current, to the framebuffer: there is no scanout
 * engine pointed at our allocations, so a flip is a CPU copy. Only when the
 * primary changes, not on every vsync. */
static void ShowPrimary(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    PUCHAR src;
    ULONG w, h, pitch;

    if (A->FbVa == NULL || P->Mdl == NULL) {
        A->FlipCopySkips++;
        return;
    }
    src = AllocationVa(A, P);             /* S9 port: the view with the pages' own caching */
    if (src == NULL) {
        A->FlipCopySkips++;
        return;
    }
    w = min(P->Width ? P->Width : A->Fb.Width, A->Fb.Width);
    h = min(P->Height ? P->Height : A->Fb.Height, A->Fb.Height);
    pitch = P->Pitch ? P->Pitch : w * 4;
    for (ULONG y = 0; y < h; y++)
        RtlCopyMemory(A->FbVa + (SIZE_T)y * A->Fb.Pitch, src + (SIZE_T)y * pitch, (SIZE_T)w * 4);
    A->FlipCopies++;
    if (NotBlack(src, (LONG)pitch, 0, 0, (LONG)w, (LONG)h))
        A->ContentFrames++;
}

static VOID VsyncDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    MW_ALLOCATION *p;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (a->Stopping || !a->VsyncOn)
        return;
    p = a->ScanAlloc;
    if (p != NULL && p != a->ShownAlloc && a->SourceVisible) {
        ShowPrimary(a, p);
        a->ShownAlloc = p;
    }
    a->Vsyncs++;
    if (Synchronize(a, SyncVsync, a))
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}

void MwCommandInit(MW_ADAPTER *A)
{
    A->Stopping = FALSE;
    KeInitializeTimer(&A->PollTimer);
    KeInitializeDpc(&A->PollDpc, PollDpc, A);
    KeInitializeTimer(&A->VsyncTimer);
    KeInitializeDpc(&A->VsyncDpc, VsyncDpc, A);
}

void MwCommandStop(MW_ADAPTER *A)
{
    A->Stopping = TRUE;
    KeMemoryBarrier();
    KeCancelTimer(&A->PollTimer);
    InterlockedExchange(&A->VsyncOn, 0);
    KeCancelTimer(&A->VsyncTimer);
    KeFlushQueuedDpcs();
    InterlockedExchange(&A->PollArmed, 0);
}

NTSTATUS APIENTRY MwSubmitCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    const MW_DMA_PRIVATE *priv = (const MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    MW_SUBMISSION sub;
    MW_SYNC sync;

    RtlZeroMemory(&sub, sizeof(sub));
    sub.FenceId = Arg->SubmissionFenceId;
    /* A present: done here, on the CPU; the submission then carries no
     * job and completes at once. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Present.Kind != MW_PRESENT_NONE) {
        /* S9 port (T13d): the source may still be being drawn by a job submitted before this present (one
         * in-order queue). Copy only once everything before it has completed: the job interrupt keeps
         * completing work while this waits (it runs above our IRQL). Bounded; a timeout is counted. */
        ULONG64 t0 = KeQueryInterruptTime();
        while (a->QCount != 0 && !a->Stopping && KeQueryInterruptTime() - t0 < 2000000ULL)
            KeStallExecutionProcessor(20);
        if (a->QCount != 0)
            a->PresentWaitTimeouts++;
        DoPresent(a, &priv->Present);
    }
    /* Paging buffers (empty: map/unmap happen on the CPU), presents and
     * anything without our private data carry no jobs. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Count <= MW_MAX_COMMANDS) {
        sub.Count = priv->Count;
        RtlCopyMemory(sub.Commands, priv->Commands, sub.Count * sizeof(MW_COMMAND));
    }

    sync.A = a;
    sync.New = &sub;
    sync.Status = STATUS_UNSUCCESSFUL;
    (void)Synchronize(a, SyncSubmit, &sync);
    if (NT_SUCCESS(sync.Status) && sub.Count != 0 && !a->Stopping &&
        InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
    a->Submits++;
    return sync.Status;
}

NTSTATUS APIENTRY MwPreemptCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *Arg)
{
    /* Preemption is off (EnablePreemption=0 in the INF). */
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryCurrentFence(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    Arg->CurrentFence = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

/* ---- recovery ---- */

static BOOLEAN SyncReset(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (a->Running)
        MkJobHardStop(&a->Gpu, a->RunningSlot);
    a->Running = FALSE;
    a->QHead = a->QCount = 0;
    a->NeedDpc = FALSE;
    if (a->Gpu.Regs != NULL) {
        a->Gpu.Resets++;
        a->GpuUp = MkGpuBringup(&a->Gpu);
    }
    return TRUE;
}

NTSTATUS APIENTRY MwResetFromTimeout(IN_CONST_HANDLE hAdapter)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    (void)Synchronize(a, SyncReset, a);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRestartFromTimeout(IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCollectDbgInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    if (Arg->pBuffer != NULL && Arg->BufferSize > 0)
        (void)RtlStringCbPrintfA((NTSTRSAFE_PSTR)Arg->pBuffer, Arg->BufferSize,
                                 "maliwddm: submitted %u completed %u running %u queued %u "
                                 "done %u failed %u timedout %u js 0x%x fault 0x%x @0x%llx",
                                 a->LastSubmittedFence, a->LastCompletedFence, a->Running, a->QCount,
                                 a->JobsDone, a->JobsFailed, a->JobsTimedOut, a->LastJsStatus,
                                 a->LastFaultStatus, a->LastFaultAddress);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwGetNodeMetadata(IN_CONST_HANDLE hAdapter, UINT NodeOrdinal,
                                    DXGKARG_GETNODEMETADATA *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    RtlZeroMemory(Arg, sizeof(*Arg));
    Arg->EngineType = DXGK_ENGINE_TYPE_3D;
    (void)RtlStringCbCopyW(Arg->FriendlyName, sizeof(Arg->FriendlyName), L"Mali-G52 job manager");
    return STATUS_SUCCESS;
}

/* ---- what WDDM 1.2+ requires of a full driver ----
 * dxgkrnl failed the adapter (code 43) right after DRIVERCAPS while these
 * five were NULL; viogpu3d fills all of them. */

NTSTATUS APIENTRY MwResetEngine(IN_CONST_HANDLE hAdapter, DXGKARG_RESETENGINE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    (void)Synchronize(a, SyncReset, a);
    Arg->LastAbortedFenceId = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryEngineStatus(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCancelCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

/* No vsync interrupt yet (the VOP2 is not ours to touch). */
/* Only VSync is controllable here. Before this, nothing ever raised one,
 * and on installed Windows (2026-10-17) the desktop hung right after the
 * commit: ControlInterrupt was called, then no SetVidPnSourceAddress or
 * Present ever came, while dxgkrnl's flip queue kept growing. */
NTSTATUS APIENTRY MwControlInterrupt(IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE Type,
                                     IN_BOOLEAN Enable)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    if (Type != DXGK_INTERRUPT_CRTC_VSYNC)
        return STATUS_SUCCESS;
    if (Enable) {
        if (InterlockedExchange(&a->VsyncOn, 1) == 0) {
            LARGE_INTEGER due;
            due.QuadPart = -166667;           /* 16.7 ms, relative */
            KeSetTimerEx(&a->VsyncTimer, due, 16, &a->VsyncDpc);
        }
    } else if (InterlockedExchange(&a->VsyncOn, 0) != 0) {
        KeCancelTimer(&a->VsyncTimer);
    }
    MwLog(a, L"VsyncOn", (ULONG)a->VsyncOn);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwGetScanLine(IN_CONST_HANDLE hAdapter, DXGKARG_GETSCANLINE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->InVerticalBlank = FALSE;
    Arg->ScanLine = 0;
    return STATUS_SUCCESS;
}

/* GDI hardware acceleration (SupportKernelModeCommandBuffer). A render-only
 * adapter is never GDI's, so this is not expected to be called; it says so
 * instead of building a DMA buffer it cannot. */
NTSTATUS APIENTRY MwRenderKm(IN_CONST_HANDLE hContext, DXGKARG_RENDER *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    UNREFERENCED_PARAMETER(Arg);
    MwLog(ctx->Device->Adapter, L"RenderKmCalled", 1);
    return STATUS_NOT_SUPPORTED;
}

/* The third DDI SupportPerEngineTDR requires (with ResetEngine and
 * QueryEngineStatus): dxgkrnl said so in WinPE run 10 ("Driver reports
 * SupportPerEngineTDR cap but does not fill in all of the required DDIs")
 * and failed the start with STATUS_INVALID_PARAMETER. One engine, so
 * nothing depends on it. */
NTSTATUS APIENTRY MwQueryDependentEngineGroup(IN_CONST_HANDLE hAdapter,
                                              DXGKARG_QUERYDEPENDENTENGINEGROUP *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->DependentNodeOrdinalMask = 0;
    return STATUS_SUCCESS;
}
