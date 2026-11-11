/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Starbite compat: IOHIDEvent.cpp and IOHIDEventDriver.cpp include <math.h> (a Kernel.framework header of the
 * SDK, which forwards to the user-space libm header). They use only the NAN constant and no libm function; the
 * kernel has no libm. Anything else stays undeclared, so a real libm call is a compile error here instead of an
 * unresolved symbol at link time.
 */
#ifndef _STARBITE_COMPAT_MATH_H
#define _STARBITE_COMPAT_MATH_H
#ifndef NAN
#define NAN __builtin_nanf("")
#endif
#endif
