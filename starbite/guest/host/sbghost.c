/* SPDX-License-Identifier: GPL-2.0-only
 * sbghost: the host (Starbite OS) side of the guest Linux on the M3 cluster. See sbghost.h and
 * docs/STARBITE-OS-LINUX-M3.md. Freestanding: no libc, word accesses only to the shared region.
 */
#include "sbghost.h"

static const struct sbg_os *os;
static struct sbg_cfg cfg;
static struct sbg_stats st;
static uint8_t host_if;
static uint64_t t_start, t_stop_req, img_pa;
static uint32_t log_pos;
static int log_seen;
static char line[160];
static unsigned line_n;
/* contract v2: what the guest said last */
static unsigned v2_changes;
static uint32_t v2_window = SBSHM_NONE, v2_purpose;
static int v2_has_window, v2_text;
/* sbblk: the host's own ring indices (never read back from the region: the guest can write there) */
static uint32_t blk_tail, blk_head;
static int blk_ring_bad;

#define SHM(off) ((uintptr_t)(cfg.shm_base + (off)))
#define HDR ((volatile struct sbshm_hdr *)SHM(SBSHM_HDR_OFF))
#define H2G ((struct sbshm_ring *)SHM(SBSHM_H2G_OFF))
#define G2H ((struct sbshm_ring *)SHM(SBSHM_G2H_OFF))

static uint64_t tramp_pa(void) { return cfg.tramp_pa ? cfg.tramp_pa : cfg.shm_base + SBSHM_TRAMP_OFF; }
static uint64_t crumb(void) { return *(volatile uint64_t *)(uintptr_t)(tramp_pa() + 4 * SBG_TRAMP_CRUMB); }

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---- small text helpers (no libc) ------------------------------------------------------------------ */
struct tb {
    char b[160];
    unsigned n;
};
static void t_s(struct tb *t, const char *s)
{
    while (*s && t->n < sizeof(t->b) - 1)
        t->b[t->n++] = *s++;
    t->b[t->n] = 0;
}
static void t_u(struct tb *t, uint64_t v)
{
    char d[24];
    int i = 0;
    do {
        d[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i && t->n < sizeof(t->b) - 1)
        t->b[t->n++] = d[--i];
    t->b[t->n] = 0;
}
static void t_x(struct tb *t, uint64_t v)
{
    t_s(t, "0x");
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) {
        unsigned d = (unsigned)(v >> s) & 15U;
        if ((d || started || !s) && t->n < sizeof(t->b) - 1) {
            t->b[t->n++] = "0123456789abcdef"[d];
            started = 1;
        }
    }
    t->b[t->n] = 0;
}
static void say(struct tb *t) { if (os->log) os->log(t->b); }
static void fail(const char *why)
{
    st.state = SBG_FAILED;
    st.why = why;
    struct tb t = {.n = 0};
    t_s(&t, "guest: FAILED: ");
    t_s(&t, why);
    say(&t);
}

/* ---- GIC: the doorbells and the host's own lines --------------------------------------------------- */
static void cfg_edge(unsigned id)
{
    uintptr_t a = (uintptr_t)cfg.gicd + 0xc00 + 4 * (id / 16);
    wr32(a, rd32(a) | (2U << (2 * (id % 16))));     /* only while the guest is off (docs 4: ICFGR is a shared word) */
}
static void line_own(unsigned id)
{
    *(volatile uint8_t *)(uintptr_t)(cfg.gicd + 0x800 + id) = host_if;      /* ITARGETSR: a byte write */
    wr32((uintptr_t)cfg.gicd + 0x100 + 4 * (id / 32), 1U << (id % 32));    /* ISENABLER: write-1-to-set */
}
static int line_ok(unsigned id)
{
    uint8_t tg = *(volatile uint8_t *)(uintptr_t)(cfg.gicd + 0x800 + id);
    int en = (int)((rd32((uintptr_t)cfg.gicd + 0x100 + 4 * (id / 32)) >> (id % 32)) & 1U);
    return en && (tg & host_if);
}

void sbg_gic_prepare(void)
{
    if (cfg.db_host) {
        cfg_edge(cfg.db_host);
        line_own(cfg.db_host);
    }
    if (cfg.db_guest)
        cfg_edge(cfg.db_guest);                     /* the guest routes and enables it (request_irq) */
}

static void lines_check(void)
{
    if (!(rd32((uintptr_t)cfg.gicd) & 1U)) {
        struct tb t = {.n = 0};
        t_s(&t, "guest: GICD_CTLR found 0 (the guest's GIC init?)");
        say(&t);
        if (cfg.repair)
            wr32((uintptr_t)cfg.gicd, 1);
    }
    uint32_t ids[9];
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && cfg.host_lines[i]; ++i)
        ids[n++] = cfg.host_lines[i];
    if (cfg.db_host)
        ids[n++] = cfg.db_host;
    for (unsigned i = 0; i < n; ++i) {
        if (line_ok(ids[i]))
            continue;
        st.lines_lost++;
        if (st.lines_lost <= 4) {
            struct tb t = {.n = 0};
            t_s(&t, "guest: host line ");
            t_u(&t, ids[i]);
            t_s(&t, " taken (target ");
            t_x(&t, *(volatile uint8_t *)(uintptr_t)(cfg.gicd + 0x800 + ids[i]));
            t_s(&t, cfg.repair ? "), re-asserted" : "), not repaired");
            say(&t);
        }
        if (cfg.repair) {
            line_own(ids[i]);
            st.lines_repaired++;
        }
    }
}

/* ---- the shared region ----------------------------------------------------------------------------- */
static void copy_words(uintptr_t d, const void *s, uint64_t n)
{
    const volatile uint64_t *q = (const volatile uint64_t *)s;
    volatile uint64_t *p = (volatile uint64_t *)d;
    for (uint64_t i = 0; i < (n + 7) / 8; ++i)
        p[i] = q[i];
}
static void zero_words(uintptr_t d, uint64_t n)
{
    volatile uint64_t *p = (volatile uint64_t *)d;
    for (uint64_t i = 0; i < n / 8; ++i)
        p[i] = 0;
}

void sbg_init(const struct sbg_os *o, const struct sbg_cfg *c, uint8_t cpuif)
{
    os = o;
    cfg = *c;
    host_if = cpuif ? cpuif : 1;
    st = (struct sbg_stats){0};
}

void sbg_set_blk(const struct sbshm_blkdev *blk)
{
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i) {
        cfg.blk[i] = blk ? blk[i] : (struct sbshm_blkdev){0};
        if (cfg.blk[i].flags & SBSHM_BLKDEV_PRESENT)
            cfg.v2 = 1;
    }
}

volatile struct sbshm_hdr *sbg_hdr(void) { return HDR; }
const struct sbg_stats *sbg_stats(void)
{
    st.guest_state = HDR->guest_state;
    st.guest_ncpus = HDR->guest_ncpus;
    st.guest_cpuif = HDR->guest_cpuif;
    return &st;
}

static int load(const void *image, uint64_t image_size, const void *dtb, uint64_t dtb_size, const void *initrd,
                uint64_t initrd_size, int placed);

int sbg_load(const void *image, uint64_t image_size, const void *dtb, uint64_t dtb_size, const void *initrd,
             uint64_t initrd_size)
{
    return load(image, image_size, dtb, dtb_size, initrd, initrd_size, 0);
}

int sbg_load_placed(uint64_t image_size, uint64_t dtb_size, uint64_t initrd_size)
{
    const void *img = (const void *)(uintptr_t)(cfg.linux_base + SBG_IMAGE_AT);
    if (((const volatile uint64_t *)img)[1] != SBG_IMAGE_AT) {
        fail("the placed Image's text_offset is not 0x80000");
        return -5;
    }
    return load(img, image_size, 0, dtb_size, 0, initrd_size, 1);
}

static int load(const void *image, uint64_t image_size, const void *dtb, uint64_t dtb_size, const void *initrd,
                uint64_t initrd_size, int placed)
{
    if (st.state != SBG_OFF && st.state != SBG_FAILED && st.state != SBG_LOADED)
        return -1;
    if (((const volatile uint32_t *)image)[14] != 0x644d5241U) {   /* "ARM\x64" at 0x38 */
        fail("the Image has no arm64 header");
        return -2;
    }
    /* text_offset: 0 in mainline since 5.8; the S9 port's Image keeps 0x80000 (Samsung's loader convention).
     * The Image goes at the 2 MiB aligned base + text_offset; image_size (with BSS) must end below the DTB. */
    uint64_t text_off = ((const volatile uint64_t *)image)[1], isz = ((const volatile uint64_t *)image)[2];
    if (!isz)
        isz = image_size;
    if (text_off >= (2U << 20) || text_off + isz > SBG_DTB_OFF || text_off + image_size > SBG_DTB_OFF ||
        dtb_size > SBG_RD_OFF - SBG_DTB_OFF || SBG_RD_OFF + initrd_size > cfg.linux_size) {
        fail("the Image, DTB or initrd does not fit the carve-out layout");
        return -3;
    }
    uint64_t stride = (uint64_t)cfg.surf_w * 4, surf = stride * cfg.surf_h;
    if (SBSHM_SURF_OFF + 2 * surf > cfg.shm_size) {
        fail("the surface does not fit the shared region");
        return -4;
    }
    if (SBSHM_SURF_OFF + 2 * surf > (cfg.v2 ? SBSHM_CAT_OFF : cfg.shm_size)) {
        fail("the surface does not fit below the v2 tables");
        return -4;
    }
    zero_words(SHM(0), SBSHM_SURF_OFF);              /* header, rings, the ramoops zone: a fresh log */
    zero_words(SHM(SBSHM_SURF_OFF), 2 * surf);
    v2_changes = 0;
    v2_window = SBSHM_NONE;
    v2_has_window = v2_text = 0;
    v2_purpose = 0;
    if (cfg.v2) {                                   /* the tables empty (seq 0, n 0); the block devices offered */
        zero_words(SHM(SBSHM_CAT_OFF), sizeof(struct sbshm_cat));
        zero_words(SHM(SBSHM_STORE_OFF), sizeof(struct sbshm_store));
        zero_words(SHM(SBSHM_BLK_OFF), sizeof(struct sbshm_blk));
        if (os->blk_io)                             /* no server, no devices */
            copy_words(SHM(SBSHM_BLK_OFF), cfg.blk, sizeof(cfg.blk));
    }
    blk_tail = blk_head = 0;
    blk_ring_bad = 0;
    img_pa = cfg.linux_base + text_off;
    if (!placed) {
        copy_words((uintptr_t)img_pa, image, image_size);
        copy_words((uintptr_t)(cfg.linux_base + SBG_DTB_OFF), dtb, dtb_size);
        if (initrd_size)
            copy_words((uintptr_t)(cfg.linux_base + SBG_RD_OFF), initrd, initrd_size);
    }
    volatile struct sbshm_hdr *h = HDR;
    h->magic = SBSHM_MAGIC;
    h->version = cfg.v2 ? SBSHM_VERSION_V2 : SBSHM_VERSION;
    h->hdr_size = sizeof(struct sbshm_hdr);
    h->boot_count = st.boots;
    h->gicd_base = cfg.gicd;
    h->db_to_guest = cfg.db_guest;
    h->db_to_host = cfg.db_host;
    h->host_cpuif = host_if;
    h->screen_on = 1;
    h->surf_w = cfg.surf_w;
    h->surf_h = cfg.surf_h;
    h->surf_stride = (uint32_t)stride;
    h->surf_format = SBSHM_FMT_XRGB8888;
    h->surf_nbuf = 2;
    h->surf_off[0] = SBSHM_SURF_OFF;
    h->surf_off[1] = (uint32_t)(SBSHM_SURF_OFF + surf);
    h->oops_off = SBSHM_OOPS_OFF;
    h->oops_size = SBSHM_OOPS_SIZE;
    volatile uint32_t *tr = (volatile uint32_t *)(uintptr_t)tramp_pa();
    for (unsigned i = 0; i < SBG_TRAMP_WORDS; ++i)
        tr[i] = sbg_tramp[i];
    uint64_t dtb_pa = cfg.linux_base + SBG_DTB_OFF;
    tr[SBG_TRAMP_IMG] = (uint32_t)img_pa;
    tr[SBG_TRAMP_IMG + 1] = (uint32_t)(img_pa >> 32);
    tr[SBG_TRAMP_DTB] = (uint32_t)dtb_pa;
    tr[SBG_TRAMP_DTB + 1] = (uint32_t)(dtb_pa >> 32);
    sbshm_dmb();
    h->host_state = SBG_HOST_LOADED;
    st.state = SBG_LOADED;
    st.hello = st.frames = st.hello_ncpus = 0;
    st.t_hello = st.t_first_frame = st.t_off = st.t_stop = 0;
    log_pos = 0;
    log_seen = 0;
    line_n = 0;
    return 0;
}

int sbg_start(void)
{
    if (st.state != SBG_LOADED)
        return -1;
    if (os->m3_prep && os->m3_prep() != 1) {
        fail("the M3 PMU defaults did not read back");
        return -2;
    }
    t_start = os->ms();
    st.boots++;
    int64_t r = os->cpu_on(cfg.first_mpidr, tramp_pa(), cfg.linux_base + SBG_DTB_OFF);
    struct tb t = {.n = 0};
    t_s(&t, "guest: CPU_ON(");
    t_x(&t, cfg.first_mpidr);
    t_s(&t, ", trampoline ");
    t_x(&t, tramp_pa());
    t_s(&t, ") = ");
    if (r < 0) {
        t_s(&t, "-");
        t_u(&t, (uint64_t)-r);
    } else {
        t_u(&t, (uint64_t)r);
    }
    say(&t);
    if (r) {
        fail("PSCI CPU_ON refused");
        return -3;
    }
    HDR->host_state = SBG_HOST_STARTED;
    st.state = SBG_BOOTING;
    st.t_cpu_on = 0;
    return 0;
}

/* the guest kernel's log, line by line, from the ramoops console zone */
static void log_stream(void)
{
    volatile struct sbshm_prz *z = (volatile struct sbshm_prz *)SHM(SBSHM_OOPS_OFF);
    if (z->sig != SBSHM_PRZ_SIG)
        return;
    uint32_t cap = SBSHM_OOPS_SIZE - 12U, start = z->start;
    if (start >= cap)
        return;
    if (!log_seen) {
        log_seen = 1;
        uint32_t size = z->size;
        log_pos = size >= cap ? start : (start + cap - size) % cap;
    }
    for (unsigned budget = 4096; log_pos != start && budget; --budget) {
        char c = (char)z->data[log_pos];
        log_pos = (log_pos + 1) % cap;
        if (c == '\n' || line_n == sizeof(line) - 1) {
            line[line_n] = 0;
            if (os->guest_log)
                os->guest_log(line);
            line_n = 0;
        } else if (c >= 32 && c < 127) {
            line[line_n++] = c;
        }
    }
}

static int put_c(uint16_t type, uint32_t a, uint32_t b, uint32_t c, int32_t x, int32_t y)
{
    if (st.state != SBG_RUNNING && !(type == SBG_MSG_STOP && st.state == SBG_BOOTING))
        return 0;
    struct sbshm_msg m = {0};
    m.type = type;
    m.a = a;
    m.b = b;
    m.c = c;
    m.x = x;
    m.y = y;
    m.t_ms = (uint32_t)os->ms();
    if (!sbshm_put(H2G, &m))
        return 0;
    if (cfg.db_guest) {
        wr32((uintptr_t)cfg.gicd + 0x200 + 4 * (cfg.db_guest / 32), 1U << (cfg.db_guest % 32));   /* ISPENDR */
        st.doorbells_out++;
    }
    return 1;
}

static int put(uint16_t type, uint32_t a, uint32_t b, int32_t x, int32_t y) { return put_c(type, a, b, 0, x, y); }

int sbg_touch(uint32_t slot, uint32_t phase, int32_t x, int32_t y)
{
    int r = put(SBG_MSG_TOUCH, slot, phase, x, y);
    st.touches_sent += (uint32_t)r;
    return r;
}
int sbg_key(uint32_t code, int down) { return put(SBG_MSG_KEY, code, down ? 1U : 0U, 0, 0); }
int sbg_send(uint16_t type, uint32_t a, uint32_t b) { return put(type, a, b, 0, 0); }
int sbg_screen(int on)
{
    HDR->screen_on = on ? 1U : 0U;
    return put(SBG_MSG_SCREEN, on ? 1U : 0U, 0, 0, 0);
}

int sbg_stop(void)
{
    if (st.state != SBG_RUNNING && st.state != SBG_BOOTING)
        return -1;
    HDR->host_state = SBG_HOST_STOPPING;
    int was_booting = st.state == SBG_BOOTING;
    st.state = SBG_RUNNING;                         /* put() wants RUNNING */
    put(SBG_MSG_STOP, 0, 0, 0, 0);
    st.state = SBG_STOPPING;
    t_stop_req = os->ms();
    st.t_stop = t_stop_req - t_start;
    struct tb t = {.n = 0};
    t_s(&t, was_booting ? "guest: MSG_STOP (still booting: it reads it after HELLO)" : "guest: MSG_STOP");
    say(&t);
    return 0;
}

void sbg_doorbell(void) { /* the poll that follows reads the ring; the count is the host's own */ }

/* ---- contract v2 ------------------------------------------------------------------------------------------ */
unsigned sbg_changes(void)
{
    unsigned c = v2_changes;
    v2_changes = 0;
    return c;
}
const volatile struct sbshm_cat *sbg_cat(void) { return (const volatile struct sbshm_cat *)SHM(SBSHM_CAT_OFF); }
const volatile struct sbshm_store *sbg_store(void) { return (const volatile struct sbshm_store *)SHM(SBSHM_STORE_OFF); }
const volatile uint32_t *sbg_icon(uint32_t i)
{
    return i < SBSHM_ICONS_MAX ? (const volatile uint32_t *)SHM(SBSHM_ICON_OFF + i * SBSHM_ICON_BYTES) : 0;
}
uint32_t sbg_window(int *has_window)
{
    if (has_window)
        *has_window = v2_has_window;
    return v2_window;
}
int sbg_text_focus(uint32_t *purpose)
{
    if (purpose)
        *purpose = v2_purpose;
    return v2_text;
}
int sbg_launch(uint32_t idx, uint32_t cat_seq) { return put(SBG_MSG_LAUNCH, idx, cat_seq, 0, 0); }
int sbg_activate(uint32_t idx) { return put(SBG_MSG_ACTIVATE, idx, 0, 0, 0); }
int sbg_app_close(uint32_t idx) { return put(SBG_MSG_APP_CLOSE, idx, 0, 0, 0); }
int sbg_viewport(uint32_t rows, uint32_t width) { return put(SBG_MSG_VIEWPORT, rows, width, 0, 0); }
int sbg_pkg(uint32_t idx, uint32_t op, uint32_t store_seq) { return put_c(SBG_MSG_PKG, idx, op, store_seq, 0, 0); }
int sbg_text(const char *s)
{
    while (*s) {
        uint8_t b[8] = {0};
        unsigned n = 0;
        while (s[n]) {                              /* whole UTF-8 characters, up to 8 bytes */
            unsigned c = (uint8_t)s[n], len = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
            if (n + len > 8)
                break;
            for (unsigned k = 0; k < len && s[n]; ++k, ++n)
                b[n] = (uint8_t)s[n];
        }
        if (!n)
            return 0;
        uint32_t a = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        uint32_t w = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
        if (!put_c(SBG_MSG_TEXT, a, w, n, 0, 0))
            return 0;
        s += n;
    }
    return 1;
}

const volatile uint32_t *sbg_rows(void)
{
    return HDR->rows_valid ? (const volatile uint32_t *)SHM(SBSHM_ROWS_OFF) : 0;
}

int sbg_frame(uint32_t *seq, const volatile uint32_t **pixels, uint32_t *stride_bytes)
{
    volatile struct sbshm_hdr *h = HDR;
    uint32_t s = h->surf_seq;
    if (s == *seq || (st.state != SBG_RUNNING && st.state != SBG_STOPPING))
        return 0;
    sbshm_dmb();
    uint32_t f = h->surf_front & 1U;
    *seq = s;
    *pixels = (const volatile uint32_t *)SHM(h->surf_off[f]);
    *stride_bytes = h->surf_stride;
    if (!st.frames++)
        st.t_first_frame = os->ms() - t_start;
    return 1;
}

/* ---- sbblk: serve the guest's block requests (sbshm.h "v2: sbblk") -----------------------------------------
 * In ring order, a bounded number per poll. Per request: read req_head, dmb, copy the entry (once: the guest
 * could change it under us), check it against cfg.blk (never against the table in the region), run os->blk_io on
 * the request's slot, write the completion entry, dmb, then req_tail and done_head; one doorbell per batch. */
static int32_t blk_one(const struct sbshm_blkreq *r)
{
    if (r->dev >= SBSHM_BLK_DEVS || !(cfg.blk[r->dev].flags & SBSHM_BLKDEV_PRESENT))
        return -SBG_ENODEV;
    const struct sbshm_blkdev *d = &cfg.blk[r->dev];
    if (r->op == SBG_BLK_FLUSH) {
        if (d->flags & SBSHM_BLKDEV_RO)
            return 0;
        int e = os->blk_io(r->dev, SBG_BLK_FLUSH, 0, 0, 0);
        if (!e)
            st.blk_flushes++;
        return e;
    }
    if (r->op != SBG_BLK_READ && r->op != SBG_BLK_WRITE)
        return -SBG_EINVAL;
    if (r->data_slot >= SBSHM_BLK_SLOTS || !r->count || r->count > SBSHM_BLK_SLOT / 512U)
        return -SBG_EINVAL;
    if (r->sector >= d->sectors || r->count > d->sectors - r->sector)
        return -SBG_EINVAL;
    if (r->op == SBG_BLK_WRITE && (d->flags & SBSHM_BLKDEV_RO))
        return -SBG_EROFS;
    int e = os->blk_io(r->dev, r->op, r->sector, r->count,
                       (void *)SHM(SBSHM_BLKDATA_OFF + (uint64_t)r->data_slot * SBSHM_BLK_SLOT));
    if (!e) {
        if (r->op == SBG_BLK_READ) {
            st.blk_reads++;
            st.blk_rd_sectors += r->count;
        } else {
            st.blk_writes++;
            st.blk_wr_sectors += r->count;
        }
    }
    return e > 0 ? -SBG_EIO : e;
}

static void blk_serve(void)
{
    if (!cfg.v2 || !os->blk_io || blk_ring_bad)
        return;
    volatile struct sbshm_blk *b = (volatile struct sbshm_blk *)SHM(SBSHM_BLK_OFF);
    uint32_t max = cfg.blk_per_poll ? cfg.blk_per_poll : SBG_BLK_PER_POLL;
    uint32_t max_ms = cfg.blk_poll_ms ? cfg.blk_poll_ms : SBG_BLK_POLL_MS, n = 0;
    uint64_t t0 = os->ms(), t1 = t0;
    for (;;) {
        uint32_t head = b->req_head;
        if (head == blk_tail)
            break;
        if (head - blk_tail > SBSHM_BLK_RING) {     /* not a ring any more: stop serving, say so once */
            blk_ring_bad = 1;
            st.blk_errors++;
            struct tb t = {.n = 0};
            t_s(&t, "guest: sbblk: the request ring is broken (req_head ");
            t_u(&t, head);
            t_s(&t, ", req_tail ");
            t_u(&t, blk_tail);
            t_s(&t, "): block service stopped");
            say(&t);
            break;
        }
        if (blk_head - b->done_tail >= SBSHM_BLK_RING)      /* the guest has not reaped: wait for it */
            break;
        if (n >= max || (n && t1 - t0 >= max_ms)) {
            st.blk_capped++;
            break;
        }
        sbshm_dmb();
        struct sbshm_blkreq r;
        const volatile uint32_t *s = (const volatile uint32_t *)&b->req[blk_tail & (SBSHM_BLK_RING - 1)];
        uint32_t *d = (uint32_t *)&r;
        for (unsigned i = 0; i < sizeof(r) / 4; ++i)
            d[i] = s[i];
        r.status = blk_one(&r);
        r.pad = 0;
        if (r.status) {
            st.blk_errors++;
            if (st.blk_errors <= 8) {
                struct tb t = {.n = 0};
                t_s(&t, "guest: sbblk: dev ");
                t_u(&t, r.dev);
                t_s(&t, " op ");
                t_u(&t, r.op);
                t_s(&t, " sector ");
                t_u(&t, r.sector);
                t_s(&t, " + ");
                t_u(&t, r.count);
                t_s(&t, ": error -");
                t_u(&t, (uint64_t)-(int64_t)r.status);
                say(&t);
            }
        }
        volatile uint32_t *c = (volatile uint32_t *)&b->done[blk_head & (SBSHM_BLK_RING - 1)];
        for (unsigned i = 0; i < sizeof(r) / 4; ++i)
            c[i] = d[i];
        sbshm_dmb();                                /* the slot's data and the entry, then the indices */
        b->req_tail = ++blk_tail;
        b->done_head = ++blk_head;
        ++n;
        t1 = os->ms();
    }
    if (!n)
        return;
    sbshm_dmb();
    if (cfg.db_guest)
        wr32((uintptr_t)cfg.gicd + 0x200 + 4 * (cfg.db_guest / 32), 1U << (cfg.db_guest % 32));   /* ISPENDR */
    st.blk_doorbells++;
    if (n > st.blk_batch_max)
        st.blk_batch_max = n;
    if (t1 - t0 > st.blk_poll_ms_max)
        st.blk_poll_ms_max = (uint32_t)(t1 - t0);
}

void sbg_poll(void)
{
    if (st.state == SBG_OFF || st.state == SBG_LOADED)
        return;
    uint64_t t = os->ms();
    volatile struct sbshm_hdr *h = HDR;
    h->host_time_ms = t;
    lines_check();
    log_stream();
    blk_serve();
    struct sbshm_msg m;
    while (sbshm_get(G2H, &m)) {
        st.msgs_in++;
        if (m.type == SBG_MSG_HELLO && !st.hello) {
            st.hello = 1;
            st.hello_ncpus = m.a;
            st.t_hello = t - t_start;
            if (st.state == SBG_BOOTING)
                st.state = SBG_RUNNING;
            struct tb tb = {.n = 0};
            t_s(&tb, "guest: HELLO after ");
            t_u(&tb, st.t_hello);
            t_s(&tb, " ms, ");
            t_u(&tb, m.a);
            t_s(&tb, " CPUs, GIC CPU interface ");
            t_x(&tb, h->guest_cpuif);
            t_s(&tb, ", kernel ");
            unsigned k = tb.n;
            for (unsigned i = 0; i < 63 && h->guest_kver[i] && k < sizeof(tb.b) - 1; ++i)
                tb.b[k++] = h->guest_kver[i];
            tb.b[k] = 0;
            tb.n = k;
            say(&tb);
        } else if (m.type == SBG_MSG_BYE) {
            struct tb tb = {.n = 0};
            t_s(&tb, "guest: BYE");
            say(&tb);
        } else if (m.type == SBG_MSG_CATALOG) {
            v2_changes |= SBG_CH_CATALOG;
        } else if (m.type == SBG_MSG_STORE) {
            v2_changes |= SBG_CH_STORE;
        } else if (m.type == SBG_MSG_PKG_STATUS) {
            v2_changes |= SBG_CH_PKG;
        } else if (m.type == SBG_MSG_WINDOW) {
            v2_window = m.a;
            v2_has_window = m.b != 0;
            v2_changes |= SBG_CH_WINDOW;
        } else if (m.type == SBG_MSG_TEXT_FOCUS) {
            v2_text = m.a != 0;
            v2_purpose = m.b;
            v2_changes |= SBG_CH_TEXT;
        }
    }
    if (st.state == SBG_BOOTING && t - t_start > cfg.boot_timeout_ms) {
        log_stream();
        struct tb tb = {.n = 0};
        t_s(&tb, "guest: no HELLO: trampoline breadcrumb ");
        t_x(&tb, crumb());
        t_s(&tb, crumb() == tramp_pa() + 4 * SBG_TRAMP_CRUMB ? " (the core ran the trampoline)"
                                                            : " (the core never reached the trampoline)");
        t_s(&tb, ", ramoops ");
        t_s(&tb, ((volatile struct sbshm_prz *)SHM(SBSHM_OOPS_OFF))->sig == SBSHM_PRZ_SIG ? "up" : "never set up");
        say(&tb);
        fail("no HELLO before the boot timeout (the M3 cluster stays on until the next reset)");
        return;
    }
    if (st.state == SBG_STOPPING) {
        int off = os->cluster_off ? os->cluster_off() : -1;
        uint32_t gs = h->guest_state;
        if (off == 1 || (off < 0 && gs == SBG_GUEST_OFF && t - t_stop_req > 50)) {
            log_stream();
            st.state = SBG_OFF;
            st.t_off = t - t_stop_req;
            h->host_state = SBG_HOST_OFF;
            struct tb tb = {.n = 0};
            t_s(&tb, off == 1 ? "guest: the M3 cluster is off, " : "guest: the guest reports off (no PSCI check), ");
            t_u(&tb, st.t_off);
            t_s(&tb, " ms after MSG_STOP");
            say(&tb);
        } else if (t - t_stop_req > cfg.stop_timeout_ms) {
            log_stream();
            fail(gs == SBG_GUEST_PANIC ? "the guest panicked while stopping" : "the cluster did not power off");
        }
    }
}
