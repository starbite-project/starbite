/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: the microSD card's probe ("sd", drvprobe.h). Read only, always: the only card commands
 * are libsb sd.c's reads (CMD17/18, the CMD6 query and switch to high speed, ACMD51), never a write.
 *
 *   level 0  the card as the phone already has it: when it is up, its CID / CSD / SCR facts and the CMD6 mode 0
 *            query (does it offer high speed); when it is not up, only why (bringing it up is level 1).
 *   level 1  the card up (sd_up), then the first 8 MiB of the OS store's partition (MBR type 0xDA, the area the
 *            store already reads) twice: on the old path (25 MHz, CPU FIFO) and on the fast path (sd_set_fast:
 *            high speed + IDMAC). PASS when both CRC32s match and the fast path reads >= 10 MB/s. The fast mode
 *            the kernel had is put back afterwards (and by the abort).
 *   level 2  nothing. */
#include "sb.h"
#include "sd.h"
#include "ufs.h"
#include "drvprobe.h"
#include "sdprobe.h"

#define SDP_BYTES (8U << 20)                /* the region read twice */
#define SDP_CHUNK 256U                      /* sectors per sd_read: one CMD18 */
#define SDP_STEP_MS 60U                     /* reading per call (the contract's ~100 ms) */
#define SDP_MIN_MBS 10U                     /* the fast path's pass mark, MB/s */

static int st;                              /* level 1: 0 start, 1 old path, 2 fast path */
static int prev = -1;                       /* sd_fast_mode before the probe, -1 not saved */
static uint8_t *buf;
static uint64_t base, ticks[2], hz[2];
static uint32_t done, crc[2];

static void put_back(void)
{
    if (prev >= 0)
        (void)sd_set_fast(prev);
    prev = -1;
    st = 0;
}

void drv_sd_abort(void)
{
    put_back();
}

static int fail(const char *why)
{
    put_back();
    sb_logf("drvprobe: sd: FAIL ", why);
    return 1;
}

static void pnm(struct ub *u, uint32_t a)
{
    char c = (char)a;
    ub_c(u, c >= 0x20 && c < 0x7f ? c : '?');
}

/* The card's facts (no serial number): CID, CSD when read, SCR when read, the high speed query. */
static void facts(void)
{
    const uint32_t *c = sd_rep.cid;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "drvprobe: sd: CID MID ");
    ub_hex(&u, c[3] >> 24, 2);
    ub_s(&u, " OID ");
    pnm(&u, c[3] >> 16);
    pnm(&u, c[3] >> 8);
    ub_s(&u, " name ");
    pnm(&u, c[3]);
    pnm(&u, c[2] >> 24);
    pnm(&u, c[2] >> 16);
    pnm(&u, c[2] >> 8);
    pnm(&u, c[2]);
    ub_s(&u, " rev ");
    ub_dec(&u, (c[1] >> 28) & 0xfU);
    ub_c(&u, '.');
    ub_dec(&u, (c[1] >> 24) & 0xfU);
    ub_s(&u, " made ");
    ub_dec(&u, 2000U + ((c[0] >> 12) & 0xffU));
    ub_c(&u, '-');
    ub_dec2(&u, (c[0] >> 8) & 0xfU);
    ub_s(&u, sd_rep.high_capacity ? ", SDHC/SDXC" : ", SDSC");
    ub_s(&u, ", bus clock ");
    ub_dec(&u, sd_rep.bus_hz / 1000U);
    ub_s(&u, " kHz, card clock ");
    ub_dec(&u, sd_rep.card_hz / 1000U);
    ub_s(&u, " kHz");
    sb_log(u.b);

    ub_init(&u);
    ub_s(&u, "drvprobe: sd: ");
    if (sd_rep.csd_ok) {
        const uint32_t *d = sd_rep.csd;
        ub_s(&u, "CSD v");
        ub_dec(&u, (d[3] >> 30) + 1U);
        ub_s(&u, " TRAN_SPEED ");
        ub_hex(&u, d[3] & 0xffU, 2);
        ub_s(&u, " CCC ");
        ub_hex(&u, (d[2] >> 20) & 0xfffU, 3);
        if ((d[3] >> 30) == 1U) {               /* CSD 2.0: C_SIZE [69:48], (C_SIZE + 1) * 512 KiB */
            uint64_t cs = ((uint64_t)(d[2] & 0x3fU) << 16) | (d[1] >> 16);
            ub_s(&u, " size ");
            ub_size(&u, (cs + 1U) << 19);
        }
    } else {
        ub_s(&u, "CSD not read (the card came up before fast reads were asked for)");
    }
    if (sd_rep.scr_ok) {
        ub_s(&u, "; SCR SD_SPEC ");
        ub_dec(&u, sd_rep.scr[0] & 0xfU);
        ub_s(&u, (sd_rep.scr[2] & 0x80U) ? " SPEC3" : "");
        ub_s(&u, " bus widths ");
        ub_hex(&u, sd_rep.scr[1] & 0xfU, 1);
    } else {
        ub_s(&u, "; SCR not read");
    }
    ub_s(&u, "; high speed ");
    ub_s(&u, !sd_rep.hs_asked ? "not asked" : sd_rep.hs_support > 0 ? "offered" :
              sd_rep.hs_support == 0 ? "not offered" : "query failed");
    ub_s(&u, sd_rep.hs_card ? ", the card runs it" : "");
    sb_log(u.b);
}

/* MB/s x 1000 for SDP_BYTES in t ticks */
static uint64_t rate_milli(uint64_t t)
{
    return t ? (uint64_t)SDP_BYTES * sb_freq() / t / 1000U : 0;
}

static void say_rate(struct ub *u, int i)
{
    ub_dec(u, ticks[i] * 1000U / sb_freq());
    ub_s(u, " ms = ");
    ub_fix(u, rate_milli(ticks[i]), 2);
    ub_s(u, " MB/s at ");
    ub_dec(u, hz[i] / 1000U);
    ub_s(u, " kHz, CRC32 ");
    ub_hex(u, crc[i], 8);
}

static int finish(void)
{
    uint64_t fast = rate_milli(ticks[1]);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "drvprobe: sd: 8 MiB of the store at sector ");
    ub_dec(&u, base);
    ub_s(&u, ": old path ");
    say_rate(&u, 0);
    sb_log(u.b);
    ub_init(&u);
    ub_s(&u, "drvprobe: sd: fast path ");
    say_rate(&u, 1);
    ub_s(&u, ", ");
    ub_dec(&u, sd_rep.dma_chunks);
    ub_s(&u, " IDMAC chunks, ");
    ub_dec(&u, sd_rep.dma_errors);
    ub_s(&u, " errors, ");
    ub_dec(&u, sd_rep.recoveries);
    ub_s(&u, " recoveries");
    sb_log(u.b);
    int was_fast = sd_rep.fast;
    put_back();
    /* the numbers first in a FAIL line too (run 10: only the summary's ~100 characters survived in pmsg) */
    const char *why = crc[0] != crc[1] ? "the fast path read different data than the old path (CRC32)"
                      : !was_fast    ? (sd_rep.fast_why ? sd_rep.fast_why : "fast reads turned off during the read")
                      : fast < SDP_MIN_MBS * 1000U ? "the fast path is under 10 MB/s" : 0;
    if (why) {
        ub_init(&u);
        ub_s(&u, "fast ");
        ub_fix(&u, fast, 2);
        ub_s(&u, " MB/s (old ");
        ub_fix(&u, rate_milli(ticks[0]), 2);
        ub_s(&u, "), IDMAC ");
        ub_dec(&u, sd_rep.dma_chunks);
        ub_s(&u, "/");
        ub_dec(&u, sd_rep.dma_errors);
        ub_s(&u, " err, card ");
        ub_dec(&u, sd_rep.card_hz / 1000000U);
        ub_s(&u, " MHz: ");
        ub_s(&u, why);
        return fail(u.b);
    }
    ub_init(&u);
    ub_s(&u, "drvprobe: sd: PASS same data, fast path ");
    ub_fix(&u, fast, 2);
    ub_s(&u, " MB/s (old ");
    ub_fix(&u, rate_milli(ticks[0]), 2);
    ub_s(&u, " MB/s)");
    sb_log(u.b);
    return 1;
}

int drv_sd_probe(int level, unsigned *wait_ms)
{
    *wait_ms = 0;
    if (level == DRV_LVL_ATTENDED) {
        sb_log("drvprobe: sd: PASS nothing at level 2 (read only)");
        return 1;
    }
    if (level == DRV_LVL_READ) {
        if (sd_rep.state != 1) {
            sb_logf("drvprobe: sd: PASS the card is not up yet (level 1 brings it up): ",
                    sd_rep.state ? (sd_rep.why ? sd_rep.why : "failed") : "not tried");
            return 1;
        }
        int q = sd_hs_query();
        facts();
        sb_log(q > 0 ? "drvprobe: sd: PASS card facts read, high speed offered" :
               q == 0 ? "drvprobe: sd: PASS card facts read, high speed not offered" :
                        "drvprobe: sd: PASS card facts read, the high speed query did not answer");
        return 1;
    }

    if (st == 0) {
        sd_allow();
        if (!sd_up())
            return fail(sd_rep.why ? sd_rep.why : "the card did not come up");
        uint64_t first, blocks;
        if (!sd_store_area(&first, &blocks))
            return fail("no store partition (MBR type 0xDA) on the card");
        if (blocks < SDP_BYTES / 4096U)
            return fail("the store partition is under 8 MiB");
        if (!buf)
            buf = sb_alloc(SDP_CHUNK * 512U);
        if (!buf)
            buf = (uint8_t *)SB_IO_BUF;         /* the lab I/O buffer (non-cacheable); the kernel does not use it */
        if (!sd_rep.hs_asked)
            (void)sd_hs_query();
        facts();
        prev = sd_fast_mode();
        (void)sd_set_fast(SD_FAST_OFF);
        base = first * 8U;
        done = 0;
        ticks[0] = ticks[1] = 0;
        crc[0] = crc[1] = 0;
        hz[0] = sd_rep.card_hz;
        st = 1;
        return 0;
    }
    int i = st - 1;
    uint64_t t0 = sb_ms();
    while (done < SDP_BYTES / 512U && sb_ms() - t0 < SDP_STEP_MS) {
        uint64_t a = sb_ticks();
        int r = sd_read(base + done, SDP_CHUNK, (uintptr_t)buf);
        ticks[i] += sb_ticks() - a;
        if (r) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, i ? "a fast-path read failed at sector " : "an old-path read failed at sector ");
            ub_dec(&u, base + done);
            ub_s(&u, ", error ");
            ub_sdec(&u, r);
            return fail(u.b);
        }
        crc[i] = sb_crc32_more(crc[i], buf, SDP_CHUNK * 512U);
        done += SDP_CHUNK;
    }
    if (done < SDP_BYTES / 512U)
        return 0;
    if (st == 2)
        return finish();
    if (!sd_set_fast(SD_FAST_ON))
        return fail(sd_rep.fast_why ? sd_rep.fast_why : "fast reads did not start");
    hz[1] = sd_rep.card_hz;
    done = 0;
    st = 2;
    return 0;
}
