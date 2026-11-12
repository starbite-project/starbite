/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's side of the firmware layer (sbfw_api.h; docs/STARBITE-FIRMWARE-LAYER.md 10.3, 10.4): the loader's
 * settings as one struct, one-shot requests, the boot menu / Starbite Recovery once, the crash reports.
 * The crash log's decoding and the request names / fits are the loader's (shim/starlte/sbfmt.c: cl_head_decode,
 * cl_report_decode, cl_newest, sb_kind_name, sbfw_req_name, sbfw_req_fits), byte for byte, only the fields used here.
 * No hardware apart from the store (store.c) and the mailbox (sys.c): the host test builds it as it is.
 */
#include "sb.h"
#include "store.h"
#include "catalog.h"
#include "sbfw_api.h"

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

/* NUL-terminated copy of at most n - 1 characters from a fixed field of n bytes (sbfmt.c sget) */
static void sget(char *dst, const uint8_t *src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && src[k]; ++k)
        dst[k] = (char)src[k];
    for (; k < n; ++k)
        dst[k] = 0;
}

static void scopy(char *dst, unsigned cap, const char *src)
{
    unsigned k = 0;
    for (; src && src[k] && k + 1 < cap; ++k)
        dst[k] = src[k];
    for (; k < cap; ++k)
        dst[k] = 0;
}

/* ---- settings ------------------------------------------------------------------------------------------------ */

int sbfw_settings_read(struct sbfw_settings *s)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof(*s); ++i)
        z[i] = 0;
    struct star_settings st;
    struct sbfw_rec r;
    uint8_t l[8];
    int ok = settings_load(&st);
    settings_fw_load(&r);
    settings_ldr_load(l);
    scopy(s->default_key, sizeof(s->default_key), st.default_key);
    s->timeout_s = st.timeout_s;
    s->debug_level = (uint8_t)((st.flags & SET_LOG_MASK) >> SET_LOG_SHIFT);
    unsigned post = (st.flags & SET_POST_MASK) >> SET_POST_SHIFT;
    s->post = (uint8_t)(post <= SBFW_POST_OFF ? post : SBFW_POST_FULL);   /* 3 reads as full (sbset_post) */
    s->crash_mode = l[0];
    s->crash_detail = l[1];
    s->crash_action = l[2];
    s->wdt_mode = l[4];
    s->wdt_s = (uint16_t)get(l + 6, 2);
    s->shell = r.fw_flags & SBFW_FLAG_SHELL;
    scopy(s->shell_key, sizeof(s->shell_key), r.shell_key);
    return ok;
}

int sbfw_settings_write(const struct sbfw_settings *s)
{
    if (s->debug_level > SBFW_DEBUG_HIGHEST || s->post > SBFW_POST_OFF || s->wdt_mode > SBFW_WDT_FIXED ||
        s->crash_mode > SBFW_CRASH_OFF || s->crash_action > SBFW_AFTER_RETRY || s->crash_detail > 3 || s->shell > 1 ||
        (s->wdt_mode == SBFW_WDT_FIXED && (s->wdt_s < SBFW_WDT_MIN_S || s->wdt_s > SBFW_WDT_MAX_S)) ||
        s->timeout_s > 3600)
        return SBFW_E_INVAL;
    if (store_home() < 0)
        return SBFW_E_NOHOME;
    struct star_settings st;
    struct sbfw_rec r;
    uint8_t l[8];
    settings_load(&st);                        /* no record yet: zeros, the loader's defaults */
    settings_fw_load(&r);                      /* the request fields and the undo record as they are */
    settings_ldr_load(l);
    scopy(st.default_key, sizeof(st.default_key), s->default_key);
    st.timeout_s = s->timeout_s;
    st.flags = (st.flags & ~(SET_LOG_MASK | SET_POST_MASK)) | (uint32_t)s->debug_level << SET_LOG_SHIFT |
               (uint32_t)s->post << SET_POST_SHIFT;
    l[0] = s->crash_mode;
    l[1] = s->crash_detail;
    l[2] = s->crash_action;
    l[4] = s->wdt_mode;
    put(l + 6, s->wdt_mode == SBFW_WDT_FIXED ? s->wdt_s : 0, 2);
    r.fw_flags = (uint8_t)((r.fw_flags & ~SBFW_FLAG_SHELL) | (s->shell ? SBFW_FLAG_SHELL : 0));
    scopy(r.shell_key, sizeof(r.shell_key), s->shell_key);
    int e = settings_all_save(&st, &r, l);
    return e == -1 ? SBFW_E_NOHOME : e;
}

/* ---- the mailbox keys ------------------------------------------------------------------------------------------ */

void sb_boot_menu_next(void)
{
    sb_boot_next("menu");
}

void sb_boot_recovery_next(void)
{
    sb_boot_next("recovery");
}

/* ---- one-shot requests (sbfmt.c's names and fits) ------------------------------------------------------------- */

const char *sbfw_req_name(unsigned q)
{
    switch (q) {
    case SBFW_REQ_VERBOSE:    return "verbose";
    case SBFW_REQ_SAFE:       return "safe";
    case SBFW_REQ_SINGLE:     return "single";
    case SBFW_REQ_PREV:       return "prev";
    case SBFW_REQ_XNU_SINGLE: return "xnu-single";
    case SBFW_REQ_WINRE:      return "winre";
    case SBFW_REQ_DRIVER_OFF: return "driver-off";
    case SBFW_REQ_NO_SHELL:   return "no-shell";
    case SBFW_REQ_UPDATE:     return "update";
    default:                  return 0;
    }
}

/* catalog.h: M_XNU 1, M_LINUX 2, M_ANDROID 3, M_UEFI 4, M_BARE 5 */
int sbfw_req_fits(unsigned q, unsigned m)
{
    switch (q) {
    case SBFW_REQ_VERBOSE:    return m >= 1 && m <= 5;
    case SBFW_REQ_SAFE:       return m == 5;
    case SBFW_REQ_SINGLE:     return m == 2;
    case SBFW_REQ_PREV:       return m == 2;
    case SBFW_REQ_XNU_SINGLE: return m == 1;
    case SBFW_REQ_WINRE:      return 0;           /* plan only (compiled out): docs 6.2 */
    case SBFW_REQ_DRIVER_OFF: return m == 4;
    case SBFW_REQ_NO_SHELL:   return 0;           /* not a start's request: the shell decision reads it */
    case SBFW_REQ_UPDATE:     return m == 5;      /* bare rows with an "@slot" file */
    default:                  return 0;
    }
}

int sbfw_request_arm(const char *key, unsigned action)
{
    if (!sbfw_req_name(action) || action == SBFW_REQ_WINRE || action == SBFW_REQ_NO_SHELL ||
        action == SBFW_REQ_UPDATE)          /* update: sbfw_update_arm (it needs the slot) */
        return SBFW_E_INVAL;
    int i = key && key[0] ? cat_find(key) : -1;
    const struct cat_ent *c = i >= 0 ? cat_get((unsigned)i) : 0;
    if (!c || !sbfw_req_fits(action, c->method))
        return SBFW_E_NOFIT;
    if (store_home() < 0)
        return SBFW_E_NOHOME;
    struct star_settings st;
    struct sbfw_rec r;
    settings_load(&st);
    settings_fw_load(&r);
    /* a write action applied and not undone yet: its undo record is the loader's to run first */
    if (r.req_state == SBFW_ST_APPLIED && r.undo[0])
        return SBFW_E_BUSY;
    r.req_action = (uint8_t)action;
    r.req_state = SBFW_ST_ARMED;
    r.req_seq = (uint16_t)(r.req_seq + 1U);
    scopy(r.req_key, sizeof(r.req_key), c->key);
    int e = settings_fw_save(&st, &r);
    if (e)
        return e == -1 ? SBFW_E_NOHOME : e;
    sb_logf("sbfw: request armed: ", sbfw_req_name(action));
    return 0;
}

int sbfw_request(const char *key, unsigned action)
{
    int e = sbfw_request_arm(key, action);
    if (e)
        return e;
    sb_boot_next(key);
}

int sbfw_request_status(unsigned *action, unsigned *state, char key[24])
{
    struct sbfw_rec r;
    settings_fw_load(&r);
    if (action)
        *action = r.req_action;
    if (state)
        *state = r.req_state;
    if (key)
        scopy(key, 24, r.req_key);
    return r.req_seq;
}

/* ---- crash reports (sbfmt.c's crash log) ----------------------------------------------------------------------- */

#define CL_BLOCK    4096U
#define CL_SLOTS    15U
#define CL_HEAD_CRC 1024U
#define CL_MAGIC 0x3148534152434253ULL   /* "SBCRASH1" */
#define CR_MAGIC 0x3150455252434253ULL   /* "SBCRREP1" */

const char *sbfw_kind_name(unsigned kind)
{
    switch (kind) {
    case 0:  return "NO OS WAS RUNNING";
    case 1:  return "CLEAN RESTART";
    case 2:  return "POWER ON";
    case 3:  return "KERNEL PANIC";
    case 4:  return "HANG (WATCHDOG)";
    case 5:  return "HANG (OS TIMER)";
    case 6:  return "FORCED RESTART";
    case 7:  return "FAULT (S-BOOT)";
    case 9:  return "WATCHDOG, NO OS";
    case 10: return "KERNEL OOPS";
    default: return "UNEXPLAINED RESET";
    }
}

static uint8_t cl_blk[CL_BLOCK];

/* the head's ring position: 1 valid (*next, *count), else SBFW_E_NOLOG / -1 */
static int cl_head(struct store_ent *e, uint32_t *next, uint32_t *count)
{
    if (!store_find("home:crashlog", e) || e->size < CL_BLOCK * (1U + CL_SLOTS))
        return SBFW_E_NOLOG;
    if (store_read_block(e, 0, cl_blk))
        return -1;
    const uint8_t *b = cl_blk;
    if (get(b, 8) != CL_MAGIC || get(b + 8, 4) != 1 || sb_crc32(b + 16, CL_HEAD_CRC - 16) != (uint32_t)get(b + 12, 4))
        return SBFW_E_NOLOG;
    uint32_t slots = (uint32_t)get(b + 16, 4), size = (uint32_t)get(b + 20, 4);
    *next = (uint32_t)get(b + 24, 4);
    *count = (uint32_t)get(b + 28, 4);
    if (slots != CL_SLOTS || size != CL_BLOCK || *next >= CL_SLOTS)
        return SBFW_E_NOLOG;
    return 1;
}

int sbfw_crash_count(void)
{
    struct store_ent e;
    uint32_t next, count;
    int r = cl_head(&e, &next, &count);
    if (r != 1)
        return r == -1 ? SBFW_E_NOLOG : r;
    return (int)(count < CL_SLOTS ? count : CL_SLOTS);
}

int sbfw_crash_get(unsigned i, struct sbfw_crash *c)
{
    uint8_t *z = (uint8_t *)c;
    for (unsigned k = 0; k < sizeof(*c); ++k)
        z[k] = 0;
    struct store_ent e;
    uint32_t next, count;
    int r = cl_head(&e, &next, &count);
    if (r != 1)
        return r;
    unsigned have = count < CL_SLOTS ? count : CL_SLOTS;
    if (i >= have)
        return SBFW_E_INVAL;
    unsigned slot = (next + CL_SLOTS - 1U - i) % CL_SLOTS;   /* cl_newest */
    if (store_read_block(&e, 1U + slot, cl_blk))
        return -1;
    const uint8_t *b = cl_blk;
    if (get(b, 8) != CR_MAGIC || get(b + 8, 4) != 1 || sb_crc32(b + 16, CL_BLOCK - 16) != (uint32_t)get(b + 12, 4))
        return -6;
    c->seq = (uint32_t)get(b + 16, 4);
    c->kind_id = b[20];
    c->severity = b[21];
    c->method = b[22];
    uint64_t detect = get(b + 24, 8), launch = get(b + 32, 8), event = get(b + 40, 8);
    c->epoch = event ? event : detect ? detect : launch;
    sget(c->key, b + 48, 24);
    sget(c->os, b + 72, 32);
    scopy(c->kind, sizeof(c->kind), sbfw_kind_name(c->kind_id));
    sget(c->line, b + 168, 96);
    if (!c->line[0]) {                         /* no reason: the text's first line that is not "[...]" */
        unsigned n = (unsigned)get(b + 264, 2), at = 0;
        if (n > CL_BLOCK - 272)
            n = CL_BLOCK - 272;
        const uint8_t *t = b + 272;
        while (at < n) {
            unsigned e2 = at;
            while (e2 < n && t[e2] != '\n')
                ++e2;
            if (e2 > at && !(t[at] == '[' && e2 - at == 5 && t[at + 1] == '.')) {
                unsigned k = 0;
                for (; at + k < e2 && k + 1 < sizeof(c->line); ++k)
                    c->line[k] = (char)t[at + k];
                c->line[k] = 0;
                break;
            }
            at = e2 + 1;
        }
    }
    return 0;
}

/* ---- update slots (docs/STARBITE-FIRMWARE-LAYER.md 10.5; the loader's shim/starlte/sbfmt.c, byte for byte) ---- */

int sbfw_slot_get(unsigned *active, unsigned *try_, unsigned *tries)
{
    struct sbfw_rec r;
    int ok = settings_fw_load(&r);
    *active = r.slot_active;
    *try_ = r.slot_try;
    *tries = r.slot_tries;
    return ok;
}

int sbfw_slot_set(unsigned active, unsigned try_, unsigned tries)
{
    if (active > 2 || try_ > 2 || tries > 255)
        return SBFW_E_INVAL;
    if (store_home() < 0)
        return SBFW_E_NOHOME;
    struct star_settings st;
    struct sbfw_rec r;
    settings_load(&st);
    settings_fw_load(&r);
    r.slot_active = (uint8_t)active;
    r.slot_try = (uint8_t)try_;
    r.slot_tries = (uint8_t)tries;
    int e = settings_fw_save(&st, &r);
    return e ? (e == -1 ? SBFW_E_NOHOME : e) : 0;
}

int sbfw_update_arm(const char *key, unsigned slot)
{
    if (slot != 1 && slot != 2)
        return SBFW_E_INVAL;
    int i = key && key[0] ? cat_find(key) : -1;
    const struct cat_ent *c = i >= 0 ? cat_get((unsigned)i) : 0;
    if (!c || !sbfw_req_fits(SBFW_REQ_UPDATE, c->method))
        return SBFW_E_NOFIT;
    if (store_home() < 0)
        return SBFW_E_NOHOME;
    struct star_settings st;
    struct sbfw_rec r;
    settings_load(&st);
    settings_fw_load(&r);
    if (r.req_state == SBFW_ST_APPLIED && r.undo[0])
        return SBFW_E_BUSY;
    if (slot == r.slot_active)
        return SBFW_E_INVAL;                 /* stage into the inactive slot */
    r.slot_try = (uint8_t)slot;
    r.slot_tries = (uint8_t)(r.slot_tries + 1U);
    r.req_action = SBFW_REQ_UPDATE;
    r.req_state = SBFW_ST_ARMED;
    r.req_seq = (uint16_t)(r.req_seq + 1U);
    scopy(r.req_key, sizeof(r.req_key), c->key);
    int e = settings_fw_save(&st, &r);
    if (e)
        return e == -1 ? SBFW_E_NOHOME : e;
    sb_logf("sbfw: update armed for the next start of ", c->key);
    return 0;
}

int sbfw_update_commit(void)
{
    if (store_home() < 0)
        return SBFW_E_NOHOME;
    struct star_settings st;
    struct sbfw_rec r;
    settings_load(&st);
    settings_fw_load(&r);
    if (r.req_action != SBFW_REQ_UPDATE || !r.slot_try || r.req_state != SBFW_ST_APPLIED)
        return SBFW_E_INVAL;                 /* not this start's trial: nothing to commit */
    r.slot_active = r.slot_try;
    r.slot_try = 0;
    r.req_state = SBFW_ST_DONE;
    int e = settings_fw_save(&st, &r);
    if (e)
        return e == -1 ? SBFW_E_NOHOME : e;
    sb_log("sbfw: update committed: the new version is the active one");
    return 0;
}

static void sput_(uint8_t *dst, const char *src, unsigned n)
{
    unsigned k = 0;
    for (; src && src[k] && k + 1 < n; ++k)
        dst[k] = (uint8_t)src[k];
    for (; k < n; ++k)
        dst[k] = 0;
}

int sbslot_check(const uint8_t *h, uint64_t entry_bytes, const char *key, struct sbslot_info *si)
{
    uint8_t *q = (uint8_t *)si;
    for (unsigned i = 0; i < sizeof(*si); ++i)
        q[i] = 0;
    static const char m[8] = { 'S', 'B', 'S', 'L', 'O', 'T', '0', '1' };
    for (unsigned i = 0; i < 8; ++i)
        if (h[i] != (uint8_t)m[i])
            return 0;
    if (get(h + 8, 4) != 1 || sb_crc32(h + 16, 4096 - 16) != (uint32_t)get(h + 12, 4))
        return 0;
    si->version = 1;
    si->size = get(h + 16, 8);
    si->crc = (uint32_t)get(h + 24, 4);
    sget(si->vstr, h + 32, 32);
    sget(si->key, h + 64, 24);
    if (!si->size || entry_bytes < 4096 || si->size > entry_bytes - 4096)
        return 0;
    unsigned k = 0;
    while (k < 23 && key[k] && key[k] == si->key[k])
        ++k;
    return (k == 23 || key[k] == si->key[k]) && si->key[0];
}

void sbslot_build(uint8_t *h, const struct sbslot_info *si)
{
    for (unsigned i = 0; i < 4096; ++i)
        h[i] = 0;
    static const char m[8] = { 'S', 'B', 'S', 'L', 'O', 'T', '0', '1' };
    for (unsigned i = 0; i < 8; ++i)
        h[i] = (uint8_t)m[i];
    put(h + 8, 1, 4);
    put(h + 16, si->size, 8);
    put(h + 24, si->crc, 4);
    sput_(h + 32, si->vstr, 32);
    sput_(h + 64, si->key, 24);
    put(h + 12, sb_crc32(h + 16, 4096 - 16), 4);
}
