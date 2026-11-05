/* SPDX-License-Identifier: GPL-2.0-only
 * Test history (EN2). Every result is kept in RAM for this session and appended to the pmsg ring
 * as one line, "SBENG1 T <PASS|FAIL> <name> | <detail>", next to a "SBENG1 B <n>" line per start.
 * The pmsg ring survives warm resets, so the next start reads the earlier sessions back from it
 * (and TWRP shows them in /sys/fs/pstore/pmsg-ramoops-0). The ring is RAM: no storage is
 * written. History on the card (survives a power-off) needs the write fence (EN4).
 */
#include "eng.h"

#define HIST_N 96

struct rec {
    char name[40], detail[96];
    uint8_t pass, earlier;
    uint32_t t;                 /* seconds since power-on (this session) or the session number */
};
static struct rec hist[HIST_N];
static unsigned nhist, sessions_seen;

static void add(const char *name, int pass, const char *detail, int earlier, uint32_t t)
{
    if (nhist >= HIST_N) {
        for (unsigned i = 1; i < HIST_N; ++i)
            hist[i - 1] = hist[i];
        nhist = HIST_N - 1;
    }
    struct rec *r = &hist[nhist++];
    sb_strcpy(r->name, sizeof(r->name), name);
    sb_strcpy(r->detail, sizeof(r->detail), detail);
    r->pass = (uint8_t)(pass != 0);
    r->earlier = (uint8_t)earlier;
    r->t = t;
}

void test_record(const char *name, int pass, const char *detail)
{
    add(name, pass, detail ? detail : "", 0, (uint32_t)(sb_ms() / 1000U));
    struct ub u;
    ub_init(&u);
    ub_s(&u, "SBENG1 T ");
    ub_s(&u, pass ? "PASS " : "FAIL ");
    ub_s(&u, name);
    ub_s(&u, " | ");
    ub_s(&u, detail ? detail : "");
    ub_c(&u, '\n');
    sb_pmsg_put(u.b);
    ub_init(&u);
    ub_s(&u, "test ");
    ub_s(&u, name);
    ub_s(&u, pass ? ": PASS" : ": FAIL");
    sb_log(u.b);
}

/* Read the ring as it was at start: every SBENG1 line before ours is an earlier session's. */
void tests_load_history(void)
{
    volatile uint32_t *h = (volatile uint32_t *)SB_PMSG;
    const volatile char *d = (const volatile char *)(SB_PMSG + 12);
    const uint32_t cap = 0x3f00;
    if (h[0] == 0x43474244U) {
        uint32_t start = h[1], size = h[2];
        if (size > cap)
            size = cap;
        uint32_t pos = size < cap ? 0 : start % cap;
        char line[200];
        unsigned k = 0;
        for (uint32_t i = 0; i <= size; ++i) {
            char c = i < size ? d[(pos + i) % cap] : '\n';
            if (c != '\n' && k < sizeof(line) - 1) {
                line[k++] = c;
                continue;
            }
            line[k] = 0;
            k = 0;
            if (sb_prefix(line, "SBENG1 B ")) {
                ++sessions_seen;
            } else if (sb_prefix(line, "SBENG1 T ")) {
                const char *p = line + 9;
                int pass = sb_prefix(p, "PASS");
                p += 5;
                char name[40];
                unsigned n = 0;
                while (*p && !(p[0] == ' ' && p[1] == '|') && n < sizeof(name) - 1)
                    name[n++] = *p++;
                name[n] = 0;
                if (*p)
                    p += 3;
                add(name, pass, p, 1, sessions_seen);
            }
        }
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "SBENG1 B ");
    ub_dec(&u, sessions_seen + 1U);
    ub_c(&u, '\n');
    sb_pmsg_put(u.b);
}

static void fill_history(void)
{
    struct ub k, v;
    unsigned now = 0, earlier = 0, fails = 0;
    for (unsigned i = 0; i < nhist; ++i) {
        if (hist[i].earlier)
            ++earlier;
        else
            ++now;
        fails += !hist[i].pass && !hist[i].earlier;
    }
    ub_init(&v);
    ub_dec(&v, now);
    ub_s(&v, now == 1 ? " result" : " results");
    if (fails) {
        ub_s(&v, ", ");
        ub_dec(&v, fails);
        ub_s(&v, " failed");
    }
    kv_bigc("this session", v.b, fails ? C_RED : now ? C_GREEN : C_L1);
    for (int pass = 0; pass < 2; ++pass) {
        int want_earlier = pass == 1;
        kv_hdr(want_earlier ? "EARLIER SESSIONS (PMSG RING)" : "THIS SESSION");
        int any = 0;
        for (unsigned j = nhist; j-- > 0;) {
            const struct rec *r = &hist[j];
            if (r->earlier != want_earlier)
                continue;
            ub_init(&k);
            ub_s(&k, r->pass ? U_CHECK " " : "x ");
            ub_s(&k, r->name);
            ub_init(&v);
            if (!want_earlier) {
                ub_dec(&v, r->t / 60U);
                ub_c(&v, ':');
                ub_dec2(&v, r->t % 60U);
                ub_s(&v, "  ");
            } else {
                ub_s(&v, "session ");
                ub_dec(&v, r->t);
                ub_s(&v, "  ");
            }
            ub_s(&v, r->detail);
            kv_addc(k.b, v.b, r->pass ? C_GREEN : C_RED);
            any = 1;
        }
        if (!any)
            kv_note(want_earlier ? "None in the ring (a cold boot or a Windows start clears it)."
                                 : "No test run yet: Tests tab.");
    }
    kv_note("Kept in the pmsg RAM ring: it survives a reboot, not a power-off. History on the card "
            "comes with the write fence (EN4).");
}

void tests_history(void)
{
    info_screen("Test history", fill_history, 0);
}
