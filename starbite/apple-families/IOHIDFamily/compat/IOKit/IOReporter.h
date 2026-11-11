/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Starbite compat: IOHIDDevice.cpp and IOHIDSystem.cpp include <IOKit/IOReporter.h>, a Kernel.framework header of
 * Apple's internal SDK. The classes they use (IOHistogramReporter, IOSimpleReporter, IOReportLegend) are XNU's
 * own, declared in <IOKit/IOKernelReporters.h>.
 */
#ifndef _STARBITE_COMPAT_IOREPORTER_H
#define _STARBITE_COMPAT_IOREPORTER_H
#include <IOKit/IOKernelReporters.h>
#endif
