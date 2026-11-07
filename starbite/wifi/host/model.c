/* SPDX-License-Identifier: GPL-2.0-only
 * A model of the S9's Wi-Fi hardware for bcmw's host test, at the register level:
 *  - the Exynos 9810 PCIe channel 0 (ELBI, PHY, PCS, DBI, the iATU's configuration window), the
 *    PMU word, CMU gates and the three GPIO banks: values kept, the link up once WLAN_REG_ON and
 *    PERST# are high and the LTSSM is let go;
 *  - the BCM4361 behind it: configuration space with two 64-bit BARs, BAR0's 4 KiB backplane
 *    window (config 0x80), chipcommon with an AXI enumeration ROM (chipcommon, PCIe2, ARM CR4,
 *    802.11, PMU), the cores' wrappers, the CR4's TCM banks, OTP's CIS (module 91 33), the PCIe2
 *    mailbox; the chip's RAM through BAR2;
 *  - a firmware, started when the ARM is let go: it checks what was downloaded (reset vector,
 *    image, NVRAM and its length token), leaves a shared area (protocol 7), reads the host's rings
 *    as Broadcom's firmware does (descriptors in its RAM, indices in its RAM or by "DMA" in host
 *    memory), checks DHD's H2D stamping (BADOPTION otherwise), answers ioctls (cur_etheraddr, ver,
 *    clmload in chunks, event_msgs, up, escan), and sends events and a data frame.
 * Every write is checked against bcmw_write_allowed() and the order of the power sequence is
 * recorded for the test (host/test.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bcmw_priv.h"
#include "model.h"

struct model M;

/* ---- the DMA arena: host memory with bus addresses below 4 GiB --------------------------------- */

#define ARENA_SIZE (8u << 20)
#define ARENA_BUS 0xA6000000u
static uint8_t arena[ARENA_SIZE] __attribute__((aligned(4096)));
static uint32_t arena_used;

void *model_dma_alloc(void *ctx, uint32_t size, uint32_t align, uint64_t *bus)
{
    (void)ctx;
    if (align < 8)
        align = 8;
    arena_used = (arena_used + align - 1) & ~(align - 1);
    if (arena_used + size > ARENA_SIZE)
        return 0;
    *bus = ARENA_BUS + arena_used;
    arena_used += size;
    M.dma_bytes = arena_used;
    return arena + (*bus - ARENA_BUS);
}

static uint8_t *bus_ptr(uint64_t bus, uint32_t len)
{
    if (bus < ARENA_BUS || bus + len > (uint64_t)ARENA_BUS + ARENA_SIZE) {
        M.errors++;
        printf("model: the chip was given a bus address outside the arena: %llx\n", (unsigned long long)bus);
        return 0;
    }
    return arena + (bus - ARENA_BUS);
}

/* ---- SoC registers: a small map ------------------------------------------------------------------ */

#define NREG 4096
static uint64_t reg_pa[NREG];
static uint32_t reg_v[NREG];
static unsigned nreg;

static uint32_t *reg(uint64_t pa)
{
    for (unsigned i = 0; i < nreg; i++)
        if (reg_pa[i] == pa)
            return &reg_v[i];
    if (nreg == NREG) {
        printf("model: register map full\n");
        exit(2);
    }
    reg_pa[nreg] = pa;
    reg_v[nreg] = 0;
    return &reg_v[nreg++];
}

/* ---- the chip's backplane --------------------------------------------------------------------------- */

#define CC_BASE    0x18000000u
#define PCIE2_BASE 0x18003000u
#define CR4_BASE   0x18002000u
#define D11_BASE   0x18001000u
#define PMU_BASE   0x18012000u
#define WRAP(b)    ((b) + 0x100000u)
#define EROM       0x180FF000u
#define RAMBASE    0x170000u
#define RAMSIZE    0xE0000u
#define CHIPSPACE  (RAMBASE + RAMSIZE)

static uint8_t tcm[CHIPSPACE];
static uint32_t erom[64];
static unsigned nerom;

static void erom_core(uint16_t id, uint8_t rev, uint32_t base, int master)
{
    erom[nerom++] = 0x4BF00000u | ((uint32_t)id << 8) | 0x1;                    /* CIA: designer, part */
    erom[nerom++] = ((uint32_t)rev << 24) | (master ? 1u << 14 : 1u << 19) | 0x1; /* CIB: one wrapper */
    if (master)
        erom[nerom++] = 0x00000003u;                                              /* a master port */
    erom[nerom++] = base | (0u << 6) | (0u << 4) | 0x5;                           /* the core's 4 KiB */
    erom[nerom++] = WRAP(base) | ((master ? 3u : 2u) << 6) | (0u << 4) | 0x5;     /* its wrapper */
}

static const uint8_t cis[] = { 0x80, 0x07, 0x00, 1, 2, 3, 4, 5, 6, 0x80, 0x03, 0x81, 0x91, 0x33, 0x80, 0x02, 0x1b, 0x05 };

void model_reset(void)
{
    memset(&M, 0, sizeof(M));
    nreg = 0;
    nerom = 0;
    arena_used = 0;
    memset(tcm, 0, sizeof(tcm));
    erom_core(0x800, 58, CC_BASE, 0);
    erom_core(0x812, 129, D11_BASE, 1);
    erom_core(0x83E, 9, CR4_BASE, 1);
    erom_core(0x83C, 19, PCIE2_BASE, 1);
    erom_core(0x827, 30, PMU_BASE, 0);
    erom[nerom++] = 0xF;
    M.cfg[0] = 0x441f14e4;
    M.cfg[2] = 0x02800001;
    M.cfg[0x10 / 4] = 0x4;             /* BAR0: 64-bit, 32 KiB (the sizes are in model_rd32) */
    M.cfg[0x18 / 4] = 0x4;             /* BAR2: 64-bit, 4 MiB */
    M.cfg[0x80 / 4] = CC_BASE;
    M.cr4_reset = 1;
    M.cr4_ioctl = 0x20;
}

static uint32_t *bp_reg(uint32_t a) { return reg(0x100000000ull | a); }

static void fw_run(void);

static uint32_t bp_rd(uint32_t a)
{
    if (a == CC_BASE)
        return (1u << 28) | (5u << 16) | 0x4361;
    if (a == CC_BASE + 0xfc)
        return EROM;
    if (a >= EROM && a < EROM + sizeof(erom))
        return erom[(a - EROM) / 4];
    if (a == CR4_BASE + 0x04)
        return 4;                       /* four A banks */
    if (a == CR4_BASE + 0x44)
        return 0x1b;                    /* 28 blocks of 8 KiB: 224 KiB each, 896 KiB in all */
    if (a == WRAP(CR4_BASE) + 0x408)
        return M.cr4_ioctl;
    if (a == WRAP(CR4_BASE) + 0x800)
        return M.cr4_reset;
    if (a == PCIE2_BASE + 0x48)
        return M.mailboxint;
    if (a >= 0x18011110 && a < 0x18011168) {
        uint32_t i = a - 0x18011110, v = 0;
        for (int k = 0; k < 4; k++)
            v |= (uint32_t)(i + k < sizeof(cis) ? cis[i + k] : 0) << (8 * k);
        return v;
    }
    return *bp_reg(a);
}

static void bp_wr(uint32_t a, uint32_t v)
{
    if (a == CC_BASE + 0x80 && v) {
        M.watchdog_resets++;
        M.cfg[0x80 / 4] = CC_BASE;      /* the window back to chipcommon */
        M.fw_running = 0;
        return;
    }
    if (a == WRAP(CR4_BASE) + 0x408) {
        M.cr4_ioctl = v;
        if (!(v & 0x20) && !M.cr4_reset && !M.fw_running)
            M.start_fw = 1;
        if (v & 0x20)
            M.fw_running = 0;
        return;
    }
    if (a == WRAP(CR4_BASE) + 0x800) {
        M.cr4_reset = v & 1;
        return;
    }
    if (a == PCIE2_BASE + 0x48) {
        M.mailboxint &= ~v;
        return;
    }
    if (a == PCIE2_BASE + 0x140) {
        M.doorbells++;
        if (M.fw_running)
            fw_run();
        return;
    }
    if (a == PCIE2_BASE + 0x144) {
        M.hostready++;
        return;
    }
    *bp_reg(a) = v;
}

/* ---- MMIO -------------------------------------------------------------------------------------------- */

static uint64_t bar(int i)
{
    return ((uint64_t)M.cfg[(0x10 + 4 * i + 4) / 4] << 32 | M.cfg[(0x10 + 4 * i) / 4]) & ~0xfull;
}

static void fw_boot(void);

static void note_access(uint64_t pa, int write)
{
    if ((pa & ~0xfffull) == EX_SYSMMU) {
        M.errors++;
        printf("model: the PCIe SysMMU's registers were %s (they stopped the bus on the phone)\n", write ? "written" : "read");
    }
    if (pa >= EX_CMU_FSYS1 && pa < EX_CMU_FSYS1 + 0x4000 && !M.t_cmu)
        M.t_cmu = M.now + 1;
    if (pa == EX_ELBI + 0x2C && write)
        M.t_ltssm = M.now + 1;
}

uint32_t model_rd32(void *ctx, uint64_t pa)
{
    uint64_t b0 = bar(0), b2 = bar(2);
    (void)ctx;
    M.reads++;
    note_access(pa, 0);
    if (pa >= EX_CFG && pa < EX_CFG + 0x1000) {
        uint32_t off = (uint32_t)(pa - EX_CFG);
        if (!M.linked)
            return 0xffffffffu;
        if (off >= 0x20 && off < 0x28)
            return 0;                   /* BAR4, BAR5: not implemented */
        if (off >= 0x10 && off < 0x20 && M.sizing[(off - 0x10) / 4]) {
            static const uint32_t size_lo[4] = { 0xffff8004u, 0xffffffffu, 0xffc00004u, 0xffffffffu };
            return size_lo[(off - 0x10) / 4];
        }
        return M.cfg[off / 4];
    }
    if (b0 && pa >= b0 && pa < b0 + 0x8000)
        return bp_rd(M.cfg[0x80 / 4] + (uint32_t)((pa - b0) & 0xfff));
    if (b2 && pa >= b2 && pa < b2 + (4u << 20)) {
        uint32_t a = (uint32_t)(pa - b2);
        if (a + 4 > CHIPSPACE)
            return 0xffffffffu;
        return (uint32_t)tcm[a] | tcm[a + 1] << 8 | tcm[a + 2] << 16 | (uint32_t)tcm[a + 3] << 24;
    }
    if (pa == EX_ELBI + 0x74)
        return M.linked ? 0x11 : 0x03;
    if (pa == EX_PHY + 0xBC)
        return 3;
    if (pa == EX_DBI + 0x80)
        return 0x00120000u | (*reg(pa) & 0xffff);
    return *reg(pa);
}

void model_wr32(void *ctx, uint64_t pa, uint32_t v)
{
    uint64_t b0 = bar(0), b2 = bar(2);
    (void)ctx;
    M.writes++;
    note_access(pa, 1);
    if (!bcmw_write_allowed(pa)) {
        M.errors++;
        printf("model: a write outside the allow-list reached the model: %llx\n", (unsigned long long)pa);
        return;
    }
    if (pa >= EX_CFG && pa < EX_CFG + 0x1000) {
        uint32_t off = (uint32_t)(pa - EX_CFG);
        if (!M.linked)
            return;
        if (off >= 0x10 && off < 0x20) {
            M.sizing[(off - 0x10) / 4] = v == 0xffffffffu;
            if (v != 0xffffffffu)
                M.cfg[off / 4] = v;
            return;
        }
        M.cfg[off / 4] = v;
        return;
    }
    if (b0 && pa >= b0 && pa < b0 + 0x8000) {
        bp_wr(M.cfg[0x80 / 4] + (uint32_t)((pa - b0) & 0xfff), v);
        if (M.start_fw) {
            M.start_fw = 0;
            fw_boot();
        }
        return;
    }
    if (b2 && pa >= b2 && pa < b2 + (4u << 20)) {
        uint32_t a = (uint32_t)(pa - b2);
        if (a + 4 <= CHIPSPACE)
            for (int k = 0; k < 4; k++)
                tcm[a + k] = (uint8_t)(v >> (8 * k));
        return;
    }
    *reg(pa) = v;
    /* the pins and the link */
    if (pa == EX_GPIO_PERIC0 + 0x84) {
        int on = (v >> 5) & 1;
        if (on && !M.reg_on)
            M.t_reg_on = M.now;
        if (!on && M.reg_on_seen_low == 0)
            M.reg_on_seen_low = M.now + 1;
        M.reg_on = on;
    }
    if (pa == EX_GPIO_FSYS1 + 0x24) {
        int hi = (v >> 2) & 1;
        if (hi && !M.perst_high)
            M.t_perst = M.now;
        M.perst_high = hi;
    }
    if (pa == EX_PMU + 0x71C && (v & 1) && !M.t_pmu)
        M.t_pmu = M.now + 1;
    if (pa == EX_ELBI + 0x2C)
        M.linked = v == 1 && M.reg_on && M.perst_high;
}

uint16_t model_rd16(void *ctx, uint64_t pa)
{
    uint32_t v = model_rd32(ctx, pa & ~3ull);
    return (uint16_t)(v >> (8 * (pa & 2)));
}

void model_wr16(void *ctx, uint64_t pa, uint16_t v)
{
    uint64_t b2 = bar(2);
    (void)ctx;
    if (b2 && pa >= b2 && pa + 2 <= b2 + CHIPSPACE) {
        uint32_t a = (uint32_t)(pa - b2);
        tcm[a] = (uint8_t)v;
        tcm[a + 1] = (uint8_t)(v >> 8);
        return;
    }
    M.errors++;
    printf("model: a 16-bit write outside the chip's RAM: %llx\n", (unsigned long long)pa);
}

void model_udelay(void *ctx, uint32_t us)
{
    (void)ctx;
    M.now += us;
}

uint64_t model_now(void *ctx)
{
    (void)ctx;
    return M.now;
}

/* ---- the firmware -------------------------------------------------------------------------------- */

static uint32_t t32(uint32_t a) { return (uint32_t)tcm[a] | tcm[a + 1] << 8 | tcm[a + 2] << 16 | (uint32_t)tcm[a + 3] << 24; }
static uint16_t t16(uint32_t a) { return (uint16_t)(tcm[a] | tcm[a + 1] << 8); }
static void w32(uint32_t a, uint32_t v) { for (int k = 0; k < 4; k++) tcm[a + k] = (uint8_t)(v >> (8 * k)); }
static void w16(uint32_t a, uint16_t v) { tcm[a] = (uint8_t)v; tcm[a + 1] = (uint8_t)(v >> 8); }

/* where the firmware keeps its things */
#define SH       (RAMBASE + 0xC0000u)
#define RINGINFO (SH + 0x100)
#define RINGMEM  (SH + 0x200)
#define H2D_W    (SH + 0x300)
#define H2D_R    (SH + 0x340)
#define D2H_W    (SH + 0x380)
#define D2H_R    (SH + 0x3C0)
#define CONSOLE  (SH + 0x400)
#define CONBUF   (SH + 0x500)
#define HTOD_MB  (SH + 0x480)
#define DTOH_MB  (SH + 0x484)

static struct { uint8_t *buf; uint16_t items, size, r, w; uint32_t seq; uint8_t phase; } ring[5];
static struct { uint32_t pktid; uint64_t bus; uint16_t len; } ctlq[2][16], rxq[300];
static unsigned nctl[2], nrx;
static uint8_t clm_got[65536];
static uint32_t clm_len;

static void fw_boot(void)
{
    uint32_t token = t32(RAMBASE + RAMSIZE - 4), words = token & 0xffff;
    const char *msg = "fake fw: hello from the model\n";

    M.boots++;
    if (t32(0) != M.fw_rstvec)
        M.fw_errors |= 1;
    if (M.fw_image && memcmp(tcm + RAMBASE, M.fw_image, M.fw_image_len < 4096 ? M.fw_image_len : 4096) != 0)
        M.fw_errors |= 2;
    if ((token >> 16) != (~words & 0xffff) || words * 4 > 0x10000 || words == 0)
        M.fw_errors |= 4;
    else
        M.nvram_seen = words * 4;
    memset(tcm + SH, 0, 0x1000);
    w32(SH + 0, 7 | 0x10000000u | (M.offer_dma_idx ? 0x10000u | 0x100000u : 0));
    w32(SH + 20, CONSOLE);
    w16(SH + 34, 255);
    w32(SH + 36, 0);
    w32(SH + 40, HTOD_MB);
    w32(SH + 44, DTOH_MB);
    w32(SH + 48, RINGINFO);
    w32(RINGINFO + 0, RINGMEM);
    w32(RINGINFO + 4, H2D_W);
    w32(RINGINFO + 8, H2D_R);
    w32(RINGINFO + 12, D2H_W);
    w32(RINGINFO + 16, D2H_R);
    w16(RINGINFO + 52, 40);
    w16(RINGINFO + 54, 42);
    w16(RINGINFO + 56, 3);
    w32(CONSOLE + 8, CONBUF);
    w32(CONSOLE + 12, 256);
    memcpy(tcm + CONBUF, msg, strlen(msg));
    w32(CONSOLE + 16, (uint32_t)strlen(msg));
    memset(ring, 0, sizeof(ring));
    nctl[0] = nctl[1] = nrx = 0;
    clm_len = 0;
    w32(RAMBASE + RAMSIZE - 4, SH);
    M.fw_running = 1;
}

/* the rings' indices: in the chip's RAM, or in host memory where the host said (DMA indices) */
static uint64_t idx_host(int which)       /* 0 h2d_w, 1 h2d_r, 2 d2h_w, 3 d2h_r */
{
    uint32_t a = RINGINFO + 20 + 8 * which;
    return (uint64_t)t32(a + 4) << 32 | t32(a);
}

static unsigned idx_size(void) { return idx_host(0) ? 2 : 0; }

static uint16_t idx_rd(int which, int n)
{
    if (idx_size()) {
        uint8_t *p = bus_ptr(idx_host(which) + 2u * n, 2);
        return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
    }
    return t16((which == 0 ? H2D_W : which == 1 ? H2D_R : which == 2 ? D2H_W : D2H_R) + 4u * n);
}

static void idx_wr(int which, int n, uint16_t v)
{
    if (idx_size()) {
        uint8_t *p = bus_ptr(idx_host(which) + 2u * n, 2);
        if (p)
            p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
        return;
    }
    w16((which == 0 ? H2D_W : which == 1 ? H2D_R : which == 2 ? D2H_W : D2H_R) + 4u * n, v);
}

static int rings_known(void)
{
    for (int i = 0; i < 5; i++) {
        uint32_t d = RINGMEM + 16u * i;
        uint64_t bus = (uint64_t)t32(d + 12) << 32 | t32(d + 8);
        ring[i].items = t16(d + 4);
        ring[i].size = t16(d + 6);
        ring[i].buf = bus ? bus_ptr(bus, (uint32_t)ring[i].items * ring[i].size) : 0;
        if (!ring[i].buf || !ring[i].items)
            return 0;
        if (ring[i].seq == 0)
            ring[i].seq = 254;
    }
    return 1;
}

static void d2h_put(int r, const uint8_t *item, uint32_t len)
{
    uint8_t *p = ring[r].buf + (uint32_t)ring[r].w * ring[r].size;
    memset(p, 0, ring[r].size);
    memcpy(p, item, len < ring[r].size ? len : ring[r].size);
    if (++ring[r].w == ring[r].items)
        ring[r].w = 0;
    idx_wr(2, r - 2, ring[r].w);
    M.mailboxint |= 0x10000;
}

static void put16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k)); }
static void put16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32be(uint8_t *p, uint32_t v) { for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (24 - 8 * k)); }

static void ring_status(uint16_t ringid, uint16_t status)
{
    uint8_t it[24] = { MSGBUF_TYPE_RING_STATUS };
    put16le(it + 8, status);
    put16le(it + 10, ringid);
    d2h_put(RING_D2H_CONTROL, it, sizeof(it));
}

static int take_ctl(int event, uint32_t *pktid, uint8_t **buf, uint16_t *len)
{
    if (!nctl[event])
        return 0;
    *pktid = ctlq[event][0].pktid;
    *len = ctlq[event][0].len;
    *buf = bus_ptr(ctlq[event][0].bus, *len);
    memmove(&ctlq[event][0], &ctlq[event][1], sizeof(ctlq[0][0]) * --nctl[event]);
    return *buf != 0;
}

static void send_event(uint32_t type, uint32_t status, const uint8_t *data, uint32_t dlen)
{
    uint32_t pktid;
    uint8_t *b, it[24] = { MSGBUF_TYPE_WL_EVENT };
    uint16_t blen;
    uint32_t n = 72 + dlen;

    if (!take_ctl(1, &pktid, &b, &blen) || n > blen) {
        M.fw_errors |= 8;               /* no event buffer posted */
        return;
    }
    memset(b, 0, 72);
    memset(b, 0xff, 6);
    b[6] = 0x02;
    put16be(b + 12, 0x886c);
    put16be(b + 14, 0x8001);
    put16be(b + 16, (uint16_t)(n - 14));
    b[19] = 0x00, b[20] = 0x10, b[21] = 0x18;
    put16be(b + 22, 1);
    put16be(b + 24, 2);
    put32be(b + 28, type);
    put32be(b + 32, status);
    put32be(b + 44, dlen);
    memcpy(b + 54, "wlan0", 5);
    memcpy(b + 72, data, dlen);
    put32le(it + 4, pktid);
    put16le(it + 12, (uint16_t)n);
    d2h_put(RING_D2H_CONTROL, it, sizeof(it));
}

static void send_rx_frame(void)
{
    uint8_t it[40] = { MSGBUF_TYPE_RX_CMPLT }, *b;
    if (!nrx) {
        M.fw_errors |= 16;
        return;
    }
    b = bus_ptr(rxq[0].bus, rxq[0].len);
    if (!b)
        return;
    memset(b, 0, 60);
    memset(b, 0xff, 6);
    b[6] = 0x02;
    put16be(b + 12, 0x0806);            /* an ARP frame */
    put32le(it + 4, rxq[0].pktid);
    put16le(it + 14, 60);
    put16le(it + 16, 0);
    memmove(&rxq[0], &rxq[1], sizeof(rxq[0]) * --nrx);
    d2h_put(RING_D2H_RXCPL, it, sizeof(it));
}

static void answer(const uint8_t *req, uint16_t trans, uint32_t cmd, const uint8_t *in, uint16_t inlen, uint16_t outlen)
{
    uint8_t ack[24] = { MSGBUF_TYPE_IOCTLPTR_REQ_ACK }, it[24] = { MSGBUF_TYPE_IOCTL_CMPLT };
    uint8_t out[1600];
    uint16_t rlen = 0, status = 0;
    uint32_t pktid;
    uint8_t *b;
    uint16_t blen;
    int scan = 0;

    (void)req;
    memset(out, 0, sizeof(out));
    d2h_put(RING_D2H_CONTROL, ack, sizeof(ack));
    if (cmd == BCMW_C_GET_VAR || cmd == BCMW_C_SET_VAR) {
        const char *name = (const char *)in;
        const uint8_t *data = in + strlen(name) + 1;
        uint32_t dlen = inlen - (uint32_t)(strlen(name) + 1);
        M.last_iovar[0] = 0;
        snprintf(M.last_iovar, sizeof(M.last_iovar), "%s", name);
        if (cmd == BCMW_C_GET_VAR && !strcmp(name, "cur_etheraddr")) {
            memcpy(out, "\x00\x90\x4c\x12\xd0\x01", 6), rlen = 6;
        } else if (cmd == BCMW_C_SET_VAR && !strcmp(name, "cur_etheraddr")) {
            memcpy(M.mac_set, data, 6);
        } else if (cmd == BCMW_C_GET_VAR && !strcmp(name, "ver")) {
            const char *v = "wl0: Nov  7 2026 model version 13.50.37 (B2 model) FWID 01-00000000\n";
            memcpy(out, v, strlen(v) + 1), rlen = (uint16_t)(strlen(v) + 1);
        } else if (cmd == BCMW_C_SET_VAR && !strcmp(name, "clmload")) {
            uint16_t flag = (uint16_t)(data[0] | data[1] << 8);
            uint32_t n = (uint32_t)data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
            if ((flag & 2) != (clm_len == 0 ? 2 : 0) || (flag >> 12) != 1 || n + 12 != dlen || clm_len + n > sizeof(clm_got))
                status = (uint16_t)-1, M.fw_errors |= 32;
            else
                memcpy(clm_got + clm_len, data + 12, n), clm_len += n;
            if (flag & 4)
                M.clm_done = clm_len;
        } else if (cmd == BCMW_C_GET_VAR && !strcmp(name, "clmload_status")) {
            rlen = 4;
        } else if (!strcmp(name, "event_msgs")) {
            if (cmd == BCMW_C_SET_VAR)
                memcpy(M.event_mask, data, 16);
            rlen = cmd == BCMW_C_GET_VAR ? 16 : 0;
        } else if (!strcmp(name, "escan")) {
            if (dlen < 72 || (uint32_t)(data[0] | data[1] << 8) != 1)
                status = (uint16_t)-1;
            else
                scan = 1;
        } else if (!strcmp(name, "mpc")) {
        } else {
            status = (uint16_t)-23;     /* BCME_UNSUPPORTED */
        }
    } else if (cmd == BCMW_C_UP) {
        M.up = 1;
    } else {
        status = (uint16_t)-23;
    }
    if (outlen < rlen)
        rlen = outlen;
    if (!take_ctl(0, &pktid, &b, &blen)) {
        M.fw_errors |= 64;              /* no ioctl response buffer */
        return;
    }
    memcpy(b, out, rlen);
    put32le(it + 4, pktid);
    put16le(it + 8, status);
    put16le(it + 12, rlen);
    put16le(it + 14, trans);
    put32le(it + 16, cmd);
    d2h_put(RING_D2H_CONTROL, it, sizeof(it));
    if (scan) {
        uint8_t r[12 + 128];
        memset(r, 0, sizeof(r));
        put32le(r, sizeof(r));
        put32le(r + 4, 109);
        put16le(r + 10, 1);
        put32le(r + 12, 109);
        put32le(r + 16, 128);
        memcpy(r + 20, "\x02\x11\x22\x33\x44\x55", 6);
        r[12 + 18] = 7;
        memcpy(r + 12 + 19, "test-ap", 7);
        put16le(r + 12 + 72, 0x1006);
        put16le(r + 12 + 78, (uint16_t)-52);
        send_event(BCMW_E_ESCAN_RESULT, 8, r, sizeof(r));
        send_event(BCMW_E_ESCAN_RESULT, 0, r, 12);
        send_rx_frame();
    }
}

static void h2d_item(int r, uint8_t *it)
{
    uint8_t want_phase, want_epoch;

    if (ring[r].r == 0)
        ring[r].phase ^= 0x80;
    want_phase = ring[r].phase;
    want_epoch = (uint8_t)(ring[r].seq % 253);
    ring[r].seq++;
    if ((it[2] & 0x80) != want_phase || it[3] != want_epoch) {
        M.badoption++;
        ring_status((uint16_t)r, 3);
        return;
    }
    switch (it[0]) {
    case MSGBUF_TYPE_IOCTLRESP_BUF_POST:
    case MSGBUF_TYPE_EVENT_BUF_POST: {
        int ev = it[0] == MSGBUF_TYPE_EVENT_BUF_POST;
        if (nctl[ev] < 16) {
            ctlq[ev][nctl[ev]].pktid = (uint32_t)it[4] | it[5] << 8 | it[6] << 16 | (uint32_t)it[7] << 24;
            ctlq[ev][nctl[ev]].len = (uint16_t)(it[8] | it[9] << 8);
            ctlq[ev][nctl[ev]].bus = (uint64_t)((uint32_t)it[16] | it[17] << 8 | it[18] << 16 | (uint32_t)it[19] << 24) |
                                     (uint64_t)((uint32_t)it[20] | it[21] << 8 | it[22] << 16 | (uint32_t)it[23] << 24) << 32;
            nctl[ev]++;
        }
        break;
    }
    case MSGBUF_TYPE_RXBUF_POST:
        if (nrx < 300) {
            rxq[nrx].pktid = (uint32_t)it[4] | it[5] << 8 | it[6] << 16 | (uint32_t)it[7] << 24;
            rxq[nrx].len = (uint16_t)(it[10] | it[11] << 8);
            rxq[nrx].bus = (uint64_t)((uint32_t)it[24] | it[25] << 8 | it[26] << 16 | (uint32_t)it[27] << 24);
            nrx++;
        }
        break;
    case MSGBUF_TYPE_IOCTLPTR_REQ: {
        uint32_t cmd = (uint32_t)it[8] | it[9] << 8 | it[10] << 16 | (uint32_t)it[11] << 24;
        uint16_t trans = (uint16_t)(it[12] | it[13] << 8), inlen = (uint16_t)(it[14] | it[15] << 8);
        uint16_t outlen = (uint16_t)(it[16] | it[17] << 8);
        uint64_t bus = (uint64_t)((uint32_t)it[24] | it[25] << 8 | it[26] << 16 | (uint32_t)it[27] << 24);
        uint8_t *in = bus_ptr(bus, inlen ? inlen : 1);
        M.ioctls++;
        if (in && *(uint32_t *)(it + 4) == 0xFFFE)
            answer(it, trans, cmd, in, inlen, outlen);
        else
            M.fw_errors |= 128;
        break;
    }
    default:
        M.fw_errors |= 256;
    }
}

static void fw_run(void)
{
    if (!rings_known()) {
        M.fw_errors |= 512;
        return;
    }
    for (int r = 0; r < 2; r++) {
        uint16_t w = idx_rd(0, r);
        while (ring[r].r != w) {
            h2d_item(r, ring[r].buf + (uint32_t)ring[r].r * ring[r].size);
            if (++ring[r].r == ring[r].items)
                ring[r].r = 0;
        }
        idx_wr(1, r, ring[r].r);
    }
}
