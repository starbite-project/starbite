/* SPDX-License-Identifier: GPL-2.0-only
 * sbwddm on the Galaxy S9: see s9.h. The accessors, the thermal gate and the ACPM lock are sbmali.c's
 * (T5/T8 on the phone), moved here.
 */
#include "s9.h"

#define S9_DSB() __dsb(_ARM64_BARRIER_SY)

static sbm_u32 IoRd(void *Ctx, unsigned B, unsigned Off)
{
    S9_PLAT *p = (S9_PLAT *)Ctx;
    if (B >= SBM_BLOCKS || !p->Mmio[B] || Off + 4 > p->MmioSize[B])
        return 0;
    return READ_REGISTER_ULONG((volatile ULONG *)(p->Mmio[B] + Off));
}

static void IoWr(void *Ctx, unsigned B, unsigned Off, sbm_u32 V)
{
    S9_PLAT *p = (S9_PLAT *)Ctx;
    /* The PMU and CMU_G3D are mapped for reading only. */
    if (B == SBM_PMU || B == SBM_CMU || B >= SBM_BLOCKS || !p->Mmio[B] || Off + 4 > p->MmioSize[B])
        return;
    S9_DSB();
    WRITE_REGISTER_ULONG((volatile ULONG *)(p->Mmio[B] + Off), V);
    S9_DSB();
}

static void IoDelay(void *Ctx, unsigned Us)
{
    UNREFERENCED_PARAMETER(Ctx);
    KeStallExecutionProcessor(Us);
}

static void IoBarrier(void *Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    S9_DSB();
}

static volatile UCHAR *MapPa(ULONG64 Pa, SIZE_T Size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = (LONGLONG)Pa;
    return (volatile UCHAR *)MmMapIoSpaceEx(p, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

NTSTATUS S9Init(S9_PLAT *P, volatile ULONG *GpuRegs, SIZE_T GpuLen)
{
    static const struct { unsigned b; ULONG64 pa; SIZE_T size; } blk[] = {
        { SBM_PMU, SBM_PMU_PA, SBM_PMU_SIZE },
        { SBM_ACPM_SRAM, SBM_ACPM_SRAM_PA, SBM_ACPM_SRAM_SIZE },
        { SBM_ACPM_MBOX, SBM_ACPM_MBOX_PA, SBM_ACPM_MBOX_SIZE },
        { SBM_CMU, SBM_CMU_PA, SBM_CMU_SIZE },
    };
    RtlZeroMemory(P, sizeof(*P));
    P->Mmio[SBM_GPU] = (volatile UCHAR *)GpuRegs;
    P->MmioSize[SBM_GPU] = GpuLen;
    for (unsigned i = 0; i < sizeof(blk) / sizeof(blk[0]); i++) {
        P->Mmio[blk[i].b] = MapPa(blk[i].pa, blk[i].size);
        P->MmioSize[blk[i].b] = blk[i].size;
        P->Owned[blk[i].b] = TRUE;
        if (!P->Mmio[blk[i].b]) {
            S9Fini(P);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    P->Io.ctx = P;
    P->Io.rd = IoRd;
    P->Io.wr = IoWr;
    P->Io.delay_us = IoDelay;
    P->Io.barrier = IoBarrier;
    return STATUS_SUCCESS;
}

void S9Fini(S9_PLAT *P)
{
    for (unsigned b = 0; b < SBM_BLOCKS; b++) {
        if (P->Owned[b] && P->Mmio[b])
            MmUnmapIoSpace((PVOID)P->Mmio[b], P->MmioSize[b]);
        P->Mmio[b] = NULL;
        P->Owned[b] = FALSE;
    }
    if (P->Cb) {
        ObDereferenceObject(P->Cb);
        P->Cb = NULL;
    }
}

BOOLEAN S9DomainOn(S9_PLAT *P)
{
    return P->Mmio[SBM_PMU] != NULL && sbm_domain_on(&P->Io);
}

ULONG S9Gate(S9_PLAT *P, ULONG *Level)
{
    struct sbth_state s;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;

    *Level = SBTH_CRITICAL;
    if (P->Cb == NULL) {
        RtlInitUnicodeString(&name, SBTH_CALLBACK_NAME);
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!NT_SUCCESS(ExCreateCallback(&P->Cb, &oa, FALSE, TRUE)))   /* open only: never create it */
            return 1;
    }
    RtlZeroMemory(&s, sizeof(s));
    ExNotifyCallback(P->Cb, SBTH_CB_QUERY, &s);
    if (s.version != SBTH_STATE_VERSION)
        return 2;
    if (KeQueryInterruptTime() - s.time_100ns > 10000000ULL)
        return 3;                         /* the watchdog stamps every reading: stale = not running */
    P->Last = s;
    *Level = s.level;
    return 0;
}

/* sbthermal owns ACPM in Windows: its lock around every ACPM sequence (a watchdog without the CPU query
 * predates the lock and never touches ACPM itself). TRUE = go ahead. */
static BOOLEAN AcpmLock(S9_PLAT *P, BOOLEAN *Locked)
{
    struct sbth_cpu cpu;
    LONG ok = 0;
    *Locked = FALSE;
    if (!P->Cb)
        return FALSE;
    RtlZeroMemory(&cpu, sizeof(cpu));
    ExNotifyCallback(P->Cb, SBTH_CB_QUERY_CPU, &cpu);
    if (cpu.version == 0)
        return TRUE;
    ExNotifyCallback(P->Cb, SBTH_CB_ACPM_LOCK, &ok);
    *Locked = ok != 0;
    return *Locked;
}

static void AcpmUnlock(S9_PLAT *P, BOOLEAN Locked)
{
    if (Locked)
        ExNotifyCallback(P->Cb, SBTH_CB_ACPM_UNLOCK, NULL);
}

int S9RailUp(S9_PLAT *P)
{
    BOOLEAN locked;
    int r;
    if (!AcpmLock(P, &locked))
        return -10;
    r = sbm_buck6_raise(&P->Io, &P->BuckOld);
    AcpmUnlock(P, locked);
    P->BuckRaised = r == 0 || r == -3;    /* -3: written, read back differs: restore anyway */
    return r;
}

void S9RailDown(S9_PLAT *P)
{
    BOOLEAN locked;
    if (!P->BuckRaised)
        return;
    if (AcpmLock(P, &locked)) {
        (void)sbm_buck6_restore(&P->Io, P->BuckOld);
        AcpmUnlock(P, locked);
    }                                     /* not locked: S-Boot resets BUCK6 at the next boot */
    P->BuckRaised = FALSE;
}

/* ---- registry: the service key ----------------------------------------------------------------------- */

static WCHAR SvcBuf[256];
static UNICODE_STRING Svc;

void S9SetServiceKey(PCUNICODE_STRING RegistryPath)
{
    RtlInitEmptyUnicodeString(&Svc, SvcBuf, sizeof(SvcBuf));
    (void)RtlAppendUnicodeStringToString(&Svc, RegistryPath);
}

static NTSTATUS Open(PCWSTR Sub, HANDLE *H)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING sub;
    HANDLE root;
    NTSTATUS st;
    InitializeObjectAttributes(&oa, &Svc, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwOpenKey(&root, KEY_READ | KEY_WRITE, &oa);
    if (!NT_SUCCESS(st) || Sub == NULL) {
        *H = root;
        return st;
    }
    RtlInitUnicodeString(&sub, Sub);
    InitializeObjectAttributes(&oa, &sub, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, root, NULL);
    st = ZwCreateKey(H, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    ZwClose(root);
    return st;
}

ULONG S9GetDword(PCWSTR Sub, PCWSTR Name, ULONG Dflt)
{
    HANDLE h;
    UNICODE_STRING n;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len, r = Dflt;
    if (!Svc.Length || !NT_SUCCESS(Open(Sub, &h)))
        return Dflt;
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(ZwQueryValueKey(h, &n, KeyValuePartialInformation, v, sizeof(buf), &len)) &&
        v->Type == REG_DWORD && v->DataLength == sizeof(ULONG))
        r = *(ULONG *)v->Data;
    ZwClose(h);
    return r;
}

void S9PutDword(PCWSTR Sub, PCWSTR Name, ULONG Value)
{
    HANDLE h;
    UNICODE_STRING n;
    if (!Svc.Length || !NT_SUCCESS(Open(Sub, &h)))
        return;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &Value, sizeof(Value));
    (void)ZwFlushKey(h);
    ZwClose(h);
}

BOOLEAN S9FileExists(PCWSTR Path)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    RtlInitUnicodeString(&us, Path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}
