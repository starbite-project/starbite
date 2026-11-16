/* x86-abi-layout.c (build/win/patchwork, the 32-bit sbumd audit, 2026-11-16): every structure the user-mode driver
 * hands the kernel driver (or gets back), with the size and field offsets a 64-bit build has. Compiled (syntax only)
 * for i686, x86_64 and aarch64 against Mesa's copy of the header (include/win32-compat/maliwddm_abi.h) and against
 * the kernel driver's (starbite/win/sbwddm/maliwddm/maliwddm_abi.h, also with -D_KERNEL_MODE): if any target lays a
 * structure out differently, that compile fails. Run: build/win/patchwork/x86-abi-layout.sh
 * -DSB_KMD_HEADER: the kernel driver's header, which has three structures Mesa's copy of 0167 does not use. */
#include <stddef.h>
#include "maliwddm_abi.h"
#define SZ(t, n) _Static_assert(sizeof(t) == (n), #t " size")
#define OFF(t, f, n) _Static_assert(offsetof(t, f) == (n), #t "." #f " offset")
#define AL(t, n) _Static_assert(_Alignof(t) == (n), #t " alignment")

SZ(MW_ALLOCATION_INFO, 32); AL(MW_ALLOCATION_INFO, 8);
OFF(MW_ALLOCATION_INFO, Version, 0); OFF(MW_ALLOCATION_INFO, Flags, 4); OFF(MW_ALLOCATION_INFO, Size, 8);
OFF(MW_ALLOCATION_INFO, Width, 16); OFF(MW_ALLOCATION_INFO, Height, 20); OFF(MW_ALLOCATION_INFO, Pitch, 24);
OFF(MW_ALLOCATION_INFO, Format, 28);

SZ(MW_COMMAND, 16); AL(MW_COMMAND, 8);
OFF(MW_COMMAND, Type, 0); OFF(MW_COMMAND, Slot, 4); OFF(MW_COMMAND, Jc, 8);

SZ(MW_ESCAPE_ALLOC_INFO_DATA, 24); AL(MW_ESCAPE_ALLOC_INFO_DATA, 8);
OFF(MW_ESCAPE_ALLOC_INFO_DATA, Code, 0); OFF(MW_ESCAPE_ALLOC_INFO_DATA, hAllocation, 4);
OFF(MW_ESCAPE_ALLOC_INFO_DATA, GpuVa, 8); OFF(MW_ESCAPE_ALLOC_INFO_DATA, Size, 16);

SZ(MW_ESCAPE_GPU_INFO_DATA, 376); AL(MW_ESCAPE_GPU_INFO_DATA, 8);
OFF(MW_ESCAPE_GPU_INFO_DATA, Code, 0); OFF(MW_ESCAPE_GPU_INFO_DATA, Count, 4); OFF(MW_ESCAPE_GPU_INFO_DATA, Value, 8);
OFF(MW_ESCAPE_GPU_INFO_DATA, Valid, 368);

SZ(MW_ESCAPE_STATS_DATA, 232); AL(MW_ESCAPE_STATS_DATA, 8);
OFF(MW_ESCAPE_STATS_DATA, Code, 0); OFF(MW_ESCAPE_STATS_DATA, LastSubmittedFence, 20);
OFF(MW_ESCAPE_STATS_DATA, LastCompletedFence, 24); OFF(MW_ESCAPE_STATS_DATA, Queued, 28);
OFF(MW_ESCAPE_STATS_DATA, Running, 32); OFF(MW_ESCAPE_STATS_DATA, JobsFailed, 40);
OFF(MW_ESCAPE_STATS_DATA, LastJsStatus, 48); OFF(MW_ESCAPE_STATS_DATA, LastFaultStatus, 52);
OFF(MW_ESCAPE_STATS_DATA, LastFaultAddress, 64); OFF(MW_ESCAPE_STATS_DATA, PagingOps, 72);
OFF(MW_ESCAPE_STATS_DATA, Maps, 136); OFF(MW_ESCAPE_STATS_DATA, LastMapVa, 160);
OFF(MW_ESCAPE_STATS_DATA, LastAllocVa, 168); OFF(MW_ESCAPE_STATS_DATA, Patches, 176);
OFF(MW_ESCAPE_STATS_DATA, PatchAddress0, 192); OFF(MW_ESCAPE_STATS_DATA, PresentBlts, 200);
OFF(MW_ESCAPE_STATS_DATA, Vsyncs, 216); OFF(MW_ESCAPE_STATS_DATA, FlipCopySkips, 228);

#ifdef SB_KMD_HEADER
SZ(MW_ESCAPE_SB_NOTE_DATA, 16); AL(MW_ESCAPE_SB_NOTE_DATA, 4);
OFF(MW_ESCAPE_SB_NOTE_DATA, Code, 0); OFF(MW_ESCAPE_SB_NOTE_DATA, Id, 4); OFF(MW_ESCAPE_SB_NOTE_DATA, Value, 8);

SZ(MW_ESCAPE_SB_RATES_DATA, 56); AL(MW_ESCAPE_SB_RATES_DATA, 8);
OFF(MW_ESCAPE_SB_RATES_DATA, Presents, 4); OFF(MW_ESCAPE_SB_RATES_DATA, GpuBusy100ns, 16);
OFF(MW_ESCAPE_SB_RATES_DATA, Now100ns, 40); OFF(MW_ESCAPE_SB_RATES_DATA, ThermalHolds, 48);

SZ(MW_ESCAPE_SB_CACHE_DATA, 32); AL(MW_ESCAPE_SB_CACHE_DATA, 8);
OFF(MW_ESCAPE_SB_CACHE_DATA, hAllocation, 4); OFF(MW_ESCAPE_SB_CACHE_DATA, Offset, 8);
OFF(MW_ESCAPE_SB_CACHE_DATA, Size, 16); OFF(MW_ESCAPE_SB_CACHE_DATA, Op, 24); OFF(MW_ESCAPE_SB_CACHE_DATA, Result, 28);
#else
/* the shim's own copy of the note (malikm_note in malikm_drm.c) */
struct sb_note { mw_u32 Code, Id, Value, Reserved; };
SZ(struct sb_note, 16);
#endif
