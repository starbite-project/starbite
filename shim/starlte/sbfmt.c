/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite formats and decisions without hardware: see sbfmt.h. Built into the loader and into
 * the host test (tools/loader-test/host_test.c), byte for byte the same code.
 */
#include "sbfmt.h"

/* ---- small helpers (no libc) ----------------------------------------------------------------- */

static void z(void *p, unsigned n)
{
    uint8_t *b = (uint8_t *)p;
    for (unsigned i = 0; i < n; ++i)
        b[i] = 0;
}

static uint64_t get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | p[i - 1];
    return v;
}

static void put(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* NUL-terminated copy of at most n - 1 characters from a fixed field of n bytes. */
static void sget(char *dst, const uint8_t *src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && src[k]; ++k)
        dst[k] = (char)src[k];
    for (; k < n; ++k)
        dst[k] = 0;
}

static void sput(uint8_t *dst, const char *src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && src[k]; ++k)
        dst[k] = (uint8_t)src[k];
    for (; k < n; ++k)
        dst[k] = 0;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}

uint32_t sb_crc32_more(uint32_t crc, const void *p, uint64_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t c = ~crc;
    for (uint64_t i = 0; i < n; ++i) {
        c ^= b[i];
        for (unsigned k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

uint32_t sb_crc32(const void *p, uint64_t n)
{
    return sb_crc32_more(0, p, n);
}

/* ---- settings record -------------------------------------------------------------------------- */

#define SET_MAGIC 0x474e545445534253ULL   /* "SBSETTNG" */

int sbset_check(const uint8_t *b)
{
    if (get(b, 8) != SET_MAGIC)
        return 0;
    if (get(b + 8, 4) != 1 || sb_crc32(b + 16, SBSET_BYTES - 16) != (uint32_t)get(b + 12, 4))
        return -1;
    return 1;
}

void sbset_defaults(struct star_settings *s)
{
    z(s, sizeof(*s));
}

void sbset_decode(const uint8_t *b, struct star_settings *s)
{
    sbset_defaults(s);
    s->seq = (uint32_t)get(b + 16, 4);
    s->timeout_s = (uint32_t)get(b + 20, 4);
    s->flags = (uint32_t)get(b + 24, 4);
    sget(s->default_key, b + 32, 24);
    sget(s->last_key, b + 56, 24);
    sget(s->next_key, b + 80, 24);
    s->crash_mode = b[104];
    s->crash_detail = b[105];
    s->crash_action = b[106];
    s->crash_timeout = b[107];
    s->wdt_mode = b[108];
    s->wdt_s = (uint16_t)get(b + 110, 2);
}

void sbset_encode(uint8_t *b, const struct star_settings *s)
{
    z(b, CL_BLOCK);
    put(b, SET_MAGIC, 8);
    put(b + 8, 1, 4);
    put(b + 16, s->seq, 4);
    put(b + 20, s->timeout_s, 4);
    put(b + 24, s->flags, 4);
    sput(b + 32, s->default_key, 24);
    sput(b + 56, s->last_key, 24);
    sput(b + 80, s->next_key, 24);
    b[104] = s->crash_mode;
    b[105] = s->crash_detail;
    b[106] = s->crash_action;
    b[107] = s->crash_timeout;
    b[108] = s->wdt_mode;
    put(b + 110, s->wdt_s, 2);
    put(b + 12, sb_crc32(b + 16, SBSET_BYTES - 16), 4);
}

unsigned sbset_post(const struct star_settings *s)
{
    unsigned v = SET_POST_OF(s->flags);
    return v <= SET_POST_OFF ? v : SET_POST_FULL;
}

unsigned sbset_log(const struct star_settings *s)
{
    return SET_LOG_OF(s->flags);
}

void sbset_set_post(struct star_settings *s, unsigned v)
{
    s->flags = (s->flags & ~SET_POST_MASK) | ((v & 3U) << SET_POST_SHIFT);
}

void sbset_set_log(struct star_settings *s, unsigned v)
{
    s->flags = (s->flags & ~SET_LOG_MASK) | ((v & 3U) << SET_LOG_SHIFT);
}

unsigned sbset_crash_timeout(const struct star_settings *s)
{
    return !s->crash_timeout ? 60U : s->crash_timeout > 60 ? 600U : 10U * s->crash_timeout;
}

const char *sbset_post_name(unsigned v)
{
    switch (v) {
    case SET_POST_FULL:  return "FULL";
    case SET_POST_QUICK: return "QUICK";
    default:             return "OFF";
    }
}

const char *sbset_log_name(unsigned v)
{
    switch (v) {
    case SET_LOG_QUIET:   return "0 QUIET";
    case SET_LOG_NORMAL:  return "1 NORMAL";
    case SET_LOG_VERBOSE: return "2 VERBOSE";
    default:              return "3 DEBUG";
    }
}

/* ---- reset reasons ---------------------------------------------------------------------------- */

const char *sb_rst_bit_name(unsigned bit)
{
    switch (bit) {
    case 0: case 1: case 2: case 3:     return "CLUSTER1_DBGRSTREQ";
    case 4: case 5: case 6: case 7:     return "CLUSTER0_DBGRSTREQ";
    case 8: case 9: case 10: case 11:   return "CLUSTER1_WARMRSTREQ";
    case 12: case 13: case 14: case 15: return "CLUSTER0_WARMRSTREQ";
    case 16: return "PINRESET";
    case 19: return "CHUB_CPU_WDTRESET";
    case 20: return "APM_WDTRESET";
    case 21: return "APM_SYSRESET";
    case 22: return "VTS_CPU_WDTRESET";
    case 23: return "CLUSTER1_WDTRESET";
    case 24: return "CLUSTER0_WDTRESET";
    case 25: return "AUD_CA7_WDTRESET";
    case 26: return "SSS_WDTRESET";
    case 28: return "WRESET";
    case 29: return "SWRESET";
    default: return 0;
    }
}

const char *sb_rr_name(int rr)
{
    switch (rr) {
    case RR_S: return "SPON (S-BOOT: FAULT)";
    case RR_W: return "WPON WATCHDOG";
    case RR_D: return "DPON (S-BOOT: DEBUG/UPLOAD)";
    case RR_K: return "KPON KERNEL PANIC";
    case RR_M: return "MPON MANUAL RESET (KEYS)";
    case RR_P: return "PPON (S-BOOT: FAULT)";
    case RR_R: return "RPON REBOOT";
    case RR_B: return "BPON";
    case RR_N: return "NPON POWER ON";
    case RR_T: return "TPON";
    default:   return "NONE";
    }
}

const char *sb_inform3_name(uint32_t v)
{
    switch (v) {
    case 0:           return "EMPTY";
    case 0x12345670U: return "NORMAL";
    case 0x12345671U: return "DOWNLOAD";
    case 0x12345672U: return "UPLOAD";
    case 0x12345673U: return "CHARGING";
    case 0x12345674U: return "RECOVERY";
    case 0x12345675U: return "FOTA";
    case 0x1234567dU: return "BOOTLOADER";
    case SEC_CAUSE_PANIC: return "PANIC (SEC_DEBUG)";
    case 0x00000066U: return "HARD RESET (SEC_DEBUG)";
    case 0x00000085U: return "POWER KEY LONG PRESS";
    case 0xCAFEBABEU: return "SEC_DEBUG INIT";
    default:          return 0;
    }
}

const char *sb_kind_name(unsigned kind)
{
    switch (kind) {
    case CK_NONE:      return "NO OS WAS RUNNING";
    case CK_CLEAN:     return "CLEAN RESTART";
    case CK_POWER:     return "POWER ON";
    case CK_PANIC:     return "KERNEL PANIC";
    case CK_WATCHDOG:  return "HANG (WATCHDOG)";
    case CK_SWHANG:    return "HANG (OS TIMER)";
    case CK_FORCED:    return "FORCED RESTART";
    case CK_FAULT:     return "FAULT (S-BOOT)";
    case CK_LOADERWDT: return "WATCHDOG, NO OS";
    case CK_OOPS:      return "KERNEL OOPS";
    default:           return "UNEXPLAINED RESET";
    }
}

/* The order matters: what the OS itself left behind first, then the hardware, then S-Boot's
 * code, then the quiet endings. */
unsigned sb_classify(const struct sb_evidence *e, unsigned *severity)
{
    unsigned sev = SEV_INFO, k;
    if (!e->launched) {
        k = (e->rst_stat & RST_WDT_ANY) ? CK_LOADERWDT : CK_NONE;
        sev = k == CK_LOADERWDT ? SEV_WARN : SEV_INFO;
    } else if (e->ev & (EV_LX_PANIC | EV_AND_PANIC | EV_XNU_PANIC)) {
        k = CK_PANIC;
        sev = SEV_CRASH;
    } else if ((e->rst_stat & RST_WDT_ANY) || e->sboot_rr == RR_W) {
        k = CK_WATCHDOG;
        sev = SEV_CRASH;
    } else if (e->ev & EV_XNU_HANG) {
        k = CK_SWHANG;
        sev = SEV_CRASH;
    } else if (e->sboot_rr == RR_K || e->inform3 == SEC_CAUSE_PANIC) {
        k = CK_PANIC;
        sev = SEV_CRASH;
    } else if (e->sboot_rr == RR_S || e->sboot_rr == RR_D || e->sboot_rr == RR_P) {
        k = CK_FAULT;
        sev = SEV_CRASH;
    } else if (e->sboot_rr == RR_M || (e->rst_stat & RST_WRESET) ||
               (e->sboot_pwron >= 0 && (e->sboot_pwron & 0x08) && !(e->rst_stat & RST_SWRESET))) {
        k = CK_FORCED;
        sev = SEV_WARN;
    } else if (e->ev & (EV_LX_OOPS | EV_LX_OTHER)) {
        k = CK_OOPS;
        sev = SEV_WARN;
    } else if (e->sboot_rr == RR_T || e->sboot_rr == RR_B) {
        k = CK_FAULT;
        sev = SEV_WARN;
    } else if ((e->rst_stat & RST_PINRESET) || e->sboot_rr == RR_N) {
        k = CK_POWER;
        /* SMPL (S2MPS18 PWRONSRC bit 6): the battery let go for a moment */
        sev = e->sboot_pwron >= 0 && (e->sboot_pwron & 0x40) ? SEV_WARN : SEV_INFO;
    } else if ((e->rst_stat & RST_SWRESET) || e->sboot_rr == RR_R) {
        k = CK_CLEAN;
    } else {
        k = CK_UNKNOWN;
        sev = SEV_WARN;
    }
    if (severity)
        *severity = sev;
    return k;
}

const char *sb_source_name(unsigned src)
{
    switch (src) {
    case RS_DMESG:   return "LINUX RAMOOPS DMESG RECORD";
    case RS_CONSOLE: return "CONSOLE RING";
    case RS_SECDBG:  return "SAMSUNG SEC_DEBUG PANIC STRING";
    case RS_PMSG:    return "PMSG RING";
    case RS_SBOOT:   return "S-BOOT RESET CODE";
    default:          return "REGISTERS ONLY";
    }
}

/* ---- crash log -------------------------------------------------------------------------------- */

#define CL_MAGIC 0x3148534152434253ULL   /* "SBCRASH1" */
#define CR_MAGIC 0x3150455252434253ULL   /* "SBCRREP1" */

void cl_head_init(struct cl_head *h)
{
    z(h, sizeof(*h));
    h->slots = CL_SLOTS;
    h->slot_size = CL_BLOCK;
}

int cl_head_decode(const uint8_t *b, struct cl_head *h)
{
    cl_head_init(h);
    if (get(b, 8) != CL_MAGIC)
        return 0;
    if (get(b + 8, 4) != 1 || sb_crc32(b + 16, CL_HEAD_CRC - 16) != (uint32_t)get(b + 12, 4))
        return -1;
    h->slots = (uint32_t)get(b + 16, 4);
    h->slot_size = (uint32_t)get(b + 20, 4);
    h->next = (uint32_t)get(b + 24, 4);
    h->count = (uint32_t)get(b + 28, 4);
    h->boots = (uint32_t)get(b + 32, 4);
    h->launch_seq = (uint32_t)get(b + 36, 4);
    h->launch_state = (uint32_t)get(b + 40, 4);
    h->method = b[44];
    h->wdt_armed = b[45];
    h->wdt_s = (uint16_t)get(b + 46, 2);
    sget(h->key, b + 48, 24);
    sget(h->name, b + 72, 32);
    h->launch_epoch = get(b + 104, 8);
    h->log_level = (uint32_t)get(b + 112, 4);
    h->lflags = (uint32_t)get(b + 116, 4);
    h->seen_dmesg = (uint32_t)get(b + 120, 4);
    h->seen_console = (uint32_t)get(b + 124, 4);
    h->seen_secmagic = (uint32_t)get(b + 128, 4);
    h->seen_pmsg = (uint32_t)get(b + 132, 4);
    if (h->slots != CL_SLOTS || h->slot_size != CL_BLOCK || h->next >= CL_SLOTS)
        return -1;
    return 1;
}

void cl_head_encode(uint8_t *b, const struct cl_head *h)
{
    z(b, CL_BLOCK);
    put(b, CL_MAGIC, 8);
    put(b + 8, 1, 4);
    put(b + 16, CL_SLOTS, 4);
    put(b + 20, CL_BLOCK, 4);
    put(b + 24, h->next % CL_SLOTS, 4);
    put(b + 28, h->count, 4);
    put(b + 32, h->boots, 4);
    put(b + 36, h->launch_seq, 4);
    put(b + 40, h->launch_state, 4);
    b[44] = h->method;
    b[45] = h->wdt_armed;
    put(b + 46, h->wdt_s, 2);
    sput(b + 48, h->key, 24);
    sput(b + 72, h->name, 32);
    put(b + 104, h->launch_epoch, 8);
    put(b + 112, h->log_level, 4);
    put(b + 116, h->lflags, 4);
    put(b + 120, h->seen_dmesg, 4);
    put(b + 124, h->seen_console, 4);
    put(b + 128, h->seen_secmagic, 4);
    put(b + 132, h->seen_pmsg, 4);
    put(b + 12, sb_crc32(b + 16, CL_HEAD_CRC - 16), 4);
}

int cl_report_decode(const uint8_t *b, struct cl_report *r)
{
    z(r, sizeof(*r));
    if (get(b, 8) != CR_MAGIC)
        return 0;
    if (get(b + 8, 4) != 1 || sb_crc32(b + 16, CL_BLOCK - 16) != (uint32_t)get(b + 12, 4))
        return -1;
    r->seq = (uint32_t)get(b + 16, 4);
    r->kind = b[20];
    r->severity = b[21];
    r->method = b[22];
    r->source = b[23];
    r->epoch_detect = get(b + 24, 8);
    r->epoch_launch = get(b + 32, 8);
    r->epoch_event = get(b + 40, 8);
    sget(r->key, b + 48, 24);
    sget(r->name, b + 72, 32);
    for (unsigned i = 0; i < 16; ++i)
        r->reg[i] = (uint32_t)get(b + 104 + 4 * i, 4);
    sget(r->reason, b + 168, 96);
    r->text_len = (uint16_t)get(b + 264, 2);
    r->text_lines = (uint16_t)get(b + 266, 2);
    if (r->text_len > CR_TEXT_MAX)
        r->text_len = CR_TEXT_MAX;
    for (unsigned i = 0; i < r->text_len; ++i)
        r->text[i] = (char)b[272 + i];
    return 1;
}

void cl_report_encode(uint8_t *b, const struct cl_report *r)
{
    z(b, CL_BLOCK);
    put(b, CR_MAGIC, 8);
    put(b + 8, 1, 4);
    put(b + 16, r->seq, 4);
    b[20] = r->kind;
    b[21] = r->severity;
    b[22] = r->method;
    b[23] = r->source;
    put(b + 24, r->epoch_detect, 8);
    put(b + 32, r->epoch_launch, 8);
    put(b + 40, r->epoch_event, 8);
    sput(b + 48, r->key, 24);
    sput(b + 72, r->name, 32);
    for (unsigned i = 0; i < 16; ++i)
        put(b + 104 + 4 * i, r->reg[i], 4);
    sput(b + 168, r->reason, 96);
    unsigned n = r->text_len > CR_TEXT_MAX ? CR_TEXT_MAX : r->text_len;
    put(b + 264, n, 2);
    put(b + 266, r->text_lines, 2);
    for (unsigned i = 0; i < n; ++i)
        b[272 + i] = (uint8_t)r->text[i];
    put(b + 12, sb_crc32(b + 16, CL_BLOCK - 16), 4);
}

int cl_newest(const struct cl_head *h, unsigned k)
{
    unsigned have = h->count < CL_SLOTS ? h->count : CL_SLOTS;
    if (k >= have)
        return -1;
    return (int)((h->next + CL_SLOTS - 1U - k) % CL_SLOTS);
}

/* Printable ASCII and '\n' only: other bytes (control codes, binary pmsg headers, UTF-8) become
 * '.', so the report and the screen stay text. */
static char tidy(char c)
{
    return (c == '\n' || (c >= 32 && c < 127)) ? c : c == '\t' ? ' ' : c == '\r' || !c ? 0 : '.';
}

unsigned cl_tail(char *dst, unsigned cap, const char *src, unsigned len, uint16_t *lines)
{
    unsigned from = len > cap ? len - cap : 0;
    if (from) {
        /* start at a line start */
        while (from < len && src[from - 1] != '\n')
            ++from;
    }
    unsigned n = 0, nl = 0;
    for (unsigned i = from; i < len && n < cap; ++i) {
        char c = tidy(src[i]);
        if (!c)
            continue;
        dst[n++] = c;
        nl += c == '\n';
    }
    if (n && dst[n - 1] != '\n') {
        if (n < cap)
            dst[n++] = '\n';
        else
            dst[n - 1] = '\n';
        ++nl;
    }
    if (lines)
        *lines = (uint16_t)nl;
    return n;
}

/* ---- watchdog --------------------------------------------------------------------------------- */

uint32_t sb_wdt_count(unsigned secs, unsigned *ms)
{
    if (!secs) {
        if (ms)
            *ms = 0;
        return 0;
    }
    if (secs < WDT_MIN_S)
        secs = WDT_MIN_S;
    if (secs > WDT_MAX_S)
        secs = WDT_MAX_S;
    uint64_t c = (uint64_t)secs * WDT_CLK_HZ / (256U * 128U);
    if (c > 0xffffU)
        c = 0xffffU;
    if (ms)
        *ms = (unsigned)(c * 256U * 128U * 1000U / WDT_CLK_HZ);
    return (uint32_t)c;
}

/* ---- rings ------------------------------------------------------------------------------------ */

unsigned sb_ring_text(const uint8_t *zone, unsigned zone_size, char *out, unsigned cap)
{
    if (zone_size < 16 || get(zone, 4) != RING_SIG)
        return 0;
    uint32_t start = (uint32_t)get(zone + 4, 4), size = (uint32_t)get(zone + 8, 4);
    uint32_t data = zone_size - 12;
    const uint8_t *d = zone + 12;
    if (size > data || start > data)
        return 0;
    uint32_t mod = 0;                      /* a full ring's length, 0 = not wrapped */
    if (size == data && start < data)
        mod = data;                        /* Linux persistent_ram, full */
    else if (size == RING_PMSG && start < RING_PMSG)
        mod = RING_PMSG;                   /* the loader's / woa's / XNU's 0x3F00 ring, full */
    unsigned n = 0;
    if (mod) {
        for (uint32_t i = start; i < mod && n < cap; ++i)
            out[n++] = (char)d[i];
        for (uint32_t i = 0; i < start && n < cap; ++i)
            out[n++] = (char)d[i];
    } else {
        for (uint32_t i = 0; i < size && n < cap; ++i)
            out[n++] = (char)d[i];
    }
    return n;
}

/* ---- Starbite Maintenance container --------------------------------------------------------- */

#define SBM_MAGIC 0x31544e49414d4253ULL   /* "SBMAINT1" */

int sbm_check(const uint8_t *h, uint64_t limit, struct sbm_info *m, const char **why)
{
    static const uint64_t lim[4] = { 64ULL << 20, 2ULL << 20, 80ULL << 20, 2ULL << 20 };
    z(m, sizeof(*m));
    const char *w = 0;
    if (limit > SBM_STAGE_MAX)
        limit = SBM_STAGE_MAX;
    if (get(h, 8) != SBM_MAGIC)
        w = "NO SBMAINT1 HEADER";
    else if (get(h + 8, 4) != 1 || get(h + 12, 4) != SBM_HDR)
        w = "UNKNOWN VERSION";
    else if (sb_crc32(h, 4092) != (uint32_t)get(h + 4092, 4))
        w = "HEADER CRC";
    if (w) {
        *why = w;
        return 0;
    }
    m->seq = (uint32_t)get(h + 16, 4);
    m->total = get(h + 24, 8);
    if (!m->seq) {
        *why = "SEQ 0";
        return 0;
    }
    if (m->total % SBM_HDR || m->total < SBM_HDR || m->total > limit) {
        *why = "SIZE OUTSIDE THE SLOT";
        return 0;
    }
    uint64_t end = SBM_HDR;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *s = h + 64 + 32 * i;
        uint64_t o = get(s, 8), n = get(s + 8, 8);
        uint32_t c = (uint32_t)get(s + 16, 4), kind = (uint32_t)get(s + 20, 4);
        if (kind != i + 1) {
            *why = "SECTION ORDER";
            return 0;
        }
        if (!n) {
            if (i != SBM_DTB_RESCUE) {
                *why = i == SBM_KERNEL ? "NO KERNEL" : i == SBM_DTB ? "NO DTB" : "NO INITRD";
                return 0;
            }
            if (o || c) {
                *why = "EMPTY SECTION WITH DATA";
                return 0;
            }
            continue;
        }
        if (n > lim[i]) {
            *why = "SECTION TOO LARGE";
            return 0;
        }
        if (o % SBM_HDR || o < end || o + n > m->total) {
            *why = "SECTION LAYOUT";
            return 0;
        }
        end = o + n;
        m->off[i] = o;
        m->size[i] = n;
        m->crc[i] = c;
    }
    sget(m->cmdline, h + 192, 256);
    sget(m->version, h + 448, 64);
    return 1;
}

uint64_t sbm_slot_b(uint64_t part_bytes)
{
    return part_bytes / 2U & ~0xfffffULL;
}

unsigned sbm_order(const int valid[2], const uint32_t seq[2], int prev, unsigned out[2])
{
    unsigned n = 0;
    if (valid[0] && valid[1]) {
        out[0] = seq[1] > seq[0] ? 1U : 0U;
        out[1] = 1U - out[0];
        n = 2;
    } else if (valid[0] || valid[1]) {
        out[0] = valid[0] ? 0U : 1U;
        n = 1;
    }
    if (prev && n == 2) {
        unsigned t = out[0];
        out[0] = out[1];
        out[1] = t;
    }
    return n;
}

/* ---- text ------------------------------------------------------------------------------------- */

int sb_find(const char *hay, unsigned n, const char *needle)
{
    unsigned m = slen(needle);
    if (!m || m > n)
        return -1;
    for (unsigned i = 0; i + m <= n; ++i) {
        unsigned k = 0;
        while (k < m && hay[i + k] == needle[k])
            ++k;
        if (k == m)
            return (int)i;
    }
    return -1;
}

int sb_line_after(const char *hay, unsigned n, const char *needle, char *out, unsigned cap)
{
    int at = sb_find(hay, n, needle);
    if (at < 0 || !cap)
        return 0;
    unsigned i = (unsigned)at + slen(needle), k = 0;
    for (; i < n && hay[i] != '\n' && k + 1 < cap; ++i) {
        char c = tidy(hay[i]);
        if (c)
            out[k++] = c;
    }
    out[k] = 0;
    return 1;
}

unsigned sb_line_at(const char *hay, unsigned n, unsigned at, char *out, unsigned cap)
{
    if (!cap)
        return 0;
    unsigned s = at < n ? at : n;
    while (s && hay[s - 1] != '\n')
        --s;
    unsigned k = 0;
    for (unsigned i = s; i < n && hay[i] != '\n' && k + 1 < cap; ++i) {
        char c = tidy(hay[i]);
        if (c)
            out[k++] = c;
    }
    out[k] = 0;
    return k;
}

int sb_arg(const char *args, unsigned len, const char *key, char *out, unsigned cap)
{
    unsigned kl = slen(key);
    for (unsigned i = 0; i < len && args[i]; ++i) {
        if (i && args[i - 1] != ' ')
            continue;
        unsigned m = 0;
        while (m < kl && i + m < len && args[i + m] == key[m])
            ++m;
        if (m < kl || i + kl >= len || args[i + kl] != '=')
            continue;
        unsigned k = 0;
        for (unsigned j = i + kl + 1; j < len && args[j] && args[j] != ' ' && k + 1 < cap; ++j)
            out[k++] = args[j];
        if (cap)
            out[k] = 0;
        return 1;
    }
    return 0;
}

int sb_num(const char *s, int64_t *v)
{
    int neg = 0;
    uint64_t x = 0;
    unsigned i = 0, digits = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        for (i += 2; s[i]; ++i, ++digits) {
            char c = s[i];
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                       : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16U;
            if (d > 15)
                return 0;
            x = x << 4 | d;
        }
    } else {
        for (; s[i]; ++i, ++digits) {
            if (s[i] < '0' || s[i] > '9')
                return 0;
            x = x * 10U + (uint64_t)(s[i] - '0');
        }
    }
    if (!digits)
        return 0;
    *v = neg ? -(int64_t)x : (int64_t)x;
    return 1;
}

/* Token [t, t + n) is "name" or starts with "name=". */
static int tok_is(const char *t, unsigned n, const char *name, unsigned nl)
{
    if (n < nl)
        return 0;
    for (unsigned k = 0; k < nl; ++k)
        if (t[k] != name[k])
            return 0;
    return n == nl || t[nl] == '=';
}

static void remove_tok(char *line, const char *name, unsigned nl)
{
    unsigned r = 0, w = 0;
    while (line[r]) {
        while (line[r] == ' ')
            ++r;
        unsigned s = r;
        while (line[r] && line[r] != ' ')
            ++r;
        if (r == s)
            break;
        if (tok_is(line + s, r - s, name, nl))
            continue;
        if (w)
            line[w++] = ' ';
        for (unsigned k = s; k < r; ++k)
            line[w++] = line[k];
    }
    line[w] = 0;
}

unsigned sb_line_edit(char *line, unsigned cap, const char *frag, int allow_remove)
{
    if (!cap)
        return 0;
    for (unsigned i = 0; frag && frag[i];) {
        while (frag[i] == ' ')
            ++i;
        unsigned s = i;
        while (frag[i] && frag[i] != ' ')
            ++i;
        unsigned n = i - s;
        if (!n)
            break;
        if (frag[s] == '!') {
            if (allow_remove && n > 1)
                remove_tok(line, frag + s + 1, n - 1);
            continue;
        }
        unsigned l = slen(line);
        if (l + (l ? 1U : 0U) + n + 1 > cap)
            break;
        if (l)
            line[l++] = ' ';
        for (unsigned k = 0; k < n; ++k)
            line[l++] = frag[s + k];
        line[l] = 0;
    }
    return slen(line);
}

/* ---- dates ------------------------------------------------------------------------------------ */

static void dig(char *o, unsigned v, unsigned n)
{
    for (unsigned i = n; i; --i) {
        o[i - 1] = (char)('0' + v % 10U);
        v /= 10U;
    }
}

void sb_date(uint64_t epoch, char *out)
{
    if (!epoch) {
        const char *u = "UNKNOWN TIME";
        unsigned k = 0;
        for (; u[k]; ++k)
            out[k] = u[k];
        out[k] = 0;
        return;
    }
    /* days to civil (H. Hinnant), valid for the Unix era */
    int64_t days = (int64_t)(epoch / 86400U);
    unsigned sod = (unsigned)(epoch % 86400U);
    int64_t zd = days + 719468;
    int64_t era = zd / 146097;
    unsigned doe = (unsigned)(zd - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    dig(out, (unsigned)y, 4);
    out[4] = '-';
    dig(out + 5, m, 2);
    out[7] = '-';
    dig(out + 8, d, 2);
    out[10] = ' ';
    dig(out + 11, sod / 3600U, 2);
    out[13] = ':';
    dig(out + 14, sod / 60U % 60U, 2);
    out[16] = 0;
}

uint64_t sb_dmesg_time(const char *t, unsigned n)
{
    if (n < 8 || t[0] != '=' || t[1] != '=' || t[2] != '=' || t[3] != '=')
        return 0;
    uint64_t v = 0;
    unsigned i = 4, d = 0;
    for (; i < n && t[i] >= '0' && t[i] <= '9' && d < 19; ++i, ++d)
        v = v * 10U + (uint64_t)(t[i] - '0');
    return d && i < n && t[i] == '.' ? v : 0;
}

/* ---- the last key=value, the watchdog's arguments ---------------------------------------------- */

int sb_arg_last(const char *args, unsigned len, const char *key, char *out, unsigned cap)
{
    unsigned kl = slen(key);
    int found = 0;
    for (unsigned i = 0; i < len && args[i]; ++i) {
        if (i && args[i - 1] != ' ')
            continue;
        unsigned m = 0;
        while (m < kl && i + m < len && args[i + m] == key[m])
            ++m;
        if (m < kl || i + kl >= len || args[i + kl] != '=')
            continue;
        unsigned k = 0;
        for (unsigned j = i + kl + 1; j < len && args[j] && args[j] != ' ' && k + 1 < cap; ++j)
            out[k++] = args[j];
        if (cap)
            out[k] = 0;
        found = 1;
    }
    return found;
}

static int arg_is(const char *line, unsigned len, const char *key, unsigned want)
{
    char v[24];
    int64_t x;
    return sb_arg_last(line, len, key, v, sizeof(v)) && sb_num(v, &x) && x == (int64_t)want;
}

int sb_wdt_args_ok(const char *line, unsigned len, unsigned how, unsigned wdt)
{
    if (!wdt)
        return 0;
    if (how == SB_WDT_XNU)
        return arg_is(line, len, "sbwdt", wdt);
    if (how == SB_WDT_LINUX)
        return arg_is(line, len, "s3c2410_wdt.tmr_atboot", 1) && arg_is(line, len, "s3c2410_wdt.tmr_margin", wdt);
    return 0;
}

/* ---- POST presentation ------------------------------------------------------------------------ */

void pv_init(struct pv_keys *k, unsigned view, int up_at_start, uint64_t now_ms)
{
    z(k, sizeof(*k));
    k->view = (uint8_t)(view > PV_NONE ? PV_LOGO : view);
    k->up_blocked = (uint8_t)(up_at_start != 0);
    if (up_at_start)
        k->up_since = now_ms + 1;
}

unsigned pv_poll(struct pv_keys *k, int up, int down, uint64_t now_ms)
{
    unsigned ev = 0;
    uint64_t t = now_ms + 1;
    if (up) {
        k->up_free_since = 0;
        if (!k->up_since)
            k->up_since = t;
        if (!k->up_blocked && !k->up_used && t - k->up_since >= PV_DEBOUNCE_MS) {
            k->up_used = 1;
            if (k->view != PV_LOG) {
                k->view = PV_LOG;
                ev |= PV_EV_LOG;
            }
        }
    } else {
        k->up_since = 0;
        if (!k->up_free_since)
            k->up_free_since = t;
        if (k->up_blocked && t - k->up_free_since >= PV_DEBOUNCE_MS)
            k->up_blocked = 0;
    }
    if (down) {
        if (!k->down_since)
            k->down_since = t;
        if (!k->skip && t - k->down_since >= PV_SKIP_MS) {
            k->skip = 1;
            ev |= PV_EV_SKIP;
        }
    } else {
        k->down_since = 0;
    }
    return ev;
}

unsigned pv_end(const struct pv_keys *k, unsigned fails)
{
    if (fails)
        return PV_END_FAIL;
    return k->view == PV_LOG ? PV_END_READ : PV_END_GO;
}

int pv_hold(const struct pv_keys *k, int up, int cut, uint64_t t_ms)
{
    if (cut)
        return 0;
    if (t_ms < PV_READ_MS)
        return 1;
    return up && k->up_used && !k->up_blocked && t_ms < PV_HOLD_MAX_MS;
}

unsigned pv_failures(const uint8_t *state, const uint8_t *order, unsigned n, unsigned fail_state,
                     uint8_t *out, unsigned max)
{
    unsigned c = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (state[order[i]] != fail_state)
            continue;
        if (c < max)
            out[c] = order[i];
        ++c;
    }
    return c;
}

/* ---- crash text ------------------------------------------------------------------------------- */

unsigned sb_crash_prefix(const char *l, unsigned n)
{
    if (n < 3 || l[0] != '<')
        return 0;
    unsigned i = 1;
    while (i < n && i < 5 && l[i] >= '0' && l[i] <= '9')
        ++i;
    return i > 1 && i < n && l[i] == '>' ? i + 1 : 0;
}

/* "[  123.456789] " at l: its length (with the one space after ']'), *us the time; 0 if none. */
static unsigned ln_stamp(const char *l, unsigned n, uint64_t *us)
{
    unsigned i = 1;
    if (!n || l[0] != '[')
        return 0;
    while (i < n && l[i] == ' ')
        ++i;
    uint64_t s = 0, f = 0;
    unsigned d = 0, fd = 0;
    for (; i < n && l[i] >= '0' && l[i] <= '9' && d < 12; ++i, ++d)
        s = s * 10U + (uint64_t)(l[i] - '0');
    if (!d || i >= n || l[i] != '.')
        return 0;
    for (++i; i < n && l[i] >= '0' && l[i] <= '9' && fd < 9; ++i, ++fd)
        f = f * 10U + (uint64_t)(l[i] - '0');
    if (!fd || i >= n || l[i] != ']')
        return 0;
    for (; fd < 6; ++fd)
        f *= 10U;
    for (; fd > 6; --fd)
        f /= 10U;
    *us = s * 1000000U + f;
    ++i;
    if (i < n && l[i] == ' ')
        ++i;
    return i;
}

unsigned sb_crash_stamp(const char *l, unsigned n)
{
    uint64_t us;
    return ln_stamp(l, n, &us);
}

static int starts(const char *c, unsigned n, const char *p)
{
    for (unsigned k = 0; p[k]; ++k)
        if (k >= n || c[k] != p[k])
            return 0;
    return 1;
}

static int find_in(const char *c, unsigned n, const char *p)
{
    unsigned m = slen(p);
    for (unsigned i = 0; i + m <= n; ++i)
        if (starts(c + i, n - i, p))
            return (int)i;
    return -1;
}

/* "x12:" / "x9 :" / "x0:" (Linux pads one-digit names with a space) */
static int xreg(const char *c, unsigned n)
{
    if (n < 3 || c[0] != 'x' || c[1] < '0' || c[1] > '9')
        return 0;
    unsigned i = 2;
    if (i < n && c[i] >= '0' && c[i] <= '9')
        ++i;
    while (i < n && c[i] == ' ')
        ++i;
    return i < n && c[i] == ':';
}

/* Printable characters of c[0..n), at most cap - 1. */
static void fcopy(char *d, unsigned cap, const char *c, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n && k + 1 < cap; ++i)
        if (c[i] >= 32 && c[i] < 127)
            d[k++] = c[i];
    d[k] = 0;
}

/* The token after key in c (to a space, ',' or ')'), 0 if key is absent. */
static unsigned tok_after(const char *c, unsigned n, const char *key, char *out, unsigned cap)
{
    int at = find_in(c, n, key);
    unsigned k = 0;
    if (at >= 0)
        for (unsigned i = (unsigned)at + slen(key); i < n && c[i] != ' ' && c[i] != ',' && c[i] != ')' &&
                                                    c[i] != ':' && k + 1 < cap; ++i)
            out[k++] = c[i];
    out[k] = 0;
    return k;
}

struct sbuf { char *b; unsigned n, cap; };

static void sb_put(struct sbuf *s, const char *p)
{
    for (; p && *p && s->n + 1 < s->cap; ++p)
        s->b[s->n++] = *p;
    s->b[s->n] = 0;
}

/* "CPU 4 PID 1234 sh" */
static void task_line(struct sb_crashinfo *ci, const char *cpu, const char *pid, const char *comm)
{
    struct sbuf s = { ci->task, 0, sizeof(ci->task) };
    ci->task[0] = 0;
    if (cpu[0]) {
        sb_put(&s, "CPU ");
        sb_put(&s, cpu);
    }
    if (pid[0]) {
        sb_put(&s, s.n ? " PID " : "PID ");
        sb_put(&s, pid);
    }
    if (comm[0]) {
        sb_put(&s, s.n ? " " : "");
        sb_put(&s, comm);
    }
}

struct cp_state { int trace, xtrace, abort; unsigned best; };

/* The kind of one line. c: its content after "<N>" and "[ time] "; stamped: it had a time (us). */
static unsigned crash_kind(struct cp_state *st, struct sb_crashinfo *ci, const char *c, unsigned n,
                           uint64_t us, int stamped)
{
    unsigned w = 0;
    while (w < n && (c[w] == ' ' || c[w] == '\t'))
        ++w;
    const char *t = c + w;
    unsigned tn = n - w;
    /* blocks that began on an earlier line */
    if (st->trace) {
        if (w && tn)
            return CT_TRACE;            /* " show_stack+0x18/0x24" */
        st->trace = 0;
    }
    if (st->xtrace) {
        if (starts(t, tn, "lr:"))
            return CT_TRACE;            /* XNU "lr: 0x...  fp: 0x..." */
        st->xtrace = 0;
    }
    if (st->abort) {
        if (w >= 2 && tn)
            return CT_MSG;              /* "  ESR = 0x...", "  EC = 0x25: DABT ..." */
        st->abort = 0;
    }
    unsigned prio = 0;
    if (starts(t, tn, "Kernel panic - not syncing") || starts(t, tn, "panic("))
        prio = 4;
    else if (find_in(t, tn, "Internal error:") >= 0 || starts(t, tn, "kernel BUG at") || starts(t, tn, "BUG: "))
        prio = 3;
    else if (starts(t, tn, "Unable to handle kernel"))
        prio = 2;
    if (prio) {
        if (prio > st->best) {
            st->best = prio;
            fcopy(ci->message, sizeof(ci->message), t, tn);
            if (stamped)
                ci->uptime_ms = us / 1000U;
        }
        return CT_MSG;
    }
    if (starts(t, tn, "Mem abort info:") || starts(t, tn, "Data abort info:")) {
        st->abort = 1;
        return CT_MSG;
    }
    if (starts(t, tn, "Call trace:")) {
        st->trace = 1;
        return CT_TRACE;
    }
    if (starts(t, tn, "Panicked thread:") && find_in(t, tn, "backtrace") >= 0) {
        st->xtrace = 1;
        return CT_TRACE;
    }
    if (starts(t, tn, "CPU: ") && find_in(t, tn, "PID: ") >= 0) {
        char cpu[8], pid[12], comm[24];
        (void)tok_after(t, tn, "CPU: ", cpu, sizeof(cpu));
        (void)tok_after(t, tn, "PID: ", pid, sizeof(pid));
        (void)tok_after(t, tn, "Comm: ", comm, sizeof(comm));
        if (!ci->task[0])
            task_line(ci, cpu, pid, comm);
        /* the release: the token before the last " #" ("Not tainted 7.1.0-starbite-1 #1") */
        int hash = -1;
        for (unsigned i = 0; i + 1 < tn; ++i)
            if (t[i] == ' ' && t[i + 1] == '#')
                hash = (int)i;
        if (hash > 0 && !ci->version[0]) {
            unsigned e = (unsigned)hash, s = e;
            while (s && t[s - 1] != ' ')
                --s;
            fcopy(ci->version, sizeof(ci->version), t + s, e - s);
        }
        return CT_INFO;
    }
    if (starts(t, tn, "Linux version ")) {
        unsigned e = 14;
        while (e < tn && t[e] != ' ')
            ++e;
        fcopy(ci->version, sizeof(ci->version), t + 14, e - 14);
        return CT_INFO;
    }
    if (starts(t, tn, "Kernel version:")) {
        int r = find_in(t, tn, "root:");
        unsigned s = r >= 0 ? (unsigned)r + 5 : 15, e;
        while (r < 0 && s < tn && t[s] == ' ')
            ++s;
        for (e = s; e < tn && (r < 0 || (t[e] != ' ' && t[e] != ';')); ++e)
            ;
        fcopy(ci->version, sizeof(ci->version), t + s, e - s);
        return CT_INFO;
    }
    if (starts(t, tn, "Panicked task")) {
        char cpu[8], pid[12], comm[24];
        (void)tok_after(ci->message, slen(ci->message), "panic(cpu ", cpu, sizeof(cpu));
        (void)tok_after(t, tn, "pid ", pid, sizeof(pid));
        comm[0] = 0;
        int p = find_in(t, tn, "pid ");
        if (p >= 0) {
            unsigned i = (unsigned)p + 4;
            while (i < tn && t[i] != ':')
                ++i;
            if (i + 1 < tn) {
                unsigned s = i + 1;
                while (s < tn && t[s] == ' ')
                    ++s;
                unsigned e = s;
                while (e < tn && t[e] != ' ' && t[e] != ',')
                    ++e;
                fcopy(comm, sizeof(comm), t + s, e - s);
            }
        }
        if (!ci->task[0])
            task_line(ci, cpu, pid, comm);
        return CT_INFO;
    }
    if (starts(t, tn, "STARBITE: xnu crash v1:")) {
        char v[16];
        int64_t x;
        if (tok_after(t, tn, "uptime ", v, sizeof(v)) && sb_num(v, &x) && x >= 0 && !ci->uptime_ms)
            ci->uptime_ms = (uint64_t)x * 1000U;
        return CT_INFO;
    }
    if (starts(t, tn, "STARBITE: XNU panic") || starts(t, tn, "STARBITE: sweetreboot timer") ||
        starts(t, tn, "Hardware name:") || starts(t, tn, "Modules linked in:") ||
        starts(t, tn, "Debugger message:") || starts(t, tn, "OS version:") || starts(t, tn, "OS release type:") ||
        starts(t, tn, "Kernel Offset:"))
        return CT_INFO;
    if (xreg(t, tn) || starts(t, tn, "pc : ") || starts(t, tn, "lr : ") || starts(t, tn, "sp : ") ||
        starts(t, tn, "pstate: ") || starts(t, tn, "Code: ") || starts(t, tn, "pc:") || starts(t, tn, "cpsr:") ||
        (starts(t, tn, "CORE ") && find_in(t, tn, "PC=") >= 0))
        return CT_REGS;
    return CT_LOG;
}

/* The lines of a text, one at a time, with their kinds. */
struct cp_walk { const char *t; unsigned n, at; struct cp_state st; };

static int cp_next(struct cp_walk *w, struct sb_crashinfo *ci, unsigned *ls, unsigned *ll, unsigned *kind)
{
    if (w->at >= w->n)
        return 0;
    unsigned s = w->at, e = s;
    while (e < w->n && w->t[e] != '\n')
        ++e;
    w->at = e < w->n ? e + 1 : e;
    unsigned len = e - s;
    while (len && (w->t[s + len - 1] == '\r' || w->t[s + len - 1] == 0))
        --len;
    unsigned p = sb_crash_prefix(w->t + s, len);
    uint64_t us = 0;
    unsigned sl = ln_stamp(w->t + s + p, len - p, &us);
    *kind = crash_kind(&w->st, ci, w->t + s + p + sl, len - p - sl, us, sl != 0);
    *ls = s;
    *ll = len;
    return 1;
}

unsigned sb_crash_parse(const char *text, unsigned n, struct sb_crashinfo *ci, uint8_t *kinds, unsigned cap)
{
    z(ci, sizeof(*ci));
    struct cp_walk w;
    z(&w, sizeof(w));
    w.t = text;
    w.n = n;
    unsigned s, l, k, i = 0;
    while (cp_next(&w, ci, &s, &l, &k)) {
        if (kinds && i < cap)
            kinds[i] = (uint8_t)k;
        ci->msg += k == CT_MSG;
        ci->info += k == CT_INFO;
        ci->regs += k == CT_REGS;
        ci->trace += k == CT_TRACE;
        ++i;
    }
    ci->lines = i;
    return i;
}

/* The excerpt's size when the tail starts at line `from` (earlier lines only if not CT_LOG). */
static unsigned ex_cost(const char *src, unsigned len, unsigned from)
{
    struct sb_crashinfo ci;
    struct cp_walk w;
    z(&ci, sizeof(ci));
    z(&w, sizeof(w));
    w.t = src;
    w.n = len;
    unsigned s, l, k, i = 0, cost = 0;
    int gap = 0;
    while (cp_next(&w, &ci, &s, &l, &k)) {
        if (i >= from || k != CT_LOG) {
            cost += (gap ? 6U : 0U) + l - sb_crash_prefix(src + s, l) + 1U;
            gap = 0;
        } else {
            gap = 1;
        }
        ++i;
    }
    return cost;
}

unsigned cl_excerpt(char *dst, unsigned cap, const char *src, unsigned len, uint16_t *lines)
{
    struct sb_crashinfo ci;
    unsigned total = sb_crash_parse(src, len, &ci, 0, 0);
    /* the tail's first line: the earliest that still fits (the size only falls as it moves on) */
    unsigned lo = 0, hi = total;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (ex_cost(src, len, mid) <= cap)
            hi = mid;
        else
            lo = mid + 1;
    }
    struct cp_walk w;
    z(&w, sizeof(w));
    w.t = src;
    w.n = len;
    unsigned s, l, k, i = 0, n = 0, nl = 0;
    int gap = 0;
    while (cp_next(&w, &ci, &s, &l, &k)) {
        if (i >= lo || k != CT_LOG) {
            unsigned p = sb_crash_prefix(src + s, l);
            if (n + (gap ? 6U : 0U) + l - p + 1U > cap)
                break;
            if (gap) {
                const char *m = "[...]\n";
                for (unsigned j = 0; j < 6; ++j)
                    dst[n++] = m[j];
                ++nl;
            }
            gap = 0;
            for (unsigned j = p; j < l; ++j) {
                char c = tidy(src[s + j]);
                dst[n++] = c ? c : '.';
            }
            dst[n++] = '\n';
            ++nl;
        } else {
            gap = 1;
        }
        ++i;
    }
    if (lines)
        *lines = (uint16_t)nl;
    return n;
}
