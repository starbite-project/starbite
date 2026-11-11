/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Compile-check only (tools/apple/kcheck.py; never part of a build). IOHIDFamily's IOKit/hid/IOHIDEvent.h is
 * not in the macOS SDK's Kernel.framework, and IOHIDFamily-2238.120.5's public tree lacks IOHIDEventTypes.h and
 * the field/struct/macro definition headers it needs (INTEGRATION.md). Until the open-source family is built in,
 * this forwards to the IOHIDFamily-1915.140.3 copy our kernel carries (build/xnu-12377/iokit/Kernel/arm/hid, on
 * the include path given to kcheck): the same IOHIDEvent API (factory functions and setters) as 2238.
 */
#include <IOHIDEvent.h>
