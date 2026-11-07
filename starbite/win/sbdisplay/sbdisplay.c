/* SPDX-License-Identifier: GPL-2.0-only
 * sbdisplay.sys: a WDDM display-only driver (KMDOD) for the Galaxy S9's DECON and its S6E3HA8
 * command-mode panel, Windows 11 23H2 ARM64. docs/WIN11-GPU.md section 7. Stage 2b of track D.
 *
 * Based on malidod.c of wddm-mali-g52 (https://github.com/gahingwoo/wddm-mali-g52, GPL-2.0,
 * read at e8bcd36): the DDI set and the lessons of its five board runs on 23H2 (DRIVERCAPS filled
 * only as far as the buffer dxgkrnl passes, WDDMv1_2 + SupportNonVGA + SupportSmoothRotation,
 * target modes without fixed frequencies, the per-DDI trace). Ours adds, from our own proven code:
 *   - tear-free presents: a second scanout buffer and DECON flips latched by the panel's TE
 *     (s9p-decon's handshake, sbdc_core.c), the firmware framebuffer as the other buffer;
 *   - brightness through DXGK_BRIGHTNESS_INTERFACE, as S-Boot dims the panel (AOR only).
 * It never configures DECON, DSIM, the panel, clocks or power: S-Boot's pipeline is inherited.
 *
 * Modes: Flip (both buffers, flips) or Copy (Basic Display's way: dirty rectangles straight into
 * the live framebuffer, no DECON write). Copy is used when the pipeline does not look exactly as
 * expected, when the second buffer cannot be allocated below 4 GiB, when Parameters\Flip = 0, or
 * after a flip failed to latch within 50 ms.
 *
 * Progress: HKLM\SYSTEM\CurrentControlSet\Services\sbdisplay\State (values, flushed at milestones).
 * Boot guard: Services\sbdisplay\GuardBoots counts starts; it is cleared only after 10 minutes with
 * presents arriving and no flip failure. A screen that is wrong (black, frozen, garbled) is a
 * person's call: force a restart within 10 minutes, twice, and the third boot runs Basic Display.
 * Kill switches, in the order to use them: the BOOT image without the Starbite SSDT (no device, no
 * driver), the file \Windows\Starbite\sbdisplay.off (from TWRP), Start = 4 (offline hive edit, only
 * on a clean hive: Windows replays a dirty hive's logs over offline edits).
 */
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "sbdc_core.h"

#define SD_TAG 'dDbS'
#define SD_GUARD_LIMIT 2
#define SD_GUARD_CLEAR_MS 600000        /* 10 min: time for a person to see a bad screen and force a restart */
#define SD_LATCH_TIMEOUT_US 50000
#define SD_DEFAULT_LEVEL 64               /* S-Boot's normal AOR 0x0440, assumed until Windows sets one */

/* GUID_DEVINTERFACE_BRIGHTNESS {FDE5BBA4-B3F9-46FB-BDAA-0728CE3100B4} (ntddvdeo.h). */
static const GUID SdBrightnessGuid = { 0xfde5bba4, 0xb3f9, 0x46fb, { 0xbd, 0xaa, 0x07, 0x28, 0xce, 0x31, 0x00, 0xb4 } };

typedef struct _SD_DEVICE {
    PDEVICE_OBJECT Pdo;
    DXGKRNL_INTERFACE Dxgk;
    DXGK_DISPLAY_INFORMATION Fb;          /* the firmware's framebuffer */
    PUCHAR Va[2];                         /* buffer 0 = firmware framebuffer, 1 = ours (write-combined) */
    SIZE_T FbSize;
    BOOLEAN SecondOwned;                  /* Va[1] is our contiguous allocation */
    BOOLEAN Flip;                         /* flip mode (else copy mode) */
    BOOLEAN Hw;                           /* DECON registers mapped and the pipeline identified */
    BOOLEAN SourceVisible, PathActive;
    BOOLEAN FullCopy;                     /* next copy-mode present redraws the whole frame */
    ULONG Scale;                          /* 1, or 2: Windows draws Lw x Lh, each pixel shown as 2 x 2 */
    ULONG Lw, Lh;                         /* the mode Windows sees (= Fb.Width/Height at scale 1) */
    volatile UCHAR *Mmio[SBDC_BLOCKS];
    SIZE_T MmioSize[SBDC_BLOCKS];
    struct sbdc_io Io;
    struct sbdc Dc;
    struct sbdc_swap Swap;
    FAST_MUTEX DsimLock;
    UCHAR Level;
    ULONG Presents, Flips, Fallbacks, BrightnessSets, BrightnessFails;
    ULONG64 Seen;
    HANDLE GuardThread;
    PKTHREAD GuardThreadObj;
    KEVENT GuardStop;
    KEVENT GuardStarted;
} SD_DEVICE, *PSD_DEVICE;

static UNICODE_STRING SdSvc;              /* the service key */
static WCHAR SdSvcBuf[256];

/* ---- registry ------------------------------------------------------------------------------- */

static NTSTATUS OpenSvc(PCWSTR Sub, HANDLE *H)
{
    WCHAR buf[300];
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    ULONG disp;
    RtlInitEmptyUnicodeString(&path, buf, sizeof(buf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path, &SdSvc)))
        return STATUS_NAME_TOO_LONG;
    if (Sub && (!NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\")) || !NT_SUCCESS(RtlAppendUnicodeToString(&path, Sub))))
        return STATUS_NAME_TOO_LONG;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(H, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp);
}

static ULONG GetDword(HANDLE H, PCWSTR Name, ULONG Dflt)
{
    UNICODE_STRING n;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len;
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(ZwQueryValueKey(H, &n, KeyValuePartialInformation, info, sizeof(buf), &len)) &&
        info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
        return *(ULONG *)info->Data;
    return Dflt;
}

static void PutDword(HANDLE H, PCWSTR Name, ULONG V)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(H, &n, 0, REG_DWORD, &V, sizeof(V));
}

/* State\<Name> = Value (PASSIVE_LEVEL only; silently skipped above it). */
static void Log(PCWSTR Name, ULONG Value, BOOLEAN Flush)
{
    HANDLE h;
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "sbdisplay: %ws = 0x%08x\n", Name, Value);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !NT_SUCCESS(OpenSvc(L"State", &h)))
        return;
    PutDword(h, Name, Value);
    if (Flush)
        (void)ZwFlushKey(h);
    ZwClose(h);
}

static ULONG Param(PCWSTR Name, ULONG Dflt)
{
    HANDLE h;
    ULONG v = Dflt;
    if (NT_SUCCESS(OpenSvc(L"Parameters", &h))) {
        v = GetDword(h, Name, Dflt);
        ZwClose(h);
    }
    return v;
}

/* The first call of each DDI (Seen_<name> = status) and every failure (Fail_<name>), as malidod. */
static NTSTATUS Ret(PSD_DEVICE Dev, PCWSTR Name, NTSTATUS St)
{
    if (Dev != NULL) {
        ULONG hash = 0;
        for (PCWSTR c = Name; *c; c++)
            hash = hash * 31 + *c;
        if (!(Dev->Seen & (1ULL << (hash % 64)))) {
            WCHAR seen[64];
            UNICODE_STRING us;
            Dev->Seen |= 1ULL << (hash % 64);
            RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
            if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Seen_%ws", Name + 5)))
                Log(seen, (ULONG)St, FALSE);
        }
        if (!NT_SUCCESS(St)) {
            Log(Name, (ULONG)St, TRUE);
            Log(L"LastFailedDdi", (ULONG)St, TRUE);
        }
    }
    return St;
}

/* ---- register access for sbdc_core ---------------------------------------------------------- */

static sbdc_u32 IoRd(void *Ctx, unsigned B, unsigned Off)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    if (B >= SBDC_BLOCKS || !dev->Mmio[B] || Off + 4 > dev->MmioSize[B])
        return 0;
    return READ_REGISTER_ULONG((volatile ULONG *)(dev->Mmio[B] + Off));
}

/* DECON is not coherent and reads write-combined (normal non-cacheable) memory: a full DSB, not a
 * DMB, between the framebuffer stores and the register write that starts the scanout of them. */
#define SD_DSB() __dsb(_ARM64_BARRIER_SY)

static void IoWr(void *Ctx, unsigned B, unsigned Off, sbdc_u32 V)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    if (B >= SBDC_BLOCKS || !dev->Mmio[B] || Off + 4 > dev->MmioSize[B])
        return;
    SD_DSB();
    WRITE_REGISTER_ULONG((volatile ULONG *)(dev->Mmio[B] + Off), V);
    SD_DSB();
}

static void IoDelay(void *Ctx, unsigned Us)
{
    UNREFERENCED_PARAMETER(Ctx);
    KeStallExecutionProcessor(Us);
}

/* The ACPI device's memory resources by base address (docs/WIN11-GPU.md 4.2). */
static void MapResources(PSD_DEVICE Dev)
{
    static const ULONG64 base[SBDC_BLOCKS] = { SBDC_DECON_PA, SBDC_DMA_PA, SBDC_DSIM_PA };
    static const ULONG size[SBDC_BLOCKS] = { SBDC_DECON_SIZE, SBDC_DMA_SIZE, SBDC_DSIM_SIZE };
    DXGK_DEVICE_INFO info;
    PCM_RESOURCE_LIST list;

    if (!NT_SUCCESS(Dev->Dxgk.DxgkCbGetDeviceInformation(Dev->Dxgk.DeviceHandle, &info)))
        return;
    list = info.TranslatedResourceList;
    if (list == NULL || list->Count < 1)
        return;
    PCM_PARTIAL_RESOURCE_LIST pl = &list->List[0].PartialResourceList;
    for (ULONG i = 0; i < pl->Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &pl->PartialDescriptors[i];
        if (d->Type != CmResourceTypeMemory)
            continue;
        for (unsigned b = 0; b < SBDC_BLOCKS; b++)
            if ((ULONG64)d->u.Memory.Start.QuadPart == base[b] && d->u.Memory.Length >= size[b] && !Dev->Mmio[b]) {
                Dev->Mmio[b] = (volatile UCHAR *)MmMapIoSpaceEx(d->u.Memory.Start, size[b], PAGE_READWRITE | PAGE_NOCACHE);
                Dev->MmioSize[b] = Dev->Mmio[b] ? size[b] : 0;
            }
    }
    Log(L"MmioMapped", (Dev->Mmio[0] ? 1u : 0u) | (Dev->Mmio[1] ? 2u : 0u) | (Dev->Mmio[2] ? 4u : 0u), FALSE);
}

static void UnmapResources(PSD_DEVICE Dev)
{
    for (unsigned b = 0; b < SBDC_BLOCKS; b++)
        if (Dev->Mmio[b]) {
            MmUnmapIoSpace((PVOID)Dev->Mmio[b], Dev->MmioSize[b]);
            Dev->Mmio[b] = NULL;
        }
}

/* ---- the one mode: the firmware's ------------------------------------------------------------ */

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

static void FillSourceMode(PSD_DEVICE Dev, D3DKMDT_VIDPN_SOURCE_MODE *M)
{
    M->Type = D3DKMDT_RMT_GRAPHICS;
    M->Format.Graphics.PrimSurfSize.cx = Dev->Lw;
    M->Format.Graphics.PrimSurfSize.cy = Dev->Lh;
    M->Format.Graphics.VisibleRegionSize = M->Format.Graphics.PrimSurfSize;
    M->Format.Graphics.Stride = Dev->Scale == 1 ? Dev->Fb.Pitch : Dev->Lw * 4;
    M->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    M->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    M->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

static void FillSignal(PSD_DEVICE Dev, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = Dev->Lw;
    S->TotalSize.cy = Dev->Lh;
    S->ActiveSize = S->TotalSize;
    S->VSyncFreq.Numerator = 60;
    S->VSyncFreq.Denominator = 1;
    S->HSyncFreq.Numerator = 60 * Dev->Lh;
    S->HSyncFreq.Denominator = 1;
    S->PixelRate = (SIZE_T)Dev->Lw * Dev->Lh * 60;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* ---- scanout ownership ----------------------------------------------------------------------- */

/* Copy rectangles of the source image into buffer i. */
/* Copy rectangles of the source image (source pixels) into buffer i, scaled by Dev->Scale (flip mode and the
 * buffer-to-buffer restore always run at scale 1). */
static void CopyRects(PSD_DEVICE Dev, int I, const UCHAR *Src, LONG SrcPitch, const struct sbdc_rects *R)
{
    for (unsigned k = 0; k < R->n; k++)
        sbdc_blit(Dev->Va[I], Dev->Fb.Pitch, Src, (unsigned)SrcPitch, R->r[k], Dev->Scale);
}

/* Leave flip mode: DECON back on the firmware framebuffer with the current image in it. Returns
 * TRUE when DECON is known to scan buffer 0 (then buffer 1 may be freed). */
static BOOLEAN BackToFirmwareBuffer(PSD_DEVICE Dev)
{
    if (!Dev->Flip)
        return TRUE;
    if (sbdc_wait_latched(&Dev->Dc, SD_LATCH_TIMEOUT_US, 50)) {
        Log(L"RestoreLatchTimeout", 1, TRUE);
        return FALSE;
    }
    if (Dev->Swap.front == 1) {
        /* buffer 0 lacks what changed since it was last written: take it from buffer 1 */
        CopyRects(Dev, 0, Dev->Va[1], (LONG)Dev->Fb.Pitch, &Dev->Swap.stale[0]);
        SD_DSB();
        if (sbdc_flip(&Dev->Dc, Dev->Swap.pa[0]) || sbdc_wait_latched(&Dev->Dc, SD_LATCH_TIMEOUT_US, 50)) {
            Log(L"RestoreFlipFailed", 1, TRUE);
            return FALSE;
        }
        sbdc_swap_flipped(&Dev->Swap);
    }
    Dev->Flip = FALSE;
    return TRUE;
}

static void ReleaseScanout(PSD_DEVICE Dev)
{
    BOOLEAN safe = BackToFirmwareBuffer(Dev);
    if (Dev->Va[1] && Dev->SecondOwned) {
        if (safe)
            MmFreeContiguousMemory(Dev->Va[1]);
        else
            Log(L"SecondBufferLeaked", 1, TRUE);  /* DECON may still read it: never free it */
        Dev->Va[1] = NULL;
        Dev->SecondOwned = FALSE;
    }
    if (Dev->Va[0]) {
        MmUnmapIoSpace(Dev->Va[0], Dev->FbSize);
        Dev->Va[0] = NULL;
    }
    UnmapResources(Dev);
    Dev->Hw = FALSE;
}

/* ---- boot guard ------------------------------------------------------------------------------ */

static KSTART_ROUTINE GuardThread;

static VOID GuardThread(PVOID Ctx)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    LARGE_INTEGER t;
    HANDLE h;
    ObReferenceObject(KeGetCurrentThread());   /* so StopGuard can always wait on it */
    dev->GuardThreadObj = KeGetCurrentThread();
    KeSetEvent(&dev->GuardStarted, IO_NO_INCREMENT, FALSE);
    t.QuadPart = -10000LL * SD_GUARD_CLEAR_MS;
    if (KeWaitForSingleObject(&dev->GuardStop, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT) {
        /* Healthy = Windows has been presenting through us for 10 minutes without a flip failure. */
        Log(L"PresentsAtGuard", dev->Presents, TRUE);
        if (dev->Presents > 0 && dev->Fallbacks == 0 && NT_SUCCESS(OpenSvc(NULL, &h))) {
            PutDword(h, L"GuardBoots", 0);
            (void)ZwFlushKey(h);
            ZwClose(h);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void StopGuard(PSD_DEVICE Dev)
{
    if (Dev->GuardThreadObj) {
        KeSetEvent(&Dev->GuardStop, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(Dev->GuardThreadObj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Dev->GuardThreadObj);
        Dev->GuardThreadObj = NULL;
    }
    if (Dev->GuardThread) {
        ZwClose(Dev->GuardThread);
        Dev->GuardThread = NULL;
    }
}

/* ---- PnP ------------------------------------------------------------------------------------- */

NTSTATUS SdAddDevice(IN_CONST_PDEVICE_OBJECT PhysicalDeviceObject, OUT_PPVOID MiniportDeviceContext)
{
    PSD_DEVICE dev = (PSD_DEVICE)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SD_DEVICE), SD_TAG);
    if (dev == NULL)
        return STATUS_NO_MEMORY;
    dev->Pdo = (PDEVICE_OBJECT)PhysicalDeviceObject;
    dev->Io.ctx = dev;
    dev->Io.rd = IoRd;
    dev->Io.wr = IoWr;
    dev->Io.delay_us = IoDelay;
    dev->Level = SD_DEFAULT_LEVEL;
    ExInitializeFastMutex(&dev->DsimLock);
    KeInitializeEvent(&dev->GuardStop, NotificationEvent, FALSE);
    KeInitializeEvent(&dev->GuardStarted, NotificationEvent, FALSE);
    *MiniportDeviceContext = dev;
    return STATUS_SUCCESS;
}

NTSTATUS SdStartDevice(IN_CONST_PVOID MiniportDeviceContext, IN_PDXGK_START_INFO DxgkStartInfo,
                       IN_PDXGKRNL_INTERFACE DxgkInterface, OUT_PULONG NumberOfVideoPresentSources,
                       OUT_PULONG NumberOfChildren)
{
    PSD_DEVICE dev = (PSD_DEVICE)MiniportDeviceContext;
    OBJECT_ATTRIBUTES oa;
    PHYSICAL_ADDRESS lo, hi, skip, pa;
    NTSTATUS st;
    int id = -100;
    UNREFERENCED_PARAMETER(DxgkStartInfo);

    dev->Dxgk = *DxgkInterface;
    st = dev->Dxgk.DxgkCbAcquirePostDisplayOwnership(dev->Dxgk.DeviceHandle, &dev->Fb);
    Log(L"AcquirePostDisplay", (ULONG)st, FALSE);
    if (!NT_SUCCESS(st) || dev->Fb.Width == 0 || BytesPerPixel(dev->Fb.ColorFormat) == 0)
        return STATUS_UNSUCCESSFUL;
    Log(L"FbWidth", dev->Fb.Width, FALSE);
    Log(L"FbHeight", dev->Fb.Height, FALSE);
    Log(L"FbPitch", dev->Fb.Pitch, FALSE);
    Log(L"FbFormat", (ULONG)dev->Fb.ColorFormat, FALSE);
    Log(L"FbPhysLo", (ULONG)dev->Fb.PhysicAddress.QuadPart, FALSE);
    Log(L"FbPhysHi", (ULONG)(dev->Fb.PhysicAddress.QuadPart >> 32), FALSE);

    dev->FbSize = (SIZE_T)dev->Fb.Pitch * dev->Fb.Height;
    dev->Va[0] = (PUCHAR)MmMapIoSpaceEx(dev->Fb.PhysicAddress, dev->FbSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (dev->Va[0] == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Scale 2 (Parameters\Scale, default 2): Windows gets a half-size mode and draws a quarter of the pixels;
     * the present doubles them. Only for an even-sized, packed framebuffer; it implies copy mode. */
    dev->Scale = Param(L"Scale", 2) == 2 && !(dev->Fb.Width & 1) && !(dev->Fb.Height & 1) &&
                 dev->Fb.Pitch == dev->Fb.Width * 4 ? 2 : 1;
    dev->Lw = dev->Fb.Width / dev->Scale;
    dev->Lh = dev->Fb.Height / dev->Scale;
    Log(L"Scale", dev->Scale, FALSE);
    Log(L"ModeWidth", dev->Lw, FALSE);
    Log(L"ModeHeight", dev->Lh, FALSE);

    /* Flip mode only when everything is exactly as S-Boot left it. */
    MapResources(dev);
    if (dev->Mmio[SBDC_DECON] && dev->Mmio[SBDC_DMA] && dev->Fb.PhysicAddress.QuadPart < 0x100000000LL &&
        dev->Fb.Pitch == dev->Fb.Width * 4)
        id = sbdc_identify(&dev->Dc, &dev->Io, (sbdc_u32)dev->Fb.PhysicAddress.QuadPart, dev->Fb.Width, dev->Fb.Height);
    Log(L"Identify", (ULONG)id, FALSE);
    dev->Hw = id == 0;
    if (dev->Hw) {
        Log(L"Window", dev->Dc.win, FALSE);
        Log(L"TrigInherited", dev->Dc.trig_inherited, FALSE);
    }
    if (dev->Hw && dev->Scale == 1 && Param(L"Flip", 0)) {
        lo.QuadPart = 0;
        hi.QuadPart = 0xffffffffLL;       /* DECON's IDMA base registers are 32 bits */
        skip.QuadPart = 0;
        dev->Va[1] = (PUCHAR)MmAllocateContiguousNodeMemory(dev->FbSize, lo, hi, skip,
                                                            PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
        if (dev->Va[1]) {
            dev->SecondOwned = TRUE;
            pa = MmGetPhysicalAddress(dev->Va[1]);
            Log(L"SecondPhys", (ULONG)pa.QuadPart, FALSE);
            if (pa.QuadPart > 0 && pa.QuadPart + (LONGLONG)dev->FbSize <= 0x100000000LL && !(pa.QuadPart & 0xfff)) {
                sbdc_swap_init(&dev->Swap, (sbdc_u32)dev->Fb.PhysicAddress.QuadPart, (sbdc_u32)pa.QuadPart,
                               dev->Fb.Width, dev->Fb.Height);
                dev->Flip = TRUE;
            } else {
                MmFreeContiguousMemory(dev->Va[1]);
                dev->Va[1] = NULL;
                dev->SecondOwned = FALSE;
            }
        }
    }
    Log(L"Mode", dev->Flip ? 2u : 1u, TRUE);   /* 2 = flip, 1 = copy */

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(PsCreateSystemThread(&dev->GuardThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, GuardThread, dev)))
        (void)KeWaitForSingleObject(&dev->GuardStarted, Executive, KernelMode, FALSE, NULL);

    dev->SourceVisible = TRUE;
    dev->PathActive = TRUE;
    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = 1;
    Log(L"Started", 1, TRUE);
    return STATUS_SUCCESS;
}

NTSTATUS SdStopDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PSD_DEVICE dev = (PSD_DEVICE)MiniportDeviceContext;
    StopGuard(dev);
    ReleaseScanout(dev);
    return STATUS_SUCCESS;
}

NTSTATUS SdRemoveDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PSD_DEVICE dev = (PSD_DEVICE)MiniportDeviceContext;
    StopGuard(dev);
    ReleaseScanout(dev);
    ExFreePoolWithTag(dev, SD_TAG);
    return STATUS_SUCCESS;
}

/* Hand the firmware framebuffer back (e.g. to Basic Display), with DECON scanning it again. */
NTSTATUS SdStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID MiniportDeviceContext,
                                                   IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    PSD_DEVICE dev = (PSD_DEVICE)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    StopGuard(dev);
    *DisplayInfo = dev->Fb;
    ReleaseScanout(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS SdDispatchIoRequestImpl(IN_CONST_PVOID Ctx, IN_ULONG Src, IN_PVIDEO_REQUEST_PACKET Vrp)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Src);
    UNREFERENCED_PARAMETER(Vrp);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS SdSetPowerState(IN_CONST_PVOID Ctx, IN_ULONG DeviceUid, IN_DEVICE_POWER_STATE State, IN_POWER_ACTION Action)
{
    /* First version: no panel power control (docs/WIN11-GPU.md 7.5); blanking is a black frame. */
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(DeviceUid);
    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(Action);
    return STATUS_SUCCESS;
}

VOID SdResetDevice(IN_CONST_PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

VOID SdUnload(VOID)
{
}

/* ---- brightness ------------------------------------------------------------------------------ */

static VOID SdIfRef(PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

static NTSTATUS SdGetPossibleBrightness(PVOID Ctx, ULONG BufferSize, PUCHAR LevelCount, PUCHAR Levels)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (LevelCount == NULL)
        return STATUS_INVALID_PARAMETER;
    *LevelCount = 101;
    if (Levels == NULL || BufferSize < 101)
        return STATUS_BUFFER_TOO_SMALL;
    for (UCHAR i = 0; i <= 100; i++)
        Levels[i] = i;
    return STATUS_SUCCESS;
}

static NTSTATUS SdSetBrightness(PVOID Ctx, UCHAR Level)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    int r;
    if (Level > 100)
        Level = 100;
    if (!dev->Hw || !dev->Mmio[SBDC_DSIM] || !Param(L"Brightness", 0))
        return STATUS_SUCCESS;            /* nothing to drive: keep Windows happy, change nothing */
    ExAcquireFastMutex(&dev->DsimLock);
    r = sbdc_set_aor(&dev->Dc, sbdc_level_to_aor(Level));
    ExReleaseFastMutex(&dev->DsimLock);
    if (r == 0) {
        dev->Level = Level;
        dev->BrightnessSets++;
    } else {
        dev->BrightnessFails++;
    }
    Log(L"BrightnessLevel", Level, FALSE);
    Log(L"BrightnessResult", (ULONG)r, dev->BrightnessSets == 1 || r != 0);
    return r == 0 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS SdGetBrightness(PVOID Ctx, PUCHAR Level)
{
    *Level = ((PSD_DEVICE)Ctx)->Level;
    return STATUS_SUCCESS;
}

static NTSTATUS SdQueryInterfaceImpl(IN_CONST_PVOID Ctx, IN_PQUERY_INTERFACE Qi)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    if (Qi->InterfaceType != NULL && RtlEqualMemory(Qi->InterfaceType, &SdBrightnessGuid, sizeof(GUID))) {
        DXGK_BRIGHTNESS_INTERFACE *b = (DXGK_BRIGHTNESS_INTERFACE *)Qi->Interface;
        if (Qi->Version != DXGK_BRIGHTNESS_INTERFACE_VERSION_1 || Qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE))
            return STATUS_NOT_SUPPORTED;
        b->Size = sizeof(DXGK_BRIGHTNESS_INTERFACE);
        b->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
        b->Context = dev;
        b->InterfaceReference = SdIfRef;
        b->InterfaceDereference = SdIfRef;
        b->GetPossibleBrightness = SdGetPossibleBrightness;
        b->SetBrightness = SdSetBrightness;
        b->GetBrightness = SdGetBrightness;
        Log(L"BrightnessInterface", 1, FALSE);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

/* ---- the panel: internal, always connected --------------------------------------------------- */

static NTSTATUS SdQueryChildRelationsImpl(IN_CONST_PVOID Ctx, PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (Size < 2 * sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;   /* one child plus the zeroed terminator */
    RtlZeroMemory(Rel, sizeof(DXGK_CHILD_DESCRIPTOR));
    Rel[0].ChildDeviceType = TypeVideoOutput;
    Rel[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    Rel[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    Rel[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    Rel[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    Rel[0].AcpiUid = 0;
    Rel[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS SdQueryChildStatusImpl(IN_CONST_PVOID Ctx, INOUT_PDXGK_CHILD_STATUS Cs, IN_BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    switch (Cs->Type) {
    case StatusConnection:
        Cs->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        Cs->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS SdQueryDeviceDescriptorImpl(IN_CONST_PVOID Ctx, IN_ULONG ChildUid, INOUT_PDXGK_DEVICE_DESCRIPTOR Desc)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(Desc);
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

/* ---- adapter caps (malidod's lessons) -------------------------------------------------------- */

#define FITS(Size, Type, Field) (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

static NTSTATUS QueryAdapterInfo(const DXGKARG_QUERYADAPTERINFO *Info)
{
    switch (Info->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)Info->pOutputData;
        UINT size = Info->OutputDataSize;
        if (!FITS(size, DXGK_DRIVERCAPS, HighestAcceptableAddress))
            return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, size);
        caps->WDDMVersion = DXGKDDI_WDDMv1_2;
        caps->HighestAcceptableAddress.QuadPart = -1;
        if (FITS(size, DXGK_DRIVERCAPS, SupportNonVGA))
            caps->SupportNonVGA = TRUE;
        if (FITS(size, DXGK_DRIVERCAPS, SupportSmoothRotation))
            caps->SupportSmoothRotation = TRUE;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS APIENTRY SdQueryAdapterInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Info)
{
    WCHAR name[32];
    UNICODE_STRING us;
    NTSTATUS st = QueryAdapterInfo(Info);
    UNREFERENCED_PARAMETER(hAdapter);
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u", (ULONG)Info->Type)))
        Log(name, (ULONG)st, FALSE);
    return st;
}

static NTSTATUS SdSetPointerPositionImpl(IN_CONST_HANDLE h, const DXGKARG_SETPOINTERPOSITION *A)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS SdSetPointerShapeImpl(IN_CONST_HANDLE h, const DXGKARG_SETPOINTERSHAPE *A)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(A);
    return STATUS_NOT_SUPPORTED;
}

/* ---- VidPN: one source, one target, one mode, identity transforms ---------------------------- */

static NTSTATUS SdIsSupportedVidPnImpl(IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    PSD_DEVICE dev = (PSD_DEVICE)hAdapter;
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
    if (NT_SUCCESS(st)) {
        Arg->IsVidPnSupported = pinned == NULL ||
            (pinned->Type == D3DKMDT_RMT_GRAPHICS && pinned->Format.Graphics.PrimSurfSize.cx == dev->Lw &&
             pinned->Format.Graphics.PrimSurfSize.cy == dev->Lh);
        if (pinned != NULL)
            setIf->pfnReleaseModeInfo(set, pinned);
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hDesiredVidPn, set);
    return STATUS_SUCCESS;
}

static NTSTATUS SdRecommendFunctionalVidPnImpl(IN_CONST_HANDLE h, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *A)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(A);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static NTSTATUS AddSourceMode(PSD_DEVICE Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
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

static NTSTATUS AddTargetMode(PSD_DEVICE Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
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
        FillSignal(Dev, &mode->VideoSignalInfo);
        /* Frequencies unspecified, as Basic Display (malidod run 4: INVALID_FREQUENCY otherwise). */
        mode->VideoSignalInfo.VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
        mode->VideoSignalInfo.VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
        mode->VideoSignalInfo.HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
        mode->VideoSignalInfo.HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
        mode->VideoSignalInfo.PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
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

static NTSTATUS SdEnumVidPnCofuncModalityImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    PSD_DEVICE dev = (PSD_DEVICE)hAdapter;
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
        D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pt = Arg->EnumPivotType;
        BOOLEAN srcPivot = pt == D3DKMDT_EPT_VIDPNSOURCE && Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgtPivot = pt == D3DKMDT_EPT_VIDPNTARGET && Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        BOOLEAN xfPivot = (pt == D3DKMDT_EPT_SCALING || pt == D3DKMDT_EPT_ROTATION) &&
                          Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
                          Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        NTSTATUS r = STATUS_SUCCESS;

        if (!srcPivot)
            r = AddSourceMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId);
        if (NT_SUCCESS(r) && !tgtPivot)
            r = AddTargetMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnTargetId);
        if (NT_SUCCESS(r) && !xfPivot &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            if (upd.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.ScalingSupport, sizeof(upd.ContentTransformation.ScalingSupport));
                upd.ContentTransformation.ScalingSupport.Identity = 1;
            }
            if (upd.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.RotationSupport, sizeof(upd.ContentTransformation.RotationSupport));
                upd.ContentTransformation.RotationSupport.Identity = 1;
            }
            r = topoIf->pfnUpdatePathSupportInfo(topo, &upd);
        }
        if (!NT_SUCCESS(r)) {
            Log(L"Fail_CofuncPath", (ULONG)r, TRUE);
            topoIf->pfnReleasePathInfo(topo, path);
            return r;
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ? STATUS_SUCCESS : st;
}

/* Invisible = blank: black in every buffer (each buffer then needs a full redraw). */
static NTSTATUS SdSetVidPnSourceVisibilityImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *Arg)
{
    PSD_DEVICE dev = (PSD_DEVICE)hAdapter;
    dev->SourceVisible = Arg->Visible;
    if (!Arg->Visible) {
        for (int i = 0; i < 2; i++)
            if (dev->Va[i])
                RtlZeroMemory(dev->Va[i], dev->FbSize);
        if (dev->Flip) {
            sbdc_rects_full(&dev->Swap.stale[0], dev->Fb.Width, dev->Fb.Height);
            sbdc_rects_full(&dev->Swap.stale[1], dev->Fb.Width, dev->Fb.Height);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SdCommitVidPnImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *Arg)
{
    PSD_DEVICE dev = (PSD_DEVICE)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnAcquireSourceModeSet(Arg->hFunctionalVidPn, 0, &set, &setIf);
    if (!NT_SUCCESS(st)) {
        dev->PathActive = FALSE;          /* no path for our source: it is being turned off */
        Log(L"CommitNoPath", (ULONG)st, FALSE);
        return STATUS_SUCCESS;
    }
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        BOOLEAN ours = pinned->Type == D3DKMDT_RMT_GRAPHICS && pinned->Format.Graphics.PrimSurfSize.cx == dev->Lw &&
                       pinned->Format.Graphics.PrimSurfSize.cy == dev->Lh;
        setIf->pfnReleaseModeInfo(set, pinned);
        st = ours ? STATUS_SUCCESS : STATUS_GRAPHICS_INVALID_VIDPN;
        dev->PathActive = ours;
    } else if (NT_SUCCESS(st)) {
        dev->PathActive = FALSE;
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hFunctionalVidPn, set);
    Log(L"CommitStatus", (ULONG)st, TRUE);
    return st;
}

static NTSTATUS SdUpdateActiveVidPnPresentPathImpl(IN_CONST_HANDLE h, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *A)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS SdRecommendMonitorModesImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    PSD_DEVICE dev = (PSD_DEVICE)hAdapter;
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
    if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        Arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Arg->hMonitorSourceModeSet, mode);
    return STATUS_SUCCESS;
}

static NTSTATUS SdQueryVidPnHWCapabilityImpl(IN_CONST_HANDLE h, DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    UNREFERENCED_PARAMETER(h);
    RtlZeroMemory(&Arg->VidPnHWCaps, sizeof(Arg->VidPnHWCaps));
    return STATUS_SUCCESS;
}

/* ---- present ---------------------------------------------------------------------------------- */

/* Flips stop working: from now on every present goes into both buffers, so whichever DECON scans
 * shows it (a pending flip may still latch later). */
static void FallBackToCopy(PSD_DEVICE Dev, PCWSTR Why)
{
    Dev->Flip = FALSE;
    Dev->FullCopy = TRUE;                 /* buffer 1 or 0 may hold an older frame anywhere */
    Dev->Fallbacks++;
    Log(Why, Dev->Dc.latch_timeouts, TRUE);
    Log(L"Mode", 3, TRUE);                /* 3 = copy into both buffers after a failed flip */
}

static NTSTATUS SdPresentDisplayOnlyImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_PRESENT_DISPLAYONLY *Arg)
{
    PSD_DEVICE dev = (PSD_DEVICE)hAdapter;
    struct sbdc_rect rect[SBDC_MAX_RECTS];
    struct sbdc_rects copy;
    unsigned n = 0;
    PMDL mdl;
    const UCHAR *src;
    SIZE_T size;

    if (!dev->SourceVisible || !dev->PathActive || dev->Va[0] == NULL)
        return STATUS_SUCCESS;
    if (Arg->BytesPerPixel != 4 || Arg->Flags.Rotate)
        return STATUS_NOT_SUPPORTED;

    size = (SIZE_T)Arg->Pitch * dev->Lh;
    mdl = IoAllocateMdl(Arg->pSource, (ULONG)size, FALSE, FALSE, NULL);
    if (mdl == NULL)
        return STATUS_NO_MEMORY;
    /* DWM presents from user mode, the console (CDD) from kernel session space (malidod PE run 6). */
    __try {
        MmProbeAndLockPages(mdl, (ULONG_PTR)Arg->pSource >= (ULONG_PTR)MM_USER_PROBE_ADDRESS ? KernelMode : UserMode,
                            IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    src = (const UCHAR *)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (src == NULL) {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
        return STATUS_NO_MEMORY;
    }

    /* This present's rectangles: moved regions are already in place in the source, so they are
     * copied like dirty ones. Beyond the table: the whole frame. */
    for (ULONG i = 0; i < Arg->NumMoves && n < SBDC_MAX_RECTS; i++, n++) {
        rect[n].l = Arg->pMoves[i].DestRect.left;
        rect[n].t = Arg->pMoves[i].DestRect.top;
        rect[n].r = Arg->pMoves[i].DestRect.right;
        rect[n].b = Arg->pMoves[i].DestRect.bottom;
    }
    for (ULONG i = 0; i < Arg->NumDirtyRects && n < SBDC_MAX_RECTS; i++, n++) {
        rect[n].l = Arg->pDirtyRect[i].left;
        rect[n].t = Arg->pDirtyRect[i].top;
        rect[n].r = Arg->pDirtyRect[i].right;
        rect[n].b = Arg->pDirtyRect[i].bottom;
    }
    if (n == SBDC_MAX_RECTS && Arg->NumMoves + Arg->NumDirtyRects > SBDC_MAX_RECTS) {
        rect[0].l = rect[0].t = 0;
        rect[0].r = (int)dev->Lw;
        rect[0].b = (int)dev->Lh;
        n = 1;
    }

    if (dev->Flip && sbdc_wait_latched(&dev->Dc, SD_LATCH_TIMEOUT_US, 50))
        FallBackToCopy(dev, L"FlipLatchTimeout");

    if (dev->Flip) {
        int back;
        sbdc_swap_damage(&dev->Swap, rect, n, dev->Fb.Width, dev->Fb.Height);
        back = sbdc_swap_back(&dev->Swap);
        /* The back buffer is not scanned: the last flip latched, DECON reads the front one. */
        CopyRects(dev, back, src, Arg->Pitch, &dev->Swap.stale[back]);
        SD_DSB();
        if (sbdc_flip(&dev->Dc, dev->Swap.pa[back]) == 0) {
            sbdc_swap_flipped(&dev->Swap);
            if (++dev->Flips == 1)
                Log(L"FirstFlip", dev->Swap.pa[back], TRUE);
        } else {
            FallBackToCopy(dev, L"FlipRefused");
        }
    } else {
        sbdc_rects_clear(&copy);
        if (dev->FullCopy) {
            sbdc_rects_full(&copy, dev->Lw, dev->Lh);
            dev->FullCopy = FALSE;
        }
        for (unsigned i = 0; i < n; i++)
            sbdc_rects_add(&copy, rect[i], dev->Lw, dev->Lh);
        CopyRects(dev, 0, src, Arg->Pitch, &copy);
        if (dev->Va[1])                   /* after a fallback: DECON may scan either buffer */
            CopyRects(dev, 1, src, Arg->Pitch, &copy);
    }

    MmUnlockPages(mdl);
    IoFreeMdl(mdl);
    if (++dev->Presents == 1 || (dev->Presents & 1023) == 0) {
        Log(L"Presents", dev->Presents, dev->Presents == 1);
        Log(L"Flips", dev->Flips, FALSE);
    }
    return STATUS_SUCCESS;
}

/* ---- the bugcheck screen: no waits, no flips; write every buffer DECON might scan ------------ */

NTSTATUS SdSystemDisplayEnable(IN_CONST_PVOID Ctx, IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                               PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, OUT UINT *Width, OUT UINT *Height,
                               OUT D3DDDIFORMAT *ColorFormat)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    if (dev->Va[0] == NULL)
        return STATUS_UNSUCCESSFUL;
    *Width = dev->Fb.Width;
    *Height = dev->Fb.Height;
    *ColorFormat = dev->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

VOID SdSystemDisplayWrite(IN_CONST_PVOID Ctx, IN_CONST_PVOID Source, UINT SourceWidth, UINT SourceHeight,
                          UINT SourceStride, UINT PositionX, UINT PositionY)
{
    PSD_DEVICE dev = (PSD_DEVICE)Ctx;
    for (int i = 0; i < 2; i++) {
        if (dev->Va[i] == NULL)
            continue;
        for (UINT y = 0; y < SourceHeight && PositionY + y < dev->Fb.Height; y++) {
            UINT w = min(SourceWidth, dev->Fb.Width > PositionX ? dev->Fb.Width - PositionX : 0);
            RtlCopyMemory(dev->Va[i] + (SIZE_T)(PositionY + y) * dev->Fb.Pitch + (SIZE_T)PositionX * 4,
                          (const UCHAR *)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
        }
    }
}

/* ---- the DDIs as registered ------------------------------------------------------------------- */

NTSTATUS SdDispatchIoRequest(IN_CONST_PVOID C, IN_ULONG S, IN_PVIDEO_REQUEST_PACKET V)
{ return Ret((PSD_DEVICE)C, L"Fail_DispatchIoRequest", SdDispatchIoRequestImpl(C, S, V)); }
NTSTATUS SdQueryChildRelations(IN_CONST_PVOID C, PDXGK_CHILD_DESCRIPTOR R, ULONG S)
{ return Ret((PSD_DEVICE)C, L"Fail_QueryChildRelations", SdQueryChildRelationsImpl(C, R, S)); }
NTSTATUS SdQueryChildStatus(IN_CONST_PVOID C, INOUT_PDXGK_CHILD_STATUS S, IN_BOOLEAN N)
{ return Ret((PSD_DEVICE)C, L"Fail_QueryChildStatus", SdQueryChildStatusImpl(C, S, N)); }
NTSTATUS SdQueryDeviceDescriptor(IN_CONST_PVOID C, IN_ULONG U, INOUT_PDXGK_DEVICE_DESCRIPTOR D)
{ return Ret((PSD_DEVICE)C, L"Fail_QueryDeviceDescriptor", SdQueryDeviceDescriptorImpl(C, U, D)); }
NTSTATUS SdQueryInterface(IN_CONST_PVOID C, IN_PQUERY_INTERFACE Q)
{ return Ret((PSD_DEVICE)C, L"Fail_QueryInterface", SdQueryInterfaceImpl(C, Q)); }
NTSTATUS APIENTRY SdSetPointerPosition(IN_CONST_HANDLE H, const DXGKARG_SETPOINTERPOSITION *A)
{ return Ret((PSD_DEVICE)H, L"Fail_SetPointerPosition", SdSetPointerPositionImpl(H, A)); }
NTSTATUS APIENTRY SdSetPointerShape(IN_CONST_HANDLE H, const DXGKARG_SETPOINTERSHAPE *A)
{ return Ret((PSD_DEVICE)H, L"Fail_SetPointerShape", SdSetPointerShapeImpl(H, A)); }
NTSTATUS APIENTRY SdIsSupportedVidPn(IN_CONST_HANDLE H, DXGKARG_ISSUPPORTEDVIDPN *A)
{ return Ret((PSD_DEVICE)H, L"Fail_IsSupportedVidPn", SdIsSupportedVidPnImpl(H, A)); }
NTSTATUS APIENTRY SdRecommendFunctionalVidPn(IN_CONST_HANDLE H, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *A)
{ return Ret((PSD_DEVICE)H, L"Fail_RecommendFunctionalVidPn", SdRecommendFunctionalVidPnImpl(H, A)); }
NTSTATUS APIENTRY SdEnumVidPnCofuncModality(IN_CONST_HANDLE H, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *A)
{ return Ret((PSD_DEVICE)H, L"Fail_EnumVidPnCofuncModality", SdEnumVidPnCofuncModalityImpl(H, A)); }
NTSTATUS APIENTRY SdSetVidPnSourceVisibility(IN_CONST_HANDLE H, const DXGKARG_SETVIDPNSOURCEVISIBILITY *A)
{ return Ret((PSD_DEVICE)H, L"Fail_SetVidPnSourceVisibility", SdSetVidPnSourceVisibilityImpl(H, A)); }
NTSTATUS APIENTRY SdCommitVidPn(IN_CONST_HANDLE H, const DXGKARG_COMMITVIDPN *A)
{ return Ret((PSD_DEVICE)H, L"Fail_CommitVidPn", SdCommitVidPnImpl(H, A)); }
NTSTATUS APIENTRY SdUpdateActiveVidPnPresentPath(IN_CONST_HANDLE H, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *A)
{ return Ret((PSD_DEVICE)H, L"Fail_UpdateActiveVidPnPresentPath", SdUpdateActiveVidPnPresentPathImpl(H, A)); }
NTSTATUS APIENTRY SdRecommendMonitorModes(IN_CONST_HANDLE H, const DXGKARG_RECOMMENDMONITORMODES *A)
{ return Ret((PSD_DEVICE)H, L"Fail_RecommendMonitorModes", SdRecommendMonitorModesImpl(H, A)); }
NTSTATUS APIENTRY SdQueryVidPnHWCapability(IN_CONST_HANDLE H, DXGKARG_QUERYVIDPNHWCAPABILITY *A)
{ return Ret((PSD_DEVICE)H, L"Fail_QueryVidPnHWCapability", SdQueryVidPnHWCapabilityImpl(H, A)); }
NTSTATUS APIENTRY SdPresentDisplayOnly(IN_CONST_HANDLE H, const DXGKARG_PRESENT_DISPLAYONLY *A)
{ return Ret((PSD_DEVICE)H, L"Fail_PresentDisplayOnly", SdPresentDisplayOnlyImpl(H, A)); }

DRIVER_INITIALIZE DriverEntry;

/* A kill switch that needs no registry: the file exists -> do not load. */
static BOOLEAN KillFile(PCWSTR Path)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    RtlInitUnicodeString(&us, Path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA init = { 0 };
    HANDLE h;
    ULONG guard;

    if (KillFile(L"\\SystemRoot\\Starbite\\sbdisplay.off"))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    RtlInitEmptyUnicodeString(&SdSvc, SdSvcBuf, sizeof(SdSvcBuf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&SdSvc, RegistryPath)))
        return STATUS_NAME_TOO_LONG;

    /* Boot guard: two starts in a row without 10 healthy minutes -> stay off, Basic Display stays. */
    if (NT_SUCCESS(OpenSvc(NULL, &h))) {
        guard = GetDword(h, L"GuardBoots", 0);
        if (guard >= SD_GUARD_LIMIT) {
            PutDword(h, L"GuardRefused", guard);
            (void)ZwFlushKey(h);
            ZwClose(h);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        PutDword(h, L"GuardBoots", guard + 1);
        (void)ZwFlushKey(h);
        ZwClose(h);
    }

    /* KMDOD arrived with Windows 8; newer WDK headers default to versions 23H2 may not take. */
    init.Version = DXGKDDI_INTERFACE_VERSION_WIN8;
    init.DxgkDdiAddDevice = SdAddDevice;
    init.DxgkDdiStartDevice = SdStartDevice;
    init.DxgkDdiStopDevice = SdStopDevice;
    init.DxgkDdiRemoveDevice = SdRemoveDevice;
    init.DxgkDdiDispatchIoRequest = SdDispatchIoRequest;
    init.DxgkDdiQueryChildRelations = SdQueryChildRelations;
    init.DxgkDdiQueryChildStatus = SdQueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = SdQueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = SdSetPowerState;
    init.DxgkDdiResetDevice = SdResetDevice;
    init.DxgkDdiUnload = SdUnload;
    init.DxgkDdiQueryInterface = SdQueryInterface;
    init.DxgkDdiQueryAdapterInfo = SdQueryAdapterInfo;
    init.DxgkDdiSetPointerPosition = SdSetPointerPosition;
    init.DxgkDdiSetPointerShape = SdSetPointerShape;
    init.DxgkDdiIsSupportedVidPn = SdIsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = SdRecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = SdEnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = SdSetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = SdCommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = SdUpdateActiveVidPnPresentPath;
    init.DxgkDdiRecommendMonitorModes = SdRecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = SdQueryVidPnHWCapability;
    init.DxgkDdiPresentDisplayOnly = SdPresentDisplayOnly;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = SdStopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = SdSystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = SdSystemDisplayWrite;
    /* No interrupts in this version: flips are latched by polling the shadow request bit, so
     * DxgkDdiControlInterrupt, DxgkDdiGetScanLine and the ISR/DPC stay NULL (as malidod). */
    return DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &init);
}
