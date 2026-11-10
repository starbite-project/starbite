/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: stage 2b of the probe (docs/MODEM.md section 5.1). T2 found the carve-out readable before any EL3 call,
 * with RADIO's TOC at modem_if + 0. Before stage 3 copies the CP image in, three questions:
 *   step 3 (read only): what did S-Boot leave there: is it RADIO raw (TOC first), is anything already at cbd's
 *          places (MAIN at + 0x10000, VSS at + 0x7900000)? Compared range by range against the RADIO partition.
 *   step 4 (user present): what are CP_CTRL_S / CP_CTRL_NS (EL3's read call, as PMUCAL does it), and can the AP
 *          store into the carve-out (two fixed words, each put back)?
 * Every block is announced with a "probe: ..." line first, so a hang shows in pmsg where it was.
 */
#include "sbm.h"

/* zlib's CRC-32, four bits at a time (a 16-entry table: no static data to set up, ~4x the bitwise speed) */
/* zlib's CRC-32, slicing-by-8 (8 KiB of tables built on first use): the streamed start CRCs about 126 MiB (the check,
 * the copy and the read-back), and the nibble version took seconds of it (night-1110: a 5.6 s start) */
uint32_t sbm_crc32(uint32_t crc, const void *buf, size_t n)
{
    static uint32_t t[8][256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        ready = 1;
    }
    const uint8_t *b = buf;
    crc = ~crc;
    while (n && ((uintptr_t)b & 7)) {
        crc = (crc >> 8) ^ t[0][(crc ^ *b++) & 0xFF];
        --n;
    }
    while (n >= 8) {
        uint32_t lo = crc ^ ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
        uint32_t hi = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
        crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
        b += 8;
        n -= 8;
    }
    while (n--)
        crc = (crc >> 8) ^ t[0][(crc ^ *b++) & 0xFF];
    return ~crc;
}

/* ---- step 3: survey ---------------------------------------------------------------------------- */

static void add(struct sbm_survey *sv, const char *what, uint64_t carve_off, uint64_t radio_off, uint64_t len)
{
    if (sv->n >= SBM_SURVEY_MAX || len == 0)
        return;
    if (len > SBM_SURVEY_CHUNK)
        len = SBM_SURVEY_CHUNK;
    if (carve_off + len > SBM_MODEM_IF_SIZE || (carve_off & 3) || (len & 3))
        return;
    struct sbm_survey_ent *e = &sv->e[sv->n++];
    sbm_strlcpy(e->what, what, sizeof(e->what));
    e->carve_off = (uint32_t)carve_off;
    e->radio_off = (uint32_t)radio_off;
    e->len = (uint32_t)len;
}

void sbm_probe_survey(const struct sbm_plat *p, struct sbm_survey *sv)
{
    sbm_memset(sv, 0, sizeof(*sv));
    if (!p || !p->probe32)
        return;
    /* the carve-out's own TOC (S-Boot's copy) */
    uint8_t toc[SBM_TOC_BYTES];
    int ok = 1;
    sbm_logf(p, "probe: reading the TOC at modem_if+0 (512 bytes)");
    for (unsigned i = 0; i < SBM_TOC_BYTES && ok; i += 4) {
        uint32_t v;
        ok = p->probe32(p->ctx, SBM_MODEM_IF_PA + i, &v);
        sbm_put32(toc + i, v);
    }
    sv->toc_ok = ok && sbm_toc_parse(toc, 0, &sv->toc) == 0;
    uint32_t boot_off = 0x200, boot_size = 0x1e40, main_off = 0x2040, main_size = 0x23d8ea0, vss_off = 0x23daee0,
             vss_size = 0x57cbf0;
    if (sv->toc_ok) {
        const struct sbm_toc *t = &sv->toc;
        boot_off = t->e[t->boot].offset;
        boot_size = t->e[t->boot].size;
        main_off = t->e[t->main].offset;
        main_size = t->e[t->main].size;
        if (t->vss >= 0) {
            vss_off = t->e[t->vss].offset;
            vss_size = t->e[t->vss].size;
        }
    }
    uint32_t tail = main_size > SBM_SURVEY_CHUNK ? (main_size - SBM_SURVEY_CHUNK) & ~3U : 0;
    add(sv, "TOC raw", 0, 0, 0x200);
    add(sv, "BOOT raw", boot_off & ~3U, boot_off & ~3U, boot_size & ~3U);
    add(sv, "MAIN head raw", main_off, main_off, SBM_SURVEY_CHUNK);
    add(sv, "MAIN head cbd", 0x10000, main_off, SBM_SURVEY_CHUNK);
    add(sv, "MAIN tail raw", (uint64_t)main_off + tail, (uint64_t)main_off + tail, SBM_SURVEY_CHUNK);
    add(sv, "MAIN tail cbd", 0x10000ULL + tail, (uint64_t)main_off + tail, SBM_SURVEY_CHUNK);
    add(sv, "VSS raw", vss_off & ~3U, vss_off & ~3U, vss_size & ~3U);
    add(sv, "VSS cbd", SBM_DT_CP_SIZE, vss_off & ~3U, vss_size & ~3U);

    int carve_dead = !ok;
    for (unsigned i = 0; i < sv->n; ++i) {
        struct sbm_survey_ent *e = &sv->e[i];
        sbm_logf(p, "probe: survey %s: modem_if+0x%08x / RADIO+0x%08x, 0x%x bytes", e->what, e->carve_off,
                 e->radio_off, e->len);
        if (!carve_dead) {
            uint32_t crc = 0;
            e->carve_ok = 1;
            for (uint32_t o = 0; o < e->len; o += 4) {
                uint32_t v;
                uint8_t b[4];
                if (!p->probe32(p->ctx, SBM_MODEM_IF_PA + e->carve_off + o, &v)) {
                    e->carve_ok = 0;
                    carve_dead = 1;     /* no more carve-out reads after a refused one */
                    break;
                }
                e->zero_words += v == 0;
                sbm_put32(b, v);
                crc = sbm_crc32(crc, b, 4);
            }
            e->crc_carve = crc;
        }
        if (p->radio_read) {
            uint8_t buf[512];
            uint32_t crc = 0;
            e->radio_ok = 1;
            for (uint32_t o = 0; o < e->len; o += sizeof(buf)) {
                uint32_t k = e->len - o < sizeof(buf) ? e->len - o : sizeof(buf);
                if (p->radio_read(p->ctx, (uint64_t)e->radio_off + o, buf, k) != 0) {
                    e->radio_ok = 0;
                    break;
                }
                crc = sbm_crc32(crc, buf, k);
            }
            e->crc_radio = crc;
        }
    }
    sbm_logf(p, "probe: survey done");
}

void sbm_survey_report(const struct sbm_plat *p, const struct sbm_survey *sv)
{
    if (!p || !p->log)
        return;
    if (sv->toc_ok)
        sbm_logf(p, "SURVEY: carve-out TOC parsed: %u entries, BOOT 0x%x+0x%x, MAIN 0x%x+0x%x", sv->toc.n,
                 sv->toc.e[sv->toc.boot].offset, sv->toc.e[sv->toc.boot].size, sv->toc.e[sv->toc.main].offset,
                 sv->toc.e[sv->toc.main].size);
    else
        sbm_logf(p, "SURVEY: no TOC at modem_if+0 (this phone's known offsets used)");
    unsigned match = 0;
    for (unsigned i = 0; i < sv->n; ++i) {
        const struct sbm_survey_ent *e = &sv->e[i];
        const char *v = !e->carve_ok ? "carve-out refused" : !e->radio_ok ? "RADIO unread"
                      : e->crc_carve == e->crc_radio ? "MATCH" : e->zero_words * 4 == e->len ? "zero" : "differs";
        match += e->carve_ok && e->radio_ok && e->crc_carve == e->crc_radio;
        sbm_logf(p, "SURVEY: %s: %s (carve crc 0x%08x, RADIO crc 0x%08x, %u zero words of %u)", e->what, v,
                 e->crc_carve, e->crc_radio, e->zero_words, e->len / 4);
    }
    sbm_logf(p, "SURVEY: %u of %u ranges match RADIO", match, sv->n);
}

/* ---- step 4: CP_CTRL through EL3, and a store test --------------------------------------------- */

int sbm_cp_ctrl_read(const struct sbm_plat *p, unsigned reg, uint32_t *v, uint32_t *err)
{
    *v = 0;
    if (err)
        *err = 0;
    if (!p || !p->smc || reg > 1)
        return SBM_EPERM;
    uint32_t half[2];
    for (unsigned h = 0; h < 2; ++h) {
        uint32_t r = (uint32_t)p->smc(p->ctx, SBM_SMC_CP, SBM_SMC_READ_CTRL, h, reg);
        if (r & 0xFFFF) {
            if (err)
                *err = r & 0xFFFF;
            return SBM_EIO;
        }
        half[h] = r >> 16;
    }
    *v = half[1] << 16 | half[0];
    return 0;
}

void sbm_probe_write_test(const struct sbm_plat *p, struct sbm_wtest w[SBM_WTEST_N])
{
    const uint64_t pa[SBM_WTEST_N] = {
        SBM_MODEM_IF_PA + SBM_DT_IPC_OFF + SBM_SBD_CMD_OFF,   /* IPC command area (IOSM off: unused) */
        SBM_MODEM_IF_PA + SBM_DT_CP_SIZE - 4,                 /* last word of the CP's own memory */
    };
    sbm_memset(w, 0, sizeof(struct sbm_wtest) * SBM_WTEST_N);
    for (unsigned i = 0; i < SBM_WTEST_N; ++i) {
        w[i].pa = pa[i];
        w[i].result = "skipped";      /* also for the words after a stop */
    }
    for (unsigned i = 0; i < SBM_WTEST_N; ++i) {
        struct sbm_wtest *t = &w[i];
        if (!p || !p->probe32 || !p->poke32)
            continue;
        sbm_logf(p, "probe: store test at 0x%08x: read", (unsigned)t->pa);
        t->read_ok = p->probe32(p->ctx, t->pa, &t->before);
        if (!t->read_ok) {
            t->result = "unreadable";
            break;                       /* nothing stored where it cannot even be read */
        }
        t->pattern = t->before ^ 0xA5A5A5A5U;
        sbm_logf(p, "probe: store test at 0x%08x: store 0x%08x", (unsigned)t->pa, t->pattern);
        t->store_ok = p->poke32(p->ctx, t->pa, t->pattern);
        t->after_ok = p->probe32(p->ctx, t->pa, &t->after);
        sbm_logf(p, "probe: store test at 0x%08x: put back 0x%08x", (unsigned)t->pa, t->before);
        if (t->after_ok && t->after != t->before) {
            p->poke32(p->ctx, t->pa, t->before);
            uint32_t r;
            t->restore_ok = p->probe32(p->ctx, t->pa, &r) && r == t->before;
            t->restored = r;
        } else {
            t->restore_ok = t->after_ok;  /* nothing changed */
            t->restored = t->after;
        }
        if (!t->store_ok)
            t->result = "store-faulted";
        else if (!t->after_ok)
            t->result = "unreadable";
        else if (t->after == t->pattern)
            t->result = "writable";
        else if (t->after == t->before)
            t->result = "write-ignored";
        else
            t->result = "other";
        if (!t->store_ok || !t->after_ok)
            break;
    }
}

void sbm_wtest_report(const struct sbm_plat *p, const struct sbm_wtest w[SBM_WTEST_N])
{
    for (unsigned i = 0; i < SBM_WTEST_N; ++i) {
        const struct sbm_wtest *t = &w[i];
        sbm_logf(p, "STORE: 0x%08x: %s (before 0x%08x, stored 0x%08x, read back 0x%08x, put back %s 0x%08x)",
                 (unsigned)t->pa, t->result, t->before, t->pattern, t->after, t->restore_ok ? "ok" : "FAILED",
                 t->restored);
    }
}
