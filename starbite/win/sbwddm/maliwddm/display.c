// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's display: malidod's, which drives the HDMI output under
 * Windows (M5.1). One HDMI child, the firmware's framebuffer and mode,
 * identity transforms, unspecified target frequencies. Flips land in
 * MwSetVidPnSourceAddress; for now it records the address and the
 * framebuffer is written by presents (step d).
 */
#include "maliwddm.h"
#include "../../sbdisplay/sbdc_core.h"   /* DECON addresses (T13f, read only) */

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

static void FillSourceMode(MW_ADAPTER *Dev, D3DKMDT_VIDPN_SOURCE_MODE *M)
{
    M->Type = D3DKMDT_RMT_GRAPHICS;
    M->Format.Graphics.PrimSurfSize.cx = Dev->Fb.Width;
    M->Format.Graphics.PrimSurfSize.cy = Dev->Fb.Height;
    M->Format.Graphics.VisibleRegionSize = M->Format.Graphics.PrimSurfSize;
    M->Format.Graphics.Stride = Dev->Fb.Pitch;
    M->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    M->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    M->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

/* A real 60 Hz timing. The firmware sets 1920x1080 with the CEA-861
 * timing (VIC 16: 2200x1125 total, 148.5 MHz); +280/+45 blanking gives
 * exactly that, and something plausible for other sizes. A full WDDM
 * driver may not leave the frequencies unspecified the way a display-only
 * one does: board run 4 got STATUS_GRAPHICS_INVALID_FREQUENCY from
 * pfnAddMode for the target mode, and Windows stopped at boot. */
static void FillSignal(MW_ADAPTER *Dev, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    UINT totalW = Dev->Fb.Width + 280, totalH = Dev->Fb.Height + 45;

    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = totalW;
    S->TotalSize.cy = totalH;
    S->ActiveSize.cx = Dev->Fb.Width;
    S->ActiveSize.cy = Dev->Fb.Height;
    S->PixelRate = (SIZE_T)totalW * totalH * 60;
    S->HSyncFreq.Numerator = (UINT)S->PixelRate;
    S->HSyncFreq.Denominator = totalW;
    S->VSyncFreq.Numerator = (UINT)S->PixelRate;
    S->VSyncFreq.Denominator = totalW * totalH;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* ---- PnP ---- */


/* ---- the firmware framebuffer ---- */

/* T24: register access for sbdc_core (DECON and DPU_DMA only; DSIM is never touched here) */
static sbdc_u32 ScanRd(void *Ctx, unsigned B, unsigned Off)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Ctx;
    return (B < 2 && a->DcMmio[B] != NULL) ? READ_REGISTER_ULONG((volatile ULONG *)(a->DcMmio[B] + Off)) : 0;
}

static void ScanWr(void *Ctx, unsigned B, unsigned Off, sbdc_u32 V)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Ctx;
    if (B < 2 && a->DcMmio[B] != NULL) {
        KeMemoryBarrier();
        WRITE_REGISTER_ULONG((volatile ULONG *)(a->DcMmio[B] + Off), V);
        KeMemoryBarrier();
    }
}

static void ScanDelay(void *Ctx, unsigned Us)
{
    UNREFERENCED_PARAMETER(Ctx);
    KeStallExecutionProcessor(Us);
}

/* T24: DECON flips between scanout buffers (Parameters\ScanFlip). Only after sbdc_identify finds S-Boot's live
 * pipeline as expected; anything else leaves the aliased single framebuffer of T13z. */
static void ScanStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS d, lo, hi, skip;
    ULONG want = S9GetDword(L"Parameters", L"ScanFlip", 0);
    int id;

    MwLog(A, L"ScanFlipWanted", want);
    if (!want || A->FbPfn == NULL || A->Fb.PhysicAddress.QuadPart >= 0x100000000LL)
        return;
    d.QuadPart = (LONGLONG)SBDC_DECON_PA;
    A->DcMmio[0] = (volatile UCHAR *)MmMapIoSpaceEx(d, SBDC_DECON_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    d.QuadPart = (LONGLONG)SBDC_DMA_PA;
    A->DcMmio[1] = (volatile UCHAR *)MmMapIoSpaceEx(d, SBDC_DMA_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (A->DcMmio[0] == NULL || A->DcMmio[1] == NULL) {
        MwLog(A, L"ScanFlipOn", 0x100);
        return;
    }
    A->DcIo.ctx = A;
    A->DcIo.rd = ScanRd;
    A->DcIo.wr = ScanWr;
    A->DcIo.delay_us = ScanDelay;
    id = sbdc_identify(&A->Dc, &A->DcIo, (sbdc_u32)A->Fb.PhysicAddress.QuadPart, A->Fb.Width, A->Fb.Height);
    MwLog(A, L"ScanIdentify", (ULONG)id);
    if (id != 0)
        return;
    A->Slot[0].Pa = (ULONG64)A->Fb.PhysicAddress.QuadPart;
    A->Slot[0].Va = A->FbVa;
    A->Slot[0].Pfn = A->FbPfn;
    A->ScanSlots = 1;
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFFLL;           /* DECON's base registers are 32 bits */
    skip.QuadPart = 0;
    for (ULONG i = 1; i < 3; i++) {
        PUCHAR va = (PUCHAR)MmAllocateContiguousNodeMemory(A->FbSize, lo, hi, skip, PAGE_READWRITE | PAGE_WRITECOMBINE,
                                                         MM_ANY_NODE_OK);
        PFN_NUMBER *pfn;
        ULONG64 pa;
        if (va == NULL)
            break;
        pa = (ULONG64)MmGetPhysicalAddress(va).QuadPart;
        pfn = (PFN_NUMBER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)A->FbPages * sizeof(PFN_NUMBER), MW_TAG);
        if (pfn == NULL || pa >= 0x100000000ULL || (pa & (PAGE_SIZE - 1))) {
            if (pfn)
                ExFreePoolWithTag(pfn, MW_TAG);
            MmFreeContiguousMemory(va);
            break;
        }
        for (ULONG p = 0; p < A->FbPages; p++)
            pfn[p] = (PFN_NUMBER)(pa >> PAGE_SHIFT) + p;
        RtlZeroMemory(va, A->FbSize);
        A->Slot[i].Pa = pa;
        A->Slot[i].Va = va;
        A->Slot[i].Pfn = pfn;
        A->ScanSlots = i + 1;
        MwLog(A, i == 1 ? L"ScanSlot1Pa" : L"ScanSlot2Pa", (ULONG)pa);
    }
    A->ScanCur = 0;
    A->FbVaOrig = A->FbVa;
    A->ScanFlipOn = A->ScanSlots >= 2;
    MwLog(A, L"ScanFlipOn", A->ScanFlipOn ? A->ScanSlots : 0);
}

/* T24: DECON back on the firmware framebuffer before anything else owns the display; our buffers are freed only
 * once that has latched (never free memory a display engine may still read). */
static void ScanStop(MW_ADAPTER *A)
{
    BOOLEAN home = TRUE;
    if (A->ScanSlots >= 2 && A->DcMmio[0] != NULL && A->DcMmio[1] != NULL) {
        A->ScanFlipOn = FALSE;
        if (A->ScanCur != 0) {
            if (A->FbVaOrig != NULL && A->FbVa != NULL && A->FbVa != A->FbVaOrig)
                RtlCopyMemory(A->FbVaOrig, A->FbVa, A->FbSize);   /* the current image, for the next owner */
            home = sbdc_wait_latched(&A->Dc, 50000, 50) == 0 &&
                   sbdc_flip(&A->Dc, (sbdc_u32)A->Slot[0].Pa) == 0 && sbdc_wait_latched(&A->Dc, 50000, 50) == 0;
            A->ScanCur = 0;
        }
        if (A->FbVaOrig != NULL)
            A->FbVa = A->FbVaOrig;
        for (ULONG i = 1; i < A->ScanSlots; i++) {
            if (home && A->Slot[i].Va != NULL)
                MmFreeContiguousMemory(A->Slot[i].Va);
            A->Slot[i].Va = NULL;
            /* Pfn arrays stay: GPU mappings made from them may outlive the display (page numbers only) */
        }
        A->ScanSlots = 1;
    }
    for (ULONG b = 0; b < 2; b++) {
        if (A->DcMmio[b] != NULL)
            MmUnmapIoSpace((PVOID)A->DcMmio[b], b == 0 ? SBDC_DECON_SIZE : SBDC_DMA_SIZE);
        A->DcMmio[b] = NULL;
    }
}

NTSTATUS MwDisplayStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    NTSTATUS st;

    /* 6.57: rotation (Settings > Display orientation), the KMDOD way: the path offers 90/180/270, source modes stay
     * in the panel's orientation, blt presents with Flags.Rotate are rotated in the CPU copy. 0 = identity only. */
    A->RotAllowed = S9GetDword(L"Parameters", L"Rotation", 1) != 0;
    A->RotateFlips = S9GetDword(L"Parameters", L"RotateFlips", 0);
    InterlockedExchange(&A->Rot, SBDC_ROT_0);
    InterlockedExchange(&A->ShowRot, 0);
    MwLog(A, L"RotAllowed", A->RotAllowed);
    MwLog(A, L"RotateFlips", A->RotateFlips);
    st = A->Dxgk.DxgkCbAcquirePostDisplayOwnership(A->Dxgk.DeviceHandle, &A->Fb);
    MwLog(A, L"AcquirePostDisplay", (ULONG)st);
    if (!NT_SUCCESS(st) || A->Fb.Width == 0 || BytesPerPixel(A->Fb.ColorFormat) == 0)
        return STATUS_UNSUCCESSFUL;
    MwLog(A, L"FbWidth", A->Fb.Width);
    MwLog(A, L"FbHeight", A->Fb.Height);
    MwLog(A, L"FbPitch", A->Fb.Pitch);
    MwLog(A, L"FbFormat", (ULONG)A->Fb.ColorFormat);
    MwLog(A, L"FbPaLo", (ULONG)A->Fb.PhysicAddress.LowPart);
    MwLog(A, L"FbPaHi", (ULONG)A->Fb.PhysicAddress.HighPart);
    {   /* T13f: DECON and its DMA block, read only (sbdisplay's addresses), for the guard thread's Live_Decon* */
        PHYSICAL_ADDRESS d;
        d.QuadPart = (LONGLONG)SBDC_DECON_PA;
        A->DeconVa = (volatile UCHAR *)MmMapIoSpaceEx(d, SBDC_DECON_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        d.QuadPart = (LONGLONG)SBDC_DMA_PA;
        A->DeconDmaVa = (volatile UCHAR *)MmMapIoSpaceEx(d, SBDC_DMA_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        MwLog(A, L"DeconMapped", (A->DeconVa != NULL) | (A->DeconDmaVa != NULL) << 1);
    }
    A->FbSize = (SIZE_T)A->Fb.Pitch * A->Fb.Height;
    pa = A->Fb.PhysicAddress;
    A->FbVa = (PUCHAR)MmMapIoSpaceEx(pa, A->FbSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (A->FbVa == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    /* T13z: the framebuffer's pages, for full-screen primaries the GPU then renders straight into (DECON scans
     * them). T13y: DWM's frames arrive as flips of its primaries, and each was copied whole on the CPU, up to
     * 1.4 s from write-combined memory. Physically contiguous by construction (the firmware's framebuffer). */
    A->PrimaryInFb = S9GetDword(L"Parameters", L"PrimaryInFb", 1);
    A->FbPages = (ULONG)((A->FbSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (A->PrimaryInFb && A->FbPfn == NULL) {
        A->FbPfn = (PFN_NUMBER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)A->FbPages * sizeof(PFN_NUMBER), MW_TAG);
        if (A->FbPfn != NULL)
            for (ULONG i = 0; i < A->FbPages; i++)
                A->FbPfn[i] = (PFN_NUMBER)((ULONG64)pa.QuadPart >> PAGE_SHIFT) + i;
    }
    MwLog(A, L"PrimaryInFb", A->PrimaryInFb && A->FbPfn != NULL);
    /* T26: blt presents read their source through a cached, line-invalidated view (command.c AllocationVaCached) */
    A->CachedBltReads = S9GetDword(L"Parameters", L"CachedBltReads", 0) != 0;
    MwLog(A, L"CachedBltReads", A->CachedBltReads);
    MwCopyStart(A);                       /* T27: blt copies split over the CPUs */
    if (A->PrimaryInFb)
        ScanStart(A);
    A->SourceVisible = TRUE;
    A->PathActive = TRUE;
    return STATUS_SUCCESS;
}

void MwDisplayStop(MW_ADAPTER *A)
{
    ScanStop(A);
    if (A->FbVa != NULL) {
        MmUnmapIoSpace(A->FbVa, A->FbSize);
        A->FbVa = NULL;
    }
    /* FbPfn stays: GPU mappings made from it may outlive the display (they are only page numbers) */
    if (A->DeconVa != NULL) {
        MmUnmapIoSpace((PVOID)A->DeconVa, SBDC_DECON_SIZE);
        A->DeconVa = NULL;
    }
    if (A->DeconDmaVa != NULL) {
        MmUnmapIoSpace((PVOID)A->DeconDmaVa, SBDC_DMA_SIZE);
        A->DeconDmaVa = NULL;
    }
}


/* Hand the framebuffer back, e.g. to Basic Display when we are disabled. */
NTSTATUS APIENTRY MwStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID MiniportDeviceContext,
                                                   IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    *DisplayInfo = dev->Fb;
    MwDisplayStop(dev);
    return STATUS_SUCCESS;
}


/* ---- the monitor: one HDMI output, always connected, no EDID read ---- */

NTSTATUS APIENTRY MwQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext,
                               PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
#if MW_RENDER_ONLY
    if (ChildRelationsSize < sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR)); /* the terminator alone */
    return STATUS_SUCCESS;
#else
    if (ChildRelationsSize < 2 * sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;   /* one child plus the zeroed terminator */
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR));
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HDMI;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = 0;
    return STATUS_SUCCESS;
#endif
}

NTSTATUS APIENTRY MwQueryChildStatus(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus,
                            IN_BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    switch (ChildStatus->Type) {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        /* 6.57: the angle of the display itself (a monitor with an orientation sensor, MonitorOrientationAwareness);
         * ours reports MOA_NONE and never turns, so 0 also when the desktop is rotated (the path carries that) */
        ChildStatus->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* The firmware's EDID is not at hand here: Windows falls back to the modes
 * RecommendMonitorModes adds. */
NTSTATUS APIENTRY MwQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid,
                                 INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(DeviceDescriptor);
    return STATUS_MONITOR_NO_DESCRIPTOR;
}


/* ---- VidPN: one source, one target, one mode, identity scaling; rotation as sbdisplay (6.57) ---- */

/* 6.57: a path's rotation as SBDC_ROT_* (identity when Windows leaves it open); 0 = not one we do (the OFFSET values,
 * or 90/180/270 with Parameters\Rotation = 0) */
static ULONG PathRot(MW_ADAPTER *Dev, D3DKMDT_VIDPN_PRESENT_PATH_ROTATION R)
{
    if (R == D3DKMDT_VPPR_UNINITIALIZED || R == D3DKMDT_VPPR_UNPINNED || R == D3DKMDT_VPPR_NOTSPECIFIED ||
        R == D3DKMDT_VPPR_IDENTITY)
        return SBDC_ROT_0;
    if (Dev->RotAllowed && (R == D3DKMDT_VPPR_ROTATE90 || R == D3DKMDT_VPPR_ROTATE180 || R == D3DKMDT_VPPR_ROTATE270))
        return (ULONG)R;                  /* SBDC_ROT_* are these values */
    return 0;
}

/* 6.57: the rotation of the VidPN's first path (ours has one), SBDC_ROT_0 when there is none */
static ULONG VidPnRot(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H, ULONG *Asked)
{
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    ULONG rot = SBDC_ROT_0;
    *Asked = 0;
    if (NT_SUCCESS(VidPn->pfnGetTopology(H, &topo, &topoIf)) &&
        NT_SUCCESS(topoIf->pfnAcquireFirstPathInfo(topo, &path)) && path != NULL) {
        *Asked = (ULONG)path->ContentTransformation.Rotation;
        rot = PathRot(Dev, path->ContentTransformation.Rotation);
        topoIf->pfnReleasePathInfo(topo, path);
    }
    return rot;
}

NTSTATUS APIENTRY MwIsSupportedVidPn(IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    Arg->IsVidPnSupported = FALSE;
    if (Arg->hDesiredVidPn == 0) {
        Arg->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnAcquireSourceModeSet(Arg->hDesiredVidPn, 0, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    /* Supported if nothing is pinned yet, or what is pinned is our mode. */
    if (NT_SUCCESS(st)) {
        Arg->IsVidPnSupported = pinned == NULL ||
            (pinned->Type == D3DKMDT_RMT_GRAPHICS &&
             pinned->Format.Graphics.PrimSurfSize.cx == dev->Fb.Width &&
             pinned->Format.Graphics.PrimSurfSize.cy == dev->Fb.Height);
        if (pinned != NULL)
            setIf->pfnReleaseModeInfo(set, pinned);
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hDesiredVidPn, set);
    if (Arg->IsVidPnSupported && dev->RotAllowed) {
        /* 6.57: and a rotation we do (Parameters\Rotation = 0 keeps the old check, which never looked) */
        ULONG asked;
        if (VidPnRot(dev, vidpn, Arg->hDesiredVidPn, &asked) == 0)
            Arg->IsVidPnSupported = FALSE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_RECOMMENDFUNCTIONALVIDPN *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

/* Give the source and target of a path our mode if nothing is pinned. */
static NTSTATUS AddSourceMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_SOURCE_ID Src)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    NTSTATUS st;

    st = VidPn->pfnAcquireSourceModeSet(H, Src, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL)
        setIf->pfnReleaseModeInfo(set, pinned);
    VidPn->pfnReleaseSourceModeSet(H, set);
    if (!NT_SUCCESS(st) || pinned != NULL)
        return st;

    st = VidPn->pfnCreateNewSourceModeSet(H, Src, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSourceMode(Dev, mode);
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignSourceModeSet(H, Src, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseSourceModeSet(H, set);
    return st;
}

static NTSTATUS AddTargetMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_TARGET_ID Tgt)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    NTSTATUS st;

    st = VidPn->pfnAcquireTargetModeSet(H, Tgt, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL)
        setIf->pfnReleaseModeInfo(set, pinned);
    VidPn->pfnReleaseTargetModeSet(H, set);
    if (!NT_SUCCESS(st) || pinned != NULL)
        return st;

    st = VidPn->pfnCreateNewTargetModeSet(H, Tgt, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        /* The same timing the monitor mode carries. */
        FillSignal(Dev, &mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignTargetModeSet(H, Tgt, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseTargetModeSet(H, set);
    return st;
}

NTSTATUS APIENTRY MwEnumVidPnCofuncModality(IN_CONST_HANDLE hAdapter,
                                            const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    NTSTATUS st;

    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnGetTopology(Arg->hConstrainingVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st))
        return st;

    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        /* The pivot is what the OS is enumerating against: the driver must
         * leave that mode set (or that path's transformation) alone. */
        D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pt = Arg->EnumPivotType;
        BOOLEAN srcPivot = pt == D3DKMDT_EPT_VIDPNSOURCE && Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgtPivot = pt == D3DKMDT_EPT_VIDPNTARGET && Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        BOOLEAN xfPivot = (pt == D3DKMDT_EPT_SCALING || pt == D3DKMDT_EPT_ROTATION) &&
                          Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
                          Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        NTSTATUS r = STATUS_SUCCESS;

        if (!srcPivot) {
            r = AddSourceMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddSourceMode", (ULONG)r);
        }
        if (NT_SUCCESS(r) && !tgtPivot) {
            r = AddTargetMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnTargetId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddTargetMode", (ULONG)r);
        }

        /* Identity scaling; identity rotation, and with Parameters\Rotation also 90/180/270 (6.57: done in the
         * blt presents' CPU copy, KMDOD's rule; DWM renders its flips pre-rotated), Offset0 as sbdisplay. */
        if (NT_SUCCESS(r) && !xfPivot &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            if (upd.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.ScalingSupport,
                              sizeof(upd.ContentTransformation.ScalingSupport));
                upd.ContentTransformation.ScalingSupport.Identity = 1;
            }
            if (upd.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.RotationSupport,
                              sizeof(upd.ContentTransformation.RotationSupport));
                upd.ContentTransformation.RotationSupport.Identity = 1;
                if (dev->RotAllowed) {
                    upd.ContentTransformation.RotationSupport.Rotate90 = 1;
                    upd.ContentTransformation.RotationSupport.Rotate180 = 1;
                    upd.ContentTransformation.RotationSupport.Rotate270 = 1;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM1_3_PATH_INDEPENDENT_ROTATION)
                    upd.ContentTransformation.RotationSupport.Offset0 = 1;
#endif
                }
            }
            r = topoIf->pfnUpdatePathSupportInfo(topo, &upd);
        }
        if (!NT_SUCCESS(r)) {
            topoIf->pfnReleasePathInfo(topo, path);
            return r;
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ? STATUS_SUCCESS : st;
}

NTSTATUS APIENTRY MwSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_SETVIDPNSOURCEVISIBILITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    dev->SourceVisible = Arg->Visible;
    /* Invisible means blank: clear the framebuffer once. */
    if (!Arg->Visible && dev->FbVa != NULL)
        RtlZeroMemory(dev->FbVa, dev->FbSize);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCommitVidPn(IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnAcquireSourceModeSet(Arg->hFunctionalVidPn, 0, &set, &setIf);
    if (st == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE || !NT_SUCCESS(st)) {
        /* No path for our source: it is being turned off. */
        dev->PathActive = FALSE;
        MwLog(dev, L"CommitNoPath", (ULONG)st);
        return STATUS_SUCCESS;
    }
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        BOOLEAN ours = pinned->Type == D3DKMDT_RMT_GRAPHICS &&
                       pinned->Format.Graphics.PrimSurfSize.cx == dev->Fb.Width &&
                       pinned->Format.Graphics.PrimSurfSize.cy == dev->Fb.Height;
        setIf->pfnReleaseModeInfo(set, pinned);
        st = ours ? STATUS_SUCCESS : STATUS_GRAPHICS_INVALID_VIDPN;
        dev->PathActive = ours;
        if (ours && dev->RotAllowed) {
            /* 6.57: the path's rotation. One we never offered (an OFFSET value) is logged and shown unrotated rather
             * than refused: a refused commit in full mode can leave the boot without a desktop. */
            ULONG asked, rot = VidPnRot(dev, vidpn, Arg->hFunctionalVidPn, &asked);
            dev->RotPathAsked = asked;
            MwLog(dev, L"CommitRotationAsked", asked);
            InterlockedExchange(&dev->Rot, (LONG)(rot ? rot : SBDC_ROT_0));
            MwLog(dev, L"Rotation", (ULONG)dev->Rot);
        }
    } else if (NT_SUCCESS(st)) {
        /* Nothing pinned: the source is being turned off. AcquirePinnedModeInfo
         * says so with an informational status (0x401E0307), which is not
         * ours to return: dxgkrnl logged "Driver returned an invalid NTSTATUS
         * code: 0x401e0307" for every such commit. */
        dev->PathActive = FALSE;
        MwLog(dev, L"CommitUnpinned", (ULONG)st);
        st = STATUS_SUCCESS;
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hFunctionalVidPn, set);
    MwLog(dev, L"CommitStatus", (ULONG)st);
    return st;
}

/* 6.57: orientation changes arrive here without a new mode (SupportSmoothRotation), as in sbdisplay */
NTSTATUS APIENTRY MwUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter,
                                                 const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    ULONG rot;
    if (!dev->RotAllowed)
        return STATUS_SUCCESS;            /* Parameters\Rotation = 0: as before */
    dev->RotPathAsked = (ULONG)Arg->VidPnPresentPathInfo.ContentTransformation.Rotation;
    MwLog(dev, L"PathRotationAsked", dev->RotPathAsked);
    rot = PathRot(dev, Arg->VidPnPresentPathInfo.ContentTransformation.Rotation);
    if (!rot)
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    if ((LONG)rot != dev->Rot) {
        InterlockedExchange(&dev->Rot, (LONG)rot);
        MwLog(dev, L"Rotation", rot);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRecommendMonitorModes(IN_CONST_HANDLE hAdapter,
                                          const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS st;

    st = Arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Arg->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(st))
        return st;
    FillSignal(dev, &mode->VideoSignalInfo);
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 0;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    st = Arg->pMonitorSourceModeSetInterface->pfnAddMode(Arg->hMonitorSourceModeSet, mode);
    MwLog(dev, L"MonitorAddMode", (ULONG)st);
    if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        Arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Arg->hMonitorSourceModeSet, mode);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryVidPnHWCapability(IN_CONST_HANDLE hAdapter,
                                           DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    RtlZeroMemory(&Arg->VidPnHWCaps, sizeof(Arg->VidPnHWCaps));
    Arg->VidPnHWCaps.DriverRotation = dev->RotAllowed ? 1 : 0;   /* 6.57: rotated by the driver, in software */
    return STATUS_SUCCESS;
}


/* ---- the bugcheck screen ---- */

NTSTATUS APIENTRY MwSystemDisplayEnable(IN_CONST_PVOID MiniportDeviceContext,
                               IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                               PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                               OUT UINT *Width, OUT UINT *Height, OUT D3DDDIFORMAT *ColorFormat)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    if (dev->FbVa == NULL)
        return STATUS_UNSUCCESSFUL;
    *Width = dev->Fb.Width;
    *Height = dev->Fb.Height;
    *ColorFormat = dev->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

VOID APIENTRY MwSystemDisplayWrite(IN_CONST_PVOID MiniportDeviceContext, IN_CONST_PVOID Source,
                          UINT SourceWidth, UINT SourceHeight, UINT SourceStride,
                          UINT PositionX, UINT PositionY)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    if (dev->FbVa == NULL)
        return;
    for (UINT y = 0; y < SourceHeight && PositionY + y < dev->Fb.Height; y++) {
        UINT w = min(SourceWidth, dev->Fb.Width > PositionX ? dev->Fb.Width - PositionX : 0);
        RtlCopyMemory(dev->FbVa + (SIZE_T)(PositionY + y) * dev->Fb.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR *)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
    }
}


/* Called at interrupt level for a flip: only record it. The next emulated
 * vsync copies the pixels and reports the flip done (command.c). */
NTSTATUS APIENTRY MwSetVidPnSourceAddress(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;

    if (Arg->VidPnSourceId != 0)
        return STATUS_INVALID_PARAMETER;
    dev->ScanAddr = Arg->PrimaryAddress;
    dev->ScanAlloc = (MW_ALLOCATION *)Arg->hAllocation;
    dev->Flips++;
    return STATUS_SUCCESS;
}
