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

NTKERNELAPI PCHAR PsGetProcessImageFileName(PEPROCESS Process);   /* T27: exported, undocumented */

#define MW_DMA_MAGIC            0x4D57444Du /* 'MWDM' in the private data */
#define MW_DMA_BUFFER_SIZE      (16 * 1024)
#define MW_LIST_SIZE            256     /* Panfrost submits reference many BOs */
/* T41 (docs/WIN11-GPU.md 6.50): a hang is ours to end, before Windows' TDR (TdrDelay 2 s) takes the engine. T38-T40a
 * hung the GPU (4 chains of 2 s each), and dxgmms1 then faulted (0x7E +0x41cd4): our 2000 ms chain timeout raced
 * Windows' 2 s packet timeout, and ResetEngine misreported what it dropped. Now a chain gets 750 ms and a whole
 * submission 1500 ms; past either, the GPU is hard-stopped and the rest of the submission completes (dropped). */
#define MW_JOB_TIMEOUT_MS       750
#define MW_SUB_TIMEOUT_MS       1500
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

/* The (non-primary) allocation of the render's list the chain starts in; NULL: none. T44: kept for the hang forensics. */
static MW_ALLOCATION *JcInList(const MW_COMMAND *C, const DXGKARG_RENDER *Arg)
{
    for (UINT i = 0; i < Arg->AllocationListSize; i++) {
        MW_ALLOCATION *a = (MW_ALLOCATION *)Arg->pAllocationList[i].hDeviceSpecificAllocation;
        if (a && !(a->Flags & MW_ALLOC_PRIMARY) && sbx_head_in(C->Jc, a->GpuVa, a->Size))
            return a;
    }
    return NULL;
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
            MW_ALLOCATION *in;
            if (c.Type != MW_CMD_JOB || c.Slot > 1 || c.Jc == 0)
                return STATUS_INVALID_PARAMETER;
            /* S9 port: the chain must start inside a (non-primary) allocation this render lists, which is also
             * what VidMm makes resident (patched below): never at an address the UMD merely claims. */
            in = JcInList(&c, Arg);
            if (in == NULL) {
                ctx->Device->Adapter->JcRejected++;
                return STATUS_INVALID_PARAMETER;
            }
            priv->Commands[i] = c;
            priv->JcAlloc[i] = in;            /* T44 */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }
    priv->Count = count;
    priv->Reserved = MW_DMA_MAGIC;
    {
        /* T46: which renders listed each allocation (the highest sequence wins), so a GPU fault can tell whether the
         * running submission listed the allocation it faulted in */
        LONG seq = InterlockedIncrement(&ctx->Device->Adapter->RenderSeq);
        priv->RenderSeq = seq;
        for (UINT i = 0; i < Arg->AllocationListSize; i++) {
            MW_ALLOCATION *a = (MW_ALLOCATION *)Arg->pAllocationList[i].hDeviceSpecificAllocation;
            LONG cur = a != NULL ? a->LastListed : seq;
            while (cur < seq) {
                LONG seen = InterlockedCompareExchange(&a->LastListed, seq, cur);
                if (seen == cur)
                    break;
                cur = seen;
            }
        }
    }
    /* S9 port (T13i): the runtime reuses DMA private-data buffers. Upstream left a previous present's op in
     * place, so render submissions replayed stale presents (T13h: 853 flips carried out for 92 flip presents,
     * 1,137 blts for 458), through allocation pointers that may since have been freed. */
    RtlZeroMemory(&priv->Present, sizeof(priv->Present));

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
    /* T13g: what kind of presents DWM sends (the first 8 in detail) */
    if (Arg->Flags.Flip)
        a->PresentFlipFlag++;
    else if (Arg->Flags.Blt)
        a->PresentBltFlag++;
    else if (Arg->Flags.ColorFill)
        a->PresentFillFlag++;
    else
        a->PresentOtherFlag++;
    if (a->PresentLogged < 8 && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        static const PCWSTR fl[8] = { L"Pres0_Flags", L"Pres1_Flags", L"Pres2_Flags", L"Pres3_Flags",
                                      L"Pres4_Flags", L"Pres5_Flags", L"Pres6_Flags", L"Pres7_Flags" };
        static const PCWSTR sr[8] = { L"Pres0_Src", L"Pres1_Src", L"Pres2_Src", L"Pres3_Src",
                                      L"Pres4_Src", L"Pres5_Src", L"Pres6_Src", L"Pres7_Src" };
        static const PCWSTR ds[8] = { L"Pres0_Dst", L"Pres1_Dst", L"Pres2_Dst", L"Pres3_Dst",
                                      L"Pres4_Dst", L"Pres5_Dst", L"Pres6_Dst", L"Pres7_Dst" };
        static const PCWSTR rc[8] = { L"Pres0_DstRB", L"Pres1_DstRB", L"Pres2_DstRB", L"Pres3_DstRB",
                                      L"Pres4_DstRB", L"Pres5_DstRB", L"Pres6_DstRB", L"Pres7_DstRB" };
        static const PCWSTR nr[8] = { L"Pres0_Rects", L"Pres1_Rects", L"Pres2_Rects", L"Pres3_Rects",
                                      L"Pres4_Rects", L"Pres5_Rects", L"Pres6_Rects", L"Pres7_Rects" };
        ULONG i = a->PresentLogged++;
        MW_ALLOCATION *s = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
        MW_ALLOCATION *d = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
        /* allocation: width << 16 | height (low 15 bits) | 0x8000 primary | 0x4000 the scanned one */
#define SB_ALLOC_TAG(x) ((x) == NULL ? 0u : ((x)->Width << 16 | ((x)->Height & 0x3FFF) | \
                         (((x)->Flags & MW_ALLOC_PRIMARY) ? 0x8000u : 0) | ((x) == a->ScanAlloc ? 0x4000u : 0)))
        MwLog(a, fl[i], Arg->Flags.Value);
        MwLog(a, sr[i], SB_ALLOC_TAG(s));
        MwLog(a, ds[i], SB_ALLOC_TAG(d));
        MwLog(a, rc[i], (ULONG)Arg->DstRect.right << 16 | (ULONG)Arg->DstRect.bottom);
        MwLog(a, nr[i], Arg->SubRectCnt);
#undef SB_ALLOC_TAG
    }
    if (Arg->Flags.Flip) {
        /* S9 port (T13h): T13g's DWM sent 94 flips and dxgkrnl never followed them with
         * SetVidPnSourceAddress, so the panel kept showing the primary of DWM's SetDisplayMode while its frames
         * went to the other two. The flip is carried out when it reaches the queue (DoPresent). */
        MW_PRESENT_OP *op = &priv->Present;
        LONG rot = a->Rot;
        op->Kind = MW_PRESENT_FLIP;
        op->Src = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
        /* 6.57: DWM is rotation-aware and renders its flips pre-rotated into the panel-oriented primaries, so a flip
         * is shown as it is. Counted while rotated, and whether dxgkrnl ever sets Rotate on one (it is a blt flag). */
        if (MW_ROTATED(rot)) {
            a->RotatedFlips++;
            if (a->RotateFlips)
                op->Rot = (ULONG)rot;     /* Parameters\RotateFlips = 1: through the emulated scanout, rotated */
        }
        if (Arg->Flags.Rotate)
            a->FlipRotateFlag++;
        /* T26: the source's address as of now (VidMm calls Patch only when an allocation moved: T25 saw 94 of 3,599) */
        if (Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].SegmentId != 0) {
            op->SrcPhys = Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress;
            op->SrcPhysValid = TRUE;
            a->FlipAddrPresent++;
        }
        if (op->Src != NULL && Arg->PatchLocationListOutSize > 0) {
            RtlZeroMemory(Arg->pPatchLocationListOut, sizeof(*Arg->pPatchLocationListOut));
            Arg->pPatchLocationListOut->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
            Arg->pPatchLocationListOut->SlotId = DXGK_PRESENT_SOURCE_INDEX;
            Arg->pPatchLocationListOut++;
            Arg->PatchLocationListOutSize--;
        }
    } else if (Arg->Flags.Blt || Arg->Flags.ColorFill) {
        MW_PRESENT_OP *op = &priv->Present;
        op->Kind = Arg->Flags.ColorFill ? MW_PRESENT_FILL : MW_PRESENT_BLT;
        op->Color = Arg->Color;
        {
            /* 6.57: the driver rotates a present blt only when Flags.Rotate is set ("Supporting Rotation in a Display
             * Miniport Driver"), into the path's current rotation */
            LONG rot = a->Rot;
            if (Arg->Flags.Rotate && MW_ROTATED(rot))
                op->Rot = (ULONG)rot;
        }
        /* A present's allocation list has fixed slots; an unused one has
         * no allocation. */
        op->Src = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
        op->Dst = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
        op->SrcRect = Arg->SrcRect;
        op->DstRect = Arg->DstRect;
        op->NumRects = Arg->SubRectCnt <= MW_MAX_RECTS ? Arg->SubRectCnt : 0;
        for (ULONG i = 0; i < op->NumRects; i++)
            op->Rects[i] = Arg->pDstSubRects[i];
        if (Arg->SubRectCnt > a->BltMaxSubRects)
            a->BltMaxSubRects = Arg->SubRectCnt;
        if (Arg->SubRectCnt > MW_MAX_RECTS && Arg->pDstSubRects != NULL) {
            /* T27: more rectangles than fit: their bounding box, not the whole DstRect (a full-screen copy) */
            RECT b = Arg->pDstSubRects[0];
            for (ULONG i = 1; i < Arg->SubRectCnt; i++) {
                const RECT *q = &Arg->pDstSubRects[i];
                if (q->left < b.left) b.left = q->left;
                if (q->top < b.top) b.top = q->top;
                if (q->right > b.right) b.right = q->right;
                if (q->bottom > b.bottom) b.bottom = q->bottom;
            }
            op->Rects[0] = b;
            op->NumRects = 1;
            a->BltBoundsFallback++;
        }
        if (op->Kind == MW_PRESENT_BLT) {
            /* T27: who sends the big blts (>= 256k pixels in all): the presenting process's image name */
            ULONG64 area = 0;
            if (op->NumRects == 0)
                area = (ULONG64)(op->DstRect.right - op->DstRect.left) * (ULONG64)(op->DstRect.bottom - op->DstRect.top);
            for (ULONG i = 0; i < op->NumRects; i++)
                area += (ULONG64)(op->Rects[i].right - op->Rects[i].left) * (ULONG64)(op->Rects[i].bottom - op->Rects[i].top);
            if (area >= 262144) {
                const char *nm = PsGetProcessImageFileName(PsGetCurrentProcess());
                a->BltBigProcs++;
                a->BltProcPx = (ULONG)(area > 0xFFFFFFFFull ? 0xFFFFFFFFull : area);
                if (nm != NULL) {
                    char b[16] = { 0 };
                    for (ULONG i = 0; i < 15 && nm[i]; i++)
                        b[i] = nm[i];
                    RtlCopyMemory(a->BltProc, b, 16);
                }
            }
        }
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
/* T26: a cached view of an allocation, for blt-present reads only. The GPU writes these pages without snooping the
 * CPU caches, so every line read through this view is invalidated first (InvalidateRect); 16 pixels per present are
 * compared with the write-combined view, and one mismatch turns cached reads off for the rest of the boot. T25: the
 * write-combined reads of DWM's blt presents took 330-540 ms of CPU a second during a window drag. */
static PUCHAR AllocationVaCached(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    if (Alloc == NULL || Alloc->Mdl == NULL)
        return NULL;
    if (Alloc->CpuMapCached == NULL) {
        PVOID m = NULL;
        ULONG64 t0 = KeQueryInterruptTime();
        __try {
            m = MmMapLockedPagesSpecifyCache(Alloc->Mdl, KernelMode, MmCached, NULL, FALSE,
                                             HighPagePriority | MdlMappingNoExecute);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            m = NULL;
        }
        A->CpuViewNew++;                  /* T27 */
        A->CpuViewNew100ns += KeQueryInterruptTime() - t0;
        if (m == NULL) {
            A->CpuMapFails++;
            return NULL;
        }
        if (InterlockedCompareExchangePointer(&Alloc->CpuMapCached, m, NULL) != NULL)
            MmUnmapLockedPages(m, Alloc->Mdl);
    }
    return (PUCHAR)Alloc->CpuMapCached + (SIZE_T)Alloc->MdlPage * PAGE_SIZE;
}

/* Drop (clean+invalidate, never dirty here) every cache line of a rectangle of a cached view. */
static void InvalidateRect(const UCHAR *Base, LONG Pitch, LONG L, LONG T, LONG R, LONG B)
{
    for (LONG y = T; y < B; y++) {
        ULONG_PTR p = (ULONG_PTR)(Base + (SIZE_T)y * Pitch + (SIZE_T)L * 4) & ~(ULONG_PTR)63;
        ULONG_PTR e = (ULONG_PTR)(Base + (SIZE_T)y * Pitch + (SIZE_T)R * 4);
        for (; p < e; p += 64)
            __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" : : : "memory");
}

static PUCHAR AllocationVa(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    if (Alloc == NULL || Alloc->Mdl == NULL)
        return NULL;
    if (Alloc->CpuMap == NULL) {
        PVOID m = NULL;
        ULONG64 t0 = KeQueryInterruptTime();
        __try {                           /* DISPATCH_LEVEL at most: SubmitCommand and the vsync DPC */
            m = MmMapLockedPagesSpecifyCache(Alloc->Mdl, KernelMode, Alloc->Coherent ? MmCached : MmWriteCombined,
                                             NULL, FALSE, HighPagePriority | MdlMappingNoExecute);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            m = NULL;
        }
        A->CpuViewNew++;                  /* T27 */
        A->CpuViewNew100ns += KeQueryInterruptTime() - t0;
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

/* T27: read Bytes through a view and return MB/s (each 8-byte word loaded once, as a copy would). */
static ULONG ReadMBps(const UCHAR *P, SIZE_T Bytes)
{
    volatile ULONG64 sink = 0;
    ULONG64 acc = 0, t0 = KeQueryInterruptTime(), dt;
    for (SIZE_T o = 0; o < Bytes; o += 64) {
        const volatile ULONG64 *q = (const volatile ULONG64 *)(P + o);
        acc += q[0] + q[1] + q[2] + q[3] + q[4] + q[5] + q[6] + q[7];
    }
    sink = acc;
    (void)sink;
    dt = KeQueryInterruptTime() - t0;
    return dt ? (ULONG)((ULONG64)Bytes * 10 / dt) : 0xFFFFFFFF;   /* bytes per 100 ns * 10 = MB/s */
}

/* T27: T26's cached view did not make DWM's blt copies any cheaper (cycle-t26-1: present copy 475-490 ms/s with every
 * blt read through it), so Windows may have given it the pages' existing (write-combined) attribute. Measure once:
 * 256 KB of a blt source read through the write-combined view, then through the cached view right after cleaning
 * and invalidating it (cold), then again (hot). A real cached view reads cold several times faster than the
 * write-combined one (line fills, prefetch) and hot faster still; cached reads stay on only if cold >= 1.5 x. */
static void ViewTest(MW_ADAPTER *A, const UCHAR *Wc, const UCHAR *Cached, SIZE_T Bytes)
{
    KIRQL old;
    if (Bytes > 256 * 1024)
        Bytes = 256 * 1024;
    Bytes &= ~(SIZE_T)63;
    if (Bytes < 4096)
        return;
    KeRaiseIrql(DISPATCH_LEVEL, &old);    /* no preemption inside the timings */
    for (SIZE_T o = 0; o < Bytes; o += 64)
        __asm__ volatile("dc civac, %0" : : "r"(Cached + o) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
    A->ViewWcMBps = ReadMBps(Wc, Bytes);
    A->ViewCachedColdMBps = ReadMBps(Cached, Bytes);
    A->ViewCachedHotMBps = ReadMBps(Cached, Bytes);
    KeLowerIrql(old);
    A->ViewTestDone = 1;
    if ((ULONG64)A->ViewCachedColdMBps * 2 < (ULONG64)A->ViewWcMBps * 3) {
        A->CachedBltReads = FALSE;
        A->ViewTestDone = 2;              /* 2: no faster: cached reads off */
    }
}

/* T31: a row copy with 64-byte NEON loads and stores, two in flight (128 bytes a turn). T27/T29: blt copies read
 * write-combined memory at 87 MB/s with 8-byte loads and ~300 MB/s split over four CPUs with RtlCopyMemory; wider
 * loads may keep more of the bus busy. Chosen by an A/B in the boot (Parameters\NeonCopy). */
static void CopyRowNeon(UCHAR *D, const UCHAR *S, SIZE_T N)
{
    while (N >= 128) {
        __asm__ volatile("ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%1], #64\n\t"
                         "ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%1], #64\n\t"
                         "st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64\n\t"
                         "st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%0], #64"
                         : "+r"(D), "+r"(S)
                         :
                         : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory");
        N -= 128;
    }
    if (N)
        RtlCopyMemory(D, S, N);
}

static __forceinline void CopyRow(BOOLEAN Neon, UCHAR *D, const UCHAR *S, SIZE_T N)
{
    if (Neon)
        CopyRowNeon(D, S, N);
    else
        RtlCopyMemory(D, S, N);
}

/* T27: rows [k * MW_COPY_CHUNK_ROWS, + MW_COPY_CHUNK_ROWS) of a job's rectangles taken one after another. */
static void CopyChunk(const MW_COPY_JOB *J, LONG K)
{
    LONG row = K * MW_COPY_CHUNK_ROWS, end = row + MW_COPY_CHUNK_ROWS;
    if (J->Rotated) {                     /* 6.57: MW_COPY_CHUNK_ROWS presented lines = as many whole panel rows */
        sbdc_xf_blit_lines(J->Dst, (unsigned)J->DstPitch, J->Src, (unsigned)J->SrcPitch, J->XR, J->XStart, J->NRects,
                           (unsigned)row, MW_COPY_CHUNK_ROWS, &J->Xf);
        return;
    }
    for (ULONG i = 0; i < J->NRects && row < end; i++) {
        if (row >= J->RowStart[i + 1])
            continue;
        const RECT *r = &J->R[i];
        LONG a = row - J->RowStart[i], b = (end < J->RowStart[i + 1] ? end : J->RowStart[i + 1]) - J->RowStart[i];
        SIZE_T bytes = (SIZE_T)(r->right - r->left) * 4;
        for (LONG y = r->top + a; y < r->top + b; y++)
            CopyRow(J->Neon, J->Dst + (SIZE_T)y * J->DstPitch + (SIZE_T)r->left * 4,
                    J->Src + (SIZE_T)(y + J->Dy) * J->SrcPitch + (SIZE_T)(r->left + J->Dx) * 4, bytes);
        row = J->RowStart[i + 1];
    }
}

/* T27: take chunks until none is left. A helper takes and copies each chunk at DISPATCH_LEVEL, so a chunk once taken
 * is never preempted by a thread: the submitting thread (possibly at DISPATCH_LEVEL itself) only ever waits for chunks
 * a running CPU is copying. The submitting thread itself stays at its own IRQL (nobody waits for it). */
static void CopyTakeChunks(MW_ADAPTER *A, MW_COPY_JOB *J, BOOLEAN Helper)
{
    for (;;) {
        KIRQL old = PASSIVE_LEVEL;
        LONG k;
        if (Helper)
            KeRaiseIrql(DISPATCH_LEVEL, &old);
        k = InterlockedIncrement(&J->Next) - 1;
        if (k >= J->Chunks) {
            if (Helper)
                KeLowerIrql(old);
            break;
        }
        CopyChunk(J, k);
        InterlockedIncrement(&J->Done);
        if (Helper) {
            A->CopyHelperChunks++;
            KeLowerIrql(old);
        }
    }
}

static VOID CopyHelperThread(PVOID Context)
{
    MW_ADAPTER *a = ((struct { MW_ADAPTER *A; ULONG I; } *)Context)->A;
    ULONG i = ((struct { MW_ADAPTER *A; ULONG I; } *)Context)->I;
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    for (;;) {
        (void)KeWaitForSingleObject(&a->CopyGo[i], Executive, KernelMode, FALSE, NULL);
        if (a->CopyStop)
            break;
        CopyTakeChunks(a, &a->CopyJob, TRUE);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* T27: the helper threads, once (StartDevice, PASSIVE_LEVEL). */
void MwCopyStart(MW_ADAPTER *A)
{
    OBJECT_ATTRIBUTES oa;
    A->ParallelBlt = S9GetDword(L"Parameters", L"ParallelBlt", 1);
    MwLog(A, L"ParallelBlt", A->ParallelBlt);
    A->NeonCopy = S9GetDword(L"Parameters", L"NeonCopy", 1);       /* T31: 0 off, 1 A/B, 2 always */
    MwLog(A, L"NeonCopy", A->NeonCopy);
    A->BltNoDrain = S9GetDword(L"Parameters", L"BltNoDrain", 0);   /* T31 */
    MwLog(A, L"BltNoDrain", A->BltNoDrain);
    if (A->CopyHelpersUp || A->ParallelBlt == 0)
        return;
    A->CopyStop = 0;
    A->CopyJob.Next = MW_COPY_IDLE;
    A->CopyJob.Chunks = 0;
    for (ULONG i = 0; i < MW_COPY_HELPERS; i++) {
        KeInitializeEvent(&A->CopyGo[i], SynchronizationEvent, FALSE);
        A->CopyCtx[i].A = A;
        A->CopyCtx[i].I = i;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!NT_SUCCESS(PsCreateSystemThread(&A->CopyThread[i], THREAD_ALL_ACCESS, &oa, NULL, NULL, CopyHelperThread,
                                             &A->CopyCtx[i])))
            break;
        (void)ObReferenceObjectByHandle(A->CopyThread[i], SYNCHRONIZE, *PsThreadType, KernelMode,
                                        (PVOID *)&A->CopyThreadObj[i], NULL);
        A->CopyHelpersUp++;
    }
    MwLog(A, L"CopyHelpers", (ULONG)A->CopyHelpersUp);
}

static void CopyStopHelpers(MW_ADAPTER *A)
{
    A->CopyStop = 1;
    KeMemoryBarrier();
    for (ULONG i = 0; i < MW_COPY_HELPERS; i++) {
        if (A->CopyThreadObj[i]) {
            KeSetEvent(&A->CopyGo[i], IO_NO_INCREMENT, FALSE);
            (void)KeWaitForSingleObject(A->CopyThreadObj[i], Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(A->CopyThreadObj[i]);
            A->CopyThreadObj[i] = NULL;
        }
        if (A->CopyThread[i]) {
            ZwClose(A->CopyThread[i]);
            A->CopyThread[i] = NULL;
        }
    }
    A->CopyHelpersUp = 0;
}

/* T27: run a job set up while Next was MW_COPY_IDLE (and CopyBusy taken): the helpers and this thread take its chunks;
 * returns when every chunk is done, with CopyBusy released. 6.57: shared by plain and rotated blts. */
static void CopyRunSplit(MW_ADAPTER *A, MW_COPY_JOB *J)
{
    J->Done = 0;
    KeMemoryBarrier();
    InterlockedExchange(&J->Next, 0);
    for (ULONG i = 0; i < MW_COPY_HELPERS; i++)
        if (A->CopyThreadObj[i])
            KeSetEvent(&A->CopyGo[i], IO_NO_INCREMENT, FALSE);
    CopyTakeChunks(A, J, FALSE);
    {
        ULONG64 w0 = KeQueryInterruptTime();
        BOOLEAN counted = FALSE;
        while (J->Done < J->Chunks) {  /* only chunks a running CPU took (at DISPATCH_LEVEL) */
            YieldProcessor();
            if (!counted && KeQueryInterruptTime() - w0 > 500000ULL) {
                A->CopyWaitLong++;
                counted = TRUE;
            }
        }
    }
    InterlockedExchange(&J->Next, MW_COPY_IDLE);
    InterlockedExchange(&A->CopyBusy, 0);
}

/* T27: a blt's rectangles into one destination, as CopyRects, timed; split over the CPUs when it is big (>= 64k
 * pixels) and splitting is on (ParallelBlt 2) or being measured or chosen (ParallelBlt 1: copies alternate between
 * one CPU and split until each has 32, then the faster per pixel stays). */
static ULONG64 CopyBlt(MW_ADAPTER *A, const MW_PRESENT_OP *Op, PUCHAR Dst, LONG DstW, LONG DstH, LONG DstPitch,
                       const UCHAR *Src, LONG SrcW, LONG SrcH, LONG SrcPitch, RECT *Last)
{
    MW_COPY_JOB *J = &A->CopyJob;
    RECT r[MW_MAX_RECTS];
    LONG rows = 0, dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;
    ULONG n = Op->NumRects ? Op->NumRects : 1, nr = 0;
    ULONG64 px = 0, t0 = KeQueryInterruptTime(), dt;
    BOOLEAN split = FALSE, big;

    for (ULONG i = 0; i < n && i < MW_MAX_RECTS; i++) {
        RECT q = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        ClipRect(&q, DstW, DstH);
        if (q.left + dx < 0) q.left = -dx;
        if (q.top + dy < 0) q.top = -dy;
        if (q.right + dx > SrcW) q.right = SrcW - dx;
        if (q.bottom + dy > SrcH) q.bottom = SrcH - dy;
        if (q.right <= q.left || q.bottom <= q.top)
            continue;
        r[nr++] = q;
        px += (ULONG64)(q.right - q.left) * (ULONG64)(q.bottom - q.top);
        if (Last)
            *Last = q;
    }
    if (nr == 0)
        return 0;
    big = px >= 65536;
    if (big && A->CopyHelpersUp && A->ParallelBlt != 0) {
        if (A->ParallelBlt == 2 || A->ParDecided == 2)
            split = TRUE;
        else if (A->ParDecided == 0)
            split = ((A->ElN[0] + A->ElN[1]) & 1) != 0;
    }
    /* T31: once one CPU vs split is settled, big copies alternate RtlCopyMemory and NEON (32 each), then the faster */
    BOOLEAN neon = FALSE;
    if (big && A->NeonCopy != 0 && (A->ParallelBlt != 1 || A->ParDecided != 0)) {
        if (A->NeonCopy == 2 || A->NeonDecided == 2)
            neon = TRUE;
        else if (A->NeonDecided == 0)
            neon = ((A->NeN[0] + A->NeN[1]) & 1) != 0;
    }
    if (split && InterlockedCompareExchange(&A->CopyBusy, 1, 0) != 0) {
        split = FALSE;                    /* another present is copying with the helpers */
        A->CopyBusySerial++;
    }
    if (split) {
        InterlockedExchange(&J->Next, MW_COPY_IDLE);  /* stragglers of the last job take nothing while this is set up */
        J->Dst = Dst;
        J->Src = Src;
        J->DstPitch = DstPitch;
        J->SrcPitch = SrcPitch;
        J->Dx = dx;
        J->Dy = dy;
        J->NRects = nr;
        for (ULONG i = 0; i < nr; i++) {
            J->R[i] = r[i];
            J->RowStart[i] = rows;
            rows += r[i].bottom - r[i].top;
        }
        J->RowStart[nr] = rows;
        J->Chunks = (rows + MW_COPY_CHUNK_ROWS - 1) / MW_COPY_CHUNK_ROWS;
        J->Neon = neon;
        J->Rotated = FALSE;
        CopyRunSplit(A, J);
    } else {
        for (ULONG i = 0; i < nr; i++)
            for (LONG y = r[i].top; y < r[i].bottom; y++)
                CopyRow(neon, Dst + (SIZE_T)y * DstPitch + (SIZE_T)r[i].left * 4,
                        Src + (SIZE_T)(y + dy) * SrcPitch + (SIZE_T)(r[i].left + dx) * 4,
                        (SIZE_T)(r[i].right - r[i].left) * 4);
    }
    dt = KeQueryInterruptTime() - t0;
    A->BltCopy100ns += dt;
    A->BltPx += px;
    if (px < 65536)
        A->BltArea64k++;
    else if (px < 1048576)
        A->BltArea1M++;
    else
        A->BltAreaBig++;
    if (big) {
        A->ElCopy100ns[split] += dt;
        A->ElPx[split] += px;
        A->ElN[split]++;
        if (A->NeonCopy == 1 && A->NeonDecided == 0 && (A->ParallelBlt != 1 || A->ParDecided != 0)) {
            A->NeCopy100ns[neon] += dt;
            A->NePx[neon] += px;
            A->NeN[neon]++;
            if (A->NeN[0] >= 32 && A->NeN[1] >= 32 && A->NeCopy100ns[0] && A->NeCopy100ns[1])
                A->NeonDecided = A->NePx[1] * A->NeCopy100ns[0] * 10 > A->NePx[0] * A->NeCopy100ns[1] * 11 ? 2 : 1;
        }
        if (A->ParallelBlt == 1 && A->ParDecided == 0 && A->ElN[0] >= 32 && A->ElN[1] >= 32 &&
            A->ElCopy100ns[0] && A->ElCopy100ns[1]) {
            /* pixels per time: split wins if 10 % faster */
            A->ParDecided = A->ElPx[1] * A->ElCopy100ns[0] * 10 > A->ElPx[0] * A->ElCopy100ns[1] * 11 ? 2 : 1;
        }
    }
    return px;
}

/* 6.57: a blt with Flags.Rotate while the path is rotated. Microsoft's rule (DXGK_PRESENTFLAGS.Rotate, "Supporting
 * Rotation in a Display Miniport Driver") and the KMDOD sample's semantics, as sbdisplay: the source holds the image
 * in the desktop's orientation (90/270: panel height x width), the rectangles are desktop coordinates, the
 * destination is in the panel's orientation (the shared primary keeps the unrotated mode size). sbdc_blit_xf maps
 * desktop (x, y) -> panel (host-tested with sbdisplay's KMDOD maps); the source pixel is (x + dx, y + dy) as in a
 * plain blt. Big copies are split over the CPUs like T27's (ParallelBlt; not when one CPU won its A/B) in bands of 16
 * desktop lines, each a run of whole panel rows. Returns the pixels; *Last = the last rectangle's panel rectangle. */
static ULONG64 CopyBltRot(MW_ADAPTER *A, const MW_PRESENT_OP *Op, PUCHAR Dst, LONG DstW, LONG DstH, LONG DstPitch,
                          const UCHAR *Src, LONG SrcW, LONG SrcH, LONG SrcPitch, RECT *Last)
{
    MW_COPY_JOB *J = &A->CopyJob;
    struct sbdc_xf xf;
    struct sbdc_rect r[MW_MAX_RECTS];
    unsigned sw, sh, start[MW_MAX_RECTS + 1];
    LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top, cl, ct, cr, cb;
    ULONG n = Op->NumRects ? Op->NumRects : 1, nr = 0;
    ULONG64 px = 0, t0 = KeQueryInterruptTime(), dt;
    BOOLEAN wide = FALSE, split = FALSE;
    const UCHAR *src0;

    if (Dst == NULL || Src == NULL || DstW <= 0 || DstH <= 0 || SrcW <= 0 || SrcH <= 0 || !MW_ROTATED(Op->Rot))
        return 0;
    xf.lw = A->Fb.Width;
    xf.lh = A->Fb.Height;
    xf.pw = (ULONG)DstW < A->Fb.Width ? (ULONG)DstW : A->Fb.Width;
    xf.ph = (ULONG)DstH < A->Fb.Height ? (ULONG)DstH : A->Fb.Height;
    xf.rot = Op->Rot;
    xf.den = 1;
    xf.shift = 0;
    xf.scratch = NULL;                    /* the tiled path: no per-CPU scratch rows needed */
    sbdc_xf_src_size(&xf, &sw, &sh);
    /* desktop rectangles that stay inside both the presented image and (moved by dx, dy) the source */
    cl = dx < 0 ? -dx : 0;
    ct = dy < 0 ? -dy : 0;
    cr = SrcW - dx < (LONG)sw ? SrcW - dx : (LONG)sw;
    cb = SrcH - dy < (LONG)sh ? SrcH - dy : (LONG)sh;
    src0 = Src + (LONG_PTR)dy * SrcPitch + (LONG_PTR)dx * 4;   /* desktop (x, y) reads src0 (x, y) */
    start[0] = 0;
    for (ULONG i = 0; i < n && i < MW_MAX_RECTS; i++) {
        RECT q = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        struct sbdc_rect s, pr;
        if (q.right > (LONG)A->Fb.Width)
            wide = TRUE;                  /* only desktop coordinates reach past the panel's width (90/270) */
        s.l = q.left > cl ? q.left : cl;
        s.t = q.top > ct ? q.top : ct;
        s.r = q.right < cr ? q.right : cr;
        s.b = q.bottom < cb ? q.bottom : cb;
        if (s.r <= s.l || s.b <= s.t)
            continue;
        pr = sbdc_xf_panel_rect(&xf, s);
        if (pr.r <= pr.l || pr.b <= pr.t)
            continue;
        r[nr] = s;
        start[nr + 1] = start[nr] + sbdc_xf_lines(&xf, s);
        nr++;
        px += (ULONG64)(pr.r - pr.l) * (ULONG64)(pr.b - pr.t);
        if (Last) {
            Last->left = pr.l;
            Last->top = pr.t;
            Last->right = pr.r;
            Last->bottom = pr.b;
        }
    }
    if (wide)
        A->RotBltWide++;
    if (nr == 0)
        return 0;
    if (px >= 65536 && A->CopyHelpersUp && A->ParallelBlt != 0 && A->ParDecided != 1) {
        if (InterlockedCompareExchange(&A->CopyBusy, 1, 0) == 0)
            split = TRUE;
        else
            A->CopyBusySerial++;          /* another present is copying with the helpers */
    }
    if (split) {
        InterlockedExchange(&J->Next, MW_COPY_IDLE);  /* stragglers of the last job take nothing while this is set up */
        J->Dst = Dst;
        J->Src = src0;
        J->DstPitch = DstPitch;
        J->SrcPitch = SrcPitch;
        J->Dx = 0;
        J->Dy = 0;
        J->NRects = nr;
        J->Xf = xf;
        for (ULONG i = 0; i < nr; i++) {
            J->XR[i] = r[i];
            J->XStart[i] = start[i];
        }
        J->XStart[nr] = start[nr];
        J->Chunks = (LONG)((start[nr] + MW_COPY_CHUNK_ROWS - 1) / MW_COPY_CHUNK_ROWS);
        J->Neon = FALSE;
        J->Rotated = TRUE;
        CopyRunSplit(A, J);
    } else {
        for (ULONG i = 0; i < nr; i++)
            sbdc_blit_xf(Dst, (unsigned)DstPitch, src0, (unsigned)SrcPitch, r[i], &xf);
    }
    dt = KeQueryInterruptTime() - t0;
    A->RotBlt100ns += dt;
    A->BltCopy100ns += dt;
    A->BltPx += px;
    return px;
}

/* Copy (or fill) a present's rectangles into one destination. Returns the pixels written. */
static ULONG64 CopyRects(MW_ADAPTER *A, const MW_PRESENT_OP *Op, PUCHAR Dst, LONG DstW, LONG DstH, LONG DstPitch,
                         const UCHAR *Src, LONG SrcW, LONG SrcH, LONG SrcPitch, RECT *Last)
{
    ULONG n = Op->NumRects ? Op->NumRects : 1;
    ULONG64 px = 0;

    for (ULONG i = 0; i < n; i++) {
        RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;

        if (Op->Kind == MW_PRESENT_FILL && MW_ROTATED(Op->Rot)) {
            /* 6.57: a fill with Flags.Rotate: its desktop rectangle turned into the panel's */
            struct sbdc_xf xf = { A->Fb.Width, A->Fb.Height, A->Fb.Width, A->Fb.Height, Op->Rot, 1, 0, NULL };
            struct sbdc_rect s = { r.left, r.top, r.right, r.bottom }, pr = sbdc_xf_panel_rect(&xf, s);
            r.left = pr.l;
            r.top = pr.t;
            r.right = pr.r;
            r.bottom = pr.b;
        }

        ClipRect(&r, DstW, DstH);
        if (Op->Kind == MW_PRESENT_BLT) {
            /* Keep the source side inside the source too. */
            if (r.left + dx < 0) r.left = -dx;
            if (r.top + dy < 0) r.top = -dy;
            if (r.right + dx > SrcW) r.right = SrcW - dx;
            if (r.bottom + dy > SrcH) r.bottom = SrcH - dy;
        }
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        for (LONG y = r.top; y < r.bottom; y++) {
            ULONG *d = (ULONG *)(Dst + (SIZE_T)y * DstPitch) + r.left;
            if (Op->Kind == MW_PRESENT_BLT) {
                const ULONG *s = (const ULONG *)(Src + (SIZE_T)(y + dy) * SrcPitch) + r.left + dx;
                RtlCopyMemory(d, s, (SIZE_T)(r.right - r.left) * 4);
            } else {
                for (LONG x = 0; x < r.right - r.left; x++)
                    d[x] = Op->Color;
            }
        }
        px += (ULONG64)(r.right - r.left) * (ULONG64)(r.bottom - r.top);
        if (Last)
            *Last = r;
    }
    return px;
}

/* Carry out a present on the CPU. 32-bit pixels only. */
static void DoPresent(MW_ADAPTER *A, const MW_PRESENT_OP *Op)
{
    PUCHAR dst, src = NULL;
    LONG dstW, dstH, dstPitch, srcPitch = 0, srcW = 0, srcH = 0;
    BOOLEAN toFb = Op->Dst != NULL && (Op->Dst->Flags & MW_ALLOC_PRIMARY) && A->FbVa != NULL;
    RECT last = { 0, 0, 0, 0 };
    ULONG64 px;

    if (Op->Kind == MW_PRESENT_FLIP) {
        A->EmuFlips++;
        if (Op->Src == NULL || !(Op->Src->Flags & MW_ALLOC_PRIMARY))
            A->EmuFlipsNotPrimary++;
        if (Op->Src != NULL) {
            /* T13q: what the flipped allocation holds at the moment of the flip (the refresh reads it up to
             * 200 ms later, after d3d10umd's rotation copies), and where the GPU sees it */
            PUCHAR v = AllocationVa(A, Op->Src);
            if (v != NULL) {
                LONG w = (LONG)(Op->Src->Width ? Op->Src->Width : 1), h = (LONG)(Op->Src->Height ? Op->Src->Height : 1);
                LONG pitch = (LONG)(Op->Src->Pitch ? Op->Src->Pitch : Op->Src->Width * 4);
                A->FlipPxCentre = ((const ULONG *)(v + (SIZE_T)(h / 2) * pitch))[w / 2];
                if (NotBlack(v, pitch, 0, 0, w, h))
                    A->FlipContent++;
                else
                    A->FlipBlack++;
            }
            A->FlipSrcVa = (ULONG)Op->Src->GpuVa;
            A->FlipSrcPages = Op->Src->Pages;
            /* T13s: DWM flips a primary nothing ever draws into (T13r: 0x40001340 all zeros, alpha too), while
             * its frames reach the primary as blts. A flip to an all-black allocation leaves the panel as it is. */
            if (v != NULL && !NotBlack(v, (LONG)(Op->Src->Pitch ? Op->Src->Pitch : Op->Src->Width * 4), 0, 0,
                                       (LONG)(Op->Src->Width ? Op->Src->Width : 1),
                                       (LONG)(Op->Src->Height ? Op->Src->Height : 1)))
                A->FlipsIgnoredBlack++;
            else {
                if (MW_ROTATED(Op->Rot)) {        /* 6.57: Parameters\RotateFlips = 1 */
                    A->RotFlipCpu++;
                    if (Op->Src->FbAliased)
                        A->RotFlipAliased++;      /* drawn where the panel scans: the refresh cannot turn it */
                }
                InterlockedExchange(&A->ShowRot, (LONG)Op->Rot);
                MwShow(A, Op->Src, TRUE); /* the guard thread copies it to the panel */
            }
        }
        return;
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
        {
            /* T31: what kinds of surfaces the blts copy between, and who made the full-screen ones */
#define SB_KIND(x) ((x) == NULL ? 3 : ((x)->Flags & MW_ALLOC_STD_SHADOW) ? 0 : ((x)->Flags & MW_ALLOC_STD_STAGING) ? 1 : \
                    ((x)->Flags & MW_ALLOC_STD_PRIMARY) ? 2 : 3)
            A->BltSrcKind[SB_KIND(Op->Src)]++;
            A->BltDstKind[SB_KIND(Op->Dst)]++;
#undef SB_KIND
            if ((ULONG64)srcW * (ULONG64)srcH >= 1048576) {
                A->BltSrcCreator[0] = Op->Src->Creator[0];
                A->BltSrcCreator[1] = Op->Src->Creator[1];
                if (Op->Dst != NULL) {
                    A->BltDstCreator[0] = Op->Dst->Creator[0];
                    A->BltDstCreator[1] = Op->Dst->Creator[1];
                }
            }
        }
        if (A->CachedBltReads && srcW > 0 && srcH > 0) {
            /* T26: read through a cached view, every line of the source rectangles invalidated first */
            PUCHAR c = AllocationVaCached(A, Op->Src);
            if (c != NULL && !A->ViewTestDone)
                ViewTest(A, src, c, (SIZE_T)srcPitch * (SIZE_T)srcH);   /* T27: may turn cached reads off */
            if (c != NULL && A->CachedBltReads) {
                ULONG n = Op->NumRects ? Op->NumRects : 1;
                LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;
                BOOLEAN same = TRUE;
                for (ULONG i = 0; i < n; i++) {
                    RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
                    r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
                    ClipRect(&r, srcW, srcH);
                    if (r.right > r.left && r.bottom > r.top)
                        InvalidateRect(c, srcPitch, r.left, r.top, r.right, r.bottom);
                    if (i == 0 && r.right > r.left && r.bottom > r.top) {
                        for (LONG k = 0; k < 16 && same; k++) {
                            LONG x = r.left + (r.right - r.left) * (k % 4) / 4, y = r.top + (r.bottom - r.top) * (k / 4) / 4;
                            SIZE_T o = (SIZE_T)y * srcPitch + (SIZE_T)x * 4;
                            same = *(const volatile ULONG *)(c + o) == *(const volatile ULONG *)(src + o);
                        }
                    }
                }
                if (same) {
                    src = c;
                    A->CachedBlts++;
                } else {
                    A->CachedBltMismatch++;
                    A->CachedBltReads = FALSE;
                }
            }
        }
    }
    /* A destination allocation's own pages. For a primary too (T13f): else a later flip to that primary
     * copies its stale pages over the framebuffer. */
    dst = AllocationVa(A, Op->Dst);
    if (dst != NULL) {
        RECT nr = { 0, 0, 0, 0 };
        dstW = (LONG)Op->Dst->Width;
        dstH = (LONG)Op->Dst->Height;
        dstPitch = (LONG)(Op->Dst->Pitch ? Op->Dst->Pitch : Op->Dst->Width * 4);
        if (Op->Kind == MW_PRESENT_BLT && MW_ROTATED(Op->Rot))
            (void)CopyBltRot(A, Op, dst, dstW, dstH, dstPitch, src, srcW, srcH, srcPitch, &nr);   /* 6.57 */
        else if (Op->Kind == MW_PRESENT_BLT)
            (void)CopyBlt(A, Op, dst, dstW, dstH, dstPitch, src, srcW, srcH, srcPitch, &nr);   /* T27 */
        else
            (void)CopyRects(A, Op, dst, dstW, dstH, dstPitch, src, srcW, srcH, srcPitch, &nr);
        if (!toFb && Op->Kind == MW_PRESENT_BLT && src != NULL) {
            /* T13t: an app's frame into a surface DWM samples: did pixels go in, and are they there after? */
#define SB_TAG(x) ((x)->Width << 16 | ((x)->Height & 0x3FFF) | (((x)->Flags & MW_ALLOC_PRIMARY) ? 0x8000u : 0))
            A->NpBlts++;
            A->NpSrcTag = SB_TAG(Op->Src);
            A->NpDstTag = SB_TAG(Op->Dst);
#undef SB_TAG
            A->NpRects = Op->NumRects;
            A->NpRectLT = (ULONG)nr.left << 16 | (ULONG)nr.top;
            A->NpRectRB = (ULONG)nr.right << 16 | (ULONG)nr.bottom;
            if (NotBlack(src, srcPitch, 0, 0, srcW, srcH))
                A->NpSrcContent++;
            if (NotBlack(dst, dstPitch, 0, 0, dstW, dstH))
                A->NpDstContent++;
            A->NpSrcPx = ((const ULONG *)(src + (SIZE_T)(srcH / 2) * srcPitch))[srcW / 2];
            A->NpDstPx = ((const ULONG *)(dst + (SIZE_T)(dstH / 2) * dstPitch))[dstW / 2];
        }
        if (toFb)
            A->PrimaryOwnCopies++;
    } else if (!toFb) {
        A->PresentSkips++;
        return;
    }
    if (toFb) {
        A->PrimaryBlts++;                 /* S9 port: this one reaches the panel */
        if (Op->Dst == A->ScanAlloc)
            A->PrimaryBltsToScan++;
        else
            A->PrimaryBltsOther++;
        /* T13s: DWM's present blts carry only dirty rectangles (T13f: ~1/6 of one frame in a minute) from its
         * full-screen back buffer, which holds the whole composed frame. A display engine would scan the primary
         * those rectangles accumulate in; ours never got the rest of the frame. So the panel shows the blt's
         * source, whole, when it is full-screen; otherwise the destination primary as before. */
        InterlockedExchange(&A->ShowRot, 0);   /* 6.57: what a blt shows is already in the panel's orientation */
        if (Op->Kind == MW_PRESENT_BLT && Op->Src != NULL && Op->Src->Width == A->Fb.Width &&
            Op->Src->Height == A->Fb.Height && !MW_ROTATED(Op->Rot)) {   /* 6.57: a rotated source is not the frame */
            A->ShowFromBltSrc++;
            /* T13y: NewContent FALSE: the whole source is copied to the panel only when it becomes the shown
             * allocation (or by the slow safety refresh). Every later frame's changes are the blt's own dirty
             * rectangles, which this function has already copied straight into the framebuffer. T13x copied
             * the whole 1440x2960 frame (up to 0.7 s from write-combined memory) after every blt. */
            /* T13t: DWM's back buffer, whole: is it black outside the blt's rectangle? */
            if (src != NULL) {
                if (NotBlack(src, srcPitch, 0, 0, srcW, srcH))
                    A->PbSrcFullContent++;
                else
                    A->PbSrcFullBlack++;
                A->PbSrcPx = ((const ULONG *)(src + (SIZE_T)(srcH / 4) * srcPitch))[srcW / 2];
            }
            MwShow(A, Op->Src, FALSE);
        } else {
            MwShow(A, Op->Dst, FALSE);    /* the blt itself drew into the framebuffer */
        }
        if (Op->Kind == MW_PRESENT_BLT && MW_ROTATED(Op->Rot))
            px = CopyBltRot(A, Op, A->FbVa, (LONG)A->Fb.Width, (LONG)A->Fb.Height, (LONG)A->Fb.Pitch, src, srcW,
                            srcH, srcPitch, &last);   /* 6.57 */
        else if (Op->Kind == MW_PRESENT_BLT)
            px = CopyBlt(A, Op, A->FbVa, (LONG)A->Fb.Width, (LONG)A->Fb.Height, (LONG)A->Fb.Pitch, src, srcW, srcH,
                         srcPitch, &last);
        else
            px = CopyRects(A, Op, A->FbVa, (LONG)A->Fb.Width, (LONG)A->Fb.Height, (LONG)A->Fb.Pitch, src, srcW, srcH,
                           srcPitch, &last);
        A->FbPixels += px;
        if (px) {
            A->LastFbRectLT = (ULONG)last.left << 16 | (ULONG)last.top;
            A->LastFbRectRB = (ULONG)last.right << 16 | (ULONG)last.bottom;
            /* read back what the framebuffer now holds there (T13f) */
            if (NotBlack(A->FbVa, (LONG)A->Fb.Pitch, last.left, last.top, last.right, last.bottom))
                A->FbContentFrames++;
        }
    }
    if (Op->Kind == MW_PRESENT_BLT)
        A->PresentBlts++;
    else
        A->PresentFills++;
    if (MW_ROTATED(Op->Rot))
        A->RotatedBlts++;                 /* 6.57: blts and fills carried out rotated */
    if (Op->Kind == MW_PRESENT_BLT && toFb && src != NULL) {
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
    {
        /* T25: a flip's source address, for the vsync that completes it (vsync reported only SetVidPnSourceAddress's
         * address, which DWM's present-flips never update: T23, ~200 ms a frame with the GPU 7 % busy) */
        MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
        if (priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) && priv->Reserved == MW_DMA_MAGIC &&
            priv->Present.Kind == MW_PRESENT_FLIP && Arg->pAllocationList != NULL &&
            Arg->AllocationListSize > DXGK_PRESENT_SOURCE_INDEX) {
            priv->Present.SrcPhys = Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress;
            priv->Present.SrcPhysValid = TRUE;
            a->FlipAddrPatched++;
        }
    }
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

/* T24: point DECON's IDMA channel 0 at a scanout buffer; latched at the next TE edge (sbdc_flip's sequence, without
 * its "still pending" refusal: a newer flip simply replaces one not latched yet). Any IRQL. */
void MwScanFlipHw(MW_ADAPTER *A, ULONG Slot)
{
    const struct sbdc_io *io = &A->DcIo;
    sbdc_u32 pa, v;
    if (Slot >= A->ScanSlots || A->Slot[Slot].Va == NULL || A->Dc.win >= SBDC_MAX_WIN)
        return;
    pa = (sbdc_u32)A->Slot[Slot].Pa;
    if (sbdc_flip_pending(&A->Dc))
        A->ScanFlipsForced++;
    io->wr(io->ctx, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_BASE_Y, pa);
    io->wr(io->ctx, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_BASE_Y + 4U, pa);
    io->wr(io->ctx, SBDC_DECON, SBDC_SHADOW_REQ, 1U << A->Dc.win);
    v = io->rd(io->ctx, SBDC_DECON, SBDC_TRIG);
    if ((v & (SBDC_TRIG_HW_EN | SBDC_TRIG_HW_MASK)) != SBDC_TRIG_HW_EN)
        io->wr(io->ctx, SBDC_DECON, SBDC_TRIG, (v | SBDC_TRIG_HW_EN) & ~SBDC_TRIG_HW_MASK);
    A->Dc.cur_pa = pa;
    A->ScanCur = (LONG)Slot;
    A->FbVa = A->Slot[Slot].Va;           /* every CPU path writes and samples what DECON scans */
    A->ScanFlips++;
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
            if (s->FlipSlot1 && A->ScanFlipOn)   /* T24: everything before it is done: show the frame */
                MwScanFlipHw(A, s->FlipSlot1 - 1);
            if (s->FlipPhysValid) {              /* T25: the vsyncs from now on report the flip done */
                A->ScanAddr = s->FlipPhys;
                A->FlipAddrShown++;
            }
            NotifyCompleted(A, s->FenceId);
            A->QHead = (A->QHead + 1) % MW_QUEUE_DEPTH;
            A->QCount--;
            continue;
        }
        const MW_COMMAND *c = &s->Commands[s->Next++];
        A->RunningSlot = c->Slot;
        A->Running = TRUE;
        A->JobStart = KeQueryInterruptTime();
        if (s->Next == 1)
            A->SubStart = A->JobStart;        /* T41: the submission's first chain */
        (void)MkJobTakeStatus(&A->Gpu, c->Slot);
        InterlockedExchange(&A->Gpu.MmuIrqStatus, 0);
        A->RunningJc = c->Jc;
        MkJobStart(&A->Gpu, c->Slot, c->Jc, (A->ChainFlag++ & 1) != 0);
    }
}

/* ---- T44: hang forensics ---- */

/* The raw PTE the GPU's own page table (AS0, ours) has at Va; 0: none. Any IRQL: the tables are the KMD's non-paged
 * memory and are freed only by MkMmuFree, after the DPCs and the interrupt are gone. */
static ULONG64 GpuPte(MK_GPU *Gpu, ULONG64 Va)
{
    const volatile ULONG64 *l3;

    if (Va < MK_VA_START || Va >= MK_VA_END)
        return 0;
    l3 = *(const volatile ULONG64 *volatile *)&Gpu->L3[(Va >> 30) & 3][(Va >> 21) & 511];
    if (l3 == NULL)
        return 0;
    return l3[(Va >> 12) & 511];
}

/* The page the GPU maps at Va; 0: none. KMD-scratch: the scratch page an evicted allocation points at counts as none
 * (no allocation's memory is there; the hang and fault views never map it). */
static PFN_NUMBER GpuPfn(MK_GPU *Gpu, ULONG64 Va)
{
    ULONG64 pa = GpuPte(Gpu, Va) & 0x0000FFFFFFFFF000ULL;
    return pa != 0 && pa != Gpu->ScratchPa ? (PFN_NUMBER)(pa >> PAGE_SHIFT) : 0;
}

/* MW_PTE_* for the fault record */
static UCHAR GpuPteState(MK_GPU *Gpu, ULONG64 Va)
{
    ULONG64 pa = GpuPte(Gpu, Va) & 0x0000FFFFFFFFF000ULL;
    return pa == 0 ? MW_PTE_NONE : Gpu->ScratchPa != 0 && pa == Gpu->ScratchPa ? MW_PTE_SCRATCH : MW_PTE_PAGE;
}

static BOOLEAN IsRam(const MW_ADAPTER *A, PFN_NUMBER Pfn)
{
    for (ULONG i = 0; i < A->RamRuns; i++)
        if (Pfn >= A->RamRun[i].First && Pfn < A->RamRun[i].End)
            return TRUE;
    return FALSE;
}

/* PollDpc, DISPATCH_LEVEL, just before SyncPoll: if the running chain is (within 2 ms of being) overdue, a CPU view
 * of the page JS_HEAD is in, for Reap's hang record. The page is the one the GPU's own page table maps there, mapped
 * only if it is RAM, write-combined like VidMm's views of our non-coherent segment, through a one-page MDL built here
 * into the page of system address space reserved at AddDevice (MmMapLockedPagesWithReservedMapping: DISPATCH_LEVEL at
 * most, no pool, no new PTEs). Nothing is read here: Reap decides under the interrupt lock whether it may be. */
static BOOLEAN HangViewMake(MW_ADAPTER *A, MW_HANG_VIEW *V)
{
    ULONG64 now = KeQueryInterruptTime(), start = A->JobStart, head;
    ULONG slot = A->RunningSlot;
    PFN_NUMBER pfn;
    PVOID va;

    RtlZeroMemory(V, sizeof(*V));
    if (A->HangMapVa == NULL || !A->Running || !A->GpuUp || A->Gpu.Regs == NULL || slot > 1)
        return FALSE;
    if ((now - start) / 10000 + 2 <= MW_JOB_TIMEOUT_MS && (now - A->SubStart) / 10000 + 2 <= MW_SUB_TIMEOUT_MS)
        return FALSE;                     /* Reap's own test (below), 2 ms early: it runs microseconds after this */
    if (InterlockedCompareExchange(&A->HangViewBusy, 1, 0) != 0)
        return FALSE;
    head = MK_RD(&A->Gpu, JS_SLOT(slot) + JS_HEAD_LO) | (ULONG64)MK_RD(&A->Gpu, JS_SLOT(slot) + JS_HEAD_HI) << 32;
    pfn = GpuPfn(&A->Gpu, head);
    if (pfn == 0 || !IsRam(A, pfn)) {
        A->HangViewFails++;
        InterlockedExchange(&A->HangViewBusy, 0);
        return FALSE;
    }
    MmInitializeMdl(&A->HangMdl.Mdl, NULL, PAGE_SIZE);
    A->HangMdl.Mdl.MdlFlags |= MDL_PAGES_LOCKED;
    MmGetMdlPfnArray(&A->HangMdl.Mdl)[0] = pfn;
    va = MmMapLockedPagesWithReservedMapping(A->HangMapVa, MW_TAG, &A->HangMdl.Mdl, MmWriteCombined);
    if (va == NULL) {
        A->HangViewFails++;
        InterlockedExchange(&A->HangViewBusy, 0);
        return FALSE;
    }
    V->Va = (const volatile UCHAR *)va;
    V->Page = head & ~(ULONG64)(PAGE_SIZE - 1);
    V->Pfn = pfn;
    V->JobStart = start;
    A->HangViews++;
    return TRUE;
}

/* PollDpc, DISPATCH_LEVEL, right after SyncPoll returned (Reap is done with the view). */
static void HangViewDrop(MW_ADAPTER *A, MW_HANG_VIEW *V)
{
    if (V->Va == NULL)
        return;
    MmUnmapReservedMapping(A->HangMapVa, MW_TAG, &A->HangMdl.Mdl);
    V->Va = NULL;
    InterlockedExchange(&A->HangViewBusy, 0);
}

/* The 16 dwords at JS_HEAD, through V, only when all of these hold (interrupt level, so the hung submission cannot
 * complete meanwhile): JS_HEAD lies in Al, an allocation a chain of the hung submission starts in (VidMm keeps it
 * resident and alive until its fence, which we have not reported); Al is mapped and neither framebuffer-aliased nor
 * coherent (V is write-combined); V is of JS_HEAD's page, made for this chain; and the GPU's page table still maps
 * that very page there. Otherwise nothing is read and the reason is returned. */
static ULONG HangDesc(MW_ADAPTER *A, MW_HANG *H, const MW_ALLOCATION *Al, const MW_HANG_VIEW *V)
{
    ULONG64 page = H->Head & ~(ULONG64)(PAGE_SIZE - 1);
    ULONG off = (ULONG)(H->Head & (PAGE_SIZE - 1));

    if (Al == NULL)
        return MW_HANG_DESC_UNKNOWN;
    if (H->Head & 63)
        return MW_HANG_DESC_ALIGN;        /* also keeps the 64 bytes inside the page */
    if (V == NULL || V->Va == NULL)
        return MW_HANG_DESC_NOVIEW;
    if (V->JobStart != A->JobStart || V->Page != page || !Al->Mapped || Al->FbAliased || Al->Coherent ||
        GpuPfn(&A->Gpu, page) != V->Pfn)
        return MW_HANG_DESC_MISMATCH;
    for (ULONG i = 0; i < MW_HANG_DESC_DWORDS; i++)
        H->Desc[i] = ((const volatile ULONG *)(V->Va + off))[i];
    return MW_HANG_DESC_OK;
}

/* A hung chain's state, before Reap hard-stops it. Interrupt level, inside SyncPoll (DxgkCbSynchronizeExecution),
 * like the rest of Reap: register reads and plain stores into the ring; no allocation, no registry (the guard thread
 * prints the ring at PASSIVE_LEVEL, MwHangLog). */
static void HangCapture(MW_ADAPTER *A, ULONG Slot, const MW_HANG_VIEW *V)
{
    MK_GPU *gpu = &A->Gpu;
    const MW_SUBMISSION *s = &A->Queue[A->QHead];
    LONG n = A->HangCount;
    MW_HANG *h = &A->Hangs[n & (MW_HANGS - 1)];
    ULONG64 now = KeQueryInterruptTime();
    ULONG js = JS_SLOT(Slot);
    const MW_ALLOCATION *al = NULL;

    h->Seq = 0;
    KeMemoryBarrier();
    h->Ms = (ULONG)((now - A->StartTime) / 10000);
    h->ChainMs = (ULONG)((now - A->JobStart) / 10000);
    h->SubMs = (ULONG)((now - A->SubStart) / 10000);
    h->Slot = Slot;
    h->Fence = s->FenceId;
    h->Cmd = s->Next;                     /* 1-based: Kick has moved Next past the running chain */
    h->Cmds = s->Count;
    h->Submits = A->Submits;
    h->Jc = A->RunningJc;
    h->Head = MK_RD(gpu, js + JS_HEAD_LO) | (ULONG64)MK_RD(gpu, js + JS_HEAD_HI) << 32;
    h->Tail = MK_RD(gpu, js + JS_TAIL_LO) | (ULONG64)MK_RD(gpu, js + JS_TAIL_HI) << 32;
    h->Affinity = MK_RD(gpu, js + JS_AFFINITY_LO) | (ULONG64)MK_RD(gpu, js + JS_AFFINITY_HI) << 32;
    h->JsStatus = MK_RD(gpu, js + JS_STATUS);
    h->JsConfig = MK_RD(gpu, js + JS_CONFIG);
    h->GpuStatus = MK_RD(gpu, GPU_STATUS);
    h->GpuFaultStatus = MK_RD(gpu, GPU_FAULT_STATUS);
    h->GpuFaultAddress = MK_RD(gpu, GPU_FAULT_ADDRESS_LO) | (ULONG64)MK_RD(gpu, GPU_FAULT_ADDRESS_HI) << 32;
    /* every chain runs in AS0 (MkJobStart: JS_CONFIG address space 0) */
    h->AsFaultStatus = MK_RD(gpu, AS0 + AS_FAULTSTATUS);
    h->AsFaultAddress = MK_RD(gpu, AS0 + AS_FAULTADDRESS_LO) | (ULONG64)MK_RD(gpu, AS0 + AS_FAULTADDRESS_HI) << 32;
    h->MmuRawStat = MK_RD(gpu, MMU_INT_RAWSTAT);
    for (ULONG i = 0; i < s->Count && i < MW_MAX_COMMANDS; i++) {
        const MW_ALLOCATION *c = s->JcAlloc[i];
        if (c != NULL && h->Head >= c->GpuVa && h->Head - c->GpuVa < c->Size) {
            al = c;
            break;
        }
    }
    h->AllocVa = al ? al->GpuVa : 0;
    h->AllocPages = al ? al->Pages : 0;
    h->Creator[0] = al ? al->Creator[0] : 0;
    h->Creator[1] = al ? al->Creator[1] : 0;
    RtlZeroMemory(h->Desc, sizeof(h->Desc));
    h->DescState = HangDesc(A, h, al, V);
    KeMemoryBarrier();
    h->Seq = n + 1;
    A->HangCount = n + 1;
}

/* ---- T46: fault forensics ---- */

/* Add Alloc to, or take it off, the live list, under the interrupt lock (Reap walks the list there). Without a
 * connected interrupt nothing else can walk it, so the change is then made directly. PASSIVE_LEVEL. */
typedef struct { MW_ADAPTER *A; MW_ALLOCATION *Alloc; BOOLEAN Add; } MW_SYNC_LIVE;

static BOOLEAN SyncLive(PVOID Context)
{
    MW_SYNC_LIVE *l = (MW_SYNC_LIVE *)Context;
    if (l->Add && !l->Alloc->InLive) {
        InsertTailList(&l->A->LiveAllocs, &l->Alloc->Live);
        l->Alloc->InLive = TRUE;
        l->A->LiveCount++;
    } else if (!l->Add && l->Alloc->InLive) {
        RemoveEntryList(&l->Alloc->Live);
        l->Alloc->InLive = FALSE;
        l->A->LiveCount--;
    }
    return TRUE;
}

void MwLiveLink(MW_ADAPTER *A, MW_ALLOCATION *Alloc, BOOLEAN Add)
{
    MW_SYNC_LIVE l = { A, Alloc, Add };
    BOOLEAN ret = FALSE;
    if (A->Dxgk.DxgkCbSynchronizeExecution == NULL ||
        !NT_SUCCESS(A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, SyncLive, &l, 0, &ret)))
        (void)SyncLive(&l);
}

/* A failed job, into the Faults ring. Interrupt level (Reap): register reads, a walk of the live list (changed only
 * under this lock, non-paged), the GPU's own page table, plain stores. The job header at JS_HEAD cannot be mapped
 * here; FaultPend asks MwFaultDescDpc (DISPATCH_LEVEL, before dxgkrnl hears of the dropped fence) to read it. */
static void FaultRecord(MW_ADAPTER *A, ULONG Slot)
{
    MK_GPU *gpu = &A->Gpu;
    LONG n = A->FaultCount;
    MW_FAULT *f = &A->Faults[n & 15];
    ULONG ms = (ULONG)((KeQueryInterruptTime() - A->StartTime) / 10000);
    ULONG64 va = gpu->MmuIrqStatus != 0 ? gpu->LastFaultAddress : 0;
    LONG next = A->FreedNext;
    const MW_ALLOCATION *hit = NULL, *below = NULL, *above = NULL, *head = NULL;
    ULONG walked = 0;

    f->Seq = 0;
    KeMemoryBarrier();
    /* T39: this failure against the newest freed ranges (newest first) */
    f->Va = va;
    f->Status = gpu->MmuIrqStatus != 0 ? gpu->LastMmuFault : 0;
    f->Js = A->LastJsStatus;
    f->Ms = ms;
    f->Submits = A->Submits;
    f->Kind = va ? 1 : 0;                 /* 0 not an MMU fault, 1 not in a recently freed range */
    f->FreedVa = 0;
    for (LONG k = 1; va && k <= 256 && k <= next; k++) {
        LONG i = (next - k) & 255;
        ULONG64 base = A->Freed[i].Va, end = base + ((ULONG64)A->Freed[i].Pages << PAGE_SHIFT);
        if (base && va >= (base & ~0xFFFULL) && va < end) {
            f->Kind = 2;                  /* inside a range freed before */
            f->FreedVa = base;
            f->FreedPages = A->Freed[i].Pages;
            f->Creator[0] = A->Freed[i].Creator[0];
            f->Creator[1] = A->Freed[i].Creator[1];
            f->AgoMs = ms - A->Freed[i].Ms;
            break;
        }
    }
    /* T46: the slot's JS_HEAD (before Reap's reset), and where the fault address and JS_HEAD fall among the live
     * allocations */
    f->Slot = Slot;
    f->Head = MK_RD(gpu, JS_SLOT(Slot) + JS_HEAD_LO) | (ULONG64)MK_RD(gpu, JS_SLOT(Slot) + JS_HEAD_HI) << 32;
    f->RunSeq = A->Queue[A->QHead].RenderSeq;   /* Reap: still the submission whose chain failed */
    for (PLIST_ENTRY e = A->LiveAllocs.Flink; e != &A->LiveAllocs && walked < 0x10000; e = e->Flink, walked++) {
        const MW_ALLOCATION *x = CONTAINING_RECORD(e, MW_ALLOCATION, Live);
        ULONG64 end = x->GpuVa + ((ULONG64)x->Pages << PAGE_SHIFT);
        if (x->GpuVa == 0)
            continue;
        if (f->Head >= x->GpuVa && f->Head < end)
            head = x;
        if (va == 0 || hit != NULL)
            continue;
        if (va >= x->GpuVa && va < end)
            hit = x;
        else if (x->GpuVa < va && (below == NULL || x->GpuVa > below->GpuVa))
            below = x;
        else if (x->GpuVa > va && (above == NULL || x->GpuVa < above->GpuVa))
            above = x;
    }
    f->LiveCount = A->LiveCount;
    f->Where = va == 0 ? MW_FAULT_NONE : hit != NULL ? MW_FAULT_IN_LIVE : MW_FAULT_IN_NONE;
    f->PteNow = va != 0 ? GpuPteState(gpu, va & ~(ULONG64)(PAGE_SIZE - 1)) : MW_PTE_NONE;
    if (hit != NULL) {
        A->FaultsInLive++;
        f->LVa = hit->GpuVa;
        f->LPages = hit->Pages;
        f->LCreator[0] = hit->Creator[0];
        f->LCreator[1] = hit->Creator[1];
        f->LCreatedMs = hit->CreatedMs;
        f->LMaps = hit->MapCount;
        f->LUnmaps = hit->UnmapCount;
        f->LMapMs = hit->LastMapMs;
        f->LUnmapMs = hit->LastUnmapMs;
        f->LListed = hit->LastListed;
        f->LMapped = hit->Mapped;
    } else if (va != 0) {
        A->FaultsInNone++;
        f->BelowVa = below ? below->GpuVa : 0;
        f->BelowPages = below ? below->Pages : 0;
        f->AboveVa = above ? above->GpuVa : 0;
        f->AbovePages = above ? above->Pages : 0;
    }
    /* the job header: only from a live, mapped, write-combined allocation the GPU maps at JS_HEAD */
    f->D0 = f->D4 = f->D5 = 0;
    if (f->Head & 63) {
        f->DescState = MW_FDESC_ALIGN;
    } else {
        PFN_NUMBER pfn = GpuPfn(gpu, f->Head);
        if (head == NULL || !head->Mapped || head->FbAliased || head->Coherent || pfn == 0) {
            f->DescState = MW_FDESC_NOALLOC;
        } else {
            f->DescState = MW_FDESC_PENDING;
            A->FaultPend.Seq = 0;
            KeMemoryBarrier();
            A->FaultPend.Head = f->Head;
            A->FaultPend.Pfn = pfn;
            KeMemoryBarrier();
            A->FaultPend.Seq = n + 1;
        }
    }
    KeMemoryBarrier();
    f->Seq = n + 1;
    A->FaultCount = n + 1;
}

/* The DPC routine, DISPATCH_LEVEL, before DxgkCbNotifyDpc: the job header the newest fault record asked for. Reap has
 * completed the faulted submission's fence, but dxgkrnl processes completions only in DxgkCbNotifyDpc, after this,
 * so VidMm cannot have released the allocation yet. The page is mapped like the hang view (RAM only, write-combined,
 * the reserved page, never while PollDpc holds it), and the read is kept only if the GPU still maps that page there
 * afterwards. Reading through our own mapping of a RAM page cannot fault in any case. */
void MwFaultDescDpc(MW_ADAPTER *A)
{
    LONG seq = A->FaultPend.Seq;
    ULONG64 head;
    PFN_NUMBER pfn;
    MW_FAULT *f;
    ULONG d0 = 0, d4 = 0, d5 = 0, state = MW_FDESC_NOVIEW;
    PVOID va;

    if (seq == 0 || seq == A->FaultPendDone)
        return;
    KeMemoryBarrier();
    head = A->FaultPend.Head;
    pfn = A->FaultPend.Pfn;
    KeMemoryBarrier();
    if (A->FaultPend.Seq != seq)
        return;                           /* rewritten meanwhile: the newer one is read next time */
    A->FaultPendDone = seq;
    f = &A->Faults[(seq - 1) & 15];
    if (A->HangMapVa != NULL && IsRam(A, pfn) && InterlockedCompareExchange(&A->HangViewBusy, 1, 0) == 0) {
        MmInitializeMdl(&A->HangMdl.Mdl, NULL, PAGE_SIZE);
        A->HangMdl.Mdl.MdlFlags |= MDL_PAGES_LOCKED;
        MmGetMdlPfnArray(&A->HangMdl.Mdl)[0] = pfn;
        va = MmMapLockedPagesWithReservedMapping(A->HangMapVa, MW_TAG, &A->HangMdl.Mdl, MmWriteCombined);
        if (va != NULL) {
            const volatile ULONG *w = (const volatile ULONG *)((const UCHAR *)va + (head & (PAGE_SIZE - 1)));
            d0 = w[0];
            d4 = w[4];
            d5 = w[5];
            KeMemoryBarrier();
            state = GpuPfn(&A->Gpu, head) == pfn ? MW_FDESC_OK : MW_FDESC_CHANGED;
            MmUnmapReservedMapping(A->HangMapVa, MW_TAG, &A->HangMdl.Mdl);
            A->FaultDescReads++;
        }
        InterlockedExchange(&A->HangViewBusy, 0);
    }
    if (f->Seq != seq)
        return;                           /* the record was reused */
    f->D0 = d0;
    f->D4 = d4;
    f->D5 = d5;
    f->DescState = state;
}

/* Collect the running chain's outcome. TRUE if it finished, either way. T44: View (SyncPoll only, may be NULL) is
 * PollDpc's CPU view of JS_HEAD's page, for the hang record. */
static BOOLEAN Reap(MW_ADAPTER *A, BOOLEAN CheckTimeout, const MW_HANG_VIEW *View)
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
    } else if (CheckTimeout && ((KeQueryInterruptTime() - A->JobStart) / 10000 > MW_JOB_TIMEOUT_MS ||
                                (KeQueryInterruptTime() - A->SubStart) / 10000 > MW_SUB_TIMEOUT_MS)) {
        HangCapture(A, slot, View);       /* T44: before the hard stop changes anything */
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        MkJobHardStop(gpu, slot);
        A->JobsTimedOut++;
        failed = TRUE;
    } else {
        return FALSE;
    }

    A->Running = FALSE;
    A->GpuBusy100ns += (LONG64)(KeQueryInterruptTime() - A->JobStart);   /* T23 (interrupt level: no interlock needed) */
    if (!failed) {
        A->JobsDone++;
        return TRUE;
    }
    /* A fault leaves the address space stopped: start the GPU over, and
     * drop the rest of this submission (its later chains depend on it). */
    A->JobsFailed++;
    if (A->JobsFailed == 1) {
        /* T14a: where the job chain stopped: JS_HEAD is the job that failed, JS_TAIL the last one done */
        A->Fail1Slot = slot;
        A->Fail1JcLo = (ULONG)A->RunningJc;
        A->Fail1HeadLo = MK_RD(gpu, JS_SLOT(slot) + JS_HEAD_LO);
        A->Fail1TailLo = MK_RD(gpu, JS_SLOT(slot) + 0x08);   /* JS_TAIL_LO */
        A->Fail1Status = A->LastJsStatus;
        A->Fail1JobsDone = A->JobsDone;
        A->Fail1Submits = A->Submits;
        A->Fail1Ms = (ULONG)((KeQueryInterruptTime() - A->StartTime) / 10000);
    }
    A->LastFaultStatus = gpu->LastMmuFault;
    A->LastFaultAddress = gpu->LastFaultAddress;
    FaultRecord(A, slot);                 /* T39, T46 */
    A->Queue[A->QHead].Next = A->Queue[A->QHead].Count;
    gpu->Resets++;
    if (!MkGpuBringup(gpu))
        A->GpuUp = FALSE;
    return TRUE;
}

/* From the ISR, after MkIsr has collected the status. */
BOOLEAN MwJobInterrupt(MW_ADAPTER *A)
{
    if (Reap(A, FALSE, NULL))
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

typedef struct {
    MW_ADAPTER *A;
    const MW_HANG_VIEW *View;             /* T44 */
} MW_SYNC_POLL;

static BOOLEAN SyncPoll(PVOID Context)
{
    const MW_SYNC_POLL *p = (const MW_SYNC_POLL *)Context;
    MW_ADAPTER *a = p->A;

    if (Reap(a, TRUE, p->View))
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
    MW_HANG_VIEW view;
    MW_SYNC_POLL poll;
    BOOLEAN busy;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (a->Stopping) {
        InterlockedExchange(&a->PollArmed, 0);
        return;
    }
    /* T44: a timeout due now gets a CPU view of the hung job for Reap's record (mapped here: not allowed at DIRQL) */
    poll.A = a;
    poll.View = HangViewMake(a, &view) ? &view : NULL;
    busy = Synchronize(a, SyncPoll, &poll);
    HangViewDrop(a, &view);
    if (busy) {
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

/* T14a: how many of 16 points spread over the whole framebuffer are not black (what the panel shows). */
ULONG MwFbSamples(MW_ADAPTER *A)
{
    ULONG n = 0;
    LONG w = (LONG)A->Fb.Width, h = (LONG)A->Fb.Height;
    if (A->FbVa == NULL || w <= 0 || h <= 0)
        return 0;
    for (LONG i = 0; i < 16; i++) {
        LONG x = w * (i % 4) / 4 + w / 8, y = h * (i / 4) / 4 + h / 8;
        if (((const ULONG *)(A->FbVa + (SIZE_T)y * A->Fb.Pitch))[x] & 0x00FFFFFFu)
            n++;
    }
    return n;
}

/* T13h: what the panel should show from now on. NewContent: a flip (new pixels even in the same allocation). */
void MwShow(MW_ADAPTER *A, MW_ALLOCATION *P, BOOLEAN NewContent)
{
    if (InterlockedExchangePointer((PVOID volatile *)&A->ShowAlloc, P) != P || NewContent)
        InterlockedIncrement(&A->ShowGen);
}

/* T13g/T13h: the emulated scanout, at PASSIVE_LEVEL from the guard thread. A display engine would show the
 * allocation the latest flip or blt made current; our framebuffer only gets what is copied into it. Copies
 * ShowAlloc whole, unless it is all black (so it never blanks what the blts drew). Its middle row is sampled
 * into RefPx[] (T13g's panel showed a green stripe: what are those pixels?). */
void MwRefreshScanout(MW_ADAPTER *A)
{
    MW_ALLOCATION *p = A->ShowAlloc;
    PUCHAR src;
    ULONG w, h, pitch;
    ULONG64 t0;

    if (A->FbVa == NULL || p == NULL || !p->Mapped || p->Mdl == NULL || !A->SourceVisible || A->Stopping) {
        A->RefreshSkipsNoScan++;
        return;
    }
    if (p->FbAliased) {                   /* T13z: the GPU drew it into the framebuffer itself */
        A->RefreshSkipsAliased++;
        return;
    }
    src = AllocationVa(A, p);
    if (src == NULL) {
        A->RefreshSkipsNoScan++;
        return;
    }
    w = min(p->Width ? p->Width : A->Fb.Width, A->Fb.Width);
    h = min(p->Height ? p->Height : A->Fb.Height, A->Fb.Height);
    pitch = p->Pitch ? p->Pitch : w * 4;
    A->RefSrcTag = p->Width << 16 | (p->Height & 0x3FFF) | ((p->Flags & MW_ALLOC_PRIMARY) ? 0x8000u : 0) |
                   (p == A->ScanAlloc ? 0x4000u : 0);
    for (ULONG i = 0; i < 8; i++)
        A->RefPx[i] = ((const ULONG *)(src + (SIZE_T)(h / 2) * pitch))[(w * (2 * i + 1)) / 16];
    A->ScanContent = NotBlack(src, (LONG)pitch, 0, 0, (LONG)w, (LONG)h);
    if (!A->ScanContent) {
        A->RefreshSkipsBlack++;
        return;
    }
    t0 = KeQueryInterruptTime();
    {
        /* 6.57 (Parameters\RotateFlips = 1): a flip made current while rotated is turned on the way, when it has the
         * desktop's size (90/270: panel height x width; 180: any full-screen primary). A panel-shaped primary at
         * 90/270 cannot be the unrotated desktop; it is copied as it is and counted. */
        LONG rot = A->ShowRot;
        if (MW_ROTATED(rot)) {
            struct sbdc_xf xf = { A->Fb.Width, A->Fb.Height, A->Fb.Width, A->Fb.Height, (unsigned)rot, 1, 0, NULL };
            unsigned sw, sh;
            sbdc_xf_src_size(&xf, &sw, &sh);
            if (p->Width == sw && p->Height == sh && pitch >= sw * 4) {
                sbdc_blit_xf(A->FbVa, A->Fb.Pitch, src, pitch, (struct sbdc_rect){ 0, 0, (int)sw, (int)sh }, &xf);
                goto copied;
            }
            A->RotFlipNoFit++;
        }
    }
    for (ULONG y = 0; y < h; y++)
        RtlCopyMemory(A->FbVa + (SIZE_T)y * A->Fb.Pitch, src + (SIZE_T)y * pitch, (SIZE_T)w * 4);
copied:
    t0 = (KeQueryInterruptTime() - t0) / 10;
    A->RefreshUsLast = (ULONG)t0;
    if (t0 > A->RefreshUsMax)
        A->RefreshUsMax = (ULONG)t0;
    A->Refreshes++;
    A->FlipCopies++;
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
        /* T13h: a primary made current by SetVidPnSourceAddress; copied by the guard thread, not here */
        MwShow(a, p, TRUE);
        a->ShownAlloc = p;
    }
    a->Vsyncs++;
    if (Synchronize(a, SyncVsync, a))
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}

void MwCommandInit(MW_ADAPTER *A)
{
    InitializeListHead(&A->LiveAllocs);   /* T46 */
    /* T44: AddDevice (PASSIVE_LEVEL): one page of system address space for the hang forensics' view, and which PFNs
     * are RAM (only those are ever mapped there). Either missing: hangs are recorded without the descriptor. */
    if (A->HangMapVa == NULL) {
        PPHYSICAL_MEMORY_RANGE r = MmGetPhysicalMemoryRanges();
        A->RamRuns = 0;
        for (ULONG i = 0; r != NULL && (r[i].BaseAddress.QuadPart != 0 || r[i].NumberOfBytes.QuadPart != 0) &&
                          A->RamRuns < RTL_NUMBER_OF(A->RamRun); i++) {
            A->RamRun[A->RamRuns].First = (ULONG64)r[i].BaseAddress.QuadPart >> PAGE_SHIFT;
            A->RamRun[A->RamRuns].End = (ULONG64)(r[i].BaseAddress.QuadPart + r[i].NumberOfBytes.QuadPart) >> PAGE_SHIFT;
            A->RamRuns++;
        }
        if (r != NULL)
            ExFreePool(r);
        A->HangMapVa = MmAllocateMappingAddress(PAGE_SIZE, MW_TAG);
    }
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
    CopyStopHelpers(A);                   /* T27 */
}

/* T44: RemoveDevice, after StopDevice (no poll DPC left, so no view is mapped) */
void MwCommandFini(MW_ADAPTER *A)
{
    if (A->HangMapVa != NULL) {
        MmFreeMappingAddress(A->HangMapVa, MW_TAG);
        A->HangMapVa = NULL;
    }
}

NTSTATUS APIENTRY MwSubmitCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    const MW_DMA_PRIVATE *priv = (const MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    MW_SUBMISSION sub;
    MW_SYNC sync;

    RtlZeroMemory(&sub, sizeof(sub));
    sub.FenceId = Arg->SubmissionFenceId;
    a->SubmitIrqlSeen |= 1u << (KeGetCurrentIrql() & 31);   /* T27 */
    /* A present: done here, on the CPU; the submission then carries no
     * job and completes at once. */
    BOOLEAN deferFlip = !Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
                        priv->Reserved == MW_DMA_MAGIC && priv->Present.Kind == MW_PRESENT_FLIP &&
                        priv->Present.Src != NULL && priv->Present.Src->FbAliased && a->ScanFlipOn &&
                        priv->Present.Src->ScanSlot >= 0;
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Present.Kind == MW_PRESENT_FLIP && priv->Present.SrcPhysValid) {
        sub.FlipPhys = priv->Present.SrcPhys;   /* T25 */
        sub.FlipPhysValid = TRUE;
    }
    if (deferFlip) {
        /* T24: a real flip, done when the queue reaches this submission (Kick), i.e. once the frame is drawn */
        sub.FlipSlot1 = (ULONG)priv->Present.Src->ScanSlot + 1;
        a->EmuFlips++;
        /* 6.57: under RotateFlips the GPU drew this one into a scanout buffer itself: no CPU pass can turn it in place,
         * so it is flipped as it is (never left unshown) and counted */
        if (MW_ROTATED(priv->Present.Rot)) {
            a->RotFlipCpu++;
            a->RotFlipAliased++;
        }
    } else if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Present.Kind != MW_PRESENT_NONE) {
        /* S9 port (T13d): the source may still be being drawn by a job submitted before this present (one
         * in-order queue). Copy only once everything before it has completed: the job interrupt keeps
         * completing work while this waits (it runs above our IRQL). Bounded; a timeout is counted. */
        ULONG64 t0 = KeQueryInterruptTime();
        /* T14e: a flip to a framebuffer-aliased primary reads nothing on the CPU (the GPU drew it where the panel
         * scans), so it does not wait for the queue to drain. Everything else (blts, fills, flips of other
         * allocations, which are sampled on the CPU) still waits for everything before it. */
        /* T31: a blt from one of CDD's CPU-only surfaces (GDI's shadow or staging) reads nothing the GPU writes, so
         * the CPU copy overlaps the queued GPU work instead of waiting for it (Parameters\BltNoDrain) */
        BOOLEAN cpuSrc = priv->Present.Kind == MW_PRESENT_BLT && priv->Present.Src != NULL &&
                         (priv->Present.Src->Flags & (MW_ALLOC_STD_SHADOW | MW_ALLOC_STD_STAGING)) != 0 &&
                         priv->Present.Dst != NULL && !(priv->Present.Dst->Flags & MW_ALLOC_PRIMARY);   /* not the panel */
        if (cpuSrc && a->BltNoDrain) {
            a->BltNoDrainCount++;
        } else if (priv->Present.Kind != MW_PRESENT_FLIP || priv->Present.Src == NULL || !priv->Present.Src->FbAliased) {
            while (a->QCount != 0 && !a->Stopping && KeQueryInterruptTime() - t0 < 2000000ULL)
                KeStallExecutionProcessor(20);
            if (a->QCount != 0)
                a->PresentWaitTimeouts++;
        }
        {
            /* T23: the wait above and the CPU work below, per present */
            ULONG64 t1 = KeQueryInterruptTime();
            LONG64 w = (LONG64)(t1 - t0);
            InterlockedAdd64(&a->PresentWait100ns, w);
            if (w > a->PresentWaitMax100ns)
                a->PresentWaitMax100ns = w;
            DoPresent(a, &priv->Present);
            {
                ULONG64 c = KeQueryInterruptTime() - t1;
                InterlockedAdd64(&a->PresentCpu100ns, (LONG64)c);
                if (c / 10000 > a->PresentMaxMs)
                    a->PresentMaxMs = (ULONG)(c / 10000);   /* T27: the longest single present on the CPU */
            }
        }
    }
    /* Paging buffers (empty: map/unmap happen on the CPU), presents and
     * anything without our private data carry no jobs. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Count <= MW_MAX_COMMANDS) {
        sub.Count = priv->Count;
        RtlCopyMemory(sub.Commands, priv->Commands, sub.Count * sizeof(MW_COMMAND));
        RtlCopyMemory(sub.JcAlloc, priv->JcAlloc, sub.Count * sizeof(priv->JcAlloc[0]));   /* T44 */
        sub.RenderSeq = priv->RenderSeq;  /* T46 */
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
    /* T41: everything submitted is gone: the fence we report from now on says so (dxgkrnl discards the rest) */
    a->LastCompletedFence = a->LastSubmittedFence;
    a->TdrAdapterResets++;
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
    /* T41: SyncReset dropped the running and every queued submission, so all of them are aborted, up to the newest
     * fence handed to us. T13-T40 reported LastCompletedFence here, so dxgkrnl thought nothing was dropped while those
     * packets silently vanished from our queue. */
    Arg->LastAbortedFenceId = a->LastSubmittedFence;
    a->LastCompletedFence = a->LastSubmittedFence;
    a->TdrEngineResets++;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryEngineStatus(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    /* T41: not responsive while a chain is overdue (was always 1) */
    BOOLEAN overdue = a->Running && (KeQueryInterruptTime() - a->JobStart) / 10000 > MW_JOB_TIMEOUT_MS;
    Arg->EngineStatus.Responsive = overdue ? 0 : 1;
    if (overdue)
        a->TdrQueriedHung++;
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
