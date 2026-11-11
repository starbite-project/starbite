/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Starbite: compile an Apple IOKit family's sources INSIDE XNU the way the family is built as a kext.
 *
 * A kext sees the kernel headers as Kernel.framework ships them: KERNEL and KERNEL_PRIVATE (the families link
 * against com.apple.kpi.private), PRIVATE, DRIVER_PRIVATE - but none of XNU's internal views. Built into the
 * kernel with iokit's own flags, the same sources would also see XNU_KERNEL_PRIVATE / BSD_KERNEL_PRIVATE /
 * MACH_KERNEL_PRIVATE parts of the headers (struct internals, C++-only helpers, private overloads), which they
 * were never written for: IONetworkingFamily then fails on <sys/mbuf.h>, <net/dlil.h> and kern/startup.h.
 *
 * Use as the first thing in each family translation unit, after the kernel's -D options:
 *     <obj>_CFLAGS_ADD += -include $(SRCROOT)/<path>/kext-view.h
 * (the same mechanism as bsd/sweet_hfs/sweet_hfs_compat.h in our tree).
 */
#ifndef STARBITE_KEXT_VIEW_H
#define STARBITE_KEXT_VIEW_H
#undef XNU_KERNEL_PRIVATE
#undef BSD_KERNEL_PRIVATE
#undef MACH_KERNEL_PRIVATE
#undef IOKIT_KERNEL_PRIVATE
/*
 * Kernel configuration options are not visible to kexts either. Only the ones that change what a kext-side
 * header pulls in are cleared here; each is layout-neutral for the KPI classes:
 *   IOKITSTATS - IOWorkLoop/IOEventSource/IOUserClient keep a pointer either way (counter vs iokitstatsReserved);
 *                with it the headers include IOStatisticsPrivate.h -> OSKext.h, which needs XNU_KERNEL_PRIVATE.
 */
#undef IOKITSTATS
#endif
