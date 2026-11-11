/* SPDX-License-Identifier: APSL-2.0 */
/* Tiny test harness and the fence-property builders shared by the StarbiteUFS host tests. */
#ifndef SBUFS_TEST_H
#define SBUFS_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/sbufs_hc.h"

extern unsigned t_checks, t_fails;

#define CHECK(c) do { \
	t_checks++; \
	if (!(c)) { \
	        t_fails++; \
	        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	} \
} while (0)
#define CHECK_EQ(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	t_checks++; \
	if (_a != _b) { \
	        t_fails++; \
	        printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
	} \
} while (0)

/* property builders: what tools/board / the loader must emit (NOTES.md "Fence property format") */
struct blob {
	uint8_t b[16 + 64 * 48];
	size_t len;
};
void blob_allow_begin(struct blob *x);
void blob_allow_add(struct blob *x, uint32_t lun, uint32_t flags, uint64_t first, uint64_t count, const char *name);
void blob_deny_begin(struct blob *x);
void blob_deny_add(struct blob *x, uint32_t lun, uint64_t first, uint64_t count, const char *name);
void le32w(uint8_t *p, uint32_t v);
void le64w(uint8_t *p, uint64_t v);

void test_fence(void);
void test_proto(void);
void test_hc(void);

#endif
