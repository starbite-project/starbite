/* SPDX-License-Identifier: MIT
 * sbtk's platform on the host (macOS / Linux): the kernel's version is sb_alloc / sb_ticks / sb_log. */
#include "../sbtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SBTK_HEAP_MB=N: the kernel's heap (tk_heap.c) on one N MiB block, as on the phone (its limit, its fragmentation) */
#define tk_alloc tkh_alloc
#define tk_free tkh_free
#define tk_heap_used tkh_used
#define tk_heap_peak tkh_peak
#include "../tk_heap.c"
#undef tk_alloc
#undef tk_free
#undef tk_heap_used
#undef tk_heap_peak
void tk_heap_oom(uint64_t n, uint64_t u)
{
    fprintf(stderr, "sbtk: the UI heap is full: %llu bytes asked, %llu MiB in use, largest free %llu MiB\n",
            (unsigned long long)n, (unsigned long long)(u >> 20), (unsigned long long)(tk_heap_largest() >> 20));
}
static int kheap = -1;
static int use_kheap(void)
{
    if (kheap < 0) {
        const char *e = getenv("SBTK_HEAP_MB");
        kheap = e && atoi(e) > 0;
        if (kheap) {
            size_t n = (size_t)atoi(e) << 20;
            void *m = malloc(n);
            tk_heap_init(m, n);
        }
    }
    return kheap;
}
static size_t used, peak;
void *tk_alloc(size_t n)
{
    if (use_kheap()) return tkh_alloc(n);
    /* a 64-byte header holds the size, for the heap figures the kernel's heap gives too */
    void *p = 0;
    if (posix_memalign(&p, 64, n + 64)) return 0;
    memset(p, 0, n + 64);
    *(size_t *)p = n;
    used += n;
    if (used > peak) peak = used;
    return (char *)p + 64;
}
void tk_free(void *p)
{
    if (!p) return;
    if (use_kheap()) { tkh_free(p); return; }
    char *b = (char *)p - 64;
    used -= *(size_t *)b;
    free(b);
}
size_t tk_heap_used(void) { return use_kheap() ? tkh_used() : used; }
size_t tk_heap_peak(void) { return use_kheap() ? tkh_peak() : peak; }
uint64_t tk_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
void tk_log(const char *s) { fprintf(stderr, "sbtk: %s\n", s); }
