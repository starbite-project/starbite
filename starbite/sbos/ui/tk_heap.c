/* SPDX-License-Identifier: MIT
 * sbtk's heap for the kernel (whose arena never frees): first fit over an address-ordered block list, a block split
 * when the rest is worth keeping, merged with free neighbours on free; every block 64-byte aligned and zeroed
 * when handed out. Tested on the host by tools/heaptest.c. */
#include "sbtk.h"

struct blk { uint64_t size; uint64_t free; struct blk *next, *prev; uint64_t pad[4]; };   /* 64 bytes */
static struct blk *heap;
static uint64_t h_used, h_peak, h_size;
void tk_heap_oom(uint64_t n, uint64_t used);

void tk_heap_init(void *mem, uint64_t size)
{
    heap = (struct blk *)(((uintptr_t)mem + 63) & ~(uintptr_t)63);
    size -= (uint64_t)((uintptr_t)heap - (uintptr_t)mem);
    size &= ~63ULL;
    h_size = size;
    heap->size = size - sizeof(struct blk);
    heap->free = 1;
    heap->next = heap->prev = 0;
    h_used = h_peak = 0;
}

/* a big block (a bitmap) is carved from the top end of the highest free block that holds it, small ones first fit
 * from the bottom: the two kinds stay apart, so freed bitmaps leave whole holes for the next bitmap instead of
 * holes split by the small blocks made meanwhile (P2l: a 17 MB page refused with 35 MiB free) */
#define BIG (1u << 20)
static void *take(struct blk *b)
{
    b->free = 0;
    h_used += b->size;
    if (h_used > h_peak) h_peak = h_used;
    tk_memset(b + 1, 0, b->size);
    return b + 1;
}
void *tk_alloc(size_t n)
{
    n = (n + 63) & ~(size_t)63;
    if (!n) n = 64;
    if (n >= BIG) {
        struct blk *last = heap;
        while (last && last->next) last = last->next;
        for (struct blk *b = last; b; b = b->prev) {
            if (!b->free || b->size < n) continue;
            if (b->size >= n + sizeof(struct blk) + 4096) {
                struct blk *r = (struct blk *)((uint8_t *)(b + 1) + b->size - n - sizeof(struct blk));
                r->size = n;
                r->next = b->next;
                r->prev = b;
                if (b->next) b->next->prev = r;
                b->next = r;
                b->size -= n + sizeof(struct blk);
                return take(r);
            }
            return take(b);
        }
        tk_heap_oom(n, h_used);
        return 0;
    }
    for (struct blk *b = heap; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(struct blk) + 4096) {
            struct blk *r = (struct blk *)((uint8_t *)(b + 1) + n);
            r->size = b->size - n - sizeof(struct blk);
            r->free = 1;
            r->next = b->next;
            r->prev = b;
            if (b->next) b->next->prev = r;
            b->next = r;
            b->size = n;
        }
        return take(b);
    }
    tk_heap_oom(n, h_used);
    return 0;
}

void tk_free(void *p)
{
    if (!p) return;
    struct blk *b = (struct blk *)p - 1;
    if (b->free) return;
    b->free = 1;
    h_used -= b->size;
    if (b->next && b->next->free) {        /* merge with the next, then the previous */
        struct blk *n = b->next;
        b->size += sizeof(struct blk) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        struct blk *q = b->prev;
        q->size += sizeof(struct blk) + b->size;
        q->next = b->next;
        if (b->next) b->next->prev = q;
    }
}
/* the largest free block (how big a bitmap can still be had) */
uint64_t tk_heap_largest(void)
{
    uint64_t m = 0;
    for (struct blk *b = heap; b; b = b->next) if (b->free && b->size > m) m = b->size;
    return m;
}
size_t tk_heap_used(void) { return h_used; }
size_t tk_heap_peak(void) { return h_peak; }
uint64_t tk_heap_size(void) { return h_size; }
/* 1 if the list is consistent (the host test) */
int tk_heap_check(void)
{
    uint64_t total = 0, used = 0;
    for (struct blk *b = heap; b; b = b->next) {
        if (b->next && b->next->prev != b) return 0;
        if (b->next && (uint8_t *)b->next != (uint8_t *)(b + 1) + b->size) return 0;
        if (b->free && b->next && b->next->free) return 0;
        total += sizeof(struct blk) + b->size;
        if (!b->free) used += b->size;
    }
    return total == h_size && used == h_used;
}
