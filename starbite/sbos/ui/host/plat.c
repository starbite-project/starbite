/* SPDX-License-Identifier: MIT
 * sbtk's platform on the host (macOS / Linux): the kernel's version is sb_alloc / sb_ticks / sb_log. */
#include "../sbtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <execinfo.h>

/* SBTK_HEAP_MB=N: the kernel's heap (tk_heap.c) on one N MiB block, as on the phone (its limit, its fragmentation) */
#define tk_alloc tkh_alloc
#define tk_free tkh_free
#define tk_heap_used tkh_used
#define tk_heap_peak tkh_peak
#define tk_shrink tkh_shrink
#include "../tk_heap.c"
#undef tk_alloc
#undef tk_free
#undef tk_heap_used
#undef tk_heap_peak
#undef tk_shrink
int host_ooms;
void tk_heap_oom(uint64_t n, uint64_t u)
{
    host_ooms++;
    fprintf(stderr, "sbtk: the UI heap is full: %llu bytes asked, %llu MiB in use, largest free %llu MiB\n",
            (unsigned long long)n, (unsigned long long)(u >> 20), (unsigned long long)(tk_heap_largest() >> 20));
    if (getenv("SBTK_OOM_TRACE")) { void *bt[16]; backtrace_symbols_fd(bt, backtrace(bt, 16), 2); }
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
/* SBTK_MEMTRACE: the live big blocks and who made them (host_memdump; atos resolves the frames) */
#include <mach-o/dyld.h>
#define MT_N 1024
static struct { void *p; size_t n; void *bt[7]; int nb; } MT[MT_N];
static int mt_on = -1;
static void mt_add(void *p, size_t n)
{
    if (mt_on < 0) mt_on = getenv("SBTK_MEMTRACE") != 0;
    if (!mt_on || !p || n < (128u << 10)) return;
    for (int i = 0; i < MT_N; i++) if (!MT[i].p) { MT[i].p = p; MT[i].n = n; MT[i].nb = backtrace(MT[i].bt, 7); return; }
}
static void mt_del(void *p) { if (mt_on > 0) for (int i = 0; i < MT_N; i++) if (MT[i].p == p) { MT[i].p = 0; return; } }
void host_memdump(const char *tag)
{
    if (mt_on <= 0) return;
    fprintf(stderr, "memdump %s slide %lx\n", tag, (unsigned long)_dyld_get_image_vmaddr_slide(0));
    for (int i = 0; i < MT_N; i++) if (MT[i].p) {
        fprintf(stderr, "mt %8zu", MT[i].n);
        for (int k = 1; k < MT[i].nb; k++) fprintf(stderr, " %p", MT[i].bt[k]);
        fputc('\n', stderr);
    }
}
static void *tk_alloc_(size_t n);
void *tk_alloc(size_t n) { void *p = tk_alloc_(n); mt_add(p, n); return p; }
static void *tk_alloc_(size_t n)
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
    mt_del(p);
    if (use_kheap()) { tkh_free(p); return; }
    char *b = (char *)p - 64;
    used -= *(size_t *)b;
    free(b);
}
void *tk_shrink(void *p, size_t n)
{
    if (!p) return 0;
    void *q;
    if (use_kheap()) q = tkh_shrink(p, n);
    else {
        size_t *h = (size_t *)((char *)p - 64);
        if (n < *h) { used -= *h - n; *h = n; }
        q = p;
    }
    if (mt_on > 0) for (int i = 0; i < MT_N; i++) if (MT[i].p == p) { MT[i].p = q; MT[i].n = n; if (n < (128u << 10)) MT[i].p = 0; break; }
    return q;
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
