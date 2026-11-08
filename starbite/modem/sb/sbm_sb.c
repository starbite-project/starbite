/* SPDX-License-Identifier: GPL-2.0-only
 * sbm on libsb (Starbite OS and test images): the struct sbm_plat for a bare-metal image.
 *  - probe32: libsb's sb_probe32 (survives a synchronous abort), plus a check for an asynchronous one: libsb
 *    runs with PSTATE.A masked, so an SError a refused read raises stays pending in ISR_EL1.A. It is then taken
 *    at once into libsb's vector, which only counts it (crt0.S vec_serror), and the read counts as faulted. An
 *    SError already pending before the read is taken first and reported separately (sbm_sb_stale_serrors).
 *    If EL3 routes SErrors to itself (SCR_EL3.EA), none of this sees them: EL3 decides. docs/MODEM.md 5.
 *  - log: the caller's line sink (screen + pmsg).
 *  - barrier: dmb ish.
 *  - smc: smc #0 (sb_psci's call). sbm only makes EL3's CP_CTRL read call with it (sbm_cp_ctrl_read).
 *  - radio_read: the RADIO partition (GPT "RADIO" on UFS LU0), read only, through a 4 KiB block buffer.
 *  - poke32: one store + clean/invalidate to DRAM, for sbm_probe_write_test's two fixed words only: any other
 *    address is refused here as well (a second lock). No other store path exists in this glue.
 */
#include "sb.h"
#include "ufs.h"
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

static uint64_t smc(void *ctx, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)ctx;
    if (id != SBM_SMC_CP || a1 != SBM_SMC_READ_CTRL || a2 > 1 || a3 > 1)   /* the read call only */
        return 0xFFFF;
    return (uint64_t)sb_psci(id, a1, a2, a3);
}

static uint64_t radio_first, radio_last;
static int radio_state;                 /* 0 not tried, 1 found, -1 not found */
static uint8_t radio_blk[4096] __attribute__((aligned(64)));
static uint64_t radio_blk_lba = ~0ULL;

static int radio_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (!radio_state)
        radio_state = ufs_up() && gpt_load() > 0 && gpt_find("RADIO", &radio_first, &radio_last) ? 1 : -1;
    if (radio_state < 0)
        return -1;
    uint8_t *d = buf;
    while (len) {
        uint64_t lba = radio_first + off / 4096;
        uint32_t o = (uint32_t)(off % 4096), k = 4096 - o < len ? 4096 - o : len;
        if (lba > radio_last)
            return -1;
        if (lba != radio_blk_lba) {
            if (sb_blk_read(SB_MED_UFS, 0, lba, 1, radio_blk) != 0) {
                radio_blk_lba = ~0ULL;
                return -1;
            }
            radio_blk_lba = lba;
        }
        memcpy(d, radio_blk + o, k);
        d += k;
        off += k;
        len -= k;
    }
    return 0;
}

static int poke(void *ctx, uint64_t pa, uint32_t v)
{
    (void)ctx;
    if (pa != SBM_MODEM_IF_PA + SBM_DT_IPC_OFF + SBM_SBD_CMD_OFF && pa != SBM_MODEM_IF_PA + SBM_DT_CP_SIZE - 4)
        return 0;
    if (serror_pending()) {
        stale_serrors++;
        serror_take();
    }
    uint64_t se = sb_fault.serror_count, c = sb_fault.count;
    sb_probe_depth++;
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" : : : "memory");
    sb_dcache_clean_inval((const volatile void *)(uintptr_t)pa, 4);   /* to DRAM; the next read comes from it */
    __asm__ volatile("isb" : : : "memory");
    sb_probe_depth--;
    int ok = sb_fault.count == c;
    if (serror_pending()) {
        serror_take();
        ok = 0;
    }
    if (sb_fault.serror_count != se)
        ok = 0;
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
    p->smc = 0;                         /* step 4 only: sbm_sb_plat_step4 */
    p->radio_read = radio_read;
    p->poke32 = 0;
}

void sbm_sb_plat_step4(struct sbm_plat *p)
{
    p->smc = smc;
    p->poke32 = poke;
}

unsigned sbm_sb_stale_serrors(void) { return stale_serrors; }
