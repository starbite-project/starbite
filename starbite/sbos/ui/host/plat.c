/* SPDX-License-Identifier: MIT
 * sbtk's platform on the host (macOS / Linux): the kernel's version is sb_alloc / sb_ticks / sb_log. */
#include "../sbtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static size_t used, peak;
void *tk_alloc(size_t n)
{
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
    char *b = (char *)p - 64;
    used -= *(size_t *)b;
    free(b);
}
size_t tk_heap_used(void) { return used; }
size_t tk_heap_peak(void) { return peak; }
uint64_t tk_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
void tk_log(const char *s) { fprintf(stderr, "sbtk: %s\n", s); }
