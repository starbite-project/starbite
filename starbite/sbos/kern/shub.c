/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the sensor hub inside the BCM4775 B0 GNSS chip (docs/SENSOR-HUB.md; Samsung's GPL kernel, LineageOS
 * android_kernel_samsung_exynos9810, read for the register meanings, the SSI framing and the SSP layouts:
 * drivers/spi/spi-s3c64xx.c, drivers/sensorhub/brcm/bbdpl/bcm_gps_spi.c, ssp_bbd.c, ssp_dev.c).
 *
 * What this file touches, and nothing else:
 *   read only   PMU 0x14064084 (the CHUB power domain; anything but 0xF: stop before any 0x13Axxxxx access), the
 *               PMIC's LDO37 control (sb_pmic_read(1, 0x6E): off or unreadable: stop), CMU_CHUB's USI00 mux and
 *               Q-channel, gpa4-0, the pins' drive strengths.
 *   CMU_CHUB    0x13A00000: the USI00 divider (+0x1808, to 16) and its three gates (+0x2010, +0x2090, +0x20A8:
 *               CG_VAL set only where MANUAL holds the gate closed).
 *   USI         the mode word 0x13A13000 (2 = SPI), the controller 0x13AC0000 (USI_CON reset released, mode 3,
 *               8-bit, the FIFO polled 32 bytes at a time as Samsung's driver does without DMA).
 *   pins        gph0-0..2 the bus (function 2, MISO pulled down), gph0-3 chip select (a GPIO, idle high), gph0-4
 *               MCU_RESP (input), gph0-5 MCU_REQ (output), gpa1-7 HOST_REQ (input, pulled down), gpa0-5 AP_SLEEP
 *               (output high), gpg0-2 NSTDBY (output; low = the chip in standby = its reset).
 *   the chip    stream-mode SSI frames only (0x58 writes, 0x50 / 0x70 reads) and the 0x80 autobaud burst; never
 *               the SSI debug-mode register / memory forms. The probe sends no write RPC at all. The full start
 *               loads the patch into the chip's RAM (RPC 0x05), which is gone at the next standby.
 * Every register changed is recorded first and put back by shub_leave (NSTDBY last). Never the PMIC, never a
 * sound, never heat.
 *
 * Unknown until it has run on the phone (docs/SENSOR-HUB.md 4): the read control byte (0x50 first, then 0x70), the
 * ROM's RPC numbers and the byte order of their arguments (taken from the version answer), MCU_RESP (a silent one
 * is normal), the axes and the magnetometer's scale. */
#include "sb.h"
#include "shub.h"
#include "shub_proto.h"
#include "survival.h"

#define PMU_CHUB_STATUS 0x14064084UL
#define CMU 0x13A00000UL
#define CMU_MUX (CMU + 0x100cUL)
#define CMU_DIV (CMU + 0x1808UL)
#define CMU_QCH (CMU + 0x305cUL)
static const uintptr_t cmu_gate[3] = { CMU + 0x2010UL, CMU + 0x2090UL, CMU + 0x20a8UL };
#define GATE_CG_VAL (1U << 21)
#define GATE_MANUAL (1U << 20)
#define USI_MODE 0x13A13000UL
#define SPI 0x13AC0000UL
#define SPI_CH_CFG (SPI + 0x00UL)
#define SPI_CLK_CFG (SPI + 0x04UL)
#define SPI_MODE_CFG (SPI + 0x08UL)
#define SPI_SLAVE_SEL (SPI + 0x0cUL)
#define SPI_INT_EN (SPI + 0x10UL)
#define SPI_STATUS (SPI + 0x14UL)
#define SPI_TX_DATA (SPI + 0x18UL)
#define SPI_RX_DATA (SPI + 0x1cUL)
#define SPI_PACKET_CNT (SPI + 0x20UL)
#define SPI_PENDING_CLR (SPI + 0x24UL)
#define SPI_SWAP_CFG (SPI + 0x28UL)
#define SPI_FB_CLK (SPI + 0x2cUL)
#define USI_CON (SPI + 0xc4UL)
#define USI_OPTION (SPI + 0xc8UL)
#define CH_TX 1U
#define CH_RX 2U
#define CH_CPHA (1U << 2)
#define CH_CPOL (1U << 3)
#define CH_SLAVE (1U << 4)
#define CH_SW_RST (1U << 5)
#define CH_HS (1U << 6)
#define SPI_CHUNK 32U                       /* half the 64-byte FIFO */

#define GPH0 0x13A80000UL                   /* CHUB: 0..2 bus, 3 CS, 4 MCU_RESP, 5 MCU_REQ */
#define GPA0 0x14050020UL                   /* ALIVE: 5 AP_SLEEP */
#define GPA1 0x14050040UL                   /* ALIVE: 7 HOST_REQ */
#define GPA4 0x140500C0UL                   /* ALIVE: 0 SHUB_INT (looked at only) */
#define GPG0 0x10430080UL                   /* PERIC0: 2 NSTDBY */
#define P_CS 3U
#define P_RESP 4U
#define P_REQ 5U
#define P_APSLEEP 5U
#define P_HOSTREQ 7U
#define P_NSTDBY 2U
#define LDO37_CTRL 0x6eU

#ifdef SB_HOST
/* the host test (starbite/tools/shub-sim-test.c): the registers, the bus and the patch come from a simulated phone
 * and chip; everything above the register and transfer level is the code the phone runs */
uint32_t shub_sim_rd(uintptr_t a);
void shub_sim_wr(uintptr_t a, uint32_t v);
int shub_sim_xfer(const uint8_t *tx, uint8_t *rx, unsigned n);
extern const uint8_t *shub_sim_patch;
extern unsigned shub_sim_patch_len;
#define PATCH_P shub_sim_patch
#define PATCH_N shub_sim_patch_len
#else
extern const uint8_t shub_patch_start[], shub_patch_end[];     /* shub_blob.S: empty unless the build embeds it */
/* 1.2-alpha28: the patch is the image's own (the test image only) or the card's /STARBITE/FW/BCM4775.BIN, read once
 * and checked (413864 bytes, CRC-32 0x544D3BFA = the blob whose sha256 is b1a3e138...08d0). Never in the repository */
static const uint8_t *patch_p;
static unsigned patch_n;
#define PATCH_P patch_p
#define PATCH_N patch_n
#define SHUB_PATCH_LEN 413864U
#define SHUB_PATCH_CRC 0x544d3bfaU
#define SHUB_PATCH_CARD "/STARBITE/FW/BCM4775.BIN"
static const char *patch_why;
int kfile_read(const char *path, void *dst, uint32_t cap);   /* files.c (kern.h): the card, read only */
#define SHUB_EINVAL (-22)                   /* SBOS_EINVAL: no such file */
/* 1 the patch is at PATCH_P; else patch_why says why not */
static int patch_ready(void)
{
    if (patch_n == SHUB_PATCH_LEN)
        return 1;
    unsigned emb = (unsigned)(shub_patch_end - shub_patch_start);
    if (emb == SHUB_PATCH_LEN) {
        patch_p = shub_patch_start;
        patch_n = emb;
        return 1;
    }
    static uint8_t *buf;
    if (!buf)
        buf = sb_alloc(SHUB_PATCH_LEN + 16U);
    if (!buf) {
        patch_why = "no memory for the sensor hub's firmware (405 KiB)";
        return 0;
    }
    int n = kfile_read(SHUB_PATCH_CARD, buf, SHUB_PATCH_LEN + 16U);
    if (n == SHUB_EINVAL) {
        patch_why = "the sensor hub's firmware is not on the card (" SHUB_PATCH_CARD ")";
        return 0;
    }
    if (n < 0) {
        patch_why = "the card could not be read for the sensor hub's firmware";
        return 0;
    }
    if ((unsigned)n != SHUB_PATCH_LEN || sb_crc32(buf, SHUB_PATCH_LEN) != SHUB_PATCH_CRC) {
        sb_logx("shub: the card's firmware file is not the expected one: bytes ", (uint64_t)n);
        patch_why = "the sensor hub's firmware file on the card is not the expected one";
        return 0;
    }
    patch_p = buf;
    patch_n = SHUB_PATCH_LEN;
    sb_log("shub: the firmware patch from the card (" SHUB_PATCH_CARD ", CRC-32 checked)");
    return 1;
}
#endif

/* ---- registers ------------------------------------------------------------------------------------------------ */
#ifdef SB_HOST
static uint32_t rd(uintptr_t a) { return shub_sim_rd(a); }
static void wr(uintptr_t a, uint32_t v) { shub_sim_wr(a, v); }
#else
static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif

static void wr_if(uintptr_t a, uint32_t v)
{
    if (rd(a) != v)
        wr(a, v);
}

static int faulted;
/* a read that survives a synchronous abort (logged by the caller through `faulted`) */
static uint32_t pr(uintptr_t a)
{
    uint32_t v = 0;
    if (sb_probe32(a, &v))
        return v;
    ++faulted;
    return 0xdeadbeefU;
}

struct bank { uint32_t con, dat, pud; };
static void bank_save(uintptr_t b, struct bank *s)
{
    s->con = rd(b);
    s->dat = rd(b + 4);
    s->pud = rd(b + 8);
}
static void pin_fn(uintptr_t b, unsigned pin, uint32_t fn)
{
    wr_if(b, (rd(b) & ~(0xfU << (4 * pin))) | fn << (4 * pin));
}
static void pin_pud(uintptr_t b, unsigned pin, uint32_t pud)
{
    wr_if(b + 8, (rd(b + 8) & ~(0xfU << (4 * pin))) | pud << (4 * pin));
}
static void pin_set(uintptr_t b, unsigned pin, int high)
{
    uint32_t d = rd(b + 4);
    wr_if(b + 4, high ? d | 1U << pin : d & ~(1U << pin));
}
static int pin_get(uintptr_t b, unsigned pin)
{
    return (rd(b + 4) >> pin) & 1U;
}
/* the pins in `pins` back as recorded: level (outputs only: dat_pins), pull, function */
static void bank_restore(uintptr_t b, const struct bank *s, uint32_t pins, uint32_t dat_pins)
{
    uint32_t m = 0;
    for (unsigned p = 0; p < 8; ++p)
        if (pins & 1U << p)
            m |= 0xfU << (4 * p);
    wr_if(b + 4, (rd(b + 4) & ~dat_pins) | (s->dat & dat_pins));
    wr_if(b + 8, (rd(b + 8) & ~m) | (s->pud & m));
    wr_if(b, (rd(b) & ~m) | (s->con & m));
}

/* what was found, and which groups were changed since */
static struct {
    int clocks, usi, spi, pins, nstdby;
    uint32_t div, gate[3], usi_mode, usi_con;
    uint32_t ch, clk, mode, sel, inten, pcnt, swap, fb;
    struct bank gph0, gpa0, gpa1, gpg0;
} F;

/* ---- the state ------------------------------------------------------------------------------------------------ */
enum {
    ST_IDLE = 0, ST_LOOK, ST_BUSUP, ST_STDBY_LO, ST_STDBY_HI, ST_RESP, ST_AUTOBAUD, ST_SYNC, ST_VER, ST_VER_WAIT,
    ST_DL, ST_START, ST_SSP_INIT, ST_SSP, ST_RUN, ST_LEAVE, ST_DONE
};
static struct {
    int st, state, probe, verbose, app;     /* app: the patch runs (the second sync / version) */
    unsigned wait;
    const char *why;
    uint64_t t;
    int n, round, sync_try, polls, slow, ver_try;
    int resp_seen;
    unsigned dl_off, dl_recs;
    uint64_t dl_t0;
    int ssp_i, ssp_tries;
    int pass;                               /* the probe: the ASIC id was read */
} M;

static struct {
    struct shp_rx rx;
    uint8_t tx_seq, tx_rel, rdctl;
    int ack_due, ack_rel_flag;
    uint8_t ack_seq, ack_rel;
    int have_rel;
    uint8_t last_rel, last_rel_crc;
    uint16_t last_rel_len;
    unsigned unacked;
    int sync_ok, ver_seen, ver_ok;
    struct shp_sync sync;
    struct shp_version ver;
    uint8_t ver_raw[12];
    int rel_wait, rel_acked, rel_nacked, rel_tries;
    uint8_t rel_seq, nack_detail;
    uint64_t rel_t, t_read;
    unsigned reln, relpn;
    uint32_t reads, read_bytes, writes, fc_short, spi_err, pe, retx, rpc_other, pkts;
    int rd_proven;                          /* a read with this control byte returned stream bytes */
    uint32_t reads_a, reads_b;
    uint8_t last_st, last_wst;
    uint16_t last_len, last_wn, last_wacc;
    int dump_left, pkt_log_left, err_log_left, rpc_log_left;
} L;
static uint8_t txf[4200], rxf[4200], pktb[SHP_STUFFED_MAX], relb[SHP_STUFFED_MAX], relp[1100];

static struct shp_ssp_rx SR;
static struct {
    int busy, type, done, go, ts_due;
    uint8_t cmd, data[32], res[16];
    unsigned dlen, rlen;
    uint64_t t;
    uint32_t frames, ssp_err, echo_stray;
    int init_done;
} C;

static struct {
    int32_t a[3], m[3];                     /* filtered, x16 */
    int16_t ra[3], rm[3];                   /* the last raw samples */
    uint8_t macc, movf;
    int have_a, have_m;
    uint64_t t_a, t_m;
    uint32_t na, nm, ns, steps, other;
} D;

static void fail(const char *why)
{
    M.why = why;
    M.state = SHUB_FAILED;
    sb_logf("shub: FAILED: ", why);
    M.st = ST_LEAVE;
    M.wait = 20;
}

static void hexdump(const char *pre, const uint8_t *p, unsigned n, unsigned max)
{
    for (unsigned i = 0; i < n && i < max; i += 32) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, pre);
        for (unsigned k = i; k < n && k < max && k < i + 32U; ++k) {
            ub_c(&u, ' ');
            ub_hexn(&u, p[k], 2);
        }
        if (i + 32U >= max && n > max)
            ub_s(&u, " ...");
        sb_log(u.b);
    }
}

/* ---- the SPI controller, polled ------------------------------------------------------------------------------- */
static void spi_flush(void)
{
#ifdef SB_HOST
    if (PATCH_N != ~0U)
        return;
#endif
    uint32_t v = rd(SPI_CH_CFG) & ~(CH_RX | CH_TX);
    wr(SPI_CH_CFG, v);
    wr(SPI_PACKET_CNT, 0);
    wr(SPI_CH_CFG, (v | CH_SW_RST) & ~CH_HS);
    uint64_t t0 = sb_us();
    while (((rd(SPI_STATUS) >> 6) & 0x7fU) && sb_us() - t0 < 1000U)
        ;
    for (unsigned i = 0; i < 128U && ((rd(SPI_STATUS) >> 15) & 0x7fU); ++i)
        (void)*(volatile uint8_t *)SPI_RX_DATA;
    wr(SPI_CH_CFG, rd(SPI_CH_CFG) & ~CH_SW_RST);
    wr(SPI_MODE_CFG, rd(SPI_MODE_CFG) & ~6U);
}

/* n bytes out and in at once, chip select untouched: the FIFO filled, the exact clock count requested, the answer
 * read back, the FIFO flushed (spi-s3c64xx.c's enable_datapath / wait_for_xfer / flush_fifo without DMA) */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
#ifdef SB_HOST
    if (PATCH_N != ~0U)
        return shub_sim_xfer(tx, rx, n);
#endif
    while (n) {
        unsigned c = n > SPI_CHUNK ? SPI_CHUNK : n;
        uint32_t ch = rd(SPI_CH_CFG) & ~(CH_RX | CH_TX);
        wr(SPI_PACKET_CNT, rd(SPI_PACKET_CNT) & ~(1U << 16));
        wr(SPI_PACKET_CNT, c);
        wr(SPI_PACKET_CNT, rd(SPI_PACKET_CNT) | 1U << 16);
        wr(SPI_MODE_CFG, rd(SPI_MODE_CFG) & ~7U);
        for (unsigned i = 0; i < c; ++i)
            *(volatile uint8_t *)SPI_TX_DATA = tx[i];
        __asm__ volatile("dsb sy" ::: "memory");
        wr(SPI_CH_CFG, ch | CH_RX | CH_TX);
        uint64_t t0 = sb_us();
        uint32_t st;
        while ((((st = rd(SPI_STATUS)) >> 15) & 0x7fU) < c) {
            if (sb_us() - t0 > 20000U) {
                ++L.spi_err;
                if (L.err_log_left > 0) {
                    --L.err_log_left;
                    sb_logx("shub: SPI: the bytes did not come back in 20 ms (the clock?): STATUS ", st);
                }
                spi_flush();
                return 0;
            }
        }
        for (unsigned i = 0; i < c; ++i)
            rx[i] = *(volatile uint8_t *)SPI_RX_DATA;
        spi_flush();
        tx += c;
        rx += c;
        n -= c;
    }
    return 1;
}

static int cs_xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
    pin_set(GPH0, P_CS, 0);
    sb_udelay(5);
    int ok = spi_xfer(tx, rx, n);
    sb_udelay(5);
    pin_set(GPH0, P_CS, 1);
    sb_udelay(10);
    return ok;
}

static int host_req(void)
{
    return pin_get(GPA1, P_HOSTREQ);
}

/* ---- SSP: what the hub sends ---------------------------------------------------------------------------------- */
static void sample(const struct shp_rec *r)
{
    uint64_t now = sb_ms();
    if (r->sensor == SSP_SENSOR_ACCEL || r->sensor == SSP_SENSOR_MAG) {
        int mag = r->sensor == SSP_SENSOR_MAG;
        int32_t *f = mag ? D.m : D.a;
        int16_t *raw = mag ? D.rm : D.ra;
        int v[3] = { r->x, r->y, r->z };
        for (int i = 0; i < 3; ++i) {
            raw[i] = (int16_t)v[i];
            f[i] = (mag ? D.have_m : D.have_a) ? f[i] + (v[i] * 16 - f[i]) / 4 : v[i] * 16;
        }
        if (mag) {
            D.have_m = 1;
            D.t_m = now;
            D.macc = r->accuracy;
            D.movf = r->overflow;
            ++D.nm;
        } else {
            D.have_a = 1;
            D.t_a = now;
            ++D.na;
        }
    } else if (r->sensor == SSP_SENSOR_STEPS) {
        D.steps += r->steps;
        ++D.ns;
    } else {
        ++D.other;
    }
}

static void frame(const uint8_t *f, unsigned n)
{
    struct shp_rec r;
    unsigned off = 0;
    int v;
    ++C.frames;
    if (C.frames % 3U == 0)
        C.ts_due = 1;                       /* Samsung sends its clock after every third frame */
    while ((v = shp_frame_next(f, n, &off, &r)) == 1) {
        if (r.kind == SSP_REC_SENSOR)
            sample(&r);
        else if (r.kind == SSP_REC_INIT_DONE)
            C.init_done = 1;
    }
    if (v < 0 && L.rpc_log_left > 0) {
        --L.rpc_log_left;
        sb_logx("shub: a data frame record of unknown length (the rest of the frame dropped), type / offset ",
                (uint64_t)f[off < n ? off : n - 1U] << 16 | off);
        hexdump("shub: frame:", f, n, 64);
    }
}

static void ssp_bytes(const uint8_t *p, unsigned n)
{
    while (n) {
        unsigned used = 0;
        int ev = shp_ssp_feed(&SR, p, n, &used);
        p += used;
        n -= used;
        if (ev == SSP_EV_READ) {
            C.rlen = SR.n < sizeof(C.res) ? SR.n : sizeof(C.res);
            memcpy(C.res, SR.buf, C.rlen);
            if (C.busy && C.type == (int)SSP_READ)
                C.done = 1;
        } else if (ev == SSP_EV_WRITE_GO) {
            if (C.busy && C.type == (int)SSP_WRITE && !C.done && !C.go)
                C.go = 1;                   /* the data phase goes out from link_service, not from here */
            else
                ++C.echo_stray;
        } else if (ev == SSP_EV_FRAME) {
            frame(SR.buf, SR.n);
        } else if (ev == SSP_EV_ERR) {
            ++C.ssp_err;
            if (L.err_log_left > 0) {
                --L.err_log_left;
                sb_logx("shub: SSP: a header that fits nothing (the stream reset), options ", SR.opts);
            }
        } else if (!used) {
            break;
        }
    }
}

/* ---- packets in ----------------------------------------------------------------------------------------------- */
static void on_packet(const struct shp_pkt *p)
{
    ++L.pkts;
    if (L.pkt_log_left > 0) {
        struct ub u;
        --L.pkt_log_left;
        ub_init(&u);
        ub_s(&u, "shub: packet: seq ");
        ub_hexn(&u, p->seq, 2);
        ub_s(&u, " flags ");
        ub_hexn(&u, p->flags, 3);
        ub_s(&u, " ack ");
        ub_hexn(&u, p->ack, 2);
        ub_s(&u, " rel ");
        ub_hexn(&u, p->rel, 2);
        ub_s(&u, " rel-ack ");
        ub_hexn(&u, p->rel_ack, 2);
        ub_s(&u, " payload ");
        ub_dec(&u, p->len);
        ub_c(&u, ':');
        for (unsigned i = 0; i < p->len && i < 24U; ++i) {
            ub_c(&u, ' ');
            ub_hexn(&u, p->payload[i], 2);
        }
        sb_log(u.b);
    }
    if ((p->flags & SHP_F_REL_ACK) && L.rel_wait && p->rel_ack == L.rel_seq)
        L.rel_acked = 1;
    if ((p->flags & SHP_F_REL_NACK) && L.rel_wait) {
        L.rel_nacked = 1;
        L.nack_detail = p->rel_nack;
    }
    if (p->flags & SHP_F_RELIABLE) {
        int dup = L.have_rel && p->rel == L.last_rel && p->crc == L.last_rel_crc && p->len == L.last_rel_len;
        L.ack_due = 1;                      /* every reliable packet is answered at once: ACK | RELIABLE_ACK */
        L.ack_rel_flag = 1;
        L.ack_rel = p->rel;
        L.ack_seq = p->seq;
        if (dup)
            return;                         /* our acknowledgement was lost: answered again, not processed twice */
        L.have_rel = 1;
        L.last_rel = p->rel;
        L.last_rel_crc = p->crc;
        L.last_rel_len = p->len;
    } else if (p->len && ++L.unacked > 200U) {
        L.unacked = 0;                      /* plain packets: a bare ACK after 200 of them */
        L.ack_due = 1;
        L.ack_seq = p->seq;
    }
    if (p->flags & SHP_F_INTERNAL) {
        if (shp_sync_parse(p, &L.sync))
            L.sync_ok = 1;
        return;
    }
    unsigned off = 0;
    uint16_t id = 0, len = 0;
    const uint8_t *d = 0;
    int v;
    while ((v = shp_rpc_next(p->payload, p->len, &off, &id, &d, &len)) == 1) {
        if (id == SHP_RPC_VERSION && len >= 12U) {
            memcpy(L.ver_raw, d, 12);
            L.ver_ok = shp_version_parse(d, len, &L.ver);
            L.ver_seen = 1;
        } else if (id == SHP_RPC_SENSOR_RSP && len >= 2U) {
            unsigned n = (unsigned)d[0] | (unsigned)d[1] << 8;
            ssp_bytes(d + 2, n < len - 2U ? n : len - 2U);
        } else {
            ++L.rpc_other;                  /* GNSS-side RPCs (log text, exceptions): skipped by length */
            if (L.rpc_log_left > 0) {
                --L.rpc_log_left;
                sb_logx("shub: another RPC (skipped), id << 16 | length ", (uint64_t)id << 16 | len);
            }
        }
    }
    if (v < 0 && L.err_log_left > 0) {
        --L.err_log_left;
        sb_log("shub: a packet whose RPCs do not add up (dropped)");
    }
}

static void rx_bytes(const uint8_t *p, unsigned n)
{
    struct shp_pkt k;
    L.read_bytes += n;
    for (unsigned i = 0; i < n; ++i)
        if (shp_rx_byte(&L.rx, p[i], &k) > 0)
            on_packet(&k);
}

/* ---- SSI ------------------------------------------------------------------------------------------------------ */
static int ssi_write(const uint8_t *d, unsigned m)
{
    static uint8_t rest[SHP_STUFFED_MAX];
    for (int tries = 0; tries < 4 && m; ++tries) {
        unsigned n = shp_ssi_wframe(txf, sizeof(txf), d, m);
        struct shp_ssi_w w;
        if (!n || !cs_xfer(txf, rxf, n))
            return 0;
        ++L.writes;
        shp_ssi_wparse(rxf, m, &w);
        L.last_wst = w.status;
        L.last_wn = (uint16_t)(rxf[1] | rxf[2] << 8);
        L.last_wacc = (uint16_t)w.accepted;
        if (w.status & SHP_SSI_ST_PE)
            ++L.pe;
        if (w.status != 0xffU || w.n != m)  /* (a dead bus reads all ones: no stream bytes in that) */
            rx_bytes(w.chip, w.n);
        if (w.accepted >= m)
            return 1;
        ++L.fc_short;                       /* the chip's ring was full: the rest again, a moment later */
        m -= w.accepted;
        memcpy(rest, d + w.accepted, m);
        d = rest;
        sb_udelay(1000);
    }
    return m == 0;
}

/* one read: the length first, then the bytes. The count of stream bytes, -1 on a bus error */
static int ssi_read(void)
{
    uint8_t h[3] = { L.rdctl, 0, 0 };
    if (!cs_xfer(h, rxf, 3))
        return -1;
    ++L.reads;
    L.t_read = sb_ms();
    uint8_t st = rxf[0];
    unsigned len = (unsigned)rxf[1] | (unsigned)rxf[2] << 8;
    int dead = (st == 0 && len == 0) || (st == 0xffU && len == 0xffffU);
    uint8_t ctl = L.rdctl;
    if (ctl == SHP_SSI_READ_A)
        ++L.reads_a;
    else
        ++L.reads_b;
    /* which read form the chip wants is not known (docs/SENSOR-HUB.md 2.3): 0x50 first; while no read has ever
     * returned stream bytes, a read that comes back all zeros or all ones makes the next one use the other form.
     * (A sync answer can arrive inside a write frame, so a good sync does not prove the read form.) */
    if (dead && !L.rd_proven)
        L.rdctl = ctl == SHP_SSI_READ_A ? SHP_SSI_READ_B : SHP_SSI_READ_A;
    if (M.verbose && L.dump_left > 0 && (st != L.last_st || len != L.last_len)) {
        struct ub u;
        --L.dump_left;
        ub_init(&u);
        ub_s(&u, "shub: read (control ");
        ub_hexn(&u, ctl, 2);
        ub_s(&u, "): status ");
        ub_hexn(&u, st, 2);
        ub_s(&u, " length ");
        ub_dec(&u, len);
        ub_s(&u, ", HOST_REQ ");
        ub_dec(&u, (uint64_t)host_req());
        sb_log(u.b);
    }
    L.last_st = st;
    L.last_len = (uint16_t)len;
    if (st & SHP_SSI_ST_PE)
        ++L.pe;
    if (!len || dead)
        return 0;
    if (len > sizeof(rxf) - 3U)
        len = sizeof(rxf) - 3U;             /* the rest waits for the next read */
    memset(txf, 0, len + 3U);
    txf[0] = ctl;
    if (!cs_xfer(txf, rxf, len + 3U))
        return -1;
    unsigned n = (unsigned)rxf[1] | (unsigned)rxf[2] << 8;
    if (n > len)
        n = len;
    if (M.verbose && L.dump_left > 0) {
        --L.dump_left;
        hexdump("shub: read:", rxf, n + 3U, 64);
    }
    if (n)
        L.rd_proven = 1;
    rx_bytes(rxf + 3, n);
    return (int)n;
}

static int plain_send(const uint8_t *pl, unsigned n)
{
    struct shp_pkt p = { 0 };
    p.seq = L.tx_seq++;
    p.payload = pl;
    p.len = (uint16_t)n;
    unsigned k = shp_pkt_build(pktb, sizeof(pktb), &p);
    return k && ssi_write(pktb, k);
}

static int rel_build(void)
{
    struct shp_pkt p = { 0 };
    p.seq = L.tx_seq++;
    p.flags = SHP_F_RELIABLE;
    p.rel = L.tx_rel;
    p.payload = relp;
    p.len = (uint16_t)L.relpn;
    L.rel_seq = p.rel;
    L.reln = shp_pkt_build(relb, sizeof(relb), &p);
    return L.reln != 0;
}

/* a reliable packet with the payload in relp[0..n): sent, then rel_poll until it is acknowledged */
static int rel_send(unsigned n)
{
    L.relpn = n;
    if (!rel_build())
        return 0;
    L.rel_wait = 1;
    L.rel_acked = L.rel_nacked = 0;
    L.rel_tries = 1;
    L.rel_t = sb_ms();
    return ssi_write(relb, L.reln);
}

/* the answers this side owes: the acknowledgement of a reliable packet, a write's data phase, the clock */
static void link_service(void)
{
    for (int i = 0; i < 4 && (L.ack_due || C.go || C.ts_due); ++i) {
        if (L.ack_due) {
            struct shp_pkt p = { 0 };
            p.seq = L.tx_seq++;
            p.flags = SHP_F_ACK | (L.ack_rel_flag ? SHP_F_REL_ACK : 0U);
            p.ack = L.ack_seq;
            p.rel_ack = L.ack_rel;
            L.ack_due = L.ack_rel_flag = 0;
            unsigned k = shp_pkt_build(pktb, sizeof(pktb), &p);
            if (k)
                (void)ssi_write(pktb, k);
        }
        if (C.go) {
            uint8_t w[48];
            unsigned k = shp_ssp_wrap(w, sizeof(w), C.data, C.dlen);
            C.go = 0;
            if (k && plain_send(w, k))
                C.done = 1;
        }
        if (C.ts_due && !C.busy) {            /* (never between a write's header and its data) */
            uint8_t m[SSP_HDR + 8], w[32];
            uint64_t ns = sb_us() * 1000U;
            C.ts_due = 0;
            shp_ssp_hdr(m, SSP_CMD_TIMESTAMP, 8, SSP_WRITE, 0);
            for (int b = 0; b < 8; ++b)
                m[SSP_HDR + b] = (uint8_t)(ns >> (8 * b));
            unsigned k = shp_ssp_wrap(w, sizeof(w), m, sizeof(m));
            if (k)
                (void)plain_send(w, k);
        }
    }
}

/* 1 acknowledged, 0 not yet, -1 given up (10 tries, 500 ms apart) */
static int rel_poll(void)
{
    (void)ssi_read();
    link_service();
    if (!L.rel_wait)
        return 1;
    if (L.rel_nacked && L.nack_detail == L.rel_seq)
        L.rel_acked = 1;                    /* the chip already has it */
    if (L.rel_acked) {
        L.rel_wait = 0;
        ++L.tx_rel;
        return 1;
    }
    if (L.rel_nacked || sb_ms() - L.rel_t >= 500U) {
        if (L.rel_tries >= 10)
            return -1;
        if (L.rel_nacked) {                 /* our numbering is off: the chip says which one it has */
            L.tx_rel = (uint8_t)(L.nack_detail + 1U);
            if (!rel_build())
                return -1;
        }
        L.rel_nacked = 0;
        ++L.rel_tries;
        ++L.retx;
        L.rel_t = sb_ms();
        (void)ssi_write(relb, L.reln);
    }
    return 0;
}

/* ---- step 0: look only ---------------------------------------------------------------------------------------- */
static void log_bank(struct ub *u, const char *name, uintptr_t b)
{
    ub_s(u, name);
    ub_s(u, " CON ");
    ub_hexn(u, pr(b), 8);
    ub_s(u, " DAT ");
    ub_hexn(u, pr(b + 4), 2);
    ub_s(u, " PUD ");
    ub_hexn(u, pr(b + 8), 8);
    ub_s(u, " DRV ");
    ub_hexn(u, pr(b + 12), 8);
}

static int look(void)
{
    struct ub u;
    uint8_t ldo = 0;
    faulted = 0;
    uint32_t pd = pr(PMU_CHUB_STATUS);
    int ldo_ok = sb_pmic_read(1, LDO37_CTRL, &ldo);
    ub_init(&u);
    ub_s(&u, "shub: step 0 (look only): CHUB power domain (PMU 0x14064084) ");
    ub_hexn(&u, pd, 8);
    ub_s(&u, (pd & 0xfU) == 0xfU ? " = on" : " = NOT on");
    ub_s(&u, "; LDO37 (PMIC 0x6E) ");
    if (ldo_ok)
        ub_hexn(&u, ldo, 2);
    else
        ub_s(&u, "did not read");
    ub_s(&u, ldo_ok && ldo == 0xecU ? " = on, 1.8 V" : ldo_ok && (ldo >> 6) ? " = on (not the expected 0xEC)" : " = OFF or unknown");
    sb_log(u.b);
    if (M.verbose) {                        /* the ALIVE and PERIC0 pins: outside the CHUB block, always readable */
        ub_init(&u);
        ub_s(&u, "shub: step 0: ");
        log_bank(&u, "gpa0 (5 AP_SLEEP)", GPA0);
        log_bank(&u, "; gpa1 (7 HOST_REQ)", GPA1);
        sb_log(u.b);
        ub_init(&u);
        ub_s(&u, "shub: step 0: ");
        log_bank(&u, "gpa4 (0 SHUB_INT)", GPA4);
        log_bank(&u, "; gpg0 (2 NSTDBY)", GPG0);
        sb_log(u.b);
    }
    if ((pd & 0xfU) != 0xfU || faulted) {
        fail("the CHUB power domain is not on (PMU 0x14064084 & 0xF is not 0xF): nothing at 0x13Axxxxx was touched");
        return 0;
    }
    if (!ldo_ok || !(ldo >> 6)) {
        fail("the hub's supply LDO37 is off or did not read (PMIC 0x6E): nothing at 0x13Axxxxx was touched");
        return 0;
    }
    ub_init(&u);
    ub_s(&u, "shub: step 0: CMU_CHUB USI00: mux (+100C) ");
    ub_hexn(&u, pr(CMU_MUX), 8);
    ub_s(&u, " div (+1808) ");
    ub_hexn(&u, pr(CMU_DIV), 8);
    ub_s(&u, " gates (+2010 +2090 +20A8) ");
    for (int i = 0; i < 3; ++i) {
        ub_hexn(&u, pr(cmu_gate[i]), 8);
        ub_c(&u, ' ');
    }
    ub_s(&u, "qch (+305C) ");
    ub_hexn(&u, pr(CMU_QCH), 8);
    ub_s(&u, "; USI mode word (0x13A13000) ");
    ub_hexn(&u, pr(USI_MODE), 8);
    sb_log(u.b);
    ub_init(&u);
    ub_s(&u, "shub: step 0: ");
    log_bank(&u, "gph0 (0-2 bus, 3 CS, 4 MCU_RESP, 5 MCU_REQ)", GPH0);
    sb_log(u.b);
    if (faulted) {
        fail("a CHUB register read faulted (step 0): nothing was changed");
        return 0;
    }
    return 1;
}

/* ---- step 1: the bus up --------------------------------------------------------------------------------------- */
static int busup(void)
{
    struct ub u;
    /* the clocks: record, open the gates MANUAL holds closed, divider 16 */
    F.div = rd(CMU_DIV);
    for (int i = 0; i < 3; ++i)
        F.gate[i] = rd(cmu_gate[i]);
    F.clocks = 1;
    for (int i = 0; i < 3; ++i)
        if ((F.gate[i] & GATE_MANUAL) && !(F.gate[i] & GATE_CG_VAL))
            wr(cmu_gate[i], F.gate[i] | GATE_CG_VAL);
    if ((F.div & 0xfU) != 15U) {
        wr(CMU_DIV, (F.div & ~0xfU) | 15U);
        uint64_t t0 = sb_us();
        while ((rd(CMU_DIV) & (1U << 16)) && sb_us() - t0 < 2000U)
            ;
    }
    /* USI_CHUB00 as SPI, out of its reset */
    F.usi_mode = rd(USI_MODE);
    F.usi = 1;
    wr_if(USI_MODE, 2);
    faulted = 0;
    F.usi_con = pr(USI_CON);
    if (faulted) {
        fail("the SPI controller at 0x13AC0000 did not answer a read (step 1)");
        return 0;
    }
    F.ch = rd(SPI_CH_CFG);
    F.clk = rd(SPI_CLK_CFG);
    F.mode = rd(SPI_MODE_CFG);
    F.sel = rd(SPI_SLAVE_SEL);
    F.inten = rd(SPI_INT_EN);
    F.pcnt = rd(SPI_PACKET_CNT);
    F.swap = rd(SPI_SWAP_CFG);
    F.fb = rd(SPI_FB_CLK);
    F.spi = 1;
    ub_init(&u);
    ub_s(&u, "shub: step 1: the controller as found: CH_CFG ");
    ub_hexn(&u, F.ch, 8);
    ub_s(&u, " CLK_CFG ");
    ub_hexn(&u, F.clk, 8);
    ub_s(&u, " MODE_CFG ");
    ub_hexn(&u, F.mode, 8);
    ub_s(&u, " SLAVE_SEL ");
    ub_hexn(&u, F.sel, 8);
    ub_s(&u, " STATUS ");
    ub_hexn(&u, rd(SPI_STATUS), 8);
    ub_s(&u, " USI_CON ");
    ub_hexn(&u, F.usi_con, 8);
    ub_s(&u, " USI_OPTION ");
    ub_hexn(&u, rd(USI_OPTION), 8);
    sb_log(u.b);
    wr_if(USI_CON, 0);                      /* USI_RESET is active high and set after a reset */
    /* spi-s3c64xx.c's hwinit and config: no interrupts, no DMA, master, mode 3, 8-bit */
    wr(SPI_SLAVE_SEL, 1);
    wr(SPI_INT_EN, 0);
    wr(SPI_MODE_CFG, 0);
    wr(SPI_PACKET_CNT, 0);
    wr(SPI_PENDING_CLR, 0x1e);
    wr(SPI_PENDING_CLR, 0);
    wr(SPI_SWAP_CFG, 0);
    wr(SPI_MODE_CFG, 0x3ffU << 19);
    wr(SPI_CH_CFG, (rd(SPI_CH_CFG) & ~(CH_SLAVE | CH_HS | CH_RX | CH_TX)) | CH_CPOL | CH_CPHA);
    spi_flush();
    wr(SPI_FB_CLK, 0);
    /* the pins: levels before directions, the controller already idles the clock high */
    bank_save(GPH0, &F.gph0);
    bank_save(GPA0, &F.gpa0);
    bank_save(GPA1, &F.gpa1);
    F.pins = 1;
    pin_set(GPH0, P_CS, 1);
    pin_fn(GPH0, P_CS, 1);
    pin_pud(GPH0, P_CS, 0);
    pin_set(GPH0, P_REQ, 0);
    pin_fn(GPH0, P_REQ, 1);
    pin_pud(GPH0, P_REQ, 0);
    pin_fn(GPH0, P_RESP, 0);
    pin_pud(GPH0, P_RESP, 0);
    pin_pud(GPH0, 0, 0);
    pin_pud(GPH0, 1, 0);
    pin_pud(GPH0, 2, 1);                    /* MISO: pulled down */
    pin_fn(GPH0, 0, 2);
    pin_fn(GPH0, 1, 2);
    pin_fn(GPH0, 2, 2);
    pin_fn(GPA1, P_HOSTREQ, 0);
    pin_pud(GPA1, P_HOSTREQ, 1);
    pin_set(GPA0, P_APSLEEP, 1);
    pin_fn(GPA0, P_APSLEEP, 1);
    ub_init(&u);
    ub_s(&u, "shub: step 1: the bus is up: div ");
    ub_hexn(&u, rd(CMU_DIV), 8);
    ub_s(&u, " gates ");
    for (int i = 0; i < 3; ++i) {
        ub_hexn(&u, rd(cmu_gate[i]), 8);
        ub_c(&u, ' ');
    }
    ub_s(&u, "USI mode ");
    ub_hexn(&u, rd(USI_MODE), 8);
    ub_s(&u, " USI_CON ");
    ub_hexn(&u, rd(USI_CON), 8);
    ub_s(&u, " CH_CFG ");
    ub_hexn(&u, rd(SPI_CH_CFG), 8);
    ub_s(&u, " STATUS ");
    ub_hexn(&u, rd(SPI_STATUS), 8);
    ub_s(&u, "; gph0 CON ");
    ub_hexn(&u, rd(GPH0), 8);
    ub_s(&u, " DAT ");
    ub_hexn(&u, rd(GPH0 + 4), 2);
    sb_log(u.b);
    return 1;
}

/* ---- leaving -------------------------------------------------------------------------------------------------- */
void shub_leave(void)
{
    if (!F.clocks && !F.nstdby && !F.pins) {
        M.st = ST_DONE;
        if (M.state != SHUB_FAILED)
            M.state = SHUB_OFF;
        return;
    }
    if (F.pins)
        pin_set(GPH0, P_REQ, 0);
    if (F.nstdby) {
        pin_set(GPG0, P_NSTDBY, 0);         /* the chip into standby: its RAM (the patch) is gone */
        sb_mdelay(2);
    }
    if (F.spi) {
        spi_flush();
        wr(SPI_MODE_CFG, F.mode);
        wr(SPI_SLAVE_SEL, F.sel);
        wr(SPI_INT_EN, F.inten);
        wr(SPI_PACKET_CNT, F.pcnt);
        wr(SPI_SWAP_CFG, F.swap);
        wr(SPI_FB_CLK, F.fb);
        wr(SPI_CLK_CFG, F.clk);
        wr(SPI_CH_CFG, F.ch);
    }
    if (F.pins) {
        bank_restore(GPH0, &F.gph0, 0x3f, 1U << P_CS | 1U << P_REQ);
        bank_restore(GPA1, &F.gpa1, 1U << P_HOSTREQ, 0);
        bank_restore(GPA0, &F.gpa0, 1U << P_APSLEEP, 1U << P_APSLEEP);
    }
    if (F.spi)
        wr_if(USI_CON, F.usi_con);
    if (F.usi)
        wr_if(USI_MODE, F.usi_mode);
    if (F.clocks) {
        if (rd(CMU_DIV) != F.div) {
            wr(CMU_DIV, F.div & ~(1U << 16));
            uint64_t t0 = sb_us();
            while ((rd(CMU_DIV) & (1U << 16)) && sb_us() - t0 < 2000U)
                ;
        }
        for (int i = 2; i >= 0; --i)
            wr_if(cmu_gate[i], F.gate[i]);
    }
    if (F.nstdby)
        bank_restore(GPG0, &F.gpg0, 1U << P_NSTDBY, 1U << P_NSTDBY);    /* NSTDBY as found, last */
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "shub: left: everything as found: div ");
        ub_hexn(&u, F.clocks ? rd(CMU_DIV) : 0, 8);
        ub_s(&u, " gates ");
        for (int i = 0; i < 3; ++i) {
            ub_hexn(&u, F.clocks ? rd(cmu_gate[i]) : 0, 8);
            ub_c(&u, ' ');
        }
        ub_s(&u, "USI mode ");
        ub_hexn(&u, F.usi ? rd(USI_MODE) : 0, 8);
        ub_s(&u, " gph0 CON ");
        ub_hexn(&u, F.pins ? rd(GPH0) : 0, 8);
        ub_s(&u, " DAT ");
        ub_hexn(&u, F.pins ? rd(GPH0 + 4) : 0, 2);
        ub_s(&u, " gpg0 CON ");
        ub_hexn(&u, rd(GPG0), 8);
        ub_s(&u, " DAT ");
        ub_hexn(&u, rd(GPG0 + 4), 2);
        ub_s(&u, "; reads ");
        ub_dec(&u, L.reads);
        ub_s(&u, " writes ");
        ub_dec(&u, L.writes);
        ub_s(&u, " packets ");
        ub_dec(&u, L.pkts);
        ub_s(&u, " refused ");
        ub_dec(&u, L.rx.bad);
        ub_s(&u, " SPI errors ");
        ub_dec(&u, L.spi_err);
        sb_log(u.b);
    }
    memset(&F, 0, sizeof(F));
    M.st = ST_DONE;
    if (M.state != SHUB_FAILED)
        M.state = SHUB_OFF;
}

/* ---- the bring-up --------------------------------------------------------------------------------------------- */
void shub_begin(int probe_only)
{
    if (F.clocks || F.pins || F.nstdby)
        shub_leave();                       /* (a second start: the first one's registers back first) */
    memset(&M, 0, sizeof(M));
    memset(&L, 0, sizeof(L));
    memset(&C, 0, sizeof(C));
    memset(&D, 0, sizeof(D));
    memset(&F, 0, sizeof(F));
    shp_rx_init(&L.rx);
    shp_ssp_rx_init(&SR);
    M.probe = probe_only;
    M.verbose = probe_only;
    M.st = ST_LOOK;
    M.state = SHUB_STARTING;
    M.why = "starting";
    M.wait = 20;
    L.rdctl = SHP_SSI_READ_A;
    L.dump_left = 14;
    L.pkt_log_left = probe_only ? 10 : 6;
    L.err_log_left = 6;
    L.rpc_log_left = 6;
}

unsigned shub_wait_ms(void) { return M.wait ? M.wait : 20U; }
int shub_state(void) { return M.state; }
const char *shub_why(void) { return M.state == SHUB_RUNNING ? "running" : M.state == SHUB_OFF ? "The sensor hub is not started." : M.why; }

static void log_round(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, M.app ? "shub: app sync: round " : "shub: step 3: round ");
    ub_dec(&u, (uint64_t)M.round + 1U);
    ub_s(&u, M.slow ? " (slower clock)" : "");
    ub_s(&u, ": 6 sync requests, no answer: reads ");
    ub_dec(&u, L.reads);
    ub_s(&u, " (0x50: ");
    ub_dec(&u, L.reads_a);
    ub_s(&u, ", 0x70: ");
    ub_dec(&u, L.reads_b);
    ub_c(&u, ')');
    ub_s(&u, ", last read: status ");
    ub_hexn(&u, L.last_st, 2);
    ub_s(&u, " length ");
    ub_dec(&u, L.last_len);
    ub_s(&u, "; last write: status ");
    ub_hexn(&u, L.last_wst, 2);
    ub_s(&u, " chip bytes ");
    ub_dec(&u, L.last_wn);
    ub_s(&u, " accepted ");
    ub_dec(&u, L.last_wacc);
    ub_s(&u, "; stream bytes ");
    ub_dec(&u, L.read_bytes);
    ub_s(&u, ", garbage ");
    ub_dec(&u, L.rx.garbage);
    ub_s(&u, ", packets refused ");
    ub_dec(&u, L.rx.bad);
    ub_s(&u, ", MCU_RESP ");
    ub_dec(&u, (uint64_t)pin_get(GPH0, P_RESP));
    ub_s(&u, " HOST_REQ ");
    ub_dec(&u, (uint64_t)host_req());
    sb_log(u.b);
}

static void log_version(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, M.app ? "shub: the patch answers: version raw" : "shub: step 4: version raw");
    for (int i = 0; i < 12; ++i) {
        ub_c(&u, ' ');
        ub_hexn(&u, L.ver_raw[i], 2);
    }
    ub_s(&u, ": ASIC ");
    ub_hex(&u, L.ver.asic, 8);
    ub_s(&u, " ROM ");
    ub_hex(&u, L.ver.rom, 8);
    ub_s(&u, " patch level ");
    ub_hex(&u, L.ver.patch, 8);
    ub_s(&u, L.ver.big_endian ? " (big-endian fields)" : " (little-endian fields)");
    ub_s(&u, L.ver_ok ? "" : ": NOT the 4775 B0");
    sb_log(u.b);
}

/* the SSP bring-up, one command a call: 1 started, 0 the list is done */
static const uint8_t mag_matrix[27] = {     /* the DT's ssp-mag-array (the AKM's PDC matrix for this phone) */
    0xd8, 0x52, 0x6e, 0x14, 0x10, 0x7b, 0xff, 0xd1, 0xd3, 0x36, 0x46, 0x25, 0xb4, 0x08,
    0x00, 0x93, 0xff, 0x71, 0x84, 0x79, 0xbe, 0xfc, 0xfa, 0x0f, 0x09, 0x28, 0xb4 };
static const char *const ssp_name[] = { "who am I", "sensor formation", "sensor scan", "magnetometer matrix", "firmware revision",
                                        "accelerometer on", "magnetometer on", "step counter on" };
#define SSP_STEPS 8

static int ssp_begin(int i)
{
    uint8_t h[SSP_HDR], w[32], cmd = 0;
    unsigned len = 0;
    int type = SSP_READ;
    switch (i) {
    case 0: cmd = SSP_CMD_WHOAMI; len = 1; break;
    case 1:
        cmd = SSP_CMD_FORMATION;
        type = SSP_WRITE;
        C.data[0] = 4;                      /* ssp-acc-position, twice (accelerometer, gyroscope) */
        C.data[1] = 4;
        C.data[2] = 5;                      /* ssp-mag-position */
        len = 3;
        break;
    case 2: cmd = SSP_CMD_SCAN; len = 8; break;
    case 3:
        cmd = SSP_CMD_MAG_MATRIX;
        type = SSP_WRITE;
        memcpy(C.data, mag_matrix, 27);
        len = 27;
        break;
    case 4: cmd = SSP_CMD_FW_REV; len = 4; break;
    default:
        cmd = SSP_CMD_ADD;
        type = SSP_WRITE;
        shp_ssp_enable(C.data, i == 5 ? SSP_SENSOR_ACCEL : i == 6 ? SSP_SENSOR_MAG : SSP_SENSOR_STEPS, 100, 0, 0);
        len = 10;
        break;
    }
    C.busy = 1;
    C.done = C.go = 0;
    C.type = type;
    C.cmd = cmd;
    C.dlen = len;
    C.rlen = 0;
    C.t = sb_ms();
    if (type == (int)SSP_READ)
        SR.read_len = (uint16_t)len;
    shp_ssp_hdr(h, cmd, (uint16_t)len, (uint16_t)type, 0);
    unsigned k = shp_ssp_wrap(w, sizeof(w), h, SSP_HDR);
    return k && plain_send(w, k);
}

static int ssp_result(int i)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "shub: SSP: ");
    ub_s(&u, ssp_name[i]);
    if (C.type == (int)SSP_READ) {
        uint64_t v = 0;
        for (unsigned b = 0; b < C.rlen && b < 8U; ++b)
            v |= (uint64_t)C.res[b] << (8 * b);
        ub_s(&u, ": ");
        if (i == 4)
            ub_dec(&u, v);
        else
            ub_hex(&u, v, C.rlen * 2U);
        if (i == 0 && v != SSP_WHOAMI) {
            sb_log(u.b);
            fail("the hub's who-am-I is not 0x55");
            return 0;
        }
        if (i == 4 && v != 21031900U)
            ub_s(&u, " (the patch's revision should be 21031900)");
    } else {
        ub_s(&u, ": sent");
    }
    sb_log(u.b);
    return 1;
}

int shub_step(void)
{
    M.wait = 20;
    switch (M.st) {
    case ST_LOOK:
#ifndef SB_HOST
        if (!M.probe && !patch_ready()) {   /* 1.2-alpha28: the firmware first (the card), before the chip wakes */
            fail(patch_why);
            return 0;
        }
#endif
        if (look())
            M.st = ST_BUSUP;
        return 0;
    case ST_BUSUP:
        if (busup())
            M.st = ST_STDBY_LO;
        return 0;
    case ST_STDBY_LO:                       /* step 2: NSTDBY low 20 ms, high, 10 ms */
        bank_save(GPG0, &F.gpg0);
        F.nstdby = 1;
        pin_set(GPG0, P_NSTDBY, 0);
        pin_fn(GPG0, P_NSTDBY, 1);
        sb_mdelay(20);
        M.st = ST_STDBY_HI;
        M.wait = 1;
        return 0;
    case ST_STDBY_HI:
        pin_set(GPG0, P_NSTDBY, 1);
        sb_mdelay(10);
        pin_set(GPH0, P_REQ, 1);            /* MCU_REQ: high from here until the driver leaves */
        M.t = sb_ms();
        M.n = 0;
        M.resp_seen = 0;
        M.st = ST_RESP;
        return 0;
    case ST_RESP:                           /* MCU_RESP watched for 100 ms; a silent one is normal */
        M.resp_seen |= pin_get(GPH0, P_RESP);
        if (sb_ms() - M.t < 100U)
            return 0;
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "shub: step 2: NSTDBY low 20 ms, then high (gpg0 DAT ");
            ub_hexn(&u, rd(GPG0 + 4), 2);
            ub_s(&u, "); MCU_REQ high; MCU_RESP in 100 ms: ");
            ub_s(&u, M.resp_seen ? "rose" : "silent (normal on this chip)");
            ub_s(&u, "; MISO idle ");
            ub_dec(&u, (uint64_t)pin_get(GPH0, 2));
            ub_s(&u, " HOST_REQ ");
            ub_dec(&u, (uint64_t)host_req());
            ub_s(&u, " gph0 DAT ");
            ub_hexn(&u, rd(GPH0 + 4), 2);
            sb_log(u.b);
        }
        M.round = 0;
        M.st = ST_AUTOBAUD;
        return 0;
    case ST_AUTOBAUD: {                     /* step 3: 36 x 0x80 in one frame, 100 ms */
        memset(txf, 0x80, 36);
        if (!cs_xfer(txf, rxf, 36)) {
            fail("the SPI controller does not clock bytes (the autobaud burst timed out)");
            return 0;
        }
        if (M.verbose && M.round == 0 && !M.slow)
            hexdump("shub: step 3: autobaud (36 x 0x80), MISO meanwhile:", rxf, 36, 16);
        shp_rx_init(&L.rx);
        L.sync_ok = 0;
        M.sync_try = 0;
        M.polls = 0;
        M.st = ST_SYNC;
        M.wait = 100;
        return 0;
    }
    case ST_SYNC:
        if (L.sync_ok) {
            struct ub u;
            L.tx_seq = (uint8_t)(L.sync.last_seq_rx + 1U);
            L.tx_rel = (uint8_t)(L.sync.last_rel_rx + 1U);
            L.have_rel = 0;
            L.ack_due = 0;
            ub_init(&u);
            ub_s(&u, M.app ? "shub: app sync: answer to request " : "shub: step 3: sync answer to request ");
            ub_dec(&u, L.sync.n);
            ub_s(&u, ": last seq rx ");
            ub_hexn(&u, L.sync.last_seq_rx, 2);
            ub_s(&u, " last ack ");
            ub_hexn(&u, L.sync.last_ack, 2);
            ub_s(&u, " last reliable rx ");
            ub_hexn(&u, L.sync.last_rel_rx, 2);
            ub_s(&u, " next seq ");
            ub_hexn(&u, L.sync.next_seq, 2);
            ub_s(&u, " next reliable ");
            ub_hexn(&u, L.sync.next_rel, 2);
            ub_s(&u, " last reliable acked ");
            ub_hexn(&u, L.sync.last_rel_acked, 2);
            ub_s(&u, L.rd_proven ? "; read control " : "; it came inside a write frame: the read control is not proven yet, now ");
            ub_hexn(&u, L.rdctl, 2);
            ub_s(&u, M.slow ? ", slower clock" : "");
            sb_log(u.b);
            M.ver_try = 0;
            M.st = ST_VER;
            return 0;
        }
        if (M.polls > 0) {
            --M.polls;
            if (ssi_read() < 0) {
                fail("the SPI controller does not clock bytes (a read timed out)");
                return 0;
            }
            return 0;
        }
        if (M.sync_try < 6) {
            uint8_t s[16];
            unsigned k = shp_sync_build(s, sizeof(s), (uint8_t)(M.sync_try + 1));
            ++M.sync_try;
            if (!k || !ssi_write(s, k)) {
                if (L.spi_err) {
                    fail("the SPI controller does not clock bytes (a write timed out)");
                    return 0;
                }
            }
            M.polls = 4;
            return 0;
        }
        log_round();
        ++M.round;
        if (M.round >= (M.slow ? 2 : 5)) {
            if (M.slow || M.app) {
                fail(M.app ? "the patch did not answer the sync request after its start" :
                             "the chip did not answer the sync request (5 autobaud rounds, then 2 at a slower clock)");
                return 0;
            }
            /* slower: the controller's own prescaler (a guess: Samsung never writes it on this SoC) */
            M.slow = 1;
            M.round = 0;
            wr(SPI_CLK_CFG, (F.clk & ~0xffU) | 3U);
            sb_logx("shub: step 3: slower: the controller's prescaler set to 3 (CLK_CFG reads) ", rd(SPI_CLK_CFG));
        }
        M.st = ST_AUTOBAUD;
        return 0;
    case ST_VER:                            /* step 4: reliable RPC 0x00 */
        L.ver_seen = 0;
        ++M.ver_try;
        if (!shp_rpc_put(relp, sizeof(relp), SHP_RPC_GET_VERSION, 0, 0) || !rel_send(2)) {
            fail("the version request could not be sent");
            return 0;
        }
        M.t = sb_ms();
        M.st = ST_VER_WAIT;
        M.wait = 10;
        return 0;
    case ST_VER_WAIT: {
        int r = rel_poll();
        M.wait = 10;
        if (L.ver_seen) {
            if (L.rel_wait) {               /* the answer came: the request arrived, acknowledged or not */
                L.rel_wait = 0;
                ++L.tx_rel;
            }
            link_service();
            log_version();
            if (!L.ver_ok) {
                fail("the chip's version answer is not a BCM4775 B0 (ASIC 0x004775B0)");
                return 0;
            }
            if (M.probe) {
                M.pass = 1;
                M.st = ST_LEAVE;
                return 0;
            }
            if (M.app) {
                M.t = sb_ms();
                M.st = ST_SSP_INIT;
                return 0;
            }
            M.dl_off = 0;
            M.dl_recs = 0;
            M.dl_t0 = sb_ms();
#ifndef SB_HOST
            if (!patch_ready()) {
                fail(patch_why);
                return 0;
            }
#endif
            if (PATCH_N != 413864U) {
                fail("the hub's firmware patch is not in this image (starbite/sbos/tools/shub-patch.py; never committed)");
                return 0;
            }
            sb_log("shub: download: 452 reliable RPC 0x05 writes of the patch into the chip's RAM");
            M.st = ST_DL;
            return 0;
        }
        if (r < 0 || sb_ms() - M.t > 2500U) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "shub: no version answer: reliable ");
            ub_s(&u, L.rel_wait ? "NOT acknowledged" : "acknowledged");
            ub_s(&u, ", tries ");
            ub_dec(&u, (uint64_t)L.rel_tries);
            ub_s(&u, ", packets ");
            ub_dec(&u, L.pkts);
            ub_s(&u, ", other RPCs ");
            ub_dec(&u, L.rpc_other);
            sb_log(u.b);
            fail(M.app ? "the patch did not answer the version request" :
                         "the ROM did not answer the version request (RPC 0x00): the next step is reading lhd");
        }
        return 0;
    }
    case ST_DL: {                           /* the patch: one record a reliable packet, stop and wait */
        uint64_t t0 = sb_ms();
        while (sb_ms() - t0 < 60U) {
            if (!L.rel_wait) {
                uint32_t addr = 0;
                const uint8_t *d = 0;
                unsigned len = 0;
                int v = shp_patch_next(PATCH_P, PATCH_N, &M.dl_off, &addr, &d, &len);
                if (v < 0) {
                    fail("the patch blob's records are malformed");
                    return 0;
                }
                if (!v) {
                    struct ub u;
                    uint8_t s[8];
                    ub_init(&u);
                    ub_s(&u, "shub: download: ");
                    ub_dec(&u, M.dl_recs);
                    ub_s(&u, " writes acknowledged in ");
                    ub_dec(&u, sb_ms() - M.dl_t0);
                    ub_s(&u, " ms, retransmits ");
                    ub_dec(&u, L.retx);
                    ub_s(&u, ", short writes ");
                    ub_dec(&u, L.fc_short);
                    ub_s(&u, ", protocol-error flags ");
                    ub_dec(&u, L.pe);
                    sb_log(u.b);
                    /* start: plain RPC 0x2E with the patch's entry (its header's PatchAddress) */
                    unsigned k = shp_rpc_start(s, sizeof(s), 0x00800161U, L.ver.big_endian);
                    if (!k || !plain_send(s, k)) {
                        fail("the start command could not be sent");
                        return 0;
                    }
                    sb_log("shub: start: RPC 0x2E 0x00800161 sent; 40 ms, then autobaud and sync again");
                    M.st = ST_START;
                    M.wait = 40;
                    return 0;
                }
                unsigned k = shp_rpc_write_mem(relp, sizeof(relp), addr, d, len, L.ver.big_endian);
                if (!k || !rel_send(k)) {
                    fail("a patch write could not be sent");
                    return 0;
                }
                ++M.dl_recs;
            }
            int r = rel_poll();
            if (r < 0) {
                sb_logx("shub: download: a write was never acknowledged, record ", M.dl_recs);
                fail("the ROM stopped acknowledging the patch writes (RPC 0x05)");
                return 0;
            }
            if (!r)
                sb_udelay(200);
        }
        M.wait = 1;
        return 0;
    }
    case ST_START:
        M.app = 1;
        M.round = 0;
        M.st = ST_AUTOBAUD;
        return 0;
    case ST_SSP_INIT:                       /* the application's first frame: "sensor init done" */
        (void)ssi_read();
        link_service();
        if (!C.init_done && sb_ms() - M.t < 3000U)
            return 0;
        sb_logx(C.init_done ? "shub: SSP: the hub's \"sensor init done\" frame came, ms " :
                              "shub: SSP: no \"sensor init done\" frame in 3 s (going on), ms ", sb_ms() - M.t);
        M.ssp_i = 0;
        M.ssp_tries = 0;
        C.busy = 0;
        M.st = ST_SSP;
        return 0;
    case ST_SSP:
        if (!C.busy) {
            if (M.ssp_i >= SSP_STEPS) {
                M.state = SHUB_RUNNING;
                M.why = "running";
                M.st = ST_RUN;
                sb_log("shub: running: accelerometer, magnetometer and step counter on (100 ms)");
                return 1;
            }
            if (!ssp_begin(M.ssp_i)) {
                fail("an SSP request could not be sent");
                return 0;
            }
            return 0;
        }
        (void)ssi_read();
        link_service();
        if (C.done) {
            C.busy = 0;
            if (!ssp_result(M.ssp_i))
                return 0;
            ++M.ssp_i;
            M.ssp_tries = 0;
        } else if (sb_ms() - C.t > 1000U) {
            C.busy = 0;
            SR.read_len = 0;
            if (++M.ssp_tries >= 3) {
                sb_logf("shub: SSP: no answer three times: ", ssp_name[M.ssp_i]);
                fail("the hub did not answer an SSP request (the RPC 0x20 wrapper is inferred, not from a source)");
            }
        }
        return 0;
    case ST_RUN:
        shub_poll();
        return 1;
    case ST_LEAVE:
        shub_leave();
        return 1;
    default:
        return 1;
    }
}

void shub_poll(void)
{
    if (M.state != SHUB_RUNNING)
        return;
    for (int i = 0; i < 6 && host_req(); ++i)
        if (ssi_read() <= 0)
            break;
    if (sb_ms() - L.t_read > 200U)
        (void)ssi_read();                   /* (should HOST_REQ not work as read: never deaf for long) */
    link_service();
}

/* ---- the kernel API (survival.h, "sensors") ------------------------------------------------------------------- */
static int fresh(void)
{
    uint64_t now = sb_ms();
    return M.state == SHUB_RUNNING && D.have_a && now - D.t_a < 2000U;
}

int surv_heading(int *deg_x10, int *accuracy)
{
    if (!fresh() || !D.have_m || sb_ms() - D.t_m >= 2000U || D.movf)
        return 0;
    if (!shp_heading(D.a[0], D.a[1], D.a[2], D.m[0], D.m[1], D.m[2], deg_x10))
        return 0;
    if (accuracy)
        *accuracy = D.macc;
    return 1;
}

int surv_tilt(int *pitch_x10, int *roll_x10)
{
    return fresh() && shp_tilt(D.a[0], D.a[1], D.a[2], pitch_x10, roll_x10);
}

int surv_steps(uint32_t *n)
{
    if (M.state != SHUB_RUNNING)
        return 0;
    *n = D.steps;
    return 1;
}

/* ---- the self tests (uitest.c) -------------------------------------------------------------------------------- */
int shub_probe_selftest_step(void)
{
    static int started;
    if (!started) {
        started = 1;
        sb_log("selftest: shubprobe: the BCM4775 sensor hub, read-only probe (docs/SENSOR-HUB.md 5): look, bus up, wake, "
               "autobaud + sync, the ROM's version; then every register as found. No chip memory write, no PMIC write");
        shub_begin(1);
        return 0;
    }
    if (!shub_step())
        return 0;
    if (M.pass) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: shubprobe: passed: ASIC ");
        ub_hex(&u, L.ver.asic, 8);
        ub_s(&u, " ROM ");
        ub_hex(&u, L.ver.rom, 8);
        ub_s(&u, ", read control ");
        ub_hexn(&u, L.rdctl, 2);
        ub_s(&u, L.ver.big_endian ? ", big-endian RPC fields" : ", little-endian RPC fields");
        ub_s(&u, M.slow ? ", slower clock" : "");
        ub_s(&u, M.resp_seen ? ", MCU_RESP rose" : ", MCU_RESP silent");
        sb_log(u.b);
    } else {
        sb_logf("selftest: shubprobe: FAILED: ", M.why ? M.why : "?");
    }
    M.wait = 20;
    return 1;
}

int shub_selftest_step(void)
{
    static int ph, lines;
    static uint64_t t0, t_log;
    M.wait = 20;
    switch (ph) {
    case 0:
        sb_log("selftest: shub: the BCM4775 sensor hub, full start: the patch into the chip's RAM, SSP, accelerometer + "
               "magnetometer + step counter for 10 s; then standby and every register as found. No PMIC write");
        shub_begin(0);
        ph = 1;
        return 0;
    case 1:
        if (!shub_step()) {
            M.wait = shub_wait_ms();
            return 0;
        }
        if (M.state != SHUB_RUNNING) {
            sb_logf("selftest: shub: FAILED: ", M.why ? M.why : "?");
            if (M.st != ST_DONE)
                shub_leave();
            return 1;
        }
        t0 = t_log = sb_ms();
        ph = 2;
        return 0;
    case 2: {
        uint64_t now = sb_ms();
        shub_poll();
        if (now - t_log >= 500U && lines < 20) {
            struct ub u;
            int h = 0, acc = 0, p = 0, r = 0;
            t_log = now;
            ++lines;
            ub_init(&u);
            ub_s(&u, "shub: t ");
            ub_fix(&u, now - t0, 1);
            ub_s(&u, " s: accel ");
            for (int i = 0; i < 3; ++i) {
                ub_sdec(&u, D.ra[i]);
                ub_c(&u, ' ');
            }
            ub_c(&u, '(');
            ub_dec(&u, D.na);
            ub_s(&u, "), mag ");
            for (int i = 0; i < 3; ++i) {
                ub_sdec(&u, D.rm[i]);
                ub_c(&u, ' ');
            }
            ub_s(&u, "accuracy ");
            ub_dec(&u, D.macc);
            ub_s(&u, D.movf ? " OVERFLOW (" : " (");
            ub_dec(&u, D.nm);
            ub_s(&u, "), steps ");
            ub_dec(&u, D.steps);
            ub_s(&u, " (");
            ub_dec(&u, D.ns);
            ub_s(&u, ")");
            if (surv_heading(&h, &acc)) {
                ub_s(&u, "; heading ");
                ub_fix(&u, (uint64_t)h * 100U, 1);
            } else {
                ub_s(&u, "; no heading");
            }
            if (surv_tilt(&p, &r)) {
                ub_s(&u, ", pitch x10 ");
                ub_sdec(&u, p);
                ub_s(&u, " roll x10 ");
                ub_sdec(&u, r);
            }
            sb_log(u.b);
        }
        if (now - t0 < 10000U)
            return 0;
        {
            struct ub u;
            int64_t g2 = (int64_t)D.ra[0] * D.ra[0] + (int64_t)D.ra[1] * D.ra[1] + (int64_t)D.ra[2] * D.ra[2];
            int g_ok = g2 > 3300LL * 3300LL && g2 < 4900LL * 4900LL;
            int ok = D.na >= 50U && D.nm >= 20U && g_ok;
            ub_init(&u);
            ub_s(&u, ok ? "selftest: shub: passed: " : "selftest: shub: FAILED: ");
            ub_s(&u, "in 10 s: accelerometer samples ");
            ub_dec(&u, D.na);
            ub_s(&u, " (at least 50), magnetometer ");
            ub_dec(&u, D.nm);
            ub_s(&u, " (at least 20), step reports ");
            ub_dec(&u, D.ns);
            ub_s(&u, g_ok ? "; gravity about 1 g" : "; gravity NOT about 1 g (4096)");
            ub_s(&u, "; frames ");
            ub_dec(&u, C.frames);
            ub_s(&u, ", other sensors ");
            ub_dec(&u, D.other);
            ub_s(&u, ", other RPCs ");
            ub_dec(&u, L.rpc_other);
            ub_s(&u, ", SSP errors ");
            ub_dec(&u, C.ssp_err);
            ub_s(&u, ", stray write echoes ");
            ub_dec(&u, C.echo_stray);
            ub_s(&u, ", packets refused ");
            ub_dec(&u, L.rx.bad);
            ub_s(&u, ", retransmits ");
            ub_dec(&u, L.retx);
            sb_log(u.b);
        }
        shub_leave();
        return 1;
    }
    default:
        return 1;
    }
}
