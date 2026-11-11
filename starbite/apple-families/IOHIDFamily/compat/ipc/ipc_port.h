/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Starbite compat: IOHIDLibUserClient.cpp includes <ipc/ipc_port.h> (osfmk-private; a kext gets the
 * Kernel.framework PrivateHeaders copy) only for ipc_port_release_send(). Declared here with XNU's own signature
 * (osfmk/ipc/ipc_port.h, iokit/IOKit/IOKitServer.h); the symbol is in config/Private.exports.
 */
#ifndef _STARBITE_COMPAT_IPC_PORT_H
#define _STARBITE_COMPAT_IPC_PORT_H
#include <sys/cdefs.h>
#include <mach/mach_types.h>
__BEGIN_DECLS
extern void ipc_port_release_send(ipc_port_t port);
__END_DECLS
#endif
