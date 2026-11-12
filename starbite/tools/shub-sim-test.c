/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the sensor hub driver (starbite/sbos/kern/shub.c) against a simulated phone and chip, run by
 * starbite/sbos/build.sh on a native host:   shub-sim-test PATCH.bin [-v]
 * shub.c is built with -DSB_HOST: its registers, its SPI transfers and the patch come from here; everything above
 * that (the state machine, SSI, packets, the download, SSP, the samples, the restore) is the code the phone runs.
 *
 * The simulated chip speaks the protocol as docs/SENSOR-HUB.md describes it (so this proves the driver's logic and
 * its register discipline, not that the description is right): autobaud, sync, the version, RPC 0x05 writes into a
 * RAM image, RPC 0x2E, the application's sync, SSP reads / writes / data frames at 100 ms. Faults: no power domain,
 * no LDO37, a chip that never answers, a chip that only reads with 0x70, big-endian RPC fields, a lost
 * acknowledgement, a short write.
 * Checked: nothing at 0x13Axxxxx is touched when the domain or the supply is off; every register is back as found
 * after every run; no SSI debug-mode frame is ever sent; the probe sends no write RPC; no slice is longer than
 * 100 ms; the chip's RAM equals the patch after the download; the samples reach surv_heading / tilt / steps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "shub.h"
#include "shub_proto.h"
#include "survival.h"

static int fails, checks, verbose;
#define CHECK(c, ...)                                                                                             \
    do {                                                                                                         \
        ++checks;                                                                                                \
        if (!(c)) {                                                                                              \
            ++fails;                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

/* ---- time and the log ---- */
static uint64_t now_us = 5000000;
static char logbuf[1 << 18];
static unsigned logn, loglines;
uint64_t sb_ms(void) { return now_us / 1000; }
uint64_t sb_us(void) { return now_us; }
void sb_udelay(uint64_t us) { now_us += us; }
void sb_mdelay(unsigned ms) { now_us += (uint64_t)ms * 1000; }
void sb_log(const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (verbose)
        printf("  %7.2f %s\n", (double)now_us / 1e6, s);
    if (logn + n + 2 < sizeof(logbuf)) {
        memcpy(logbuf + logn, s, n);
        logn += n;
        logbuf[logn++] = '\n';
        logbuf[logn] = 0;
    }
    ++loglines;
}
void sb_logf(const char *a, const char *b)
{
    char t[512];
    snprintf(t, sizeof(t), "%s%s", a, b);
    sb_log(t);
}
void sb_logx(const char *a, uint64_t v)
{
    char t[512];
    snprintf(t, sizeof(t), "%s0x%08llx", a, (unsigned long long)v);
    sb_log(t);
}

/* ---- the scenario ---- */
static struct {
    int pd_off, ldo_off, dead, need70, big_endian, resp, lose_ack_at, short_write_at;
} cfg;

/* ---- the phone's registers ---- */
#define GPH0 0x13A80000UL
#define GPA0 0x14050020UL
#define GPA1 0x14050040UL
#define GPG0 0x10430080UL
#define CMU 0x13A00000UL
#define USI_MODE 0x13A13000UL
#define SPI 0x13AC0000UL
struct reg { uintptr_t a; uint32_t v, init; };
static struct reg regs[96];
static int nregs;
static unsigned chub_access, reg_writes, bus_not_up;

static struct reg *reg(uintptr_t a)
{
    for (int i = 0; i < nregs; ++i)
        if (regs[i].a == a)
            return &regs[i];
    if (nregs >= 96) {
        printf("shub sim: too many registers\n");
        exit(2);
    }
    regs[nregs].a = a;
    regs[nregs].v = regs[nregs].init = 0;
    return &regs[nregs++];
}
static void reg_init(uintptr_t a, uint32_t v)
{
    struct reg *r = reg(a);
    r->v = r->init = v;
}

static void chip_reset(void);
static void chip_tick(void);
static struct { int lost_ack_done, short_done, started; unsigned writes, bad_writes, acks_from_host; } flt;   /* kept over a chip reset */
static struct {
    int powered, autobaud, app, init_sent;
    struct shp_rx rx;
    uint8_t out[1 << 16];
    unsigned outn;
    uint8_t seq, rel, last_rx, last_rel_rx;
    uint8_t ram[0x70000];
    uint32_t next;
    unsigned rel_seen, wr_frames, violations, bad_ctl, ts_msgs, ssp_bad;
    int wpend, formation_ok, matrix_ok;
    uint8_t wcmd;
    unsigned wlen;
    int en[64];
    unsigned per[64], removes;              /* drivers track: shub_enable_type's periods and removals */
    uint64_t t_sample;
    unsigned ticks;
} chip;

uint32_t shub_sim_rd(uintptr_t a)
{
    now_us += 1;
    if (a >= 0x13A00000UL && a < 0x13B00000UL)
        ++chub_access;
    uint32_t v = reg(a)->v;
    if (a == 0x14064084UL)
        return cfg.pd_off ? 0 : 0xf;
    if (a == GPA1 + 4) {                    /* HOST_REQ: the chip has bytes */
        if (chip.powered && !cfg.dead)
            chip_tick();
        v = (v & ~0x80U) | (chip.powered && !cfg.dead && chip.outn ? 0x80U : 0);
    }
    if (a == GPH0 + 4)
        v = (v & ~0x14U) | (cfg.resp && (v & 0x20U) ? 0x10U : 0);
    return v;
}

void shub_sim_wr(uintptr_t a, uint32_t v)
{
    now_us += 1;
    ++reg_writes;
    if (a >= 0x13A00000UL && a < 0x13B00000UL)
        ++chub_access;
    if (a == GPA1 + 4)
        v &= ~0x80U;
    if (a == GPH0 + 4)
        v &= ~0x14U;
    if (a == GPG0 + 4) {
        int was = (reg(a)->v >> 2) & 1, is = (v >> 2) & 1;
        int out = ((reg(GPG0)->v >> 8) & 0xf) == 1;
        if (was && !is && out)
            chip_reset();
        chip.powered = is || !out;
    }
    if (a == GPG0) {                        /* NSTDBY's function: an output drives DAT, anything else floats high here */
        int out = ((v >> 8) & 0xf) == 1, lvl = (reg(GPG0 + 4)->v >> 2) & 1;
        if (out && !lvl && chip.powered)
            chip_reset();
        chip.powered = !out || lvl;
    }
    reg(a)->v = v;
}

int sb_probe32(uint64_t addr, uint32_t *v)
{
    *v = shub_sim_rd((uintptr_t)addr);
    return 1;
}

int sb_pmic_read(uint32_t type, uint32_t r, uint8_t *val)
{
    if (type != 1 || r != 0x6e)
        return 0;
    *val = cfg.ldo_off ? 0x2c : 0xec;
    return 1;
}

static void regs_fresh(void)
{
    nregs = 0;
    chub_access = reg_writes = bus_not_up = 0;
    reg_init(CMU + 0x100c, 0x00000001);
    reg_init(CMU + 0x1808, 0x00000000);
    reg_init(CMU + 0x2010, 0x00100000);     /* MANUAL, closed */
    reg_init(CMU + 0x2090, 0x00300000);     /* MANUAL, open */
    reg_init(CMU + 0x20a8, 0x10000000);     /* automatic */
    reg_init(CMU + 0x305c, 0x00000001);
    reg_init(USI_MODE, 0);
    reg_init(SPI + 0x00, 0x00000040);
    reg_init(SPI + 0x04, 0x00000700);
    reg_init(SPI + 0x08, 0x00000002);
    reg_init(SPI + 0x0c, 0x00000000);
    reg_init(SPI + 0x10, 0x00000003);
    reg_init(SPI + 0x20, 0x00010005);
    reg_init(SPI + 0x28, 0x00000005);
    reg_init(SPI + 0x2c, 0x00000002);
    reg_init(SPI + 0xc4, 0x00000001);
    reg_init(GPH0, 0x00222222);
    reg_init(GPH0 + 4, 0x00000008);
    reg_init(GPH0 + 8, 0x00111111);
    reg_init(GPA0, 0xfffff0ff);
    reg_init(GPA0 + 4, 0x00000058);
    reg_init(GPA0 + 8, 0x33333333);
    reg_init(GPA1, 0xf0000000);
    reg_init(GPA1 + 8, 0x30000000);
    reg_init(GPG0, 0x00000200);             /* NSTDBY in function 2, reading high, as an earlier port found it */
    reg_init(GPG0 + 4, 0x00000004);
    reg_init(GPG0 + 8, 0x00000100);
}

static int regs_as_found(void)
{
    int bad = 0;
    for (int i = 0; i < nregs; ++i)
        if (regs[i].v != regs[i].init && regs[i].a != SPI + 0x24) {
            ++bad;
            if (bad < 6)
                printf("  register %08lx: found %08x, left %08x\n", (unsigned long)regs[i].a, regs[i].init, regs[i].v);
        }
    return bad == 0;
}

/* ---- the chip ---- */
const uint8_t *shub_sim_patch;
unsigned shub_sim_patch_len;
static uint8_t patch_ram[0x70000];
static unsigned patch_ram_len;

static void chip_reset(void)
{
    int v = (int)chip.violations, b = (int)chip.bad_ctl;
    memset(&chip, 0, sizeof(chip));
    chip.violations = (unsigned)v;
    chip.bad_ctl = (unsigned)b;
    chip.last_rel_rx = 0xff;
    chip.next = 0x00800000;
    shp_rx_init(&chip.rx);
}

static void chip_out(struct shp_pkt *p)
{
    unsigned n = shp_pkt_build(chip.out + chip.outn, sizeof(chip.out) - chip.outn, p);
    if (!n) {
        printf("shub sim: the chip's output overflowed\n");
        exit(2);
    }
    chip.outn += n;
}

static void chip_plain(const uint8_t *pl, unsigned n)
{
    struct shp_pkt p = { 0 };
    p.seq = chip.seq++;
    p.payload = pl;
    p.len = (uint16_t)n;
    chip_out(&p);
}

static void chip_ssp_out(const uint8_t *ssp, unsigned n)
{
    uint8_t b[300], r[310];
    b[0] = (uint8_t)n;
    b[1] = (uint8_t)(n >> 8);
    memcpy(b + 2, ssp, n);
    unsigned k = shp_rpc_put(r, sizeof(r), SHP_RPC_SENSOR_RSP, b, n + 2);
    chip_plain(r, k);
}

static void chip_apply(uint8_t cmd, const uint8_t *d, unsigned n)
{
    if (cmd == SSP_CMD_FORMATION)
        chip.formation_ok = n == 3 && d[0] == 4 && d[1] == 4 && d[2] == 5;
    else if (cmd == SSP_CMD_MAG_MATRIX)
        chip.matrix_ok = n == 27 && d[0] == 0xd8 && d[26] == 0xb4;
    else if (cmd == SSP_CMD_ADD && n == 10 && d[0] < 64 && !d[5] && !d[6] && !d[7] && !d[8] && !d[9] &&
             ((d[0] != 0 && d[0] != 4 && d[0] != 18) || (d[1] == 100 && !d[2]))) {
        chip.en[d[0]] = 1;                  /* (the three of shub.c's own start at 100 ms; any period for others) */
        chip.per[d[0]] = (unsigned)d[1] | (unsigned)d[2] << 8 | (unsigned)d[3] << 16 | (unsigned)d[4] << 24;
    } else if (cmd == SSP_CMD_REMOVE && n == 5 && d[0] < 64) {
        chip.en[d[0]] = 0;                  /* Samsung's disable_sensor: [type][s32 period] */
        ++chip.removes;
    }
    else if (cmd == SSP_CMD_TIMESTAMP && n == 8)
        ++chip.ts_msgs;
    else
        ++chip.ssp_bad;
}

static void chip_ssp(const uint8_t *b, unsigned n)
{
    if (chip.wpend) {
        chip.wpend = 0;
        if (n != chip.wlen)
            ++chip.ssp_bad;
        chip_apply(chip.wcmd, b, n);
        return;
    }
    if (n < 9) {
        ++chip.ssp_bad;
        return;
    }
    uint8_t cmd = b[0];
    unsigned len = (unsigned)b[1] | (unsigned)b[2] << 8, opts = (unsigned)b[3] | (unsigned)b[4] << 8;
    uint8_t a[16] = { (uint8_t)opts, (uint8_t)(opts >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    if ((opts & 3) == SSP_READ) {
        uint64_t v = cmd == SSP_CMD_WHOAMI ? 0x55 : cmd == SSP_CMD_SCAN ? 0x1fff07f6bfULL : cmd == SSP_CMD_FW_REV ? 21031900 : 0;
        if (len > 8 || (cmd != SSP_CMD_WHOAMI && cmd != SSP_CMD_SCAN && cmd != SSP_CMD_FW_REV)) {
            ++chip.ssp_bad;
            return;
        }
        for (unsigned i = 0; i < len; ++i)
            a[4 + i] = (uint8_t)(v >> (8 * i));
        chip_ssp_out(a, 4 + len);
    } else if ((opts & 3) == SSP_WRITE) {
        if (n == 9 + len) {                 /* header and data in one go (the timestamp): no echo */
            chip_apply(cmd, b + 9, len);
            return;
        }
        chip.wpend = 1;
        chip.wcmd = cmd;
        chip.wlen = len;
        chip_ssp_out(a, 4);
    } else {
        ++chip.ssp_bad;
    }
}

static uint32_t rd32(const uint8_t *p, int be)
{
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static void chip_rpc(uint16_t id, const uint8_t *d, unsigned len)
{
    int be = cfg.big_endian;
    if (id == SHP_RPC_GET_VERSION) {
        uint8_t v[12], r[16];
        uint32_t w[3] = { 0x004775B0, chip.app ? 0x0006431f : 0x0004527c, chip.app ? 1 : 0 };
        for (int i = 0; i < 3; ++i)
            for (int b = 0; b < 4; ++b)
                v[4 * i + b] = (uint8_t)(w[i] >> (be ? 24 - 8 * b : 8 * b));
        unsigned k = shp_rpc_put(r, sizeof(r), SHP_RPC_VERSION, v, 12);
        struct shp_pkt p = { 0 };
        p.seq = chip.seq++;
        p.flags = SHP_F_RELIABLE;
        p.rel = chip.rel++;
        p.payload = r;
        p.len = (uint16_t)k;
        chip_out(&p);
    } else if (id == SHP_RPC_WRITE_MEM && !chip.app && len >= 6) {
        uint32_t addr = rd32(d, be);
        unsigned n = be ? (unsigned)d[4] << 8 | d[5] : (unsigned)d[5] << 8 | d[4];
        if (n != len - 6 || addr != chip.next || addr - 0x00800000 + n > sizeof(chip.ram)) {
            ++flt.bad_writes;
            return;
        }
        memcpy(chip.ram + (addr - 0x00800000), d + 6, n);
        chip.next += n;
        ++flt.writes;
    } else if (id == SHP_RPC_START && !chip.app && len == 4) {
        if (rd32(d, be) != 0x00800161 || chip.next - 0x00800000 != patch_ram_len ||
            memcmp(chip.ram, patch_ram, patch_ram_len)) {
            ++flt.bad_writes;
            return;
        }
        chip.app = 1;                       /* the patch runs: a new transport, waiting for autobaud and sync */
        flt.started = 1;
        chip.autobaud = 0;
        chip.outn = 0;
        chip.seq = chip.rel = chip.last_rx = 0;
        chip.last_rel_rx = 0xff;
    } else if (id == SHP_RPC_SENSOR_REQ && chip.app && len >= 2) {
        unsigned n = (unsigned)d[0] | (unsigned)d[1] << 8;
        if (n != len - 2)
            ++chip.ssp_bad;
        else
            chip_ssp(d + 2, n);
    } else {
        ++flt.bad_writes;
    }
}

static void chip_packet(const struct shp_pkt *p)
{
    if (p->flags & SHP_F_INTERNAL) {
        if (p->len >= 2 && p->payload[0] == 0) {
            uint8_t pl[8] = { 1, p->payload[1], chip.last_rx, 0, chip.last_rel_rx, chip.seq, chip.rel, 0xff };
            struct shp_pkt q = { 0 };
            q.flags = SHP_F_INTERNAL;
            q.payload = pl;
            q.len = 8;
            chip_out(&q);
            if (chip.app && !chip.init_sent) {
                static const uint8_t init[] = { 0x02, 0x00, 0x01, 0x00, 0x0A };
                chip.init_sent = 1;
                chip_ssp_out(init, sizeof(init));
            }
        }
        return;
    }
    chip.last_rx = p->seq;
    if (p->flags & SHP_F_ACK)
        ++flt.acks_from_host;
    if (p->flags & SHP_F_RELIABLE) {
        struct shp_pkt a = { 0 };
        int process = 0;
        a.seq = chip.seq++;
        a.flags = SHP_F_ACK | SHP_F_REL_ACK;
        a.ack = p->seq;
        a.rel_ack = p->rel;
        if (p->rel == (uint8_t)(chip.last_rel_rx + 1)) {
            chip.last_rel_rx = p->rel;
            process = 1;
            ++chip.rel_seen;
        } else if (p->rel != chip.last_rel_rx) {
            a.flags = SHP_F_ACK | SHP_F_REL_NACK;
            a.rel_nack = chip.last_rel_rx;
        }
        if (process && cfg.lose_ack_at && (int)chip.rel_seen == cfg.lose_ack_at && !flt.lost_ack_done)
            flt.lost_ack_done = 1;         /* processed, the acknowledgement lost on the way */
        else
            chip_out(&a);
        if (!process)
            return;
    }
    unsigned off = 0;
    uint16_t id, len;
    const uint8_t *d;
    int v;
    while ((v = shp_rpc_next(p->payload, p->len, &off, &id, &d, &len)) == 1)
        chip_rpc(id, d, len);
    if (v < 0)
        ++flt.bad_writes;
}

static void chip_tick(void)
{
    if (!chip.app || !chip.en[0] || now_us - chip.t_sample < 100000)
        return;
    chip.t_sample = now_us;
    ++chip.ticks;
    uint8_t f[80], n = 0;
    uint64_t ts = now_us * 1000;
    f[n++] = 0x02;
    f[n++] = 0x00;
    n += 2;                                 /* the frame's length, below */
    if (chip.ticks % 5 == 0) {
        f[n++] = 0xA8;
        memcpy(f + n, &ts, 8);
        n += 8;
    }
    static const uint8_t acc[] = { 0x37, 0x00, 0x1F, 0x00, 0xC7, 0xFF, 0xEC, 0x0F };
    memcpy(f + n, acc, sizeof(acc));
    n += sizeof(acc);
    memcpy(f + n, &ts, 8);
    n += 8;
    if (chip.en[4]) {
        static const uint8_t mag[] = { 0x37, 0x04, 0x00, 0x00, 0x90, 0x01, 0xD4, 0xFE, 0x03, 0x00 };  /* 0 400 -300 */
        memcpy(f + n, mag, sizeof(mag));
        n += sizeof(mag);
        memcpy(f + n, &ts, 8);
        n += 8;
    }
    if (chip.en[7]) {                       /* drivers track: the proximity, near, adc 900 */
        static const uint8_t px[] = { 0x37, 0x07, 0x01, 0x84, 0x03 };
        memcpy(f + n, px, sizeof(px));
        n += sizeof(px);
        memcpy(f + n, &ts, 8);
        n += 8;
    }
    if (chip.en[18] && chip.ticks % 10 == 0) {
        static const uint8_t st[] = { 0x37, 0x12, 0x02, 0x00, 0x00, 0x00 };
        memcpy(f + n, st, sizeof(st));
        n += sizeof(st);
        memcpy(f + n, &ts, 8);
        n += 8;
    }
    f[2] = (uint8_t)(n - 4);
    f[3] = 0;
    chip_ssp_out(f, n);
    if (chip.ticks % 7 == 0) {              /* a GNSS-side RPC in between: must be skipped by its length */
        static const uint8_t gn[] = { 0x19, 0x05, 'h', 'e', 'l', 'l', 'o' };
        chip_plain(gn, sizeof(gn));
    }
}

static void take(uint8_t *dst, unsigned k)
{
    memcpy(dst, chip.out, k);
    memmove(chip.out, chip.out + k, chip.outn - k);
    chip.outn -= k;
}

int shub_sim_xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
    now_us += 20 + (uint64_t)n * 8 * 1000000 / 5500000;
    memset(rx, 0, n);
    /* the bus must be up: SPI mode, out of reset, the gates open, mode 3, the pins muxed, chip select low */
    if (reg(USI_MODE)->v != 2 || reg(SPI + 0xc4)->v != 0 || !(reg(CMU + 0x2010)->v & 1U << 21) ||
        (reg(CMU + 0x1808)->v & 0xf) != 15 || (reg(SPI)->v & 0x1c) != 0x0c || (reg(GPH0)->v & 0x00ffffff) != 0x00101222 ||
        (reg(GPH0 + 4)->v & 8) || (reg(GPH0 + 8)->v & 0x00ffffff) != 0x00000100 || ((reg(GPA0 + 4)->v >> 5) & 1) != 1 ||
        ((reg(GPA0)->v >> 20) & 0xf) != 1 || ((reg(GPA1)->v >> 28) & 0xf) != 0 || ((reg(GPA1 + 8)->v >> 28) & 0xf) != 1)
        ++bus_not_up;
    if (tx[0] & 0x80) {                     /* debug mode: only the 36-byte autobaud burst is allowed */
        int burst = n == 36;
        for (unsigned i = 0; i < n; ++i)
            burst &= tx[i] == 0x80;
        if (!burst)
            ++chip.violations;
    }
    if (cfg.dead || !chip.powered)
        return 1;
    chip_tick();
    if (tx[0] == 0x80) {
        chip.autobaud = 1;
        shp_rx_init(&chip.rx);
        return 1;
    }
    if (tx[0] == SHP_SSI_WRITE) {
        unsigned m = (unsigned)tx[1] | (unsigned)tx[2] << 8, acc = m;
        if (n != m + 5) {
            ++chip.bad_ctl;
            return 1;
        }
        ++chip.wr_frames;
        if (cfg.short_write_at && (int)chip.wr_frames == cfg.short_write_at && !flt.short_done) {
            flt.short_done = 1;
            acc = m / 2;
        }
        unsigned pend = chip.outn, k = pend < m ? pend : m;
        rx[0] = 0x0b;
        rx[1] = (uint8_t)pend;
        rx[2] = (uint8_t)(pend >> 8);
        take(rx + 3, k);
        rx[3 + m] = (uint8_t)acc;
        rx[4 + m] = (uint8_t)(acc >> 8);
        if (chip.autobaud)
            for (unsigned i = 0; i < acc; ++i) {
                struct shp_pkt p;
                if (shp_rx_byte(&chip.rx, tx[3 + i], &p) > 0)
                    chip_packet(&p);
            }
        return 1;
    }
    if (tx[0] == SHP_SSI_READ_A || tx[0] == SHP_SSI_READ_B) {
        if (cfg.need70 && tx[0] == SHP_SSI_READ_A)
            return 1;                       /* this chip only knows 0x70: zeros */
        unsigned pend = chip.outn, k = pend < n - 3 ? pend : n - 3;
        rx[0] = 0x0b;
        rx[1] = (uint8_t)pend;
        rx[2] = (uint8_t)(pend >> 8);
        take(rx + 3, k);
        return 1;
    }
    ++chip.bad_ctl;
    return 1;
}

/* ---- running ---- */
static uint64_t slice_max;
/* drivers track: shub_more.c's hooks, as a recorder */
static struct { unsigned n7, len7, other, polls, adc; int near; } hook;
static void hook_sample(unsigned type, const uint8_t *d, unsigned n, uint64_t ts)
{
    (void)ts;
    if (type == 7) {
        ++hook.n7;
        hook.len7 = n;
        hook.near = d[0];
        hook.adc = (unsigned)d[1] | (unsigned)d[2] << 8;
    } else {
        ++hook.other;
    }
}
static void hook_poll(void) { ++hook.polls; }

static int run(int (*step)(void), unsigned max_ms)
{
    uint64_t end = now_us + (uint64_t)max_ms * 1000;
    while (now_us < end) {
        uint64_t t0 = now_us;
        int d = step();
        if (now_us - t0 > slice_max)
            slice_max = now_us - t0;
        if (d)
            return 1;
        now_us += (uint64_t)shub_wait_ms() * 1000;
    }
    return 0;
}

static void fresh(const char *name)
{
    memset(&cfg, 0, sizeof(cfg));
    regs_fresh();
    chip_reset();
    chip.violations = chip.bad_ctl = 0;
    chip.powered = 1;
    memset(&flt, 0, sizeof(flt));
    logn = loglines = 0;
    logbuf[0] = 0;
    slice_max = 0;
    if (verbose)
        printf("---- %s\n", name);
}

static void common(const char *name)
{
    CHECK(regs_as_found(), "%s: every register is back as found", name);
    CHECK(chip.violations == 0 && chip.bad_ctl == 0, "%s: no debug-mode frame, no unknown control byte (%u, %u)", name, chip.violations, chip.bad_ctl);
    CHECK(bus_not_up == 0, "%s: every transfer with the bus fully up (%u)", name, bus_not_up);
    CHECK(slice_max <= 100000, "%s: no slice over 100 ms (%llu us)", name, (unsigned long long)slice_max);
    CHECK(((reg(GPG0)->v >> 8) & 0xf) == 2, "%s: NSTDBY's pin is back in its function", name);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: shub-sim-test PATCH.bin [-v]\n");
        return 2;
    }
    verbose = argc > 2;
    static uint8_t blob[500000];
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("shub sim: cannot open %s\n", argv[1]);
        return 2;
    }
    shub_sim_patch_len = (unsigned)fread(blob, 1, sizeof(blob), f);
    fclose(f);
    shub_sim_patch = blob;
    {
        unsigned off = 0, len = 0;
        uint32_t addr = 0;
        const uint8_t *d = 0;
        while (shp_patch_next(blob, shub_sim_patch_len, &off, &addr, &d, &len) == 1) {
            memcpy(patch_ram + (addr - 0x00800000), d, len);
            patch_ram_len = addr - 0x00800000 + len;
        }
    }
    CHECK(shub_sim_patch_len == 413864 && patch_ram_len == 410700, "the patch blob (%u, %u)", shub_sim_patch_len, patch_ram_len);
    int h = 0, a = 0, p = 0, r = 0;
    uint32_t st = 0;
    CHECK(!surv_heading(&h, &a) && !surv_tilt(&p, &r) && !surv_steps(&st), "nothing valid before a start");

    fresh("the CHUB power domain off");
    cfg.pd_off = 1;
    shub_begin(1);
    CHECK(run(shub_step, 5000) && shub_state() == SHUB_FAILED, "power domain off: the probe stops");
    CHECK(chub_access == 0 && reg_writes == 0, "power domain off: nothing at 0x13Axxxxx touched, nothing written (%u, %u)", chub_access, reg_writes);
    CHECK(strstr(shub_why(), "power domain") != 0, "power domain off: the reason (%s)", shub_why());

    fresh("LDO37 off");
    cfg.ldo_off = 1;
    shub_begin(1);
    CHECK(run(shub_step, 5000) && shub_state() == SHUB_FAILED, "LDO37 off: the probe stops");
    CHECK(chub_access == 0 && reg_writes == 0, "LDO37 off: nothing at 0x13Axxxxx touched, nothing written (%u, %u)", chub_access, reg_writes);

    fresh("a chip that never answers");
    cfg.dead = 1;
    shub_begin(1);
    uint64_t t0 = now_us;
    CHECK(run(shub_step, 20000) && shub_state() == SHUB_FAILED, "dead chip: the probe gives up");
    CHECK(now_us - t0 < 8000000, "dead chip: within 8 s (%llu ms)", (unsigned long long)((now_us - t0) / 1000));
    CHECK(strstr(logbuf, "slower") != 0 && !strstr(logbuf, "(0x50: 0,") && !strstr(logbuf, "0x70: 0)"), "dead chip: both read forms and the slower clock were tried");
    CHECK(logn < 6000, "dead chip: the log stays small (%u bytes, %u lines)", logn, loglines);
    common("dead chip");

    fresh("the probe");
    cfg.resp = 1;
    shub_begin(1);
    CHECK(run(shub_step, 5000) && shub_state() == SHUB_OFF, "probe: done and left (%s)", shub_why());
    CHECK(strstr(logbuf, "ASIC 0x004775B0 ROM 0x0004527C") != 0 && strstr(logbuf, "little-endian") != 0, "probe: the version is logged");
    CHECK(strstr(logbuf, "MCU_RESP in 100 ms: rose") != 0, "probe: MCU_RESP seen");
    CHECK(flt.writes == 0 && flt.bad_writes == 0 && !flt.started, "probe: no write RPC, no start (%u, %u)", flt.writes, flt.bad_writes);
    CHECK(flt.acks_from_host >= 1, "probe: the version answer was acknowledged");
    CHECK(logn < 4500, "probe: the log stays small (%u bytes, %u lines)", logn, loglines);
    common("probe");
    printf("shub sim: the probe's log: %u lines, %u bytes\n", loglines, logn);

    fresh("a chip that reads only with 0x70");
    cfg.need70 = 1;
    shub_begin(1);
    CHECK(run(shub_step, 8000) && shub_state() == SHUB_OFF, "0x70 chip: the probe passes (%s)", shub_why());
    CHECK(strstr(logbuf, "read control 70") != 0, "0x70 chip: found with 0x70");
    common("0x70 chip");

    fresh("the full start, big-endian fields, a 0x70 chip");
    cfg.big_endian = 1;
    cfg.need70 = 1;
    shub_begin(0);
    t0 = now_us;
    CHECK(run(shub_step, 30000) && shub_state() == SHUB_RUNNING, "full start (big-endian): running (%s)", shub_why());
    CHECK(flt.writes == 452 && flt.bad_writes == 0 && flt.started && !memcmp(chip.ram, patch_ram, patch_ram_len),
          "full start: 452 writes, the chip's RAM is the patch (%u writes, %u refused)", flt.writes, flt.bad_writes);
    CHECK(chip.formation_ok && chip.matrix_ok && chip.en[0] && chip.en[4] && chip.en[18] && chip.ssp_bad == 0,
          "full start: formation, matrix and the three sensors arrived (%d %d %d %d %d, bad %u)", chip.formation_ok, chip.matrix_ok,
          chip.en[0], chip.en[4], chip.en[18], chip.ssp_bad);
    printf("shub sim: the full start took %llu ms of simulated time\n", (unsigned long long)((now_us - t0) / 1000));
    for (int i = 0; i < 150; ++i) {         /* 3 s of the kernel's polling */
        uint64_t s0 = now_us;
        shub_poll();
        if (now_us - s0 > slice_max)
            slice_max = now_us - s0;
        now_us += 20000;
    }
    CHECK(surv_heading(&h, &a) && (h < 100 || h > 3500) && a == 3, "running: a heading near north (%d, accuracy %d)", h, a);
    CHECK(surv_tilt(&p, &r) && p > -20 && p < 0 && r > 0 && r < 20, "running: nearly flat (%d %d)", p, r);
    CHECK(surv_steps(&st) && st >= 4 && st <= 8, "running: steps counted (%u)", st);
    CHECK(chip.ts_msgs >= 5, "running: the clock message after every third frame (%u)", chip.ts_msgs);
    /* drivers track: one more sensor through shub_enable_type, its samples through the hook */
    shub_set_hooks(hook_sample, hook_poll);
    CHECK(shub_starts() >= 1, "running: counted as a start (%u)", shub_starts());
    CHECK(shub_enable_type(7, 200, 1) == 0 && shub_enable_type(9, 200, 1) == -1, "enable: sent; a second waits for the slot");
    for (int i = 0; i < 50; ++i) {
        shub_poll();
        now_us += 20000;
    }
    CHECK(chip.en[7] && chip.per[7] == 200 && chip.ssp_bad == 0, "enable: the chip has the proximity at 200 ms (%d %u, bad %u)",
          chip.en[7], chip.per[7], chip.ssp_bad);
    CHECK(hook.n7 > 3 && hook.len7 == 3 && hook.near == 1 && hook.adc == 900 && hook.polls >= 50,
          "hook: proximity samples (%u, %u bytes, near %d adc %u), polls %u", hook.n7, hook.len7, hook.near, hook.adc, hook.polls);
    CHECK(hook.other == 0, "hook: the accelerometer / magnetometer / steps stay shub.c's (%u)", hook.other);
    CHECK(shub_enable_type(7, 200, 0) == 0, "disable: sent");
    for (int i = 0; i < 20; ++i) {
        shub_poll();
        now_us += 20000;
    }
    CHECK(!chip.en[7] && chip.removes == 1 && chip.ssp_bad == 0, "disable: the chip removed it (%u, bad %u)", chip.removes, chip.ssp_bad);
    CHECK(surv_heading(&h, &a) && surv_steps(&st), "the compass and the steps still run");
    shub_set_hooks(0, 0);
    shub_leave();
    CHECK(shub_enable_type(7, 200, 1) == -1, "enable refused when the hub is not running");
    CHECK(shub_state() == SHUB_OFF && !surv_heading(&h, &a) && !surv_steps(&st), "left: nothing valid any more");
    common("full start");

    /* the two self-test entry points as uitest.c runs them (each once: they keep their own state) */
    fresh("starbite-os-shubprobe.img");
    CHECK(run(shub_probe_selftest_step, 8000), "shubprobe self test: ends");
    CHECK(strstr(logbuf, "selftest: shubprobe: passed: ASIC 0x004775B0") != 0, "shubprobe self test: passed");
    common("shubprobe self test");

    fresh("starbite-os-shub.img, with a lost acknowledgement and a short write");
    cfg.lose_ack_at = 5;
    cfg.short_write_at = 9;
    CHECK(run(shub_selftest_step, 40000), "shub self test: ends");
    CHECK(strstr(logbuf, "selftest: shub: passed") != 0, "shub self test: passed");
    CHECK(strstr(logbuf, "retransmits 1, short writes 1") != 0, "shub self test: the lost acknowledgement and the short write were recovered");
    CHECK(flt.lost_ack_done && flt.short_done, "shub self test: both faults happened");
    CHECK(logn < 9000, "shub self test: the log stays small (%u bytes, %u lines)", logn, loglines);
    common("shub self test");
    printf("shub sim: the full self test's log: %u lines, %u bytes\n", loglines, logn);

    printf("shub sim test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
