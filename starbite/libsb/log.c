/* SPDX-License-Identifier: GPL-2.0-only
 * The Starbite log ring: this session's lines in RAM, each also appended to the ramoops pmsg ring
 * at 0xFED14000 ("DBGC" header: u32 magic, start, size; data from +12, 0x3F00 bytes as the
 * loader's pmsg_put keeps it, clear of the latch page). The ring survives a warm reset, so TWRP
 * shows it in /sys/fs/pstore/pmsg-ramoops-0, and Engineer OS reads its own test-history lines
 * back from it after a reboot (engineer/tests.c). Plain RAM: not a storage write.
 */
#include "sb.h"

#define LOG_LINES 512U
#define LOG_COLS 120U
#define PMSG_DATA 0x3F00U

static char ring[LOG_LINES][LOG_COLS];
static unsigned head, count;

void sb_pmsg_put(const char *s)
{
    volatile uint32_t *h = (volatile uint32_t *)SB_PMSG;
    volatile uint8_t *d = (volatile uint8_t *)(SB_PMSG + 12);
    if (h[0] != 0x43474244U || h[1] >= PMSG_DATA || h[2] > PMSG_DATA) {
        h[0] = 0x43474244U;             /* "DBGC" */
        h[1] = 0;
        h[2] = 0;
    }
    for (; *s; ++s) {
        uint32_t start = h[1];
        d[start] = (uint8_t)*s;
        h[1] = (start + 1) % PMSG_DATA;
        if (h[2] < PMSG_DATA)
            h[2] = h[2] + 1;
    }
    __asm__ volatile("dsb sy" : : : "memory");
}

void sb_log(const char *s)
{
    char *l = ring[head];
    unsigned n = 0;
    uint64_t ms = sb_ms();
    struct ub t;
    ub_init(&t);
    ub_dec(&t, ms / 1000U);
    ub_c(&t, '.');
    ub_c(&t, (char)('0' + (ms / 100U) % 10U));
    ub_c(&t, (char)('0' + (ms / 10U) % 10U));
    ub_c(&t, ' ');
    for (unsigned i = 0; t.b[i] && n + 1 < LOG_COLS; ++i)
        l[n++] = t.b[i];
    for (unsigned i = 0; s[i] && n + 1 < LOG_COLS; ++i)
        l[n++] = s[i];
    l[n] = 0;
    head = (head + 1) % LOG_LINES;
    if (count < LOG_LINES)
        ++count;
    sb_pmsg_put("eng: ");
    sb_pmsg_put(s);
    sb_pmsg_put("\n");
}

void sb_logf(const char *a, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    ub_s(&u, b);
    sb_log(u.b);
}

void sb_logx(const char *a, uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    ub_hex(&u, v, v >> 32 ? 16 : 8);
    sb_log(u.b);
}

unsigned sb_log_count(void)
{
    return count;
}

const char *sb_log_line(unsigned i)
{
    if (i >= count)
        return "";
    return ring[(head + LOG_LINES - count + i) % LOG_LINES];
}
