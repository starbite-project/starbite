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
        op->Kind = MW_PRESENT_FLIP;
        op->Src = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
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

/* Copy (or fill) a present's rectangles into one destination. Returns the pixels written. */
static ULONG64 CopyRects(const MW_PRESENT_OP *Op, PUCHAR Dst, LONG DstW, LONG DstH, LONG DstPitch,
                         const UCHAR *Src, LONG SrcW, LONG SrcH, LONG SrcPitch, RECT *Last)
{
    ULONG n = Op->NumRects ? Op->NumRects : 1;
    ULONG64 px = 0;

    for (ULONG i = 0; i < n; i++) {
        RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;

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
            else
                MwShow(A, Op->Src, TRUE); /* the guard thread copies it to the panel */
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
        if (Op->Kind == MW_PRESENT_BLT)
            (void)CopyBlt(A, Op, dst, dstW, dstH, dstPitch, src, srcW, srcH, srcPitch, &nr);   /* T27 */
        else
            (void)CopyRects(Op, dst, dstW, dstH, dstPitch, src, srcW, srcH, srcPitch, &nr);
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
        if (Op->Kind == MW_PRESENT_BLT && Op->Src != NULL && Op->Src->Width == A->Fb.Width &&
            Op->Src->Height == A->Fb.Height) {
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
        if (Op->Kind == MW_PRESENT_BLT)
            px = CopyBlt(A, Op, A->FbVa, (LONG)A->Fb.Width, (LONG)A->Fb.Height, (LONG)A->Fb.Pitch, src, srcW, srcH,
                         srcPitch, &last);
        else
            px = CopyRects(Op, A->FbVa, (LONG)A->Fb.Width, (LONG)A->Fb.Height, (LONG)A->Fb.Pitch, src, srcW, srcH,
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
        (void)MkJobTakeStatus(&A->Gpu, c->Slot);
        InterlockedExchange(&A->Gpu.MmuIrqStatus, 0);
        A->RunningJc = c->Jc;
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
    {
        /* T39: this failure against the newest freed ranges (newest first) */
        LONG n = A->FaultCount;
        ULONG ms = (ULONG)((KeQueryInterruptTime() - A->StartTime) / 10000);
        ULONG64 va = gpu->MmuIrqStatus != 0 ? gpu->LastFaultAddress : 0;
        LONG next = A->FreedNext;
        A->Faults[n & 15].Va = va;
        A->Faults[n & 15].Status = gpu->MmuIrqStatus != 0 ? gpu->LastMmuFault : 0;
        A->Faults[n & 15].Js = A->LastJsStatus;
        A->Faults[n & 15].Ms = ms;
        A->Faults[n & 15].Submits = A->Submits;
        A->Faults[n & 15].Kind = va ? 1 : 0;      /* 0 not an MMU fault, 1 not in a recently freed range */
        for (LONG k = 1; va && k <= 256 && k <= next; k++) {
            LONG i = (next - k) & 255;
            ULONG64 base = A->Freed[i].Va, end = base + ((ULONG64)A->Freed[i].Pages << PAGE_SHIFT);
            if (base && va >= (base & ~0xFFFULL) && va < end) {
                A->Faults[n & 15].Kind = 2;       /* inside a range freed before */
                A->Faults[n & 15].FreedVa = base;
                A->Faults[n & 15].FreedPages = A->Freed[i].Pages;
                A->Faults[n & 15].Creator[0] = A->Freed[i].Creator[0];
                A->Faults[n & 15].Creator[1] = A->Freed[i].Creator[1];
                A->Faults[n & 15].AgoMs = ms - A->Freed[i].Ms;
                break;
            }
        }
        A->FaultCount = n + 1;
    }
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
    for (ULONG y = 0; y < h; y++)
        RtlCopyMemory(A->FbVa + (SIZE_T)y * A->Fb.Pitch, src + (SIZE_T)y * pitch, (SIZE_T)w * 4);
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
