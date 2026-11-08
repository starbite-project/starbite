/* SPDX-License-Identifier: GPL-2.0-only
 * sbm on libsb (Starbite OS and test images): the struct sbm_plat for a bare-metal image.
 *  - probe32: libsb's sb_probe32 (survives a synchronous abort), plus a check for an asynchronous one: libsb
 *    runs with PSTATE.A masked, so an SError a refused read raises stays pending in ISR_EL1.A. It is then taken
 *    at once into libsb's vector, which only counts it (crt0.S vec_serror), and the read counts as faulted. An
 *    SError already pending before the read is taken first and reported separately (sbm_sb_stale_serrors).
 *    If EL3 routes SErrors to itself (SCR_EL3.EA), none of this sees them: EL3 decides. docs/MODEM.md 5.
 *  - log: the caller's line sink (screen + pmsg).
 *  - barrier: dmb ish.
 * No write path: this glue cannot store to a register.
 */
#include "sb.h"
#include "../sbm.h"
#include "sbm_sb.h"

static unsigned stale_serrors;

static int serror_pending(void)
{
    uint64_t isr;
    __asm__ volatile("isb\n\tmrs %0, isr_el1" : "=r"(isr) : : "memory");
    return (isr >> 8) & 1;
}

static void serror_take(void)
{
    /* unmask A for a moment: the pending SError enters vec_serror (count, eret); then mask it again */
    __asm__ volatile("msr daifclr, #4\n\tisb\n\tnop\n\tmsr daifset, #4\n\tisb" : : : "memory");
}

static int probe(void *ctx, uint64_t pa, uint32_t *v)
{
    (void)ctx;
    if (serror_pending()) {
        stale_serrors++;
        serror_take();
    }
    uint64_t se = sb_fault.serror_count;
    int ok = sb_probe32(pa, v);
    __asm__ volatile("dsb sy" : : : "memory");
    if (serror_pending()) {
        serror_take();
        ok = 0;
    }
    if (sb_fault.serror_count != se)
        ok = 0;
    if (!ok)
        *v = 0;
    return ok;
}

static void barrier(void *ctx)
{
    (void)ctx;
    __asm__ volatile("dmb ish" : : : "memory");
}

static void (*sink)(const char *);
static void log_line(void *ctx, const char *s)
{
    (void)ctx;
    if (sink)
        sink(s);
}

void sbm_sb_plat(struct sbm_plat *p, void (*line)(const char *))
{
    sink = line;
    p->ctx = 0;
    p->probe32 = probe;
    p->log = log_line;
    p->barrier = barrier;
}

unsigned sbm_sb_stale_serrors(void) { return stale_serrors; }
