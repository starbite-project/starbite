/* SPDX-License-Identifier: GPL-2.0-only
 * The S2MPS18 PMIC through ACPM (port of shim/starlte/menu.c "ACPM / PMIC", itself after the iOS
 * driver bsd/dev/starlte_touch.c). The APM firmware owns the PMIC's I2C; the AP asks it through
 * the IPC queue of channel 2 in APM SRAM 0x02039000 and rings the mailbox 0x14100000. That queue
 * protocol is the only use of the SRAM and the mailbox here: one command slot written, the
 * front index advanced, INTGR0 rung, the reply read and the rear index advanced, exactly as the
 * loader and the kernel do. Nothing else in SRAM or the mailbox is ever written.
 *
 * Writes: pmic_write is static. Its callers: the RTC read latch (RTC block register 0x02 UPDATE
 * bit, needed to read the clock; the loader and Engineer Lite do the same on every status bar
 * refresh), and sb_pmic_write, which writes only what an open, confirmed authorisation covers
 * (fence.c gate; the tiers are engineer/policy.c's pmic_tier: the touch and card rails at the
 * kernel's values tier 1, every other regulator, BUCK and TOP register tier 3).
 */
#include "sb.h"

#define ACPM_MBOX 0x14100000UL
#define ACPM_SRAM 0x02039000UL
#define ACPM_SRAM_SIZE 0x30000U
#define ACPM_INITDATA 0x7f00U
#define ACPM_PMIC_CH 2U

static uint32_t ard(uint32_t off)
{
    return *(volatile uint32_t *)(ACPM_SRAM + off);
}

static void awr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(ACPM_SRAM + off) = v;
}

static uint32_t acpm_seq;

static uint32_t acpm_find(uint32_t id)
{
    uint32_t chans = ard(ACPM_INITDATA + 0x08), ap_max = ard(ACPM_INITDATA + 0x18);
    if (!ap_max || ap_max > 32 || chans >= ACPM_SRAM_SIZE - ap_max * 0x48)
        return 0;
    for (uint32_t i = 0; i < ap_max; ++i) {
        uint32_t c = chans + i * 0x48;
        if (ard(c) == id) {
            uint32_t q = ard(c + 0x34), e = ard(c + 0x38);
            return (q && q <= 64 && e >= 16 && e <= 64 && !(e & 3)) ? c : 0;
        }
    }
    return 0;
}

static int acpm_send(uint32_t c, uint32_t id, uint32_t cmd[4])
{
    uint32_t qlen = ard(c + 0x34), esz = ard(c + 0x38);
    uint32_t tx_rear = ard(c + 0x10), tx_front = ard(c + 0x14), tx_base = ard(c + 0x18);
    uint32_t rx_rear = ard(c + 0x28), rx_front = ard(c + 0x2c), rx_base = ard(c + 0x30);
    uint32_t front = ard(tx_front), next = (front + 1) % qlen;
    for (unsigned us = 0; next == ard(tx_rear); ++us) {
        if (us > 100000)
            return 0;
        sb_udelay(1);
    }
    if (++acpm_seq >= 64)
        acpm_seq = 1;
    uint32_t seq = acpm_seq;
    cmd[0] = (cmd[0] & ~(0x3fU << 16)) | (seq << 16);
    uint32_t slot = tx_base + esz * front;
    for (uint32_t i = 0; i < esz / 4; ++i)
        awr(slot + 4 * i, i < 4 ? cmd[i] : 0);
    __asm__ volatile("dsb sy" : : : "memory");
    awr(tx_front, next);
    __asm__ volatile("dsb sy" : : : "memory");
    *(volatile uint32_t *)(ACPM_MBOX + 0x08) = (1U << id) << 16;    /* INTGR0 */
    for (unsigned us = 0; us < 100000; ++us) {
        uint32_t rear = ard(rx_rear), rfront = ard(rx_front);
        for (uint32_t i = rear; i != rfront; i = (i + 1) % qlen) {
            uint32_t e = rx_base + esz * i;
            if (((ard(e) >> 16) & 0x3fU) != seq)
                continue;
            for (uint32_t w = 0; w < 4; ++w)
                cmd[w] = ard(e + 4 * w);
            if (i != rear)
                for (uint32_t w = 0; w < esz / 4; ++w)
                    awr(e + 4 * w, ard(rx_base + esz * rear + 4 * w));
            rear = (rear + 1) % qlen;
            awr(rx_rear, rear);
            __asm__ volatile("dsb sy" : : : "memory");
            if (rear == ard(rx_front))
                *(volatile uint32_t *)(ACPM_MBOX + 0x20) = 1U << id;  /* INTCR1 */
            return 1;
        }
        sb_udelay(1);
    }
    return 0;
}

int sb_pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
    uint32_t c = acpm_find(ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = cmd[2] = cmd[3] = 0;
    if (!c || !acpm_send(c, ACPM_PMIC_CH, cmd) || (cmd[1] >> 24))
        return 0;
    *val = (uint8_t)(cmd[1] >> 8);
    return 1;
}

static int pmic_write(uint32_t type, uint32_t reg, uint8_t val)
{
    uint32_t c = acpm_find(ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = 1U | (uint32_t)val << 8;
    cmd[2] = cmd[3] = 0;
    return c && acpm_send(c, ACPM_PMIC_CH, cmd) && !(cmd[1] >> 24);
}

/* Any PMIC register, with the gate: the caller's policy (engineer/policy.c pmic_tier) gives the
 * tier, at least 1; an open authorisation must cover SB_SP_PMIC type << 8 | reg. Read back. */
int sb_pmic_write(uint32_t type, uint32_t reg, uint8_t val, int tier)
{
    const char *why;
    uint8_t back = 0;
    if (type > 2 || reg > 0xff ||
        sb_gate_check(SB_SP_PMIC, 0, (uint64_t)type << 8 | reg, 1, tier < SB_TIER1 ? SB_TIER1 : tier, &why)) {
        sb_logx("pmic write refused, reg ", (uint64_t)type << 8 | reg);
        return 0;
    }
    int ok = pmic_write(type, reg, val) && sb_pmic_read(type, reg, &back);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "pmic write ");
    ub_hex(&u, (uint64_t)type << 8 | reg, 3);
    ub_s(&u, " = ");
    ub_hex(&u, val, 2);
    ub_s(&u, ok ? ", reads back " : ": ACPM did not answer");
    if (ok)
        ub_hex(&u, back, 2);
    sb_log(u.b);
    return ok;
}

/* ACPM DVFS (vendor cal-if/acpm_dvfs.c): cmd = { domain, kHz, 0 = FREQ_REQ, 0 } on the channel at
 * index 0 of the AP's channel table (Linux sends DVFS there; Starbite's iOS port raised the GPU
 * to 572 MHz this way, xnu/series-12377/0303). ACPM sets the PLL and, from its own table, the
 * voltage. With the gate: SB_SP_DVFS, first = domain. */
int sb_acpm_dvfs(uint32_t domain, uint32_t khz, int tier)
{
    const char *why;
    uint32_t chans = ard(ACPM_INITDATA + 0x08), ap_max = ard(ACPM_INITDATA + 0x18), cmd[4];
    if (sb_gate_check(SB_SP_DVFS, 0, domain, 1, tier < SB_TIER3 ? SB_TIER3 : tier, &why)) {
        sb_logx("acpm dvfs refused, domain ", domain);
        return 0;
    }
    if (!ap_max || ap_max > 32 || chans >= ACPM_SRAM_SIZE - ap_max * 0x48)
        return 0;
    uint32_t q = ard(chans + 0x34), e = ard(chans + 0x38), id = ard(chans);
    if (!q || q > 64 || e < 16 || e > 64 || (e & 3U) || id > 31)
        return 0;
    cmd[0] = domain;
    cmd[1] = khz;
    cmd[2] = cmd[3] = 0;
    int ok = acpm_send(chans, id, cmd);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "acpm dvfs domain ");
    ub_dec(&u, domain);
    ub_s(&u, " to ");
    ub_dec(&u, khz);
    ub_s(&u, ok ? " kHz: answered" : " kHz: no answer");
    sb_log(u.b);
    return ok;
}

static int acpm_dvfs_send(uint32_t domain, uint32_t khz);

/* 1.1-beta6: the M3 above 1794 MHz for at most Samsung's core count (sb_m3_ocp_max_khz); busy = the M3 cores that
 * run or may run at this clock (the caller counts them) */
int sb_acpm_dvfs_m3_ocp(uint32_t khz, unsigned busy)
{
    if (!sb_m3_ocp_level_ok(khz, busy)) {
        sb_logx("acpm dvfs (m3 ocp table) refused, busy cores ", busy);
        return 0;
    }
    return acpm_dvfs_send(3, khz);
}

int sb_acpm_dvfs_listed(uint32_t domain, uint32_t khz)
{
    if (!sb_dvfs_level_listed(domain, khz)) {
        sb_logx("acpm dvfs (listed) refused, domain ", domain);
        return 0;
    }
    return acpm_dvfs_send(domain, khz);
}

static int acpm_dvfs_send(uint32_t domain, uint32_t khz)
{
    uint32_t chans = ard(ACPM_INITDATA + 0x08), ap_max = ard(ACPM_INITDATA + 0x18), cmd[4];
    if (!ap_max || ap_max > 32 || chans >= ACPM_SRAM_SIZE - ap_max * 0x48)
        return 0;
    uint32_t q = ard(chans + 0x34), e = ard(chans + 0x38), id = ard(chans);
    if (!q || q > 64 || e < 16 || e > 64 || (e & 3U) || id > 31)
        return 0;
    cmd[0] = domain;
    cmd[1] = khz;
    cmd[2] = cmd[3] = 0;
    int ok = acpm_send(chans, id, cmd);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "acpm dvfs domain ");
    ub_dec(&u, domain);
    ub_s(&u, " to ");
    ub_dec(&u, khz);
    ub_s(&u, ok ? " kHz (listed): answered" : " kHz (listed): no answer");
    sb_log(u.b);
    return ok;
}

int sb_acpm_build(char out[25])
{
    if (!acpm_find(ACPM_PMIC_CH)) {
        out[0] = 0;
        return 0;
    }
    for (unsigned i = 0; i < 24; i += 4) {
        uint32_t w = ard(ACPM_INITDATA + 0x60 + i);
        for (unsigned b = 0; b < 4; ++b) {
            char ch = (char)(w >> (8 * b));
            out[i + b] = (ch >= 32 && ch < 127) ? ch : '.';
        }
    }
    out[24] = 0;
    return 1;
}

/* ---- RTC (S2MPS18 RTC block = ACPM type 2, build/woa/rtc-research.md) ----------------------- */

void sb_rtc_read(struct sb_rtc *r)
{
    uint8_t u = 0, ctrl = 0, v[7];
    r->ok = 0;
    if (!sb_pmic_read(2, 0x00, &ctrl) || !sb_pmic_read(2, 0x02, &u))
        return;
    /* UPDATE (RUDR): copy the counters into the read buffer; RTC_WAKE and unknown bits kept */
    if (!pmic_write(2, 0x02, (uint8_t)((u & ~0x17U) | 0x01U)))
        return;
    sb_mdelay(2);
    for (unsigned i = 0; i < 7; ++i)
        if (!sb_pmic_read(2, 0x05 + i, &v[i]))
            return;
    r->s = v[0] & 0x7fU;
    r->mi = v[1] & 0x7fU;
    r->h = v[2] & 0x1fU;
    r->d = v[4] & 0x1fU;
    r->mo = v[5] & 0x0fU;
    r->y = 2000U + (v[6] & 0x7fU);
    r->ctrl = ctrl;
    r->ok = r->s < 60 && r->mi < 60 && r->h < 24 && r->d >= 1 && r->d <= 31 && r->mo >= 1 && r->mo <= 12;
}

int sb_rtc_epoch(uint64_t *epoch)
{
    struct sb_rtc r;
    sb_rtc_read(&r);
    if (!r.ok)
        return 0;
    unsigned y = r.y - (r.mo <= 2), era = y / 400, yoe = y - era * 400;
    unsigned doy = (153 * (r.mo + (r.mo > 2 ? -3 : 9)) + 2) / 5 + r.d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint64_t days = (uint64_t)era * 146097 + doe - 719468;
    *epoch = days * 86400 + r.h * 3600 + r.mi * 60 + r.s;
    return 1;
}

/* ---- touch rails (menu.c tp_power / hw_touch_rails) ----------------------------------------- */

/* Read only in v1: switching a rail on is a Tier 1 action (engineer/armed.c, dry-run). The
 * loader's menu turned the touch rails on before Engineer OS was started. 1 on (with the
 * kernel's voltage checks passing), 0 off or unexpected, -1 ACPM did not answer. */
int sb_touch_rails(uint8_t *l35, uint8_t *l43)
{
    uint8_t id = 0;
    if (!sb_pmic_read(0, 0x00, &id) || !sb_pmic_read(1, 0x6c, l35) || !sb_pmic_read(1, 0x74, l43))
        return -1;
    if (((*l35 & 0x3fU) != 0x2cU && (*l35 & 0x3fU) != 0) ||
        ((*l43 & 0x3fU) != 0 && ((*l43 & 0x3fU) < 0x28U || (*l43 & 0x3fU) > 0x3cU)))
        return 0;
    return (*l35 & 0xc0U) == 0xc0U && (*l43 & 0xc0U) == 0xc0U;
}

/* The microSD rails (LDO31 vmmc, LDO2 vqmmc): 1 when both are on in the card's range. */
int sb_sd_rails(uint8_t *l31, uint8_t *l2)
{
    if (!sb_pmic_read(1, 0x68, l31) || !sb_pmic_read(1, 0x4a, l2))
        return -1;
    unsigned v = *l31 & 0x3fU;
    return (*l31 & 0xc0U) == 0xc0U && v >= 0x24U && v <= 0x3cU && (*l2 & 0xc0U) == 0xc0U;
}
