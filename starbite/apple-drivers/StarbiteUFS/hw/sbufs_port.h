/* SPDX-License-Identifier: APSL-2.0 */
/* StarbiteUFS core: the few things the plain-C core needs from its host (the kernel or the Mac test build). */
#ifndef SBUFS_PORT_H
#define SBUFS_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#if defined(KERNEL)
#include <libkern/libkern.h>      /* snprintf */
#else
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* error codes of the core (negative, so a slot number or count can share the return) */
#define SBUFS_OK                0
#define SBUFS_E_INVAL           (-1)    /* malformed request or property */
#define SBUFS_E_FENCE           (-2)    /* refused by the fence */
#define SBUFS_E_BUSY            (-3)    /* no free slot */
#define SBUFS_E_IO              (-4)    /* device or host reported an error */
#define SBUFS_E_TIMEOUT         (-5)
#define SBUFS_E_NODEV           (-6)    /* host not ready / not ours / dead */
#define SBUFS_E_DMA             (-7)    /* a segment the host cannot take (address, alignment, size) */
#define SBUFS_E_ROFS            (-8)    /* write while writes are off */
#define SBUFS_E_GATE            (-9)    /* the doorbell audit refused the descriptor as built */

static inline uint16_t sbufs_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t sbufs_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t sbufs_be64(const uint8_t *p) { return ((uint64_t)sbufs_be32(p) << 32) | sbufs_be32(p + 4); }
static inline void sbufs_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void sbufs_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void sbufs_put_be64(uint8_t *p, uint64_t v) { sbufs_put_be32(p, (uint32_t)(v >> 32)); sbufs_put_be32(p + 4, (uint32_t)v); }
static inline uint32_t sbufs_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t sbufs_le64(const uint8_t *p) { return (uint64_t)sbufs_le32(p) | ((uint64_t)sbufs_le32(p + 4) << 32); }

#ifdef __cplusplus
}
#endif

#endif /* SBUFS_PORT_H */
