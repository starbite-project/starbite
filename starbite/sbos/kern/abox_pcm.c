/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: ABOX bypass engine for media sound (abox_pcm.h has the what, the wiring and the sources).
 *
 * The bring-up (apcm_up), each step with its source:
 *   1  refuse if Calliope owns ABOX (abox.c), so a call is never disturbed;
 *   2  AUD domain: PMU 0x14064000 [3:0] = 0xF until 0x14064004 [3:0] reads 0xF (flexpmu aud_on), then the DTZPC
 *      restore SMC 0x82000410 (1, 0x17C10204) (exynos_pd_tz_restore) when we switched it on; IP_INDEX must read "ABOX";
 *   3  ABOX's CPU off if the boot left it on (abox_cpu_enable(false), abox_cpu_power(false); an earlier port saw it on);
 *   4  clocks: SYSREG_AUD MISC_CON 1, the CPU clock's Q-channel asked on (abox_enable()), dividers BUS /2, BUSP /3,
 *      AUDIF /16 (73.728 MHz), UAIF0/1 mux 0 and /48 (BCLK 1.536 MHz = 48000 x 2 x 16), PLL_AUD 1179.648 MHz (M 45
 *      P 1 S 0 K 24319) unless already there; the SysMMU's and both BCLKs' Q-channels asked on (kern/abox.c, cmucal);
 *   5  SysMMU_AUD: CTRL 0 (pass-through) if the boot left it translating or blocked (an earlier port: CTRL 4 found, 0 works);
 *   6  pins gpb1-0..3 (UAIF1) and gpb0-1..4 (UAIF0): function 2, pull 1 (aud-i2s*-bus); pad retention released;
 *   7  SYSPOWER_CTRL 1 and QCHANNEL_DISABLE ACLK + BCLK_UAIF0/1 while up (an earlier port; Samsung for UAIF0);
 *   8  UAIF0/1 CTRL1 0x0984F000 (I2S, 2 x 16, NB_NF), CTRL0 master; UAIF0 DATA_MODE 1, IRQ_MODE 0 (abox_if.c).
 * apcm_down undoes each step that was done, in reverse, and powers the domain off only if step 2 switched it on.
 *
 * Writes only: the PMU's audio offsets, CMU_AUD, SYSREG_AUD, ABOX SFR, SysMMU_AUD, the AUD pinctrl (reg_ok refuses
 * anything else before it reaches the HAL), and abox_rmem's two ring windows (through H.ram). Never ABOX's SRAM,
 * GIC or CPU registers beyond switching the CPU off. */
#include "abox_pcm.h"
#include <stdarg.h>

#ifdef SBOS_KERNEL
#include "sb.h"
#else
void *memset(void *d, int c, size_t n);
#endif

/* CMU_AUD (cmucal-sfr.c; as kern/abox.c) */
#define CMU_PLL_LOCKTIME   0x0000U
#define CMU_PLL_CON0       0x0120U
#define CMU_PLL_CON3       0x018CU
#define CMU_MUX_UAIF0      0x1004U
#define CMU_MUX_UAIF1      0x1008U
#define CMU_DIV_AUDIF      0x1800U
#define CMU_DIV_BUS        0x1804U
#define CMU_DIV_BUSP       0x1808U
#define CMU_MUX_CPU        0x1000U
#define CMU_DIV_CPU_ACLK   0x180CU
#define CMU_DIV_PLL        0x1820U
#define CMU_DIV_UAIF0      0x1824U
#define CMU_DIV_UAIF1      0x1828U
#define CMU_QCH_DUMMY      0x3000U
#define CMU_QCH_BCLK0      0x3028U
#define CMU_QCH_BCLK1      0x302CU
#define CMU_QCH_SYSMMU     0x3060U
#define DIV_BUSY           (1U << 16)
#define PLL_ENABLE         (1U << 31)
#define PLL_STABLE         (1U << 29)
#define PLL_MUX_BUSY       (1U << 7)
#define PLL_MUX_SEL        (1U << 4)
#define PLL_MPS_MASK       0x03FF3F07U
#define PLL_WANT           ((45U << 16) | (1U << 8) | 0U)
#define PLL_K              24319U
#define PLL_LOCK           500U
#define QCH_ENABLE         (1U << 0)
#define QCH_CLOCK_REQ      (1U << 1)

#define SYSREG_MISC_CON    0x032CU
#define SFR_IP_INDEX       0x0000U
#define SFR_SYSPOWER_ST    0x0014U
#define SFR_SYS_CONFIG0    0x0020U          /* reset 0x4444 */
#define SFR_REMAP_MASK     0x0024U
#define SFR_REMAP_ADDR     0x0028U
#define SFR_DYN_CLK        0x0030U          /* DYN_CLOCK_OFF, reset 0x7FFFFFFF */
#define SFR_ROUTE_CTRL2    0x0048U
#define SFR_SPUS_CTRL2     0x0208U
#define SFR_SPUS_CTRL3     0x020CU
#define SFR_SBANK_MIXP     0x0260U
#define SFR_SPUM_CTRL1     0x0304U
#define SFR_ROUTE_CTRL0    0x0040U          /* UAIFn SPK [4n+3:4n]: 2 = SIFS1 (uaif_spkx_texts) */
#define SFR_ROUTE_CTRL1    0x0044U          /* NSRCn [4n+3:4n]: 8 = UAIF0 (nsrcx_texts) */
#define SFR_SPUS_CTRL0     0x0200U          /* chain 0 [3:0]: IN bit 3 (0 RDMA), OUT [2:1] (0 SIFS1), ASRC bit 0 */
#define SFR_SPUS_CTRL1     0x0204U          /* SIFS_OUT1_SEL [18:16]: 0 = SPUS OUT0 */
#define SFR_SPUM_CTRL0     0x0300U          /* NSRC0: ASRC bit 4, OUT bit 7 (0 = WDMA) */
#define SFR_SPUM_CTRL3     0x030CU          /* SIFM0 flush bit 0 */
#define SFR_UAIF_CTRL0(n)  (0x0500U + 0x10U * (n))
#define SFR_UAIF_CTRL1(n)  (0x0504U + 0x10U * (n))
#define SFR_UAIF_STATUS(n) (0x050CU + 0x10U * (n))
#define UAIF_SPK_EN        (1U << 0)
#define UAIF_MIC_EN        (1U << 1)
#define UAIF_MASTER        (1U << 2)
#define UAIF_IRQ_MODE      (1U << 3)
#define UAIF_DATA_MODE     (1U << 4)
#define UAIF_CTRL1_VAL     0x0984F000U      /* FORMAT 9 (16 bit, 2 ch), BCLK_POLARITY 1, I2S, SLOT_MAX 1, SBIT_MAX 15 */
#define QCH_DIS_ACLK       (1U << 1)
#define QCH_DIS_BCLK0      (1U << 2)
#define QCH_DIS_BCLK1      (1U << 3)
#define SBANK_SIZE         (0x3FFU << 20)
#define DMA_ENABLE         (1U << 0)
#define DMA_BURST          (0xFU << 19)
#define DMA_PROGRESS       (1U << 31)
#define VOL_MASK           0xFFFFFFU
#define VOL_UNITY          0x800000U

#define MMU_CTRL           0x000U
#define MMU_TRANSLATE      (1U << 2)
#define MMU_CFG            0x004U
#define MMU_STATUS         0x008U
#define MMU_INT_STATUS     0x060U
#define MMU_FAULT_ADDR     0x070U
#define MMU_BLOCK          (1U << 1)

#define GPB0_CON  0x00U
#define GPB0_PUD  0x08U
#define GPB1_CON  0x20U
#define GPB1_PUD  0x28U
#define UAIF0_PINS 0x000FFFF0U              /* gpb0-1..4 */
#define UAIF1_PINS 0x0000FFFFU              /* gpb1-0..3 */

#define STOP_US   40000U                    /* abox.h ABOX_DMA_TIMEOUT_NS */
#define FLUSH_US  1000U                     /* abox_rdma_hw_free(): 600-1200 us between the DMA and the UAIF stopping */

static struct abox_hal H;
static int have_hal;
static int (*owns_fn)(void);
static struct apcm_cfg C = { 0xFB200000ULL, 0x2800000ULL, 512U, 8U, 0U, APCM_KEEP, APCM_KEEP, APCM_KEEP, 0U };

/* what was found, to put back; and what was done, to undo */
static struct {
    int state, err;
    int powered, mmu_set, pins_set, saved, play, cap;
    unsigned qch_set;
    uint8_t *rm;
    uint32_t mmu_was, gpb0_con, gpb0_pud, gpb1_con, gpb1_pud, sysp_was, qdis_was, route0, route1, spus0, spus1, spum0,
             uaif0[2], uaif1[2], sb_rdma0, sb_nsrc0, misc_was, dyn_was, rd_c0, rd_c1, rd_lim, wd_c0, wd_lim;
} S;
static char why_buf[160] = "off";
static const uint32_t qch_regs[4] = { CMU_QCH_DUMMY, CMU_QCH_SYSMMU, CMU_QCH_BCLK0, CMU_QCH_BCLK1 };

/* ---- log: "apcm: " + %s %u %x ------------------------------------------------------------------------------- */
static void vfmt(char *b, unsigned cap, const char *fmt, va_list ap)
{
    unsigned n = 0;
    for (const char *p = "apcm: "; *p && n + 1 < cap; ++p)
        b[n++] = *p;
    for (const char *f = fmt; *f && n + 1 < cap; ++f) {
        if (*f != '%') {
            b[n++] = *f;
            continue;
        }
        ++f;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "?"; *s && n + 1 < cap; ++s)
                b[n++] = *s;
        } else if (*f == 'u' || *f == 'x') {
            unsigned long v = va_arg(ap, unsigned long);
            char t[24];
            unsigned i = 0, base = *f == 'x' ? 16U : 10U;
            if (base == 16 && n + 3 < cap)
                b[n++] = '0', b[n++] = 'x';
            do {
                t[i++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v && i < sizeof t);
            while (i && n + 1 < cap)
                b[n++] = t[--i];
        } else if (*f) {
            b[n++] = *f;
        } else {
            break;
        }
    }
    b[n] = 0;
}

/* every argument of %u / %x is passed as unsigned long */
static void say(const char *fmt, ...)
{
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, fmt, ap);
    va_end(ap);
    if (have_hal)
        H.log(H.ctx, b);
}

/* logged, and kept as apcm_why() (without the prefix) */
static void sayw(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, fmt, ap);
    va_end(ap);
    if (have_hal)
        H.log(H.ctx, b);
    unsigned i = 0;
    for (const char *s = b + 6; *s && i + 1 < sizeof why_buf; ++s)
        why_buf[i++] = *s;
    why_buf[i] = 0;
}
#define UL(x) ((unsigned long)(x))

/* ---- guarded access ------------------------------------------------------------------------------------------ */
static int in(uint64_t pa, uint64_t base, uint64_t size) { return pa >= base && pa < base + size; }

static int reg_ok(uint64_t pa)
{
    if (pa & 3U)
        return 0;
    if (pa == ABOX_PMU + ABOX_PMU_AUD_CONF || pa == ABOX_PMU + ABOX_PMU_CPU_CONF || pa == ABOX_PMU + ABOX_PMU_CPU_OPT ||
        pa == ABOX_PMU + ABOX_PMU_PAD_RET || pa == ABOX_PMU + ABOX_PMU_GPIO_MODE)
        return 1;
    return in(pa, ABOX_CMU, 0x8000) || in(pa, ABOX_SYSREG, 0x1000) || in(pa, ABOX_SFR, 0x10000) ||
           in(pa, ABOX_GPIO, 0x1000) || in(pa, ABOX_SYSMMU, 0x10000);
}

static unsigned refused;
static uint32_t r32(uint64_t pa) { return H.rd32(H.ctx, pa); }
static void w32(uint64_t pa, uint32_t v)
{
    if (!reg_ok(pa)) {
        if (!refused++)
            say("REFUSED a write outside the audio blocks (%x): a bug, nothing written", UL(pa));
        return;
    }
    H.wr32(H.ctx, pa, v);
}
static void m32(uint64_t pa, uint32_t mask, uint32_t val) { w32(pa, (r32(pa) & ~mask) | (val & mask)); }

static int wait32(uint64_t pa, uint32_t mask, uint32_t want, uint32_t timeout_us, uint32_t step_us, uint32_t *took)
{
    uint32_t t = 0;
    for (;;) {
        if ((r32(pa) & mask) == want) {
            if (took)
                *took = t;
            return 1;
        }
        if (t >= timeout_us)
            break;
        H.delay_us(H.ctx, step_us);
        t += step_us;
    }
    if (took)
        *took = t;
    return 0;
}

#define SFR(o) (ABOX_SFR + (o))
#define CMU(o) (ABOX_CMU + (o))
#define PMU(o) (ABOX_PMU + (o))

/* ---- set-up ----------------------------------------------------------------------------------------------------- */
void apcm_cfg_default(struct apcm_cfg *c)
{
    c->rmem_base = 0xFB200000ULL, c->rmem_size = 0x2800000ULL;
    c->sbank_bytes = 512U, c->burst = 8U, c->rdma_ctrl0_or = 0;
    c->dma_ctrl1 = c->dma_lim = c->dyn_clk = APCM_KEEP, c->qdis_or = 0;
}

int apcm_cfg_set(const struct apcm_cfg *c)
{
    if (!c || S.state == APCM_ST_UP || (c->rmem_base & 0xFFFFFU) || c->rmem_size < APCM_RM_CAP + APCM_RM_WINDOW ||
        c->rmem_base + c->rmem_size > (1ULL << 32) || (c->sbank_bytes & 15U) || c->sbank_bytes > 16368U ||
        c->burst > 15U || (c->rdma_ctrl0_or & (DMA_ENABLE | DMA_BURST)))
        return ABOX_ECFG;
    C = *c;
    return 0;
}

int apcm_hal_set(const struct abox_hal *h, int (*owns)(void))
{
    if (!h || !h->rd32 || !h->wr32 || !h->smc || !h->delay_us || !h->log || !h->ram) {
        have_hal = 0;
        return ABOX_ESTATE;
    }
    H = *h;
    have_hal = 1;
    owns_fn = owns;
    return 0;
}

int apcm_state(void) { return S.state; }
int apcm_playing(void) { return S.play ? S.play - 1 : -1; }
int apcm_capturing(void) { return S.cap; }
const char *apcm_why(void) { return why_buf; }
void apcm_sync(void)
{
    if (have_hal && H.sync)
        H.sync(H.ctx);
}

static int pd_on(void) { return (r32(PMU(ABOX_PMU_AUD_STAT)) & 0xFU) == 0xFU; }

void apcm_look(struct apcm_look *l)
{
    memset(l, 0, sizeof(*l));
    if (!have_hal)
        return;
    l->aud_stat = r32(PMU(ABOX_PMU_AUD_STAT));
    l->cpu_stat = r32(PMU(ABOX_PMU_CPU_STAT));
    l->aud_on = (l->aud_stat & 0xFU) == 0xFU;
    if (!l->aud_on)
        return;                             /* CMU_AUD and the SFRs live in the domain: not read while it is off */
    l->pll_con0 = r32(CMU(CMU_PLL_CON0));
    l->pll_con3 = r32(CMU(CMU_PLL_CON3));
    l->mmu_ctrl = r32(ABOX_SYSMMU + MMU_CTRL);
    l->ip_index = r32(SFR(SFR_IP_INDEX));
    uint32_t m = (l->pll_con0 >> 16) & 0x3FFU, p = (l->pll_con0 >> 8) & 0x3FU, s = l->pll_con0 & 7U;
    int64_t k = (int16_t)(l->pll_con3 & 0xFFFFU);
    if (!(l->pll_con0 & PLL_MUX_SEL))
        l->pll_hz = 26000000ULL;
    else if ((l->pll_con0 & PLL_ENABLE) && p)
        l->pll_hz = (uint64_t)((26000000LL * (((int64_t)m << 16) + k)) / ((int64_t)p << s) >> 16);
}

static uint64_t pll_hz(void)
{
    struct apcm_look l;
    apcm_look(&l);
    return l.pll_hz;
}

static void qch_on(unsigned i)
{
    uint64_t pa = CMU(qch_regs[i]);
    uint32_t v = r32(pa);
    if (!(v & QCH_ENABLE) && !(v & QCH_CLOCK_REQ)) {
        w32(pa, v | QCH_CLOCK_REQ);
        S.qch_set |= 1U << i;
    }
}

static int cmu_set(uint32_t off, uint32_t mask, uint32_t val)
{
    m32(CMU(off), mask, val);
    if (!wait32(CMU(off), DIV_BUSY, 0, 1000, 10, 0))
        return 0;
    return (r32(CMU(off)) & mask) == val;
}

static int cpu_stop(void)
{
    m32(PMU(ABOX_PMU_CPU_OPT), 1U << 28, 0);
    m32(PMU(ABOX_PMU_CPU_CONF), 1U, 0);
    return wait32(PMU(ABOX_PMU_CPU_STAT), 1U, 0, 10000, 10, 0);
}

static int dma_stop(uint64_t base, const char *name)
{
    uint32_t took = 0;
    if (!(r32(base + APCM_DMA_CTRL0) & DMA_ENABLE))
        return 1;
    m32(base + APCM_DMA_CTRL0, DMA_ENABLE, 0);
    /* abox_rdma_disable_barrier(): CTRL0's ENABLE reads 0 once the engine has stopped (40 ms at most) */
    int ok = wait32(base + APCM_DMA_CTRL0, DMA_ENABLE, 0, STOP_US, 100, &took);
    if (!ok)
        sayw("%s did not say it stopped within 40 ms (CTRL0 %x, STATUS %x)", name, UL(r32(base + APCM_DMA_CTRL0)),
             UL(r32(base + APCM_DMA_STATUS)));
    return ok;
}

/* everything put back; logs one line. Safe at any state. */
static void undo(void)
{
    int pd_ok = 1, stopped = 1;
    if (!have_hal)
        return;
    if (S.powered || S.saved) {
        if (pd_on()) {
            if (S.play) {
                stopped &= dma_stop(SFR(APCM_RDMA(0)), "RDMA0");
                H.delay_us(H.ctx, FLUSH_US);
            }
            if (S.cap) {
                m32(SFR(SFR_UAIF_CTRL0(0)), UAIF_MIC_EN, 0);
                stopped &= dma_stop(SFR(APCM_WDMA(1)), "WDMA1");
            }
            if (S.saved) {
                /* the SFRs, SysMMU and pins as found (all in the domain: only while it is on) */
                w32(SFR(SFR_UAIF_CTRL0(1)), S.uaif1[0] & ~(UAIF_SPK_EN | UAIF_MIC_EN));
                w32(SFR(SFR_UAIF_CTRL0(0)), S.uaif0[0] & ~(UAIF_SPK_EN | UAIF_MIC_EN));
                w32(SFR(SFR_UAIF_CTRL1(1)), S.uaif1[1]);
                w32(SFR(SFR_UAIF_CTRL1(0)), S.uaif0[1]);
                w32(SFR(SFR_UAIF_CTRL0(1)), S.uaif1[0]);
                w32(SFR(SFR_UAIF_CTRL0(0)), S.uaif0[0]);
                w32(SFR(SFR_ROUTE_CTRL0), S.route0);
                w32(SFR(SFR_ROUTE_CTRL1), S.route1);
                w32(SFR(SFR_SPUS_CTRL0), S.spus0);
                w32(SFR(SFR_SPUS_CTRL1), S.spus1);
                w32(SFR(SFR_SPUM_CTRL0), S.spum0);
                w32(SFR(APCM_SBANK_RDMA0), S.sb_rdma0);
                w32(SFR(APCM_SBANK_NSRC0), S.sb_nsrc0);
                w32(SFR(APCM_SFR_QCH_DIS), S.qdis_was);
                w32(SFR(APCM_SFR_SYSPOWER), S.sysp_was);
                if (S.mmu_set)
                    w32(ABOX_SYSMMU + MMU_CTRL, S.mmu_was);
                if (S.pins_set) {
                    w32(ABOX_GPIO + GPB1_CON, S.gpb1_con);
                    w32(ABOX_GPIO + GPB1_PUD, S.gpb1_pud);
                    w32(ABOX_GPIO + GPB0_CON, S.gpb0_con);
                    w32(ABOX_GPIO + GPB0_PUD, S.gpb0_pud);
                }
                for (unsigned i = 0; i < 4; ++i)
                    if (S.qch_set & (1U << i))
                        m32(CMU(qch_regs[i]), QCH_CLOCK_REQ, 0);
                w32(ABOX_SYSREG + SYSREG_MISC_CON, S.misc_was);
                w32(SFR(SFR_DYN_CLK), S.dyn_was);
                w32(SFR(APCM_RDMA(0) + APCM_DMA_CTRL0), S.rd_c0 & ~DMA_ENABLE);
                w32(SFR(APCM_RDMA(0) + APCM_DMA_CTRL1), S.rd_c1);
                w32(SFR(APCM_RDMA(0) + APCM_DMA_SBLIM), S.rd_lim);
                w32(SFR(APCM_WDMA(1) + APCM_DMA_CTRL0), S.wd_c0 & ~DMA_ENABLE);
                w32(SFR(APCM_WDMA(1) + APCM_DMA_SBLIM), S.wd_lim);
            }
            if (S.powered) {
                m32(PMU(ABOX_PMU_GPIO_MODE), 1U, 1U);               /* abox_pad_retention(true) */
                (void)H.smc(H.ctx, ABOX_SMC_PD, 0, ABOX_DTZPC, 0);  /* exynos_pd_tz_save() */
            }
        }
        if (S.powered) {
            /* aud_off, also when the domain never reported on: the request is withdrawn either way */
            m32(PMU(ABOX_PMU_AUD_CONF), 0xFU, 0);
            pd_ok = wait32(PMU(ABOX_PMU_AUD_STAT), 0xFU, 0, 10000, 10, 0);
        }
        say("down: DMAs %s, registers as found, AUD domain %s", stopped ? "stopped" : "NOT CONFIRMED STOPPED",
            !S.powered ? "left on (it was on before)" : pd_ok ? "off again" : "STILL ON");
    }
    S.powered = S.saved = S.mmu_set = S.pins_set = S.play = S.cap = 0;
    S.qch_set = 0;
    S.rm = 0;
}

static int fail(int err)
{
    char keep[sizeof why_buf];
    for (unsigned i = 0; i < sizeof keep; ++i)
        keep[i] = why_buf[i];
    undo();
    for (unsigned i = 0; i < sizeof keep; ++i)
        why_buf[i] = keep[i];
    S.state = APCM_ST_FAILED;
    S.err = err;
    return err;
}

/* ---- bring-up ---------------------------------------------------------------------------------------------------- */
int apcm_up(void)
{
    uint32_t took = 0;
    if (!have_hal)
        return ABOX_ESTATE;
    if (S.state == APCM_ST_UP)
        return 0;
    if (owns_fn && owns_fn()) {
        sayw("up: refused: the call driver (Calliope) owns ABOX");
        return ABOX_ESTATE;
    }
    S.rm = (uint8_t *)H.ram(H.ctx, C.rmem_base + APCM_RM_PLAY, APCM_RM_CAP + APCM_RM_WINDOW - APCM_RM_PLAY);
    if (!S.rm) {
        sayw("up: abox_rmem's ring windows are not mappable as RAM: nothing touched");
        S.state = APCM_ST_FAILED;
        return ABOX_EMMU;
    }
    /* 2: power */
    if (!pd_on()) {
        S.powered = 1;
        m32(PMU(ABOX_PMU_AUD_CONF), 0xFU, 0xFU);
        if (!wait32(PMU(ABOX_PMU_AUD_STAT), 0xFU, 0xFU, 10000, 10, &took)) {
            sayw("up: the AUD power domain did not come on in 10 ms");
            return fail(ABOX_EPOWER);
        }
    }
    /* the DTZPC restore, also when the boot left the domain on (an earlier port makes it every time; a refusal only counts
     * when we switched the domain on ourselves) */
    int64_t smc = H.smc(H.ctx, ABOX_SMC_PD, 1, ABOX_DTZPC, 0);
    if (smc && S.powered) {
        sayw("up: the DTZPC restore SMC answered %x, not 0", UL(smc));
        return fail(ABOX_EPOWER);
    }
    if (S.powered)
        say("up: 1/6 AUD domain on in %u us, DTZPC restored", UL(took));
    else
        say("up: 1/6 AUD domain already on (the boot's; stays on at the end), DTZPC restore SMC answered %x", UL(smc));
    uint32_t ip = r32(SFR(SFR_IP_INDEX));
    if (ip != 0x41424F58U && ip != 0x584F4241U) {
        sayw("up: ABOX's IP_INDEX reads %x, not \"ABOX\": the block does not answer", UL(ip));
        return fail(ABOX_EPOWER);
    }
    /* 3: the CPU stays off in bypass */
    if ((r32(PMU(ABOX_PMU_CPU_STAT)) & 1U) || (r32(PMU(ABOX_PMU_CPU_OPT)) & (1U << 28))) {
        if (!cpu_stop()) {
            sayw("up: ABOX's CPU was on and did not power down in 10 ms");
            return fail(ABOX_ECPU);
        }
        say("up: 2/6 ABOX's CPU was on (the boot's): stopped, and it stays off (bypass)");
    } else {
        say("up: 2/6 ABOX's CPU is off and stays off (bypass)");
    }
    /* what is put back at the end */
    S.mmu_was = r32(ABOX_SYSMMU + MMU_CTRL);
    S.gpb0_con = r32(ABOX_GPIO + GPB0_CON), S.gpb0_pud = r32(ABOX_GPIO + GPB0_PUD);
    S.gpb1_con = r32(ABOX_GPIO + GPB1_CON), S.gpb1_pud = r32(ABOX_GPIO + GPB1_PUD);
    S.sysp_was = r32(SFR(APCM_SFR_SYSPOWER)), S.qdis_was = r32(SFR(APCM_SFR_QCH_DIS));
    S.route0 = r32(SFR(SFR_ROUTE_CTRL0)), S.route1 = r32(SFR(SFR_ROUTE_CTRL1));
    S.spus0 = r32(SFR(SFR_SPUS_CTRL0)), S.spus1 = r32(SFR(SFR_SPUS_CTRL1)), S.spum0 = r32(SFR(SFR_SPUM_CTRL0));
    S.uaif0[0] = r32(SFR(SFR_UAIF_CTRL0(0))), S.uaif0[1] = r32(SFR(SFR_UAIF_CTRL1(0)));
    S.uaif1[0] = r32(SFR(SFR_UAIF_CTRL0(1))), S.uaif1[1] = r32(SFR(SFR_UAIF_CTRL1(1)));
    S.sb_rdma0 = r32(SFR(APCM_SBANK_RDMA0)), S.sb_nsrc0 = r32(SFR(APCM_SBANK_NSRC0));
    S.misc_was = r32(ABOX_SYSREG + SYSREG_MISC_CON);
    S.dyn_was = r32(SFR(SFR_DYN_CLK));
    S.rd_c0 = r32(SFR(APCM_RDMA(0) + APCM_DMA_CTRL0)), S.rd_c1 = r32(SFR(APCM_RDMA(0) + APCM_DMA_CTRL1));
    S.rd_lim = r32(SFR(APCM_RDMA(0) + APCM_DMA_SBLIM));
    S.wd_c0 = r32(SFR(APCM_WDMA(1) + APCM_DMA_CTRL0)), S.wd_lim = r32(SFR(APCM_WDMA(1) + APCM_DMA_SBLIM));
    S.saved = 1;
    say("up: found RDMA0 CTRL0 %x CTRL1 %x LIM %x, WDMA1 CTRL0 %x, UAIF1 CTRL0 %x, DYN_CLOCK_OFF %x, QCH_DIS %x, "
        "SYSPOWER %x/%x", UL(S.rd_c0), UL(S.rd_c1), UL(S.rd_lim), UL(S.wd_c0), UL(S.uaif1[0]), UL(S.dyn_was),
        UL(S.qdis_was), UL(S.sysp_was), UL(r32(SFR(SFR_SYSPOWER_ST))));
    if ((r32(SFR(APCM_RDMA(0) + APCM_DMA_CTRL0)) | r32(SFR(APCM_WDMA(1) + APCM_DMA_CTRL0))) & DMA_ENABLE) {
        S.play = S.cap = 1;                 /* someone's DMA runs: stopped by undo() */
        sayw("up: RDMA0 or WDMA1 was already running (not ours): stopped again, nothing else changed");
        return fail(ABOX_ESTATE);
    }
    /* 4: clocks */
    w32(ABOX_SYSREG + SYSREG_MISC_CON, 1);
    qch_on(0);
    if (!cmu_set(CMU_DIV_BUS, 0x7U, 1U) || !cmu_set(CMU_DIV_BUSP, 0x3U, 2U) || !cmu_set(CMU_DIV_AUDIF, 0x1FFU, 15U) ||
        !cmu_set(CMU_MUX_UAIF0, 1U, 0) || !cmu_set(CMU_MUX_UAIF1, 1U, 0) || !cmu_set(CMU_DIV_UAIF0, 0x1FFU, 47U) ||
        !cmu_set(CMU_DIV_UAIF1, 0x1FFU, 47U)) {
        sayw("up: a CMU_AUD divider or mux stayed busy or did not take its value");
        return fail(ABOX_ECLOCK);
    }
    uint32_t c0 = r32(CMU(CMU_PLL_CON0));
    if ((c0 & (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL)) == (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL) &&
        (c0 & PLL_MPS_MASK) == PLL_WANT && (r32(CMU(CMU_PLL_CON3)) & 0xFFFFU) == PLL_K) {
        took = 0;
    } else {
        if (c0 & PLL_MUX_SEL) {
            m32(CMU(CMU_PLL_CON0), PLL_MUX_SEL, 0);
            if (!wait32(CMU(CMU_PLL_CON0), PLL_MUX_BUSY, 0, 1000, 10, 0)) {
                sayw("up: PLL_AUD's mux stayed busy");
                return fail(ABOX_ECLOCK);
            }
        }
        m32(CMU(CMU_PLL_CON0), PLL_ENABLE, 0);
        m32(CMU(CMU_PLL_LOCKTIME), 0xFFFFFU, PLL_LOCK);
        m32(CMU(CMU_PLL_CON3), 0xFFFFU, PLL_K);
        m32(CMU(CMU_PLL_CON0), PLL_MPS_MASK | PLL_MUX_SEL | PLL_ENABLE, PLL_WANT | PLL_MUX_SEL | PLL_ENABLE);
        if (!wait32(CMU(CMU_PLL_CON0), PLL_STABLE, PLL_STABLE, 5000, 10, &took)) {
            sayw("up: PLL_AUD not stable after 5 ms");
            return fail(ABOX_ECLOCK);
        }
    }
    uint64_t hz = pll_hz();
    if (hz < 1179000000ULL || hz > 1180300000ULL) {
        sayw("up: PLL_AUD runs at %u Hz, not the 48 kHz family's 1179648000", UL(hz));
        return fail(ABOX_ECLOCK);
    }
    qch_on(1);
    qch_on(2);
    qch_on(3);
    say("up: 3/6 clocks: PLL_AUD %u Hz (stable after %u us), AUDIF 73.728 MHz, UAIF0/1 BCLK 1.536 MHz", UL(hz),
        UL(took));
    /* 5: SysMMU through */
    if (S.mmu_was & (MMU_TRANSLATE | MMU_BLOCK)) {
        S.mmu_set = 1;
        w32(ABOX_SYSMMU + MMU_CTRL, 0);
        if (r32(ABOX_SYSMMU + MMU_CTRL) & (MMU_TRANSLATE | MMU_BLOCK)) {
            sayw("up: ABOX's SysMMU did not stop translating (CTRL %x): its addresses would not be physical",
                 UL(r32(ABOX_SYSMMU + MMU_CTRL)));
            return fail(ABOX_EMMU);
        }
    }
    say("up: 4/6 SysMMU passes addresses through (CTRL was %x, now %x)", UL(S.mmu_was), UL(r32(ABOX_SYSMMU + MMU_CTRL)));
    /* 6: pins, pads */
    S.pins_set = 1;
    m32(ABOX_GPIO + GPB1_CON, UAIF1_PINS, 0x22222222U);
    m32(ABOX_GPIO + GPB1_PUD, UAIF1_PINS, 0x11111111U);
    m32(ABOX_GPIO + GPB0_CON, UAIF0_PINS, 0x22222222U);
    m32(ABOX_GPIO + GPB0_PUD, UAIF0_PINS, 0x11111111U);
    m32(PMU(ABOX_PMU_PAD_RET), 1U << 28, 1U << 28);
    m32(PMU(ABOX_PMU_GPIO_MODE), 1U, 1U);
    /* 7, 8: the bus kept up, the UAIF formats */
    m32(SFR(APCM_SFR_SYSPOWER), 1U, 1U);
    m32(SFR(APCM_SFR_QCH_DIS), QCH_DIS_ACLK | QCH_DIS_BCLK0 | QCH_DIS_BCLK1 | C.qdis_or,
        QCH_DIS_ACLK | QCH_DIS_BCLK0 | QCH_DIS_BCLK1 | C.qdis_or);
    if (C.dyn_clk != APCM_KEEP)
        w32(SFR(SFR_DYN_CLK), C.dyn_clk);
    for (unsigned n = 0; n < 2; ++n) {
        m32(SFR(SFR_UAIF_CTRL0(n)), UAIF_SPK_EN | UAIF_MIC_EN, 0);
        w32(SFR(SFR_UAIF_CTRL1(n)), (r32(SFR(SFR_UAIF_CTRL1(n))) & ~0x1FFFFFFFU) | UAIF_CTRL1_VAL);
        m32(SFR(SFR_UAIF_CTRL0(n)), UAIF_MASTER | (n == 0 ? UAIF_DATA_MODE | UAIF_IRQ_MODE : 0),
            UAIF_MASTER | (n == 0 ? UAIF_DATA_MODE : 0));
    }
    if ((r32(SFR(SFR_UAIF_CTRL1(1))) & 0x1FFFFFFFU) != UAIF_CTRL1_VAL) {
        sayw("up: UAIF1's format did not take (CTRL1 %x)", UL(r32(SFR(SFR_UAIF_CTRL1(1)))));
        return fail(ABOX_ECLOCK);
    }
    say("up: 5/6 pins gpb1-0..3 / gpb0-1..4 to I2S, pads released; SYSPOWER 1, ACLK/BCLK Q-channels held");
    say("up: 6/6 UAIF0/1: I2S masters, 48 kHz 2 x 16 (CTRL1 0x0984f000), nothing enabled");
    S.state = APCM_ST_UP;
    S.err = 0;
    sayw("up: ABOX in bypass, ready (CPU off, no firmware)");
    return 0;
}

static uint8_t *ring(int cap) { return S.rm ? S.rm + (cap ? APCM_RM_CAP - APCM_RM_PLAY : 0) : 0; }

static void sbank(uint32_t off, const char *name)
{
    uint32_t v = r32(SFR(off));
    if ((v & SBANK_SIZE) || !C.sbank_bytes)
        return;
    m32(SFR(off), SBANK_SIZE, (C.sbank_bytes / 16U) << 20);
    say("%s's sample bank read 0 (the boot leaves it so; the DMA waits then): given %u bytes, reads %x", name,
        UL(C.sbank_bytes), UL(r32(SFR(off))));
}

/* BUF_*, STR_POINT, VOL unity if 0, BURST_LEN if 0; ENABLE stays 0. 1: the addresses read back. */
static int dma_program(uint64_t base, uint32_t pa, uint32_t bytes)
{
    m32(base + APCM_DMA_CTRL0, DMA_ENABLE, 0);
    w32(base + APCM_DMA_BUF_STR, pa);
    w32(base + APCM_DMA_BUF_END, pa + bytes);
    w32(base + APCM_DMA_BUF_OFFSET, APCM_PERIOD_BYTES);
    w32(base + APCM_DMA_STR_POINT, pa);
    if (!(r32(base + APCM_DMA_VOL_FACTOR) & VOL_MASK))
        m32(base + APCM_DMA_VOL_FACTOR, VOL_MASK, VOL_UNITY);
    uint32_t c = r32(base + APCM_DMA_CTRL0) & ~DMA_ENABLE;
    if (!(c & DMA_BURST) && C.burst)
        c |= C.burst << 19;
    w32(base + APCM_DMA_CTRL0, c | C.rdma_ctrl0_or);
    if (C.dma_ctrl1 != APCM_KEEP && base == SFR(APCM_RDMA(0)))
        w32(base + APCM_DMA_CTRL1, C.dma_ctrl1);
    if (C.dma_lim != APCM_KEEP)
        w32(base + APCM_DMA_SBLIM, C.dma_lim);
    return r32(base + APCM_DMA_BUF_STR) == pa && r32(base + APCM_DMA_BUF_END) == pa + bytes;
}

int apcm_play_start(int uaif)
{
    if (S.state != APCM_ST_UP || (uaif != APCM_UAIF0 && uaif != APCM_UAIF1)) {
        sayw("play: not up (apcm_up first) or no such UAIF");
        return ABOX_ESTATE;
    }
    if (S.play)
        return apcm_play_route(uaif);
    if (owns_fn && owns_fn()) {
        sayw("play: refused: the call driver owns ABOX");
        return ABOX_ESTATE;
    }
    uint64_t rdma = SFR(APCM_RDMA(0));
    uint32_t pa = (uint32_t)(C.rmem_base + APCM_RM_PLAY);
    memset(ring(0), 0, APCM_RM_WINDOW);
    apcm_sync();
    /* SPUS chain 0: RDMA0, no ASRC, out to SIFS1; SIFS1 from SPUS OUT0; the UAIF's speaker side from SIFS1 */
    m32(SFR(SFR_SPUS_CTRL0), 0xFU, 0);
    m32(SFR(SFR_SPUS_CTRL1), 0x7U << 16, 0);
    m32(SFR(SFR_ROUTE_CTRL0), 0xFFU, 2U << (4 * uaif));
    sbank(APCM_SBANK_RDMA0, "RDMA0");
    if (!dma_program(rdma, pa, APCM_RING_BYTES)) {
        sayw("play: RDMA0 did not take the ring's addresses");
        return fail(ABOX_EMMU);
    }
    /* Samsung's order for these links (POST_PRE): the UAIF first, then the DMA */
    m32(SFR(SFR_UAIF_CTRL0(uaif)), UAIF_SPK_EN, UAIF_SPK_EN);
    S.play = uaif + 1;
    m32(rdma + APCM_DMA_CTRL0, DMA_ENABLE, DMA_ENABLE);
    say("play: RDMA0 -> SPUS 0 -> SIFS1 -> UAIF%u, ring %u bytes at %x (periods of %u), CTRL0 %x", UL(uaif),
        UL(APCM_RING_BYTES), UL(pa), UL(APCM_PERIOD_BYTES), UL(r32(rdma + APCM_DMA_CTRL0)));
    return 0;
}

int apcm_play_route(int uaif)
{
    if (!S.play || (uaif != APCM_UAIF0 && uaif != APCM_UAIF1))
        return ABOX_ESTATE;
    int old = S.play - 1;
    if (old == uaif)
        return 0;
    m32(SFR(SFR_UAIF_CTRL0(old)), UAIF_SPK_EN, 0);
    m32(SFR(SFR_ROUTE_CTRL0), 0xFFU, 2U << (4 * uaif));
    m32(SFR(SFR_UAIF_CTRL0(uaif)), UAIF_SPK_EN, UAIF_SPK_EN);
    S.play = uaif + 1;
    say("play: moved from UAIF%u to UAIF%u", UL(old), UL(uaif));
    return 0;
}

static uint32_t pos_of(uint32_t s)
{
    return (((s >> 16) & 0x1FFFU) << 4) + (s & 0x1FFFU);   /* abox_rdma_pointer(): offset in 16 bytes + count */
}

uint32_t apcm_play_pos(uint32_t *status)
{
    if (!S.play)
        return APCM_NOPOS;
    uint32_t s = r32(SFR(APCM_RDMA(0) + APCM_DMA_STATUS)), p = pos_of(s);
    if (status)
        *status = s;
    return (s & DMA_PROGRESS) && p < APCM_RING_BYTES ? p : APCM_NOPOS;
}

uint8_t *apcm_play_buf(void) { return S.state == APCM_ST_UP ? ring(0) : 0; }

void apcm_play_stop(void)
{
    if (!S.play)
        return;
    int uaif = S.play - 1;
    int ok = dma_stop(SFR(APCM_RDMA(0)), "RDMA0");
    H.delay_us(H.ctx, FLUSH_US);
    m32(SFR(SFR_UAIF_CTRL0(uaif)), UAIF_SPK_EN, 0);
    m32(SFR(SFR_ROUTE_CTRL0), 0xFFU, S.route0);
    if (!ok) {
        /* the engine may still read the ring: the domain goes down with it, so nothing is left reading */
        S.state = APCM_ST_UP;
        (void)fail(ABOX_EFAULT);
        return;
    }
    S.play = 0;
    say("play: stopped (RDMA0, then UAIF%u)", UL(uaif));
}

int apcm_cap_start(void)
{
    if (S.state != APCM_ST_UP) {
        sayw("capture: not up (apcm_up first)");
        return ABOX_ESTATE;
    }
    if (S.cap)
        return 0;
    if (owns_fn && owns_fn()) {
        sayw("capture: refused: the call driver owns ABOX");
        return ABOX_ESTATE;
    }
    uint64_t wdma = SFR(APCM_WDMA(1));
    uint32_t pa = (uint32_t)(C.rmem_base + APCM_RM_CAP), took = 0;
    memset(ring(1), 0, APCM_RM_WINDOW);
    apcm_sync();
    /* UAIF0 -> NSRC0 -> SPUM (no ASRC) -> SIFM0 -> WDMA1 (SIFMn feeds WDMA n+1, as the call's SIFM1 -> WDMA2) */
    m32(SFR(SFR_ROUTE_CTRL1), 0xFU, 8U);
    m32(SFR(SFR_SPUM_CTRL0), (1U << 4) | (1U << 7), 0);
    m32(SFR(SFR_SPUM_CTRL3), 1U, 1U);
    sbank(APCM_SBANK_NSRC0, "NSRC0");
    if (!dma_program(wdma, pa, APCM_RING_BYTES)) {
        sayw("capture: WDMA1 did not take the ring's addresses");
        return fail(ABOX_EMMU);
    }
    /* capture (PRE_POST): the DMA first, then the UAIF */
    m32(wdma + APCM_DMA_CTRL0, DMA_ENABLE, DMA_ENABLE);
    S.cap = 1;
    m32(SFR(SFR_UAIF_CTRL0(0)), UAIF_MIC_EN, UAIF_MIC_EN);
    int started = wait32(wdma + APCM_DMA_STATUS, DMA_PROGRESS, DMA_PROGRESS, 2000, 50, &took);
    say("capture: UAIF0 MIC -> NSRC0 -> SIFM0 -> WDMA1, ring %u bytes at %x; %s (STATUS %x, UAIF0 STATUS %x)",
        UL(APCM_RING_BYTES), UL(pa), started ? "WDMA1 in progress" : "WDMA1 not in progress after 2 ms",
        UL(r32(wdma + APCM_DMA_STATUS)), UL(r32(SFR(SFR_UAIF_STATUS(0)))));
    return 0;
}

uint32_t apcm_cap_pos(uint32_t *status)
{
    if (!S.cap)
        return APCM_NOPOS;
    uint32_t s = r32(SFR(APCM_WDMA(1) + APCM_DMA_STATUS)), p = pos_of(s);
    if (status)
        *status = s;
    return (s & DMA_PROGRESS) && p < APCM_RING_BYTES ? p : APCM_NOPOS;
}

const uint8_t *apcm_cap_buf(void) { return S.state == APCM_ST_UP ? ring(1) : 0; }

void apcm_cap_stop(void)
{
    if (!S.cap)
        return;
    m32(SFR(SFR_UAIF_CTRL0(0)), UAIF_MIC_EN, 0);
    int ok = dma_stop(SFR(APCM_WDMA(1)), "WDMA1");
    m32(SFR(SFR_ROUTE_CTRL1), 0xFU, S.route1);
    if (!ok) {
        (void)fail(ABOX_EFAULT);
        return;
    }
    S.cap = 0;
    say("capture: stopped (UAIF0 MIC, then WDMA1)");
}

void apcm_forget(void)
{
    if (S.state == APCM_ST_OFF && !S.saved && !S.powered)
        return;
    memset(&S, 0, sizeof(S));
    S.state = APCM_ST_OFF;
    sayw("forgotten: the call driver took ABOX while media sound had it (nothing written)");
}

uint32_t apcm_uaif_status(int uaif)
{
    if (S.state != APCM_ST_UP || (uaif != 0 && uaif != 1))
        return 0;
    return r32(SFR(SFR_UAIF_STATUS(uaif)));
}

void apcm_down(void)
{
    if (!have_hal)
        return;
    if (S.state == APCM_ST_OFF && !S.powered && !S.saved)
        return;
    undo();
    S.state = APCM_ST_OFF;
    S.err = 0;
    sayw("off");
}

/* ---- diagnostics ------------------------------------------------------------------------------------------------- */
void apcm_dump(const char *tag)
{
    if (!have_hal)
        return;
    uint32_t a = r32(PMU(ABOX_PMU_AUD_STAT));
    if ((a & 0xFU) != 0xFU) {
        say("dump %s: AUD domain off (%x): nothing else read", tag, UL(a));
        return;
    }
#define RS(o) UL(r32(SFR(o)))
#define RC(o) UL(r32(CMU(o)))
    say("dump %s: PMU AUD %x CPU %x/%x PADRET %x GPIOM %x | SYSPOWER %x/%x CFG0 %x REMAP %x/%x DYNCLK %x QCHDIS %x",
        tag, UL(a), UL(r32(PMU(ABOX_PMU_CPU_STAT))), UL(r32(PMU(ABOX_PMU_CPU_OPT))), UL(r32(PMU(ABOX_PMU_PAD_RET))),
        UL(r32(PMU(ABOX_PMU_GPIO_MODE))), RS(APCM_SFR_SYSPOWER), RS(SFR_SYSPOWER_ST), RS(SFR_SYS_CONFIG0),
        RS(SFR_REMAP_MASK), RS(SFR_REMAP_ADDR), RS(SFR_DYN_CLK), RS(APCM_SFR_QCH_DIS));
    say("dump %s: CMU PLL %x/%x LOCK %x MUX cpu %x u0 %x u1 %x DIV audif %x bus %x busp %x cpuaclk %x pll %x u0 %x u1 %x",
        tag, RC(CMU_PLL_CON0), RC(CMU_PLL_CON3), RC(CMU_PLL_LOCKTIME), RC(CMU_MUX_CPU), RC(CMU_MUX_UAIF0),
        RC(CMU_MUX_UAIF1), RC(CMU_DIV_AUDIF), RC(CMU_DIV_BUS), RC(CMU_DIV_BUSP), RC(CMU_DIV_CPU_ACLK), RC(CMU_DIV_PLL),
        RC(CMU_DIV_UAIF0), RC(CMU_DIV_UAIF1));
    say("dump %s: CMU QCH 3000.. %x %x %x %x %x %x %x %x | 3020.. %x %x %x %x | 3060 %x | MMU %x %x %x int %x fa %x",
        tag, RC(0x3000), RC(0x3004), RC(0x3008), RC(0x300C), RC(0x3010), RC(0x3014), RC(0x3018), RC(0x301C), RC(0x3020),
        RC(0x3024), RC(0x3028), RC(0x302C), RC(0x3060), UL(r32(ABOX_SYSMMU + MMU_CTRL)), UL(r32(ABOX_SYSMMU + MMU_CFG)),
        UL(r32(ABOX_SYSMMU + MMU_STATUS)), UL(r32(ABOX_SYSMMU + MMU_INT_STATUS)), UL(r32(ABOX_SYSMMU + MMU_FAULT_ADDR)));
    say("dump %s: ROUTE %x %x %x SPUS %x %x %x %x SBANK r0 %x mixp %x SPUM %x %x %x %x nsrc0 %x", tag,
        RS(SFR_ROUTE_CTRL0), RS(SFR_ROUTE_CTRL1), RS(SFR_ROUTE_CTRL2), RS(SFR_SPUS_CTRL0), RS(SFR_SPUS_CTRL1),
        RS(SFR_SPUS_CTRL2), RS(SFR_SPUS_CTRL3), RS(APCM_SBANK_RDMA0), RS(SFR_SBANK_MIXP), RS(SFR_SPUM_CTRL0),
        RS(SFR_SPUM_CTRL1), RS(0x308), RS(SFR_SPUM_CTRL3), RS(APCM_SBANK_NSRC0));
    say("dump %s: UAIF0 %x %x st %x | UAIF1 %x %x st %x | GPB0 %x %x GPB1 %x %x", tag, RS(SFR_UAIF_CTRL0(0)),
        RS(SFR_UAIF_CTRL1(0)), RS(SFR_UAIF_STATUS(0)), RS(SFR_UAIF_CTRL0(1)), RS(SFR_UAIF_CTRL1(1)),
        RS(SFR_UAIF_STATUS(1)), UL(r32(ABOX_GPIO + GPB0_CON)), UL(r32(ABOX_GPIO + GPB0_PUD)),
        UL(r32(ABOX_GPIO + GPB1_CON)), UL(r32(ABOX_GPIO + GPB1_PUD)));
    for (unsigned d = 0; d < 2; ++d) {
        uint32_t b = d ? APCM_WDMA(1) : APCM_RDMA(0);
        say("dump %s: %s c0 %x c1 %x str %x end %x off %x pt %x vol %x chg %x lim %x 24 %x 28 %x 2c %x st %x", tag,
            d ? "WDMA1" : "RDMA0", RS(b), RS(b + 4), RS(b + 8), RS(b + 0xC), RS(b + 0x10), RS(b + 0x14), RS(b + 0x18),
            RS(b + 0x1C), RS(b + 0x20), RS(b + 0x24), RS(b + 0x28), RS(b + 0x2C), RS(b + 0x30));
    }
#undef RS
#undef RC
}

static void fmt_desc(char *d, unsigned cap, const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, fmt, ap);
    va_end(ap);
    unsigned i = 0;
    for (const char *s = b + 6; *s && i + 1 < cap; ++s)
        d[i++] = *s;
    if (cap)
        d[i] = 0;
}

/* trial i -> what to change; 0: no such trial */
struct tcfg {
    uint32_t c0_or, c1, lim, dyn, qdis;
    int set_c1, set_lim, set_dyn, set_qdis;
};
static int trial_cfg(unsigned i, struct tcfg *t, char *desc, unsigned cap)
{
    memset(t, 0, sizeof(*t));
    if (i == 0) {
        fmt_desc(desc, cap, "baseline again");
    } else if (i == 1) {
        t->set_dyn = 1, t->dyn = 0;
        fmt_desc(desc, cap, "DYN_CLOCK_OFF 0");
    } else if (i == 2) {
        t->set_qdis = 1, t->qdis = 0xFFFFFFFFU;
        fmt_desc(desc, cap, "QCHANNEL_DISABLE all");
    } else if (i <= 6) {
        t->set_lim = 1, t->lim = 4U << (8 * (i - 3));
        fmt_desc(desc, cap, "SBANK_LIMIT %x", UL(t->lim));
    } else if (i <= 33) {
        unsigned k = i - 7, bit = k < 18 ? k + 1 : k - 18 + 23;
        t->c0_or = 1U << bit;
        fmt_desc(desc, cap, "CTRL0 bit %u", UL(bit));
    } else if (i <= 65) {
        t->set_c1 = 1, t->c1 = 1U << (i - 34);
        fmt_desc(desc, cap, "CTRL1 bit %u", UL(i - 34));
    } else if (i < APCM_TRIAL_FIRST_FMT) {
        t->set_c1 = 1, t->c1 = 9U << (i - 66);
        fmt_desc(desc, cap, "CTRL1 9<<%u", UL(i - 66));
    } else {
        unsigned j = i - APCM_TRIAL_FIRST_FMT;
        for (unsigned s = 1; s <= 14; ++s)
            for (unsigned b = 1; b <= 18; ++b) {
                if (b >= s && b <= s + 4)
                    continue;
                if (j-- == 0) {
                    t->c0_or = (9U << s) | (1U << b);
                    fmt_desc(desc, cap, "CTRL0 9<<%u + bit %u", UL(s), UL(b));
                    return 1;
                }
            }
        return 0;
    }
    return 1;
}

int apcm_trial(unsigned i, uint32_t *moved, char *desc, unsigned cap)
{
    struct tcfg t;
    uint64_t rdma = SFR(APCM_RDMA(0));
    *moved = 0;
    if (!S.play || S.state != APCM_ST_UP)
        return 0;
    if (!trial_cfg(i, &t, desc, cap))
        return 0;
    if (!dma_stop(rdma, "RDMA0"))
        return -1;
    uint32_t qd = r32(SFR(APCM_SFR_QCH_DIS));
    (void)dma_program(rdma, (uint32_t)(C.rmem_base + APCM_RM_PLAY), APCM_RING_BYTES);
    if (t.c0_or)
        m32(rdma + APCM_DMA_CTRL0, t.c0_or, t.c0_or);
    if (t.set_c1)
        w32(rdma + APCM_DMA_CTRL1, t.c1);
    if (t.set_lim)
        w32(rdma + APCM_DMA_SBLIM, t.lim);
    if (t.set_dyn)
        w32(SFR(SFR_DYN_CLK), t.dyn);
    if (t.set_qdis)
        w32(SFR(APCM_SFR_QCH_DIS), t.qdis);
    m32(rdma + APCM_DMA_CTRL0, DMA_ENABLE, DMA_ENABLE);
    H.delay_us(H.ctx, 3000);
    uint32_t p1 = pos_of(r32(rdma + APCM_DMA_STATUS)) % APCM_RING_BYTES;
    H.delay_us(H.ctx, 5000);
    uint32_t p2 = pos_of(r32(rdma + APCM_DMA_STATUS)) % APCM_RING_BYTES;
    *moved = (p2 + APCM_RING_BYTES - p1) % APCM_RING_BYTES;
    int ok = dma_stop(rdma, "RDMA0");
    /* what the trial changed, back as it was while playing */
    w32(rdma + APCM_DMA_CTRL0, r32(rdma + APCM_DMA_CTRL0) & ~(t.c0_or | DMA_ENABLE));
    w32(rdma + APCM_DMA_CTRL1, S.rd_c1);
    w32(rdma + APCM_DMA_SBLIM, S.rd_lim);
    w32(SFR(SFR_DYN_CLK), S.dyn_was);
    w32(SFR(APCM_SFR_QCH_DIS), qd);
    return ok ? 1 : -1;
}

int apcm_trial_adopt(unsigned i)
{
    struct tcfg t;
    char d[48];
    if (!trial_cfg(i, &t, d, sizeof d))
        return 0;
    C.rdma_ctrl0_or = t.c0_or;
    C.dma_ctrl1 = t.set_c1 ? t.c1 : APCM_KEEP;
    C.dma_lim = t.set_lim ? t.lim : APCM_KEEP;
    C.dyn_clk = t.set_dyn ? t.dyn : APCM_KEEP;
    C.qdis_or = t.set_qdis ? t.qdis : 0;
    say("config: trial %u (%s) adopted for RDMA0 and WDMA1", UL(i), d);
    return 1;
}
