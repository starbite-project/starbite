/* SPDX-License-Identifier: GPL-2.0-only
 * The BCM4775 sensor hub's wire formats, pure C (shub_proto.h; docs/SENSOR-HUB.md). The packet format, its CRC and
 * the receiver's rules follow Samsung's GPL bbdpl/bbd_rpc_lh.c (read for the format; nothing copied but the
 * constants); the SSP layouts follow ssp_bbd.c / ssp_data.c of the same tree. */
#include "shub_proto.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; }

static void put32(uint8_t *o, uint32_t v, int be)
{
    for (int i = 0; i < 4; ++i)
        o[i] = (uint8_t)(v >> (be ? 24 - 8 * i : 8 * i));
}

/* ---- packets ----------------------------------------------------------------------------------------------- */
uint8_t shp_crc(const uint8_t *p, unsigned n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i)
            c = (uint8_t)(c & 0x80U ? ((unsigned)c << 1) ^ 0x4dU : (unsigned)c << 1);
    }
    return (uint8_t)((c & 0x0fU) << 4 | c >> 4);
}

static unsigned stuff(uint8_t *out, unsigned cap, unsigned o, uint8_t b)
{
    uint8_t esc = b == 0xb0U ? 0x03U : b == 0x11U ? 0x04U : b == 0x13U ? 0x05U : 0U;
    if (esc) {
        if (o + 2U > cap)
            return 0;
        out[o++] = 0xb0U;
        out[o++] = esc;
    } else {
        if (o + 1U > cap)
            return 0;
        out[o++] = b;
    }
    return o;
}

unsigned shp_pkt_build(uint8_t *out, unsigned cap, const struct shp_pkt *p)
{
    uint8_t hdr[14];
    unsigned h = 0, o = 0;
    uint16_t fl = p->flags;
    if (p->len > SHP_MAX_PAYLOAD || (p->len && !p->payload))
        return 0;
    fl &= (uint16_t)~(SHP_F_SIZE_EXT | SHP_F_EXT);
    if (p->len > 0xffU)
        fl |= SHP_F_SIZE_EXT;
    if (fl >> 8)
        fl |= SHP_F_EXT;
    hdr[h++] = p->seq;
    hdr[h++] = (uint8_t)p->len;
    hdr[h++] = (uint8_t)fl;
    if (fl & SHP_F_ACK)
        hdr[h++] = p->ack;
    if (fl & SHP_F_RELIABLE)
        hdr[h++] = p->rel;
    if (fl & SHP_F_REL_ACK)
        hdr[h++] = p->rel_ack;
    if (fl & SHP_F_REL_NACK)
        hdr[h++] = p->rel_nack;
    if (fl & SHP_F_LOST)
        hdr[h++] = p->lost;
    if (fl & SHP_F_GARBAGE)
        hdr[h++] = p->garbage;
    if (fl & SHP_F_SIZE_EXT)
        hdr[h++] = (uint8_t)(p->len >> 8);
    if (fl & SHP_F_EXT)
        hdr[h++] = (uint8_t)(fl >> 8);
    if (fl & SHP_F_INTERNAL)
        hdr[h++] = 0;
    if (fl & SHP_F_IGNORE_SEQ)
        hdr[h++] = 0;
    /* the CRC runs over everything after seq: the header from size on, then the payload */
    uint8_t c = 0;
    for (unsigned i = 1; i < h + p->len; ++i) {
        c ^= i < h ? hdr[i] : p->payload[i - h];
        for (int k = 0; k < 8; ++k)
            c = (uint8_t)(c & 0x80U ? ((unsigned)c << 1) ^ 0x4dU : (unsigned)c << 1);
    }
    c = (uint8_t)((c & 0x0fU) << 4 | c >> 4);
    if (cap < 4U)
        return 0;
    out[o++] = 0xb0U;
    out[o++] = 0x00U;
    for (unsigned i = 0; i < h; ++i)
        if (!(o = stuff(out, cap, o, hdr[i])))
            return 0;
    for (unsigned i = 0; i < p->len; ++i)
        if (!(o = stuff(out, cap, o, p->payload[i])))
            return 0;
    if (!(o = stuff(out, cap, o, c)) || o + 2U > cap)
        return 0;
    out[o++] = 0xb0U;
    out[o++] = 0x01U;
    return o;
}

int shp_pkt_parse(const uint8_t *raw, unsigned n, struct shp_pkt *out)
{
    if (n < 4U)
        return 0;
    uint8_t crc = shp_crc(raw + 1, n - 2U);
    if (crc != raw[n - 1U])
        return 0;
    struct shp_pkt p = { 0 };
    const uint8_t *d = raw;
    unsigned left = n - 1U;
    p.seq = *d++;
    unsigned size = *d++;
    uint16_t fl = *d++, seen = 0;
    left -= 3U;
    for (unsigned i = 0; i < 16U && fl && left; ++i) {
        uint16_t bit = (uint16_t)(1U << i);
        if (!(fl & bit))
            continue;
        fl &= (uint16_t)~bit;
        seen |= bit;
        uint8_t det = *d++;
        --left;
        switch (bit) {
        case SHP_F_ACK: p.ack = det; break;
        case SHP_F_RELIABLE: p.rel = det; break;
        case SHP_F_REL_ACK: p.rel_ack = det; break;
        case SHP_F_REL_NACK: p.rel_nack = det; break;
        case SHP_F_LOST: p.lost = det; break;
        case SHP_F_GARBAGE: p.garbage = det; break;
        case SHP_F_SIZE_EXT: size |= (unsigned)det << 8; break;
        case SHP_F_EXT: fl |= (uint16_t)(det << 8); break;
        case SHP_F_INTERNAL:
        case SHP_F_IGNORE_SEQ: break;
        default: return 0;              /* a flag this format does not have */
        }
    }
    if (fl || size != left)
        return 0;
    p.flags = seen;
    p.payload = d;
    p.len = (uint16_t)left;
    p.crc = crc;
    *out = p;
    return 1;
}

enum { RX_ESC_SOP = 0, RX_SOP, RX_BODY, RX_ESC };

void shp_rx_init(struct shp_rx *r)
{
    r->st = RX_ESC_SOP;
    r->n = 0;
    r->good = r->bad = r->garbage = 0;
}

int shp_rx_byte(struct shp_rx *r, uint8_t b, struct shp_pkt *out)
{
    if (b == 0x11U || b == 0x13U)           /* XON / XOFF never travel raw */
        return 0;
    switch (r->st) {
    case RX_ESC_SOP:
        if (b == 0xb0U)
            r->st = RX_SOP;
        else
            ++r->garbage;
        return 0;
    case RX_SOP:
        if (b == 0x00U) {
            r->st = RX_BODY;
            r->n = 0;
        } else if (b != 0xb0U) {
            r->st = RX_ESC_SOP;
            r->garbage += 2;
        }
        return 0;
    case RX_BODY:
        if (b == 0xb0U)
            r->st = RX_ESC;
        else if (r->n < sizeof(r->buf))
            r->buf[r->n++] = b;
        else
            r->st = RX_ESC_SOP;
        return 0;
    default: {
        uint8_t lit = b == 0x03U ? 0xb0U : b == 0x04U ? 0x11U : b == 0x05U ? 0x13U : 0U;
        if (b == 0x01U) {
            r->st = RX_ESC_SOP;
            if (shp_pkt_parse(r->buf, r->n, out)) {
                ++r->good;
                return 1;
            }
            ++r->bad;
            return -1;
        }
        if (lit) {
            if (r->n < sizeof(r->buf)) {
                r->buf[r->n++] = lit;
                r->st = RX_BODY;
            } else {
                r->st = RX_ESC_SOP;
            }
        } else if (b == 0x00U) {            /* the end was missed; a new packet starts */
            r->st = RX_BODY;
            r->n = 0;
            ++r->bad;
        } else {
            r->st = b == 0xb0U ? RX_SOP : RX_ESC_SOP;
        }
        return 0;
    }
    }
}

/* ---- RPC --------------------------------------------------------------------------------------------------- */
unsigned shp_rpc_put(uint8_t *out, unsigned cap, uint16_t id, const uint8_t *data, unsigned len)
{
    unsigned o = 0, need = (id > 0x7fU ? 2U : 1U) + (len > 0x7fU ? 2U : 1U) + len;
    if (need > cap || id > 0x7fffU || len > 0x7fffU)
        return 0;
    if (id > 0x7fU)
        out[o++] = (uint8_t)(0x80U | id >> 8);
    out[o++] = (uint8_t)id;
    if (len > 0x7fU)
        out[o++] = (uint8_t)(0x80U | len >> 8);
    out[o++] = (uint8_t)len;
    for (unsigned i = 0; i < len; ++i)
        out[o++] = data[i];
    return o;
}

int shp_rpc_next(const uint8_t *p, unsigned n, unsigned *off, uint16_t *id, const uint8_t **data, uint16_t *len)
{
    unsigned o = *off;
    if (o >= n)
        return 0;
    unsigned v = p[o++];
    if (v & 0x80U) {
        if (o >= n)
            return -1;
        v = (v & 0x7fU) << 8 | p[o++];
    }
    if (o >= n)
        return -1;
    unsigned l = p[o++];
    if (l & 0x80U) {
        if (o >= n)
            return -1;
        l = (l & 0x7fU) << 8 | p[o++];
    }
    if (l > n - o)
        return -1;
    *id = (uint16_t)v;
    *data = p + o;
    *len = (uint16_t)l;
    *off = o + l;
    return 1;
}

/* ---- SSI --------------------------------------------------------------------------------------------------- */
unsigned shp_ssi_wframe(uint8_t *mosi, unsigned cap, const uint8_t *d, unsigned m)
{
    if (m > 0xffffU || m + SHP_SSI_OVERHEAD > cap)
        return 0;
    mosi[0] = SHP_SSI_WRITE;
    mosi[1] = (uint8_t)m;
    mosi[2] = (uint8_t)(m >> 8);
    for (unsigned i = 0; i < m; ++i)
        mosi[3U + i] = d[i];
    mosi[3U + m] = 0;
    mosi[4U + m] = 0;
    return m + SHP_SSI_OVERHEAD;
}

void shp_ssi_wparse(const uint8_t *miso, unsigned m, struct shp_ssi_w *r)
{
    r->status = miso[0];
    r->n = le16(miso + 1);
    if (r->n > m)
        r->n = m;                           /* the frame held room for m of them; the rest waits for a read */
    r->chip = miso + 3;
    r->accepted = le16(miso + 3U + m);
}

unsigned shp_ssi_space(uint8_t status)
{
    static const unsigned blk[4] = { 32, 256, 2048, 16384 };
    return (status & 7U) * blk[(status >> 3) & 3U];
}

/* ---- the ROM dialogue -------------------------------------------------------------------------------------- */
unsigned shp_sync_build(uint8_t *out, unsigned cap, uint8_t attempt)
{
    uint8_t pl[2] = { 0, attempt };
    struct shp_pkt p = { 0 };
    p.flags = SHP_F_INTERNAL | SHP_F_IGNORE_SEQ;
    p.payload = pl;
    p.len = 2;
    return shp_pkt_build(out, cap, &p);
}

int shp_sync_parse(const struct shp_pkt *p, struct shp_sync *s)
{
    if (!(p->flags & SHP_F_INTERNAL) || p->len < 8U || p->payload[0] != 1U)
        return 0;
    s->n = p->payload[1];
    s->last_seq_rx = p->payload[2];
    s->last_ack = p->payload[3];
    s->last_rel_rx = p->payload[4];
    s->next_seq = p->payload[5];
    s->next_rel = p->payload[6];
    s->last_rel_acked = p->payload[7];
    return 1;
}

int shp_version_parse(const uint8_t *d, unsigned n, struct shp_version *v)
{
    v->asic = v->rom = v->patch = 0;
    v->big_endian = 0;
    if (n < 12U)
        return 0;
    if (be32(d) == SHP_ASIC_4775B0 && le32(d) != SHP_ASIC_4775B0) {
        v->big_endian = 1;
        v->asic = be32(d);
        v->rom = be32(d + 4);
        v->patch = be32(d + 8);
        return 1;
    }
    v->asic = le32(d);
    v->rom = le32(d + 4);
    v->patch = le32(d + 8);
    return v->asic == SHP_ASIC_4775B0;
}

int shp_patch_next(const uint8_t *blob, unsigned n, unsigned *off, uint32_t *addr, const uint8_t **data, unsigned *len)
{
    unsigned o = *off;
    if (o == n)
        return 0;
    if (n - o < 3U)
        return -1;
    unsigned l = le16(blob + o + 1U);
    if (blob[o] != 0xf1U || l < 4U || l > n - o - 3U)
        return -1;
    *addr = le32(blob + o + 3U);
    *data = blob + o + 7U;
    *len = l - 4U;
    *off = o + 3U + l;
    return 1;
}

unsigned shp_rpc_write_mem(uint8_t *out, unsigned cap, uint32_t addr, const uint8_t *d, unsigned n, int big_endian)
{
    unsigned body = 6U + n, o = 0;
    if (n > 0xffffU || body > 0x7fffU || body + 3U > cap)
        return 0;
    out[o++] = SHP_RPC_WRITE_MEM;
    if (body > 0x7fU)
        out[o++] = (uint8_t)(0x80U | body >> 8);
    out[o++] = (uint8_t)body;
    put32(out + o, addr, big_endian);
    o += 4U;
    out[o++] = (uint8_t)(big_endian ? n >> 8 : n);
    out[o++] = (uint8_t)(big_endian ? n : n >> 8);
    for (unsigned i = 0; i < n; ++i)
        out[o++] = d[i];
    return o;
}

unsigned shp_rpc_start(uint8_t *out, unsigned cap, uint32_t addr, int big_endian)
{
    uint8_t a[4];
    put32(a, addr, big_endian);
    return shp_rpc_put(out, cap, SHP_RPC_START, a, 4);
}

/* ---- SSP --------------------------------------------------------------------------------------------------- */
void shp_ssp_hdr(uint8_t out[SSP_HDR], uint8_t cmd, uint16_t len, uint16_t opts, uint32_t data)
{
    out[0] = cmd;
    out[1] = (uint8_t)len;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)opts;
    out[4] = (uint8_t)(opts >> 8);
    put32(out + 5, data, 0);
}

void shp_ssp_enable(uint8_t out[10], uint8_t sensor, int32_t period_ms, int32_t latency_ms, int8_t batch_opts)
{
    out[0] = sensor;
    put32(out + 1, (uint32_t)period_ms, 0);
    put32(out + 5, (uint32_t)latency_ms, 0);
    out[9] = (uint8_t)batch_opts;
}

unsigned shp_ssp_wrap(uint8_t *out, unsigned cap, const uint8_t *ssp, unsigned n)
{
    unsigned body = n + 2U, o = 0;
    if (body > 0x7fffU || body + 3U > cap)
        return 0;
    out[o++] = SHP_RPC_SENSOR_REQ;
    if (body > 0x7fU)
        out[o++] = (uint8_t)(0x80U | body >> 8);
    out[o++] = (uint8_t)body;
    out[o++] = (uint8_t)n;
    out[o++] = (uint8_t)(n >> 8);
    for (unsigned i = 0; i < n; ++i)
        out[o++] = ssp[i];
    return o;
}

void shp_ssp_rx_init(struct shp_ssp_rx *s)
{
    s->st = 0;
    s->type = 0;
    s->opts = 0;
    s->need = 4;
    s->n = 0;
    s->read_len = 0;
}

static void ssp_rx_reset(struct shp_ssp_rx *s)
{
    s->st = 0;
    s->need = 4;
    s->n = 0;
}

int shp_ssp_feed(struct shp_ssp_rx *s, const uint8_t *p, unsigned n, unsigned *used)
{
    unsigned i = 0;
    if (s->st == 0 && s->need == 4U)
        s->n = 0;                           /* the last event's body is gone */
    while (i < n) {
        unsigned take = n - i < s->need ? n - i : s->need;
        for (unsigned k = 0; k < take; ++k)
            s->buf[s->n + k] = p[i + k];
        s->n = (uint16_t)(s->n + take);
        s->need = (uint16_t)(s->need - take);
        i += take;
        if (s->need)
            break;
        if (s->st == 0) {                   /* the 4-byte header [options][length] */
            unsigned len = le16(s->buf + 2);
            s->opts = le16(s->buf);
            s->type = (uint8_t)(s->opts & 3U);
            s->n = 0;
            if (s->type == SSP_WRITE) {
                ssp_rx_reset(s);
                *used = i;
                return SSP_EV_WRITE_GO;
            }
            if (s->type == SSP_READ)
                len = s->read_len;          /* the length is the one the request asked for */
            else if (s->type != SSP_HUB2AP)
                len = SSP_DATA_MAX + 1U;
            if (len > SSP_DATA_MAX || (s->type == SSP_READ && !len)) {
                ssp_rx_reset(s);
                *used = i;
                return SSP_EV_ERR;
            }
            if (!len) {
                ssp_rx_reset(s);
                continue;
            }
            s->st = 1;
            s->need = (uint16_t)len;
        } else {
            int ev = s->type == SSP_READ ? SSP_EV_READ : SSP_EV_FRAME;
            s->st = 0;
            s->need = 4;                    /* s->n stays the body's length until the next feed */
            *used = i;
            if (ev == SSP_EV_READ)
                s->read_len = 0;
            return ev;
        }
    }
    *used = i;
    return SSP_EV_NONE;
}

/* the sample length of a sensor in a 0x37 record (ssp_data.c's get_*_sensordata), 0 unknown */
static unsigned sample_len(unsigned sensor)
{
    switch (sensor) {
    case 0: return 6;                       /* accelerometer */
    case 1: return 12;                      /* gyroscope */
    case 3: return 6;                       /* magnetometer raw */
    case 4: return 8;                       /* magnetometer: x y z, accuracy, overflow */
    case 5: return 6;                       /* pressure */
    case 13: return 1;                      /* step detector */
    case 14: return 1;                      /* significant motion */
    case 18: return 4;                      /* step counter */
    default: return 0;
    }
}

int shp_frame_next(const uint8_t *f, unsigned n, unsigned *off, struct shp_rec *r)
{
    unsigned o = *off;
    if (o >= n)
        return 0;
    r->kind = SSP_REC_OTHER;
    r->type = f[o++];
    r->sensor = 0xff;
    unsigned skip;
    switch (r->type) {
    case 0x37: {
        if (o >= n)
            return -1;
        r->sensor = f[o++];
        unsigned sl = sample_len(r->sensor);
        if (!sl || n - o < sl + 8U)
            return -1;
        r->kind = SSP_REC_SENSOR;
        r->x = r->y = r->z = 0;
        r->accuracy = r->overflow = 0;
        r->steps = 0;
        if (sl >= 6U) {
            r->x = (int16_t)le16(f + o);
            r->y = (int16_t)le16(f + o + 2U);
            r->z = (int16_t)le16(f + o + 4U);
        }
        if (r->sensor == SSP_SENSOR_MAG) {
            r->accuracy = f[o + 6U];
            r->overflow = f[o + 7U];
        }
        if (r->sensor == SSP_SENSOR_STEPS)
            r->steps = le32(f + o);
        o += sl;
        r->ts = (uint64_t)le32(f + o) | (uint64_t)le32(f + o + 4U) << 32;
        *off = o + 8U;
        return 1;
    }
    case 0x0a:
        r->kind = SSP_REC_INIT_DONE;
        skip = 0;
        break;
    case 0xa8: skip = 8; break;             /* timestamp offset */
    case 0x05: skip = 2; break;             /* meta: what, sensor */
    case 0x06: skip = 0; break;             /* time sync */
    case 0x08: skip = 12; break;            /* gyroscope calibration */
    case 0x04: skip = 9; break;             /* big data: type, length, address */
    case 0x0c: skip = 72; break;            /* scontext */
    case 0x03:                              /* debug text: u16 length, text */
        if (n - o < 2U)
            return -1;
        skip = 2U + le16(f + o);
        if (skip == 2U)
            return -1;
        break;
    default:
        return -1;
    }
    if (skip > n - o)
        return -1;
    *off = o + skip;
    return 1;
}

/* ---- compass and level ------------------------------------------------------------------------------------- */
#define SHP_PI 3.14159265358979323846

static double p_sqrt(double x) { return x > 0 ? __builtin_sqrt(x) : 0; }

static double p_atan(double x)
{
    int neg = x < 0, inv = 0;
    if (neg)
        x = -x;
    if (x > 1) {
        x = 1 / x;
        inv = 1;
    }
    x = x / (1 + p_sqrt(1 + x * x));        /* halve the angle twice: x <= tan(pi / 16) */
    x = x / (1 + p_sqrt(1 + x * x));
    double x2 = x * x;
    double a = 4 * x * (1 - x2 * (1.0 / 3 - x2 * (1.0 / 5 - x2 * (1.0 / 7 - x2 * (1.0 / 9 - x2 / 11)))));
    if (inv)
        a = SHP_PI / 2 - a;
    return neg ? -a : a;
}

static double p_atan2(double y, double x)
{
    if (x > 0)
        return p_atan(y / x);
    if (x < 0)
        return p_atan(y / x) + (y >= 0 ? SHP_PI : -SHP_PI);
    return y > 0 ? SHP_PI / 2 : y < 0 ? -SHP_PI / 2 : 0;
}

static int round10(double deg)
{
    double v = deg * 10;
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

int shp_heading(int ax, int ay, int az, int mx, int my, int mz, int *deg_x10)
{
    double Ax = ax, Ay = ay, Az = az, Ex = mx, Ey = my, Ez = mz;
    double an = p_sqrt(Ax * Ax + Ay * Ay + Az * Az), en = p_sqrt(Ex * Ex + Ey * Ey + Ez * Ez);
    if (an < 1 || en < 1)
        return 0;
    Ax /= an, Ay /= an, Az /= an;
    Ex /= en, Ey /= en, Ez /= en;
    /* H = E x A points east, M = A x H points north, both in the phone's axes (Android's getRotationMatrix) */
    double Hx = Ey * Az - Ez * Ay, Hy = Ez * Ax - Ex * Az, Hz = Ex * Ay - Ey * Ax;
    double hn = p_sqrt(Hx * Hx + Hy * Hy + Hz * Hz);
    if (hn < 0.1)                           /* the field within about 6 degrees of gravity: no horizontal part */
        return 0;
    Hx /= hn, Hy /= hn, Hz /= hn;
    double My = Az * Hx - Ax * Hz;
    int d = round10(p_atan2(Hy, My) * 180 / SHP_PI);
    d %= 3600;
    if (d < 0)
        d += 3600;
    *deg_x10 = d;
    return 1;
}

int shp_tilt(int ax, int ay, int az, int *pitch_x10, int *roll_x10)
{
    double Ax = ax, Ay = ay, Az = az;
    if (Ax * Ax + Ay * Ay + Az * Az < 1)
        return 0;
    *pitch_x10 = round10(p_atan2(Ay, p_sqrt(Ax * Ax + Az * Az)) * 180 / SHP_PI);
    *roll_x10 = round10(p_atan2(Ax, p_sqrt(Ay * Ay + Az * Az)) * 180 / SHP_PI);
    return 1;
}
