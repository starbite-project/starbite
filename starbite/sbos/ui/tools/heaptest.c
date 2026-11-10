/* heaptest.c: tk_heap.c under random allocations and frees (sizes 1 B .. 20 MB), every block checked for overlap,
 * zeroing and the list's consistency; then everything freed must leave one free block. */
#include "../sbtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void tk_heap_init(void *mem, uint64_t size);
int tk_heap_check(void);
void tk_heap_oom(uint64_t n, uint64_t used) { (void)n; (void)used; }
uint64_t tk_us(void) { return 0; }
void tk_log(const char *s) { puts(s); }
int main(void)
{
    uint64_t size = 128ull << 20;
    void *mem = malloc(size);
    tk_heap_init((char *)mem + 8, size - 8);
    enum { N = 400 };
    unsigned char *p[N] = { 0 };
    size_t sz[N] = { 0 };
    srand(7);
    long allocs = 0, fails = 0;
    for (int it = 0; it < 200000; it++) {
        int i = rand() % N;
        if (p[i]) {
            for (size_t k = 0; k < sz[i]; k += 4093) if (p[i][k] != (unsigned char)i) { printf("FAIL: block %d overwritten\n", i); return 1; }
            tk_free(p[i]);
            p[i] = 0;
        } else {
            size_t n = rand() % 4 == 0 ? (size_t)(rand() % (20 << 20)) : (size_t)(rand() % 70000) + 1;
            p[i] = tk_alloc(n);
            allocs++;
            if (!p[i]) { fails++; continue; }
            if (((uintptr_t)p[i] & 63) != 0) { printf("FAIL: not 64-byte aligned\n"); return 1; }
            for (size_t k = 0; k < n; k += 997) if (p[i][k]) { printf("FAIL: not zeroed\n"); return 1; }
            memset(p[i], i, n);
            sz[i] = n;
        }
        if (it % 1000 == 0 && !tk_heap_check()) { printf("FAIL: list inconsistent at %d\n", it); return 1; }
    }
    for (int i = 0; i < N; i++) if (p[i]) tk_free(p[i]);
    if (!tk_heap_check() || tk_heap_used()) { printf("FAIL: not all free at the end\n"); return 1; }
    void *big = tk_alloc((size_t)(size - 4096));
    printf("heaptest: PASSED: %ld allocations (%ld refused: full), peak %.1f MB, one block again: %s\n", allocs, fails, tk_heap_peak() / 1e6, big ? "yes" : "no");
    return big ? 0 : 1;
}
