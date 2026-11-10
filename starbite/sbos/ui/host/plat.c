/* SPDX-License-Identifier: MIT
 * sbtk's platform on the host (macOS / Linux): the kernel's version is sb_alloc / sb_ticks / sb_log. */
#include "../sbtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *tk_alloc(size_t n)
{
    void *p = 0;
    if (posix_memalign(&p, 64, n ? n : 1)) return 0;
    memset(p, 0, n);
    return p;
}
void tk_free(void *p) { free(p); }
uint64_t tk_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
void tk_log(const char *s) { fprintf(stderr, "sbtk: %s\n", s); }
