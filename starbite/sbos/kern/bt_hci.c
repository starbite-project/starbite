/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the Bluetooth host side that touches no hardware: H4 framing, HCI events, the .hcd patch, the UART
 * divisor, scan results with names, pairing groundwork. See bt_hci.h for the formats and their sources. */
#include "bt_hci.h"

/* ---- small helpers (the kernel is freestanding) ---- */

void btl_init(struct btl *l)
{
    l->n = 0;
    l->b[0] = 0;
}

static void btl_c(struct btl *l, char c)
{
    if (l->n + 1 < sizeof(l->b)) {
        l->b[l->n++] = c;
        l->b[l->n] = 0;
    }
}

void btl_s(struct btl *l, const char *s)
{
    while (s && *s)
        btl_c(l, *s++);
}

void btl_u(struct btl *l, uint64_t v)
{
    char t[24];
    unsigned k = 0;
    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        btl_c(l, t[--k]);
}

void btl_i(struct btl *l, int64_t v)
{
    if (v < 0) {
        btl_c(l, '-');
        btl_u(l, (uint64_t)(-v));
    } else {
        btl_u(l, (uint64_t)v);
    }
}

void btl_x(struct btl *l, uint64_t v, unsigned digits)
{
    static const char hx[] = "0123456789abcdef";
    while (digits--)
        btl_c(l, hx[(v >> (4 * digits)) & 15]);
}

int bthci_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

void bthci_copy(void *dst, const void *src, unsigned n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
}

/* ---- H4 ---- */

unsigned bthci_cmd(uint8_t *out, unsigned cap, uint16_t op, const uint8_t *p, uint8_t n)
{
    if (cap < 4U + n)
        return 0;
    out[0] = H4_CMD;
    out[1] = (uint8_t)op;
    out[2] = (uint8_t)(op >> 8);
    out[3] = n;
    for (unsigned i = 0; i < n; i++)
        out[4 + i] = p[i];
    return 4U + n;
}

void bthci_rx_reset(struct bthci_rx *r)
{
    r->type = 0;
    r->have = r->need = 0;
}

int bthci_rx_byte(struct bthci_rx *r, uint8_t b)
{
    if (!r->type) {
        if (b == H4_EVT || b == H4_ACL || b == H4_SCO) {
            r->type = b;
            r->have = r->need = 0;
            return BTHCI_RX_MORE;
        }
        r->junk++;
        return BTHCI_RX_JUNK;
    }
    if (r->type == H4_EVT) {
        r->ev[r->have++] = b;
        if (r->have == 2)
            r->need = (uint16_t)(2U + r->ev[1]);
        if (r->have >= 2 && r->have == r->need) {
            r->type = 0;
            return BTHCI_RX_EVENT;
        }
        return BTHCI_RX_MORE;
    }
    /* ACL: a 4-byte header with the length le16 in its last two (kept whole when it fits); SCO: 3 bytes, the length
     * u8 last (skipped) */
    unsigned hdr = r->type == H4_ACL ? 4U : 3U;
    if (r->type == H4_ACL && r->have < sizeof(r->acl))
        r->acl[r->have] = b;
    else if (r->have < hdr)
        r->ev[r->have] = b;
    r->have++;
    if (r->have == hdr)
        r->need = (uint16_t)(hdr + (r->type == H4_ACL ? (unsigned)(r->acl[2] | r->acl[3] << 8) : r->ev[2]));
    if (r->have >= hdr && r->have >= r->need) {
        int acl = r->type == H4_ACL && r->need <= sizeof(r->acl);
        r->type = 0;
        if (acl)
            return BTHCI_RX_ACL;
        r->skipped++;
        return BTHCI_RX_SKIPPED;
    }
    return BTHCI_RX_MORE;
}

/* ---- events ---- */

int bthci_cmd_complete(const uint8_t *ev, uint16_t *op, uint8_t *status, const uint8_t **ret, uint8_t *retlen)
{
    uint8_t n = ev[1];
    if (ev[0] != BTEV_CMD_COMPLETE || n < 3)
        return 0;
    *op = (uint16_t)(ev[3] | ev[4] << 8);
    *status = n >= 4 ? ev[5] : 0xFF;
    *ret = ev + 6;
    *retlen = n >= 4 ? (uint8_t)(n - 4) : 0;
    return 1;
}

int bthci_cmd_status(const uint8_t *ev, uint16_t *op, uint8_t *status)
{
    if (ev[0] != BTEV_CMD_STATUS || ev[1] < 4)
        return 0;
    *status = ev[2];
    *op = (uint16_t)(ev[4] | ev[5] << 8);
    return 1;
}

int bthci_version(const uint8_t *ret, uint8_t n, struct bthci_version *v)
{
    if (n < 8)
        return 0;
    v->hci_ver = ret[0];
    v->hci_rev = (uint16_t)(ret[1] | ret[2] << 8);
    v->lmp_ver = ret[3];
    v->manufacturer = (uint16_t)(ret[4] | ret[5] << 8);
    v->lmp_subver = (uint16_t)(ret[6] | ret[7] << 8);
    return 1;
}

/* ---- the .hcd ---- */

int bthci_hcd_next(const uint8_t *d, uint32_t size, uint32_t *off, struct bthci_rec *r)
{
    uint32_t at = *off;
    if (at >= size)
        return 0;
    if (size - at < 3)
        return -1;
    r->op = (uint16_t)(d[at] | d[at + 1] << 8);
    r->n = d[at + 2];
    if (size - at - 3 < r->n)
        return -1;
    r->p = d + at + 3;
    *off = at + 3 + r->n;
    return 1;
}

/* the first run of >= 12 printable bytes starting "BCM" in a Write_RAM's data */
static void hcd_name(const uint8_t *p, unsigned n, char *name, unsigned cap)
{
    if (name[0])
        return;
    for (unsigned i = 0; i + 3 <= n; i++) {
        if (p[i] != 'B' || p[i + 1] != 'C' || p[i + 2] != 'M')
            continue;
        unsigned len = 0;
        while (i + len < n && p[i + len] >= 0x20 && p[i + len] < 0x7f)
            len++;
        if (len >= 12) {
            unsigned k;
            for (k = 0; k < len && k + 1 < cap; k++)
                name[k] = (char)p[i + k];
            name[k] = 0;
            return;
        }
    }
}

int bthci_hcd_check(const uint8_t *d, uint32_t size, struct bthci_hcd *i)
{
    struct bthci_rec r;
    uint32_t off = 0;
    int e, launched = 0;
    i->records = i->writes = i->bytes = i->launch = 0;
    i->name[0] = 0;
    while ((e = bthci_hcd_next(d, size, &off, &r)) == 1) {
        if (launched)
            return BTHCI_HCD_EARLYLAUNCH;
        i->records++;
        if ((r.op >> 10) != 0x3F)
            return BTHCI_HCD_NOTVENDOR;
        if (r.op == BCM_WRITE_RAM) {
            if (r.n < 4)
                return BTHCI_HCD_SHORTWRITE;
            i->writes++;
            i->bytes += r.n - 4U;
            hcd_name(r.p + 4, r.n - 4U, i->name, sizeof(i->name));
        } else if (r.op == BCM_LAUNCH_RAM) {
            if (r.n < 4)
                return BTHCI_HCD_BADLAUNCH;
            i->launch = (uint32_t)r.p[0] | (uint32_t)r.p[1] << 8 | (uint32_t)r.p[2] << 16 | (uint32_t)r.p[3] << 24;
            launched = 1;
        }
    }
    if (e < 0)
        return BTHCI_HCD_TRUNC;
    if (!i->records)
        return BTHCI_HCD_EMPTY;
    return launched ? BTHCI_HCD_OK : BTHCI_HCD_NOLAUNCH;
}

const char *bthci_hcd_why(int e)
{
    switch (e) {
    case BTHCI_HCD_OK: return "good";
    case BTHCI_HCD_EMPTY: return "it has no records";
    case BTHCI_HCD_TRUNC: return "the file ends inside a record";
    case BTHCI_HCD_NOTVENDOR: return "a record is not a vendor command";
    case BTHCI_HCD_SHORTWRITE: return "a Write_RAM has no address";
    case BTHCI_HCD_NOLAUNCH: return "the last record is not Launch_RAM";
    case BTHCI_HCD_EARLYLAUNCH: return "a Launch_RAM comes before the end";
    case BTHCI_HCD_BADLAUNCH: return "a Launch_RAM has no address";
    default: return "unknown";
    }
}

/* ---- the UART's divisor ---- */

int bthci_uart_div(uint32_t clk_hz, uint32_t baud, struct bthci_div *d)
{
    if (!clk_hz || !baud || clk_hz / baud < 16)
        return 0;
    uint32_t div = (uint32_t)(((uint64_t)clk_hz + baud / 2) / baud);
    d->ubrdiv = div / 16 - 1;
    d->frac = div & 15;
    d->actual = clk_hz / div;
    return 1;
}

uint32_t bthci_pick_baud(uint32_t clk_hz, uint32_t top)
{
    static const uint32_t rates[] = { 3000000, 2000000, 1500000, 1000000, 921600, 460800 };
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        struct bthci_div d;
        if (rates[i] > top || !bthci_uart_div(clk_hz, rates[i], &d))
            continue;
        uint32_t e = d.actual > rates[i] ? d.actual - rates[i] : rates[i] - d.actual;
        if ((uint64_t)e * 1000 <= (uint64_t)rates[i] * 15)
            return rates[i];
    }
    return 0;
}

/* ---- scan results ---- */

void bthci_devs_clear(struct bthci_devs *l)
{
    l->n = l->reports = l->full = 0;
    l->inquiry_done = 0;
    l->inquiry_status = 0;
}

static void name_copy(const uint8_t *p, unsigned n, char *out, unsigned cap)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n && p[i] && k + 1 < cap; i++)
        if (p[i] >= 0x20 && p[i] != 0x7f)
            out[k++] = (char)p[i];
    out[k] = 0;
}

void bthci_ad_name(const uint8_t *d, unsigned n, char *out, unsigned cap)
{
    unsigned at = 0;
    int got = 0;
    out[0] = 0;
    while (at < n) {
        unsigned len = d[at];
        if (!len || at + 1 + len > n)
            break;
        uint8_t type = d[at + 1];
        if (type == 0x09 || (type == 0x08 && !got)) {
            name_copy(d + at + 2, len - 1, out, cap);
            got = type == 0x09 ? 2 : 1;
            if (got == 2)
                return;
        }
        at += 1 + len;
    }
}

static int same(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void heard(struct bthci_devs *l, const uint8_t *addr, int le, int addr_type, int rssi, uint32_t cod,
                  const uint8_t *ad, unsigned adlen, uint8_t psrm, uint16_t clkoff)
{
    struct bt_dev *s = 0;
    l->reports++;
    for (unsigned i = 0; i < l->n; i++)
        if (l->d[i].le == le && same(l->d[i].addr, addr)) {
            s = &l->d[i];
            break;
        }
    if (!s) {
        if (l->n >= BT_MAX_DEV) {
            l->full++;
            return;
        }
        s = &l->d[l->n++];
        bthci_copy(s->addr, addr, 6);
        s->le = (uint8_t)le;
        s->has_rssi = 0;
        s->rssi = 0;
        s->cod = 0;
        s->psrm = 1;
        s->clkoff = 0;
        s->hits = 0;
        s->name[0] = 0;
    }
    s->hits++;
    s->addr_type = (uint8_t)addr_type;
    if (rssi != 127 && rssi >= -127 && rssi <= 20 && (!s->has_rssi || rssi > s->rssi)) {
        s->rssi = (int8_t)rssi;
        s->has_rssi = 1;
    }
    if (cod)
        s->cod = cod;
    if (!le) {
        s->psrm = psrm;
        s->clkoff = (uint16_t)(clkoff | 0x8000U);
    }
    if (ad && adlen) {
        char nm[sizeof(s->name)];
        bthci_ad_name(ad, adlen, nm, sizeof(nm));
        if (nm[0])
            bthci_copy(s->name, nm, sizeof(nm));
    }
}

int bthci_scan_event(struct bthci_devs *l, const uint8_t *ev)
{
    unsigned len = ev[1];
    const uint8_t *p = ev + 2;
    switch (ev[0]) {
    case BTEV_INQUIRY_COMPLETE:
        l->inquiry_done = 1;
        l->inquiry_status = len ? p[0] : 0;
        return 1;
    case BTEV_INQUIRY_RESULT:      /* addr 6, page scan mode 1, reserved 2, class 3, clock offset 2 */
        for (unsigned i = 0; len >= 1 && i < p[0] && 1 + 14 * (i + 1) <= len; i++) {
            const uint8_t *r = p + 1 + 14 * i;
            heard(l, r, 0, 0, 127, (uint32_t)r[9] | (uint32_t)r[10] << 8 | (uint32_t)r[11] << 16, 0, 0, r[6],
                  (uint16_t)(r[12] | r[13] << 8));
        }
        return 1;
    case BTEV_INQUIRY_RESULT_RSSI: /* addr 6, page scan mode 1, reserved 1, class 3, clock offset 2, rssi 1 */
        for (unsigned i = 0; len >= 1 && i < p[0] && 1 + 14 * (i + 1) <= len; i++) {
            const uint8_t *r = p + 1 + 14 * i;
            heard(l, r, 0, 0, (int8_t)r[13], (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16, 0, 0, r[6],
                  (uint16_t)(r[11] | r[12] << 8));
        }
        return 1;
    case BTEV_EXT_INQUIRY_RESULT:  /* one: as with RSSI, then 240 bytes of EIR */
        if (len >= 15) {
            const uint8_t *r = p + 1;
            heard(l, r, 0, 0, (int8_t)r[13], (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16, r + 14,
                  len - 15, r[6], (uint16_t)(r[11] | r[12] << 8));
        }
        return 1;
    case BTEV_LE_META:             /* subevent 2: n x (type, addr type, addr 6, length, data, rssi) */
        if (len >= 2 && p[0] == BTEV_LE_ADV_REPORT) {
            unsigned n = p[1], at = 2;
            for (unsigned i = 0; i < n && at + 10 <= len; i++) {
                unsigned dl = p[at + 8];
                if (at + 10 + dl > len)
                    break;
                heard(l, p + at + 2, 1, p[at + 1], (int8_t)p[at + 9 + dl], 0, p + at + 9, dl, 0, 0);
                at += 10 + dl;
            }
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static int stronger(const struct bt_dev *c, const struct bt_dev *a)
{
    return c->has_rssi && (!a->has_rssi || c->rssi > a->rssi);
}

unsigned bthci_devs_sorted(const struct bthci_devs *l, unsigned *idx, unsigned max)
{
    unsigned all[BT_MAX_DEV], n = l->n < BT_MAX_DEV ? l->n : BT_MAX_DEV;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = i;
        while (j > 0 && stronger(&l->d[i], &l->d[all[j - 1]])) {
            all[j] = all[j - 1];
            j--;
        }
        all[j] = i;
    }
    if (n > max)
        n = max;
    for (unsigned i = 0; i < n; i++)
        idx[i] = all[i];
    return n;
}

/* ---- pairing groundwork ---- */

unsigned bthci_ssp_answer(const uint8_t *ev, uint8_t *out, unsigned cap)
{
    if (ev[0] == BTEV_IO_CAP_REQUEST && ev[1] >= 6) {
        /* addr 6, IO capability 3 NoInputNoOutput, OOB 0, authentication 0x02 dedicated bonding, no MITM */
        uint8_t p[9];
        bthci_copy(p, ev + 2, 6);
        p[6] = 0x03;
        p[7] = 0x00;
        p[8] = 0x02;
        return bthci_cmd(out, cap, BTHCI_IO_CAP_REPLY, p, sizeof(p));
    }
    if (ev[0] == BTEV_USER_CONFIRM_REQUEST && ev[1] >= 10)
        return bthci_cmd(out, cap, BTHCI_USER_CONFIRM_REPLY, ev + 2, 6);   /* Just Works: accept */
    return 0;
}
