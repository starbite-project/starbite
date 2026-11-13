/* SPDX-License-Identifier: GPL-2.0-only
 * libsb copy of shim/starlte/sd.c: no CMU or PMIC writes (see sd_up). A polling, read-only driver for the Exynos 9810's
 * card controller (dwmmc2, DesignWare MSHC 2.70a at 0x11500000, Exynos7 type: CLKSEL at 0xa8,
 * 64-bit FIFO at 0x200), after what mainline Linux needed on this phone (NOTES.md 2026-10-27):
 *
 *   clocks   CMU gates opened by hand (bit 20 manual, bit 21 on, as s9p-clkgate.c): CMU top
 *            GATE_CLKCMU_FSYS1_MMC_CARD 0x1a242070, FSYS1 ACLK 0x11402030 and SDCLKIN 0x11402034.
 *            PLL_MMC (~806 MHz) / (DIV_CLKCMU_FSYS1_MMC_CARD 0x1a241864 + 1) / (CLKSEL div 3 + 1)
 *            = the controller's bus clock (~200 MHz when the CMU divider is /1, as Linux sets it;
 *            the loader leaves the divider alone and computes with it). DIVRATIO is bits 8:0
 *            (Samsung cmucal-sfr.c); S-Boot leaves 15 (/16: a 12.5 MHz bus, 6.25 MHz card clock,
 *            XNU 0347 and drivers run 11). libsb never writes it: Starbite OS's kernel may, through
 *            sd_set_bus_hook (kern/sdclk.c), with the card clock stopped, and then says the bus clock
 *            it read off the PLL (sd_rep.bus_set)
 *   pins    GPF1-0..5 function 2 (pinctrl_fsys1 0x11430000), no pulls, drive 4 (clock) / 3
 *   card     detect GPA1-5 active low; rails S2MPS18 LDO31 2.8 V + LDO2 3.3 V (read only here,
 *            which power-cycles the card), 3.3 V signalling only: no UHS
 *   TMOUT    always ~0: with the default every read ended in a data read timeout
 *
 * Init at 400 kHz: CMD0, CMD8, ACMD41 until ready, CMD2, CMD3, CMD7, ACMD6 (4 bit), then 25 MHz.
 * Reads: CMD17 / CMD18 with auto stop, the FIFO emptied by the CPU (no DMA, no interrupts), every
 * wait bounded by the generic timer. A failed read is followed by sd_recover() (controller + FIFO
 * reset, clock reload, CMD12 with stop_abort, CMD13 until tran; Linux dw_mci_reset +
 * send_stop_abort) before the retry, so no error leaves the controller inside the CMD18. CLKSEL is
 * changed in its timing fields only (Linux dw_mci_exynos_set_clksel_timing); a CMU gate that did
 * not open stops the bring-up before the controller is read.
 *
 * Writes (Engineer OS v2): sd_write / sd_write4k, CMD24 / CMD25 with auto stop, the FIFO filled
 * by the CPU while it is not full, then DTO, ACD, the busy line and CMD13 until tran, every wait
 * bounded. They refuse (-9) before touching the controller unless the write fence is active
 * (fence.c sb_write, after the user's confirmation for that range). Not yet run on the phone.
 *
 * Fast reads (opt-in, sd_set_fast; Starbite OS's kernel turns them on, every other image keeps the
 * path above). Read only: the only new commands are CMD9 (CSD, at init), ACMD51 (SCR), CMD6 and
 * CMD18; CMD6 mode 1 always carries 0x80fffff1 (function group 1 = high speed, every other group
 * 0xf = no change), and only after the mode 0 query 0x00fffff1 said the card offers it.
 *   high speed  Linux core/sd.c mmc_read_switch / mmc_sd_switch_hs: CSD class 10 and SCR SD_SPEC >= 1,
 *               the query's group 1 support byte 13 bit 1 and result nibble (byte 16) == 1, then the
 *               switch, whose result nibble must read 1; then the card clock. The DT's dwmmc2 node
 *               (build/linux-bundle/lts.dts: samsung,dw-mshc-sdr-timing <3 0 2 0> = ciu div 3, drive 2,
 *               sample 0; ciu-div 3; clock-frequency 800 MHz; ciu_clkin 50 MHz for SD HS) gives the
 *               CLKSEL timing init already writes: Linux dw_mmc-exynos.c uses sdr_timing for every
 *               timing but DDR / HS400, so SD HS keeps CLKSEL 0x03020000 (timing fields). The clock:
 *               PLL_MMC 806 MHz / CMU /1 / CLKSEL /4 = 201.5 MHz; CLKDIV 2 gives 50.375 MHz, over the
 *               50 MHz HS limit (what Linux, XNU 0348 and stock Samsung, at CMU /4 and CLKDIV bypass,
 *               actually run), so SD_FAST_ON takes the highest exact rate <= 50 MHz of the bus clock
 *               it has: CLKDIV 3 = 33.58 MHz from that estimate, CLKDIV 2 = 50.0 MHz from the 200.0 MHz
 *               kern/sdclk.c reads off PLL_MMC (799.99988 MHz). SD_FAST_HS50 takes CLKDIV 2. libsb
 *               never writes the CMU divider.
 *   IDMAC       CMD18 (2..256 sectors) through the controller's internal DMA in 64-bit address mode
 *               (HCON bits 17:16 = 0 IDMAC, bit 27 = 1 64-bit; Linux dw_mmc.c dw_mci_idmac_init /
 *               dw_mci_prepare_desc64 / dw_mci_idmac_start_dma): chained 32-byte descriptors of 4 KiB,
 *               FD on the first, LD (no CH, no DIC) on the last; IDSTS64 0x90 cleared, IDINTEN64 0x94
 *               NI|RI|TI, DBADDRL/U 0x88/0x8c, FIFOTH MSIZE 32 (dw_mci_adjust_fifoth for a 64-deep
 *               8-byte FIFO), CTRL USE_IDMAC|DMA_ENABLE, BMOD ENABLE|FB, PLDMND. Polled: DTO and IDSTS
 *               RI, then ACD, then every descriptor's OWN clear. The descriptors are in the image's
 *               write-back RAM, cleaned before the start; a write-back destination is cleaned and
 *               invalidated before and after (as ufs_read_to). DMA only into 64-byte aligned RAM below
 *               4 GiB (Linux needed starbite,dma-32bit, NOTES.md 2026-10-27), else that read is PIO.
 *   fallbacks   sd_set_fast checks both on sectors 0-7 against the old path; any failure there puts
 *               the card clock back to 25 MHz, PIO, with one log line. A chunk whose IDMAC read fails
 *               is stopped, recovered (sd_recover) and read again on the PIO path; three such
 *               failures turn fast reads off. Writes run at the old clock with the old code.
 */
#include <stdint.h>
#include "sd.h"
#include "sb.h"

#define SD        0x11500000UL
#define CTRL      0x00
#define PWREN     0x04
#define CLKDIV    0x08
#define CLKSRC    0x0c
#define CLKENA    0x10
#define TMOUT     0x14
#define CTYPE     0x18
#define BLKSIZ    0x1c
#define BYTCNT    0x20
#define INTMASK   0x24
#define CMDARG    0x28
#define CMD       0x2c
#define RESP0     0x30
#define RINTSTS   0x44
#define STATUS    0x48
#define FIFOTH    0x4c
#define VERID     0x6c
#define HCON      0x70
#define BMOD      0x80
#define CLKSEL64  0xa8
#define DATA      0x200

#define C_START    (1U << 31)
#define C_HOLD     (1U << 29)
#define C_UPD_CLK  (1U << 21)
#define C_INIT     (1U << 15)
#define C_STOP     (1U << 14)           /* stop_abort_cmd (Linux SDMMC_CMD_STOP) */
#define C_PRV_WAIT (1U << 13)
#define C_AUTOSTOP (1U << 12)
#define C_WRITE    (1U << 10)
#define C_DATA     (1U << 9)
#define C_CRC      (1U << 8)
#define C_LONG     (1U << 7)
#define C_RESP     (1U << 6)
#define R1         (C_RESP | C_CRC)

#define I_RE   (1U << 1)
#define I_CD   (1U << 2)
#define I_DTO  (1U << 3)
#define I_RCRC (1U << 6)
#define I_DCRC (1U << 7)
#define I_RTO  (1U << 8)
#define I_DRTO (1U << 9)
#define I_HTO  (1U << 10)
#define I_FRUN (1U << 11)
#define I_HLE  (1U << 12)
#define I_SBE  (1U << 13)
#define I_ACD  (1U << 14)
#define I_EBE  (1U << 15)
#define I_DERR (I_DCRC | I_DRTO | I_HTO | I_FRUN | I_SBE | I_EBE)

#define PLL_MMC_HZ 806000000ULL
#define PINCTRL_FSYS1 0x11430000UL
#define ALIVE_GPA1_DAT 0x14050044UL

struct sd_report sd_rep;
static int allowed;
static sd_bus_fn bus_hook;                  /* sd_set_bus_hook: the kernel's CMU divider change, 0 none */
static uint64_t bus_fixed;                  /* the bus clock the hook said, 0 = the estimate from the divider */
static int fast_want;                       /* sd_set_fast: SD_FAST_OFF / SD_FAST_ON / SD_FAST_HS50 */
static int fast_try(void);

void sd_allow(void)
{
    allowed = 1;
}

int sd_allowed(void)
{
    return allowed;
}

#ifdef SD_HOST_FAKE                        /* host test (sbos/kern/host/sd-test.c): a fake register file */
uint32_t sd_fake_rd(uintptr_t off);
void sd_fake_wr(uintptr_t off, uint32_t v);
uint64_t sd_fake_fifo_rd(void);
void sd_fake_fifo_wr(uint64_t v);
uint32_t sd_fake_mrd(uintptr_t a);
void sd_fake_mwr(uintptr_t a, uint32_t v);
static uint32_t rd(uintptr_t off) { return sd_fake_rd(off); }
static void wr(uintptr_t off, uint32_t v) { sd_fake_wr(off, v); }
static uint32_t mrd(uintptr_t a) { return sd_fake_mrd(a); }
static void mwr(uintptr_t a, uint32_t v) { sd_fake_mwr(a, v); }
static uint64_t fifo_rd(void) { return sd_fake_fifo_rd(); }
static void fifo_wr(uint64_t v) { sd_fake_fifo_wr(v); }
#define DMA_BARRIER() __asm__ volatile("" : : : "memory")
#else
static uint32_t rd(uintptr_t off)
{
    uint32_t v = *(volatile uint32_t *)(SD + off);
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t off, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)(SD + off) = v;
}

static uint32_t mrd(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}

static void mwr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static uint64_t fifo_rd(void)
{
    return *(volatile uint64_t *)(SD + DATA);
}

static void fifo_wr(uint64_t v)
{
    *(volatile uint64_t *)(SD + DATA) = v;
}
#define DMA_BARRIER() __asm__ volatile("dsb sy" : : : "memory")
#endif

/* Deadline helpers on the generic timer. */
static uint64_t deadline(unsigned ms)
{
    return sb_ticks() + sb_freq() / 1000U * ms;
}

static int expired(uint64_t t)
{
    return sb_ticks() > t;
}

/* Wait until (reg & mask) == want; 0 or -1 on timeout. */
static int wait_reg(uintptr_t off, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t t = deadline(ms);
    while ((rd(off) & mask) != want)
        if (expired(t))
            return -1;
    return 0;
}

/* prv_wait: after the last data transfer (clock changes); without it right after a controller
 * reset, as Linux dw_mci_reset's SDMMC_CMD_UPD_CLK. */
static int update_clock(int prv_wait)
{
    wr(CMDARG, 0);
    wr(CMD, C_START | C_HOLD | C_UPD_CLK | (prv_wait ? C_PRV_WAIT : 0));
    return wait_reg(CMD, C_START, 0, 100);
}

/* CLKDIV div: the card clock is bus_hz / (2 div), bus_hz at div 0 (bypass). */
static int set_div(uint64_t div)
{
    wr(CLKENA, 0);
    if (update_clock(1))
        return -1;
    wr(CLKDIV, (uint32_t)div);
    wr(CLKSRC, 0);
    if (update_clock(1))
        return -1;
    wr(CLKENA, 1);
    if (update_clock(1))
        return -1;
    sd_rep.card_hz = div ? sd_rep.bus_hz / (2 * div) : sd_rep.bus_hz;
    return 0;
}

/* The fastest card clock <= hz (round-up division, as always). */
static int set_clock(uint64_t hz)
{
    uint64_t div = (sd_rep.bus_hz + 2 * hz - 1) / (2 * hz);
    if (div > 255)
        div = 255;
    return set_div(div);
}

/* One command; 0, -1 controller stuck, -2 no response, -3 response CRC, -4 response error.
 * A stop (C_STOP) goes out at once, as Linux sends it: no busy wait, no PRV_WAIT. */
static int cmd(unsigned idx, uint32_t arg, uint32_t flags)
{
    int stop = (flags & C_STOP) != 0;
    if (!stop && wait_reg(STATUS, 1U << 9, 0, 500))   /* card busy (R1b of the last command) */
        return -1;
    wr(RINTSTS, 0xffffffffU);
    wr(CMDARG, arg);
    wr(CMD, C_START | C_HOLD | (stop ? 0 : C_PRV_WAIT) | flags | idx);
    if (wait_reg(CMD, C_START, 0, 100))
        return -1;
    uint64_t t = deadline(500);
    uint32_t s;
    while (!((s = rd(RINTSTS)) & (I_CD | I_HLE)))
        if (expired(t))
            return -1;
    sd_rep.last_rintsts = s;
    if (s & I_HLE)
        return -1;
    if (s & I_RTO)
        return -2;
    if ((flags & C_CRC) && (s & I_RCRC))
        return -3;
    if (s & I_RE)
        return -4;
    return 0;
}

static int fail(const char *why)
{
    sd_rep.why = why;
    sd_rep.state = -1;
    sd_rep.last_status = rd(STATUS);
    return 0;
}

static int acmd(unsigned idx, uint32_t arg, uint32_t flags)
{
    int r = cmd(55, sd_rep.rca << 16, R1);
    return r ? r : cmd(idx, arg, flags);
}

void sd_set_bus_hook(sd_bus_fn fn)
{
    bus_hook = fn;
}

/* PLL_MMC_HZ / (DIVRATIO + 1) / 4, or what the hook measured */
static void bus_from_cmu(void)
{
    sd_rep.cmu_div = mrd(0x1a241864UL);
    sd_rep.bus_hz = bus_fixed ? bus_fixed : PLL_MMC_HZ / ((sd_rep.cmu_div & 0x1ffU) + 1U) / 4U;
}

/* The hook's answer: hz the bus clock now (0 = unknown: the estimate); the divider read again for the report. */
static void bus_took(uint64_t hz)
{
    bus_fixed = hz;
    bus_from_cmu();
}

static int fast_fail(const char *why);
static int hs_clock(int mode);

static int up_once(void)
{
    sd_rep.state = -1;
    sd_rep.present = !(mrd(ALIVE_GPA1_DAT) & (1U << 5));
    if (!sd_rep.present) {
        sd_rep.why = "no card (detect GPA1-5 high)";
        return 0;
    }
    /* libsb: the CMU gates (CMU_TOP 0x1a242070, CMU_FSYS1 0x11402030/34) are only read, never
     * written, and the card rails are only read: both are what the loader left when it read the
     * card's store at boot ("Card store at boot"). Without them the card is not touched. */
    for (unsigned i = 0; i < 3; ++i) {
        uintptr_t a = i == 0 ? 0x1a242070UL : i == 1 ? 0x11402030UL : 0x11402034UL;
        sd_rep.gate[i] = mrd(a);
    }
    bus_from_cmu();
    if (!sd_rep.bus_set) {
        sd_rep.cmu_div_was = sd_rep.cmu_div;
        sd_rep.bus_hz_was = sd_rep.bus_hz;
    }
    for (unsigned i = 0; i < 3; ++i)               /* an unclocked controller can stall the bus */
        if ((sd_rep.gate[i] & (3U << 20)) != (3U << 20)) {
            sd_rep.why = "card clocks off (Engineer OS does not write CMU blocks)";
            return 0;
        }
    sd_rep.rails = sb_sd_rails(&sd_rep.l31, &sd_rep.l2);
    if (sd_rep.rails <= 0) {
        sd_rep.why = sd_rep.rails < 0 ? "PMIC (ACPM) did not answer" : "card rails off (Tier 1: Storage > microSD)";
        return 0;
    }
    mwr(PINCTRL_FSYS1 + 0x0, (mrd(PINCTRL_FSYS1 + 0x0) & 0xff000000U) | 0x00222222U);
    mwr(PINCTRL_FSYS1 + 0x8, mrd(PINCTRL_FSYS1 + 0x8) & 0xff000000U);
    mwr(PINCTRL_FSYS1 + 0xc, (mrd(PINCTRL_FSYS1 + 0xc) & 0xff000000U) | 0x00333334U);
    sb_mdelay(10);
    sd_rep.verid = rd(VERID);
    sd_rep.hcon = rd(HCON);
    sd_rep.clksel_was = rd(CLKSEL64);
    if ((sd_rep.verid & 0xffffU) != 0x270aU)
        return fail("controller not answering (VERID)");
    if (((sd_rep.hcon >> 7) & 7U) != 2U)
        return fail("FIFO is not 64 bit (HCON)");

    wr(CTRL, rd(CTRL) | 7U);                   /* controller, FIFO and DMA reset */
    if (wait_reg(CTRL, 7U, 0, 100))
        return fail("controller reset");
    wr(BMOD, 0);                               /* no internal DMA */
    wr(CTRL, rd(CTRL) & ~((1U << 25) | (1U << 4)));   /* no IDMAC, no interrupts */
    wr(INTMASK, 0);
    wr(RINTSTS, 0xffffffffU);
    wr(TMOUT, 0xffffffffU);
    wr(FIFOTH, (2U << 28) | (31U << 16) | 32U);
    /* ciu div 3, drive phase 2, sample phase 0; the bits outside SDMMC_CLKSEL_TIMING_MASK stay */
    wr(CLKSEL64, (rd(CLKSEL64) & ~0x07070007U) | (3U << 24) | (2U << 16));
    sd_rep.clksel = rd(CLKSEL64);
    if (bus_hook && !sd_rep.bus_set) {         /* the kernel's CMU divider, once, with the card clock stopped */
        wr(CLKENA, 0);
        if (update_clock(0)) {
            sd_rep.bus_set = -2;
        } else {
            uint64_t hz = bus_hook(0);
            bus_took(hz);
            sd_rep.bus_set = !hz ? -3 : sd_rep.cmu_div != sd_rep.cmu_div_was ? 1 : 2;
        }
    }
    wr(CTYPE, 0);
    wr(PWREN, 1);
    sb_mdelay(5);
    if (set_clock(400000))
        return fail("clock update (400 kHz)");
    sb_mdelay(2);

    sd_rep.rca = 0;
    if (cmd(0, 0, C_INIT))
        return fail("CMD0 (go idle)");
    int r = cmd(8, 0x1aa, R1);
    sd_rep.r7 = r ? 0 : rd(RESP0);
    if (r || (sd_rep.r7 & 0xfffU) != 0x1aaU)
        return fail("CMD8 (not an SD 2.0 card at 3.3 V)");
    uint64_t t = deadline(1500);
    for (;;) {
        r = acmd(41, 0x40ff8000U, C_RESP);     /* HCS, 2.7-3.6 V; R3 has no CRC */
        sd_rep.ocr = r ? 0 : rd(RESP0);
        if (!r && (sd_rep.ocr & (1U << 31)))
            break;
        if (expired(t))
            return fail("ACMD41 (card never ready)");
        sb_mdelay(10);
    }
    sd_rep.high_capacity = (sd_rep.ocr >> 30) & 1U;
    if (cmd(2, 0, C_RESP | C_LONG | C_CRC))
        return fail("CMD2 (CID)");
    for (unsigned i = 0; i < 4; ++i)
        sd_rep.cid[i] = rd(RESP0 + 4 * i);
    if (cmd(3, 0, R1))
        return fail("CMD3 (address)");
    sd_rep.rca = rd(RESP0) >> 16;
    if (fast_want && !cmd(9, sd_rep.rca << 16, C_RESP | C_LONG | C_CRC)) {   /* CSD (stand-by only); fast mode only */
        for (unsigned i = 0; i < 4; ++i)
            sd_rep.csd[i] = rd(RESP0 + 4 * i);
        sd_rep.csd_ok = 1;
    }
    if (cmd(7, sd_rep.rca << 16, R1))
        return fail("CMD7 (select)");
    if (!sd_rep.high_capacity && cmd(16, 512, R1))
        return fail("CMD16 (block length)");
    if (acmd(6, 2, R1))
        return fail("ACMD6 (4-bit bus)");
    wr(CTYPE, 1);
    if (set_clock(25000000))
        return fail("clock update (25 MHz)");
    wr(BLKSIZ, 512);
    sd_rep.why = 0;
    sd_rep.state = 1;
    if (fast_want)
        (void)fast_try();                      /* any failure: back to 25 MHz PIO, one log line */
    return 1;
}

int sd_up(void)
{
    if (sd_rep.state)
        return sd_rep.state > 0;
    if (!allowed)
        return 0;
    if (up_once())
        return 1;
    if (sd_rep.bus_set != 1)
        return 0;
    /* The card did not come up on the bus clock the hook set: the old divider back (card clock stopped), and once
     * more from it, as before the hook existed. One log line. */
    const char *why = sd_rep.why;
    wr(CLKENA, 0);
    (void)update_clock(0);
    bus_took(bus_hook(1));
    sd_rep.bus_set = -1;
    sd_rep.bus_why = why;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "sd: the card failed on the raised bus clock (");
    ub_s(&u, why ? why : "?");
    ub_s(&u, "): CMU divider put back, bus ");
    ub_dec(&u, sd_rep.bus_hz / 1000U);
    ub_s(&u, " kHz, once more");
    sb_log(u.b);
    return up_once();
}

int sd_rebus(sd_bus_fn fn, int restore)
{
    if (sd_rep.state != 1 || !fn)
        return -1;
    wr(CLKENA, 0);                             /* between commands: nothing runs on the card */
    if (update_clock(1)) {
        if (sd_rep.fast)
            (void)fast_fail("the card clock did not stop for a bus clock change");
        else
            (void)set_clock(25000000);
        return -1;
    }
    uint64_t hz = fn(restore);                 /* restore: the hook's measure of the old divider */
    bus_took(hz);
    sd_rep.bus_set = restore ? 0 : hz ? 1 : -3;
    if (sd_rep.fast) {
        if (hs_clock(sd_rep.fast)) {
            (void)fast_fail("clock update after a bus clock change");
            return -1;
        }
        return 0;
    }
    return set_clock(25000000) ? -1 : 0;
}

/* Up to 256 sectors with one CMD17/18 (auto stop), drained from the FIFO by the CPU. */
static int read_chunk(uint64_t sector, uint32_t n, uintptr_t dst)
{
    wr(CTRL, rd(CTRL) | 2U);                   /* FIFO reset */
    if (wait_reg(CTRL, 2U, 0, 50))
        return -1;
    wr(BLKSIZ, 512);
    wr(BYTCNT, n * 512U);
    uint32_t arg = (uint32_t)(sd_rep.high_capacity ? sector : sector * 512U);
    int r = cmd(n > 1 ? 18 : 17, arg, R1 | C_DATA | (n > 1 ? C_AUTOSTOP : 0));
    if (r)
        return r;
    volatile uint64_t *out = (volatile uint64_t *)dst;
    uint64_t want = (uint64_t)n * 64U, got = 0;   /* 64-bit FIFO words */
    uint64_t t = deadline(200 + n * 2U);
    for (;;) {
        uint32_t s = rd(RINTSTS);
        uint32_t c = (rd(STATUS) >> 17) & 0x1fffU;
        while (c-- && got < want)
            out[got++] = fifo_rd();
        if (s & I_DERR) {
            sd_rep.last_rintsts = s;
            return -5;
        }
        if ((s & I_DTO) && got >= want)
            break;
        if ((s & I_DTO) && !((rd(STATUS) >> 17) & 0x1fffU)) {
            sd_rep.last_rintsts = s;
            return -6;                         /* transfer over, data short */
        }
        if (expired(t)) {
            sd_rep.last_rintsts = s;
            return -7;
        }
    }
    if (n > 1 && wait_reg(RINTSTS, I_ACD, I_ACD, 100))
        return -8;
    return 0;
}

/*
 * After a failed read_chunk the controller can still be inside the CMD18 (the FIFO full, the card
 * clock stopped by flow control), and the next command would wait for it forever. As Linux after a
 * data error: controller + FIFO reset, RINTSTS cleared, UPD_CLK; CMD12 with stop_abort (a card
 * already in tran does not answer: ignored); CMD13 until tran (4), at most 250 ms. Returns the
 * last card state or -1.
 */
static int sd_recover(void)
{
    sd_rep.recoveries++;
    wr(CTRL, rd(CTRL) | 3U);
    (void)wait_reg(CTRL, 3U, 0, 100);
    wr(RINTSTS, 0xffffffffU);
    (void)update_clock(0);
    (void)cmd(12, 0, R1 | C_STOP);
    (void)wait_reg(STATUS, 1U << 9, 0, 500);   /* R1b busy */
    int state = -1, restopped = 0;
    uint64_t t = deadline(250);
    for (;;) {
        if (cmd(13, sd_rep.rca << 16, R1) == 0) {
            state = (int)((rd(RESP0) >> 9) & 0xfU);
            if (state == 4)
                break;
            if (state == 5 && !restopped) {
                restopped = 1;
                (void)cmd(12, 0, R1 | C_STOP);
                (void)wait_reg(STATUS, 1U << 9, 0, 500);
            }
        }
        if (expired(t))
            break;
        sb_mdelay(1);
    }
    wr(RINTSTS, 0xffffffffU);
    sd_rep.card_state = state;
    return state;
}

/* ---- fast reads (sd_set_fast; the header has the facts and their sources) --------------------- */

#define PLDMND     0x84
#define DBADDRL    0x88
#define DBADDRU    0x8c
#define IDSTS64    0x90
#define IDINTEN64  0x94

#define CTRL_USE_IDMAC (1U << 25)
#define CTRL_DMA_EN    (1U << 5)
#define CTRL_DMA_RST   (1U << 2)
#define BMOD_SWR   (1U << 0)
#define BMOD_FB    (1U << 1)
#define BMOD_DE    (1U << 7)
#define ID_TI      (1U << 0)
#define ID_RI      (1U << 1)
#define ID_FBE     (1U << 2)
#define ID_CES     (1U << 5)
#define ID_NI      (1U << 8)
#define ID_AI      (1U << 9)
#define ID_ALL     0x337U
#define D_OWN      (1U << 31)
#define D_CH       (1U << 4)
#define D_FD       (1U << 3)
#define D_LD       (1U << 2)
#define D_DIC      (1U << 1)

#define FIFOTH_PIO (2U << 28 | 31U << 16 | 32U)   /* what sd_up writes */
#define FIFOTH_DMA (4U << 28 | 31U << 16 | 32U)   /* MSIZE 32 transfers */

#define SW_QUERY_HS 0x00fffff1U             /* CMD6 mode 0: would group 1 take function 1? (no change) */
#define SW_SET_HS   0x80fffff1U             /* CMD6 mode 1: group 1 = 1 (high speed), groups 2-6 0xf = keep */
#define SD_WRITE_HZ 6250000U              /* writes with fast reads on: the card clock writes always had (see sd_write) */
#define SW_SET_DS   0x80fffff0U             /* CMD6 mode 1: group 1 = 0 (default speed), groups 2-6 0xf = keep: before writes */

#ifndef SD_DMA_LIMIT
#define SD_DMA_LIMIT 0x100000000ULL         /* IDMAC buffers and descriptors below 4 GiB */
#endif

struct sd_desc {                            /* dw_mmc struct idmac_desc_64addr */
    uint32_t des0, des1, des2, des3, des4, des5, des6, des7;
};
#define SD_DESC_BYTES 4096U
#define SD_NDESC (256U * 512U / SD_DESC_BYTES)
static struct sd_desc descs[SD_NDESC] __attribute__((aligned(64)));
static uint64_t vbuf[2][512] __attribute__((aligned(64)));   /* the 8-sector checks */

/* A short PIO data read (ACMD51's SCR, CMD6's status) of bytes (8..64, a multiple of 8) into st. */
static int short_read(unsigned idx, uint32_t arg, int app, unsigned bytes, uint64_t *st)
{
    wr(CTRL, rd(CTRL) | 2U);
    if (wait_reg(CTRL, 2U, 0, 50))
        return -1;
    wr(BLKSIZ, bytes);
    wr(BYTCNT, bytes);
    int r = app ? acmd(idx, arg, R1 | C_DATA) : cmd(idx, arg, R1 | C_DATA);
    unsigned got = 0, want = bytes / 8U;
    if (!r) {
        uint64_t t = deadline(200);
        for (;;) {
            uint32_t s = rd(RINTSTS);
            uint32_t c = (rd(STATUS) >> 17) & 0x1fffU;
            while (c-- && got < want)
                st[got++] = fifo_rd();
            if (s & I_DERR) {
                sd_rep.last_rintsts = s;
                r = -5;
                break;
            }
            if ((s & I_DTO) && got >= want)
                break;
            if ((s & I_DTO) && !((rd(STATUS) >> 17) & 0x1fffU)) {
                r = -6;
                break;
            }
            if (expired(t)) {
                r = -7;
                break;
            }
        }
    }
    wr(BLKSIZ, 512);
    if (r)
        (void)sd_recover();
    return r;
}

/* ACMD51: the SCR, 8 bytes in bus order (byte 0 = SCR[63:56]). */
static int read_scr(void)
{
    uint64_t w = 0;
    if (short_read(51, 0, 1, 8, &w))
        return -1;
    for (unsigned i = 0; i < 8; ++i)
        sd_rep.scr[i] = (uint8_t)(w >> (8 * i));
    sd_rep.scr_ok = 1;
    return 0;
}

int sd_hs_query(void)
{
    if (sd_rep.state != 1)
        return -1;
    if (!sd_rep.scr_ok)
        (void)read_scr();
    uint64_t st[8];
    sd_rep.hs_asked = 1;
    if (short_read(6, SW_QUERY_HS, 0, 64, st)) {
        sd_rep.hs_support = -1;
        return -1;
    }
    const uint8_t *b = (const uint8_t *)st;
    sd_rep.hs_support = (b[13] & 2U) && (b[16] & 0xfU) == 1U;
    return sd_rep.hs_support;
}

/* Can this chunk go by DMA? 2 write-back (cache maintenance), 3 non-cacheable, 0 no. */
static int dma_kind(uintptr_t dst, uint64_t len)
{
    if ((dst & 63U) || !len || (uint64_t)dst + len > SD_DMA_LIMIT || (uint64_t)dst + len < (uint64_t)dst)
        return 0;
    int a = sb_map_kind(dst), b = sb_map_kind(dst + len - 1);
    if ((a != 2 && a != 3) || (b != 2 && b != 3))
        return 0;
    return a == 2 || b == 2 ? 2 : 3;
}

/* The controller back to PIO: IDMAC off and reset, DMA interface reset, FIFOTH as sd_up left it. */
static void dma_stop(void)
{
    wr(CTRL, (rd(CTRL) & ~(CTRL_USE_IDMAC | CTRL_DMA_EN)) | CTRL_DMA_RST);
    (void)wait_reg(CTRL, CTRL_DMA_RST, 0, 50);
    wr(BMOD, BMOD_SWR);
    (void)wait_reg(BMOD, BMOD_SWR, 0, 50);
    wr(BMOD, 0);
    wr(IDSTS64, ID_ALL);
    wr(FIFOTH, FIFOTH_PIO);
}

/* 2..256 sectors with one CMD18 (auto stop) through the IDMAC; the same codes as read_chunk, and -12
 * an IDMAC error (IDSTS64 in sd_rep.dma_idsts), -13 the IDMAC did not finish (DTO without RI, or a
 * descriptor still owned). kind: dma_kind's answer for dst. */
static int read_chunk_dma(uint64_t sector, uint32_t n, uintptr_t dst, int kind)
{
    uint32_t len = n * 512U;
    unsigned nd = (len + SD_DESC_BYTES - 1U) / SD_DESC_BYTES;
    for (unsigned i = 0; i < nd; ++i) {
        struct sd_desc *d = &descs[i];
        uint64_t buf = (uint64_t)dst + (uint64_t)i * SD_DESC_BYTES;
        uint64_t next = (uint64_t)(uintptr_t)&descs[i + 1 < nd ? i + 1 : 0];
        uint32_t sz = len - i * SD_DESC_BYTES;
        d->des0 = D_OWN | D_CH | D_DIC | (i == 0 ? D_FD : 0);
        d->des1 = 0;
        d->des2 = sz > SD_DESC_BYTES ? SD_DESC_BYTES : sz;   /* buffer 1 size, bits 12:0 */
        d->des3 = 0;
        d->des4 = (uint32_t)buf;
        d->des5 = (uint32_t)(buf >> 32);
        d->des6 = (uint32_t)next;
        d->des7 = (uint32_t)(next >> 32);
    }
    descs[nd - 1].des0 = (descs[nd - 1].des0 & ~(D_CH | D_DIC)) | D_LD;
    sb_dcache_clean_inval(descs, nd * sizeof(descs[0]));
    if (kind == 2)                             /* no dirty line may be evicted over the DMA data */
        sb_dcache_clean_inval((const void *)dst, len);
    DMA_BARRIER();

    wr(CTRL, rd(CTRL) | 2U | CTRL_DMA_RST);    /* FIFO + DMA interface reset */
    if (wait_reg(CTRL, 2U | CTRL_DMA_RST, 0, 50))
        return -1;
    wr(BMOD, BMOD_SWR);
    if (wait_reg(BMOD, BMOD_SWR, 0, 50))
        return -1;
    wr(IDSTS64, ID_ALL);
    wr(IDINTEN64, ID_NI | ID_RI | ID_TI);      /* status only: CTRL's interrupt enable stays off */
    uint64_t da = (uint64_t)(uintptr_t)descs;
    wr(DBADDRL, (uint32_t)da);
    wr(DBADDRU, (uint32_t)(da >> 32));
    wr(FIFOTH, FIFOTH_DMA);
    wr(BLKSIZ, 512);
    wr(BYTCNT, len);
    wr(CTRL, rd(CTRL) | CTRL_USE_IDMAC | CTRL_DMA_EN);
    wr(BMOD, BMOD_DE | BMOD_FB);
    wr(PLDMND, 1);
    sd_rep.dma_chunks++;

    uint32_t arg = (uint32_t)(sd_rep.high_capacity ? sector : sector * 512U);
    int r = cmd(18, arg, R1 | C_DATA | C_AUTOSTOP);
    uint64_t t = deadline(200 + n * 2U);
    while (!r) {
        uint32_t s = rd(RINTSTS), id = rd(IDSTS64);
        sd_rep.dma_idsts = id;
        if (s & I_DERR) {
            sd_rep.last_rintsts = s;
            r = -5;
        } else if (id & (ID_FBE | ID_CES | ID_AI)) {
            sd_rep.last_rintsts = s;
            r = -12;
        } else if ((s & I_DTO) && (id & ID_RI)) {
            break;
        } else if (expired(t)) {
            sd_rep.last_rintsts = s;
            r = (s & I_DTO) ? -13 : -7;
        }
    }
    if (!r && wait_reg(RINTSTS, I_ACD, I_ACD, 100))
        r = -8;
    sb_dcache_inval(descs, nd * sizeof(descs[0]));
    for (unsigned i = 0; !r && i < nd; ++i)
        if (((volatile struct sd_desc *)&descs[i])->des0 & D_OWN)
            r = -13;
    dma_stop();
    if (kind == 2)
        sb_dcache_inval((const void *)dst, len);
    return r;
}

static int same(const uint64_t *a, const uint64_t *b, unsigned words)
{
    for (unsigned i = 0; i < words; ++i)
        if (((const volatile uint64_t *)a)[i] != ((const volatile uint64_t *)b)[i])
            return 0;
    return 1;
}

static void fill(uint64_t *a, unsigned words, uint64_t v)
{
    for (unsigned i = 0; i < words; ++i)
        ((volatile uint64_t *)a)[i] = v;
}

/* The card clock for a fast mode: 0, -1 the update failed, -2 no such setting on this bus clock. */
static int hs_clock(int mode)
{
    if (mode == SD_FAST_HS50)                  /* 201.5 MHz / 4 = 50.375 MHz, as Linux and XNU 0348 run it */
        return sd_rep.bus_hz >= 196000000ULL && sd_rep.bus_hz <= 202000000ULL ? set_div(2) : -2;
    return set_clock(50000000);                /* the highest exact rate <= 50 MHz: 33.58 MHz from 201.5 */
}

static void fast_log(const char *why)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, why ? "sd: fast reads off (" : "sd: fast reads on: high speed at ");
    if (why) {
        ub_s(&u, why);
        ub_s(&u, "): PIO at ");
    }
    ub_dec(&u, sd_rep.card_hz / 1000U);
    ub_s(&u, why ? " kHz" : " kHz, IDMAC, sectors 0-7 match the old path");
    sb_log(u.b);
}

/* Off again: the card clock back to sd_up's 25 MHz setting, PIO. One log line. */
static int fast_fail(const char *why)
{
    sd_rep.fast = 0;
    sd_rep.fast_why = why;
    if (set_clock(25000000))
        sd_rep.fast_why = why = "the clock update back to 25 MHz failed";
    fast_log(why);
    return 0;
}

/* High speed, then the IDMAC, each checked on sectors 0-7 against the old path. 1 on, 0 off. */
static int fast_try(void)
{
    int mode = fast_want;
    if (sd_rep.fast == mode)
        return 1;
    if (sd_rep.fast) {                         /* from one fast mode to the other: from the old clock */
        sd_rep.fast = 0;
        if (set_clock(25000000))
            return fast_fail("clock update (25 MHz)");
    }
    if (((sd_rep.hcon >> 16) & 3U) != 0 || !((sd_rep.hcon >> 27) & 1U))
        return fast_fail("the controller has no 64-bit IDMAC (HCON)");
    if (sd_rep.csd_ok && !(((sd_rep.csd[2] >> 20) & 0xfffU) & (1U << 10)))
        return fast_fail("the card has no switch command class (CSD)");
    if (!sd_rep.scr_ok && read_scr())
        return fast_fail("ACMD51 (SCR) failed");
    if ((sd_rep.scr[0] & 0xfU) < 1U)
        return fast_fail("an SD 1.0 card has no CMD6 (SCR)");
    if (read_chunk(0, 8, (uintptr_t)vbuf[0])) {
        (void)sd_recover();
        return fast_fail("the reference read of sectors 0-7 failed");
    }
    int q = sd_hs_query();
    if (q < 0)
        return fast_fail("CMD6 query failed");
    if (!q)
        return fast_fail("the card does not offer high speed");
    uint64_t st[8];
    if (short_read(6, SW_SET_HS, 0, 64, st))
        return fast_fail("CMD6 switch failed");
    if ((((const uint8_t *)st)[16] & 0xfU) != 1U)
        return fast_fail("the card did not switch to high speed");
    sd_rep.hs_card = 1;
    sb_mdelay(1);                              /* the switch is done 8 clocks after the status block */
    int r = hs_clock(mode);
    if (r)
        return fast_fail(r == -2 ? "no 50.375 MHz setting on this bus clock" : "clock update (high speed)");
    fill(vbuf[1], 512, 0xa5a5a5a5a5a5a5a5ULL);
    if (read_chunk(0, 8, (uintptr_t)vbuf[1])) {
        (void)sd_recover();
        return fast_fail("PIO read at high speed failed");
    }
    if (!same(vbuf[0], vbuf[1], 512))
        return fast_fail("PIO read at high speed differs");
    fill(vbuf[1], 512, 0x5a5a5a5a5a5a5a5aULL);
    int k = dma_kind((uintptr_t)vbuf[1], sizeof(vbuf[1]));
    if (!k)
        return fast_fail("no DMA-able buffer");
    if ((r = read_chunk_dma(0, 8, (uintptr_t)vbuf[1], k))) {
        sd_rep.dma_last_error = r;
        (void)sd_recover();
        return fast_fail("IDMAC read failed");
    }
    if (!same(vbuf[0], vbuf[1], 512))
        return fast_fail("IDMAC read differs");
    sd_rep.fast = mode;
    sd_rep.fast_why = 0;
    fast_log(0);
    return 1;
}

int sd_set_fast(int mode)
{
    fast_want = mode == SD_FAST_ON || mode == SD_FAST_HS50 ? mode : SD_FAST_OFF;
    if (sd_rep.state != 1)
        return 0;                              /* the next sd_up switches */
    if (fast_want)
        return fast_try();
    if (sd_rep.fast) {
        sd_rep.fast = 0;
        sd_rep.fast_why = "turned off";
        if (set_clock(25000000))
            sd_rep.fast_why = "turned off; the clock update failed";
    }
    return 0;
}

int sd_fast_mode(void)
{
    return fast_want;
}

int sd_read(uint64_t sector, uint32_t count, uintptr_t dst)
{
    if (!sd_up())
        return -1;
    while (count) {
        uint32_t n = count > 256U ? 256U : count;
        int r = -1, k = sd_rep.fast && n > 1 ? dma_kind(dst, (uint64_t)n * 512U) : 0;
        if (k) {
            r = read_chunk_dma(sector, n, dst, k);
            if (r) {                               /* this chunk again on the PIO path, from a clean controller */
                uint32_t ri = sd_rep.last_rintsts;
                sd_rep.dma_errors++;
                sd_rep.dma_last_error = r;
                (void)sd_recover();
                sd_rep.last_rintsts = ri;
                if (sd_rep.dma_errors % 3U == 0)
                    (void)fast_fail("three IDMAC read errors");
            }
        }
        for (int a = 0; a < 3 && r; ++a) {
            r = read_chunk(sector, n, dst);
            if (r) {                               /* the last failure too: the next read starts clean */
                uint32_t ri = sd_rep.last_rintsts;
                sd_rep.last_error = r;
                (void)sd_recover();
                sd_rep.last_rintsts = ri;
            }
        }
        if (r)
            return r;
        sector += n;
        count -= n;
        dst += (uintptr_t)n * 512U;
    }
    return 0;
}

int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (count > 0xffffffffULL / 8U)
        return -1;
    return sd_read(lba * 8U, (uint32_t)(count * 8U), dst);
}

/* Sector 0: the card's MBR. */
static uint64_t mbr[64];

int sd_store_area(uint64_t *first, uint64_t *blocks)
{
    *first = *blocks = 0;
    if (!allowed || sd_read(0, 1, (uintptr_t)mbr))
        return 0;
    const volatile uint8_t *b = (const volatile uint8_t *)mbr;
    if (b[510] != 0x55 || b[511] != 0xaa)
        return 0;
    for (unsigned i = 0; i < 4; ++i) {
        const volatile uint8_t *e = b + 446 + 16 * i;
        uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        uint32_t len = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        if (e[4] != 0xda || !lba || (lba & 7U) || len < 64)
            continue;
        *first = lba / 8U;
        *blocks = len / 8U;
        return 1;
    }
    return 0;
}

/* ---- writes (fence.c sb_write only) -------------------------------------------------------- */

/* Up to 256 sectors with one CMD24 / CMD25 (auto stop), the FIFO filled by the CPU. */
static int write_chunk(uint64_t sector, uint32_t n, uintptr_t src)
{
    wr(CTRL, rd(CTRL) | 2U);                   /* FIFO reset */
    if (wait_reg(CTRL, 2U, 0, 50))
        return -1;
    wr(BLKSIZ, 512);
    wr(BYTCNT, n * 512U);
    uint32_t arg = (uint32_t)(sd_rep.high_capacity ? sector : sector * 512U);
    int r = cmd(n > 1 ? 25 : 24, arg, R1 | C_DATA | C_WRITE | (n > 1 ? C_AUTOSTOP : 0));
    if (r)
        return r;
    const volatile uint64_t *in = (const volatile uint64_t *)src;
    uint64_t want = (uint64_t)n * 64U, put = 0;
    uint64_t t = deadline(300 + n * 4U);
    for (;;) {
        uint32_t s = rd(RINTSTS);
        while (put < want && !(rd(STATUS) & (1U << 3)))   /* FIFO not full */
            fifo_wr(in[put++]);
        if (s & I_DERR) {
            sd_rep.last_rintsts = s;
            return -5;
        }
        if ((s & I_DTO) && put >= want)
            break;
        if (expired(t)) {
            sd_rep.last_rintsts = s;
            return -7;
        }
    }
    if (n > 1 && wait_reg(RINTSTS, I_ACD, I_ACD, 200))
        return -8;
    if (wait_reg(STATUS, 1U << 9, 0, 1000))    /* the card programs (busy on DAT0) */
        return -10;
    uint64_t t2 = deadline(1000);
    for (;;) {                                 /* CMD13 until tran: the data is in the card */
        if (cmd(13, sd_rep.rca << 16, R1) == 0 && ((rd(RESP0) >> 9) & 0xfU) == 4U)
            return 0;
        if (expired(t2))
            return -11;
        sb_mdelay(1);
    }
}

static int write_sectors(uint64_t sector, uint32_t count, uintptr_t src)
{
    while (count) {
        uint32_t n = count > 256U ? 256U : count;
        int r = -1;
        for (int a = 0; a < 3 && r; ++a) {
            r = write_chunk(sector, n, src);
            if (r) {
                uint32_t ri = sd_rep.last_rintsts;
                sd_rep.last_error = r;
                (void)sd_recover();
                sd_rep.last_rintsts = ri;
            }
        }
        if (r)
            return r;
        sector += n;
        count -= n;
        src += (uintptr_t)n * 512U;
    }
    return 0;
}

/* With fast reads on, a write still runs at sd_up's 25 MHz clock and on the code above. */
int sd_write(uint64_t sector, uint32_t count, uintptr_t src)
{
    if (!sb_fence_active())
        return -9;                             /* not through the fence: nothing touched */
    if (!sd_up())
        return -1;
    int was = sd_rep.fast, hs = sd_rep.hs_card;
    if (was) {
        sd_rep.fast = 0;
        /* the card clock every write on this phone had until now: the "25 MHz" setting ran at 6.25 MHz under S-Boot's
         * CMU /16 (drivers run 11). With the kernel's sdclk at /1 a 25 MHz setting would really be 25 MHz, never tried
         * for writes to the OS store: so writes keep 6.25 MHz (CLKDIV, no CMU change) */
        if (set_clock(SD_WRITE_HZ)) {
            (void)fast_fail("clock update before a write");
            return -1;
        }
    }
    /* Fast reads off (or turned off after an error) but the bus clock raised (the kernel's sdclk): the card clock
     * would be faster than any write ever ran here; a write still runs at SD_WRITE_HZ, the old setting after it */
    uint32_t div_before = rd(CLKDIV) & 0xffU;
    int slowed = 0;
    if (!was && sd_rep.card_hz > SD_WRITE_HZ) {
        if (set_clock(SD_WRITE_HZ))
            return -1;
        slowed = 1;
    }
    /* A write runs exactly as before the fast reads existed: the card back in default speed too (CMD6 mode 1, group 1
     * function 0), not only the clock. If that switch does not take, the write is refused (nothing written): the card
     * holds the OS store, and writes in high-speed mode at 25 MHz were never tried on this phone. */
    if (hs) {
        uint64_t st[8];
        if (short_read(6, SW_SET_DS, 0, 64, st) || (((const uint8_t *)st)[16] & 0xfU) != 0U) {
            (void)sd_recover();
            (void)fast_fail("CMD6 back to default speed before a write: refused, nothing written");
            sd_rep.hs_card = 0;
            return -1;
        }
        sd_rep.hs_card = 0;
        sb_mdelay(1);
    }
    int r = write_sectors(sector, count, src);
    if (slowed)
        (void)set_div(div_before);
    if (hs && was) {
        uint64_t st[8];
        if (short_read(6, SW_SET_HS, 0, 64, st) || (((const uint8_t *)st)[16] & 0xfU) != 1U) {
            (void)fast_fail("CMD6 back to high speed after a write");
            return r;
        }
        sd_rep.hs_card = 1;
        sb_mdelay(1);
    }
    if (was) {
        if (hs_clock(was))
            (void)fast_fail("clock update after a write");
        else
            sd_rep.fast = was;
    }
    return r;
}

int sd_write4k(uint64_t lba, uint64_t count, uintptr_t src)
{
    if (count > 0xffffffffULL / 8U)
        return -1;
    return sd_write(lba * 8U, (uint32_t)(count * 8U), src);
}

/* Card capacity in 512-byte sectors from the MBR's partitions' end (no CSD read): 0 unknown. */
uint64_t sd_sectors_hint(void)
{
    if (!allowed || sd_read(0, 1, (uintptr_t)mbr))
        return 0;
    const volatile uint8_t *b = (const volatile uint8_t *)mbr;
    if (b[510] != 0x55 || b[511] != 0xaa)
        return 0;
    uint64_t end = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const volatile uint8_t *e = b + 446 + 16 * i;
        uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        uint32_t len = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        if ((uint64_t)lba + len > end)
            end = (uint64_t)lba + len;
    }
    return end;
}
