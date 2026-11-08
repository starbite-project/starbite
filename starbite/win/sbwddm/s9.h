/* SPDX-License-Identifier: GPL-2.0-only
 * sbwddm on the Galaxy S9 (docs/WIN11-GPU.md 6.8): what the Exynos 9810 needs around the Mali that
 * wddm-mali-g52's board did not. Built on sbmali's phone-proven code (sbm_core, the sbthermal callback):
 *   - the G3D power domain (PMU, read only), ACPM (BUCK6 = vdd_g3d raised to 950 mV under sbthermal's ACPM
 *     lock, put back on stop), CMU_G3D (read only);
 *   - the temperature watchdog: the GPU is started only while sbthermal answers, and work is held (fences
 *     complete without running) at Stop/Critical or when the watchdog goes silent;
 *   - the registry helpers for the driver's own service key (mode choice and guard in DriverEntry).
 * The GPU's clock stays at S-Boot's 260 MHz in this stage (no DVFS).
 */
#ifndef SBWDDM_S9_H
#define SBWDDM_S9_H

#include <ntddk.h>
#include "../sbmali/sbm_core.h"
#include "../sbthermal/sbth_core.h"

typedef struct _S9_PLAT {
    volatile UCHAR *Mmio[SBM_BLOCKS];     /* SBM_GPU is the caller's mapping, the rest are ours */
    SIZE_T MmioSize[SBM_BLOCKS];
    BOOLEAN Owned[SBM_BLOCKS];
    struct sbm_io Io;
    PCALLBACK_OBJECT Cb;                  /* \Callback\StarbiteThermal (opened, never created) */
    struct sbth_state Last;
    UCHAR BuckOld;
    BOOLEAN BuckRaised;
} S9_PLAT;

/* Map the fixed SoC blocks around the GPU registers the adapter already mapped. */
NTSTATUS S9Init(S9_PLAT *P, volatile ULONG *GpuRegs, SIZE_T GpuLen);
void S9Fini(S9_PLAT *P);

/* The G3D domain is on (PMU G3D STATUS & 0xf == 0xf): GPU registers may be read. */
BOOLEAN S9DomainOn(S9_PLAT *P);

/* 0 = the watchdog answers with a fresh reading; else 1 no object, 2 no answer, 3 stale. *Level = its level
 * (SBTH_CRITICAL when not alive). PASSIVE_LEVEL. */
ULONG S9Gate(S9_PLAT *P, ULONG *Level);

/* vdd_g3d to 950 mV through ACPM under sbthermal's lock (sbm_buck6_raise: only from 0.5..1.0 V, read back).
 * 0 = done. S9RailDown puts the value found back. PASSIVE_LEVEL. */
int S9RailUp(S9_PLAT *P);
void S9RailDown(S9_PLAT *P);

/* The driver's service key (RegistryPath of DriverEntry), and DWORDs in it or its Parameters subkey. */
void S9SetServiceKey(PCUNICODE_STRING RegistryPath);
ULONG S9GetDword(PCWSTR Sub, PCWSTR Name, ULONG Dflt);
void S9PutDword(PCWSTR Sub, PCWSTR Name, ULONG Value);   /* flushed */
BOOLEAN S9FileExists(PCWSTR Path);

#endif
