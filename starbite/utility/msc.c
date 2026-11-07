/* SPDX-License-Identifier: GPL-2.0-only
 * USB Mass Storage, Bulk-Only Transport 1.0, with the SCSI commands macOS, Linux and Windows use (SPC-2 /
 * SBC-2 subset), on the DWC3 driver's endpoints 0x83 / 0x03 (usb_desc.c interface 2). Drives (LUNs) are
 * callbacks (msc.h): the RAM disk and the microSD card in main.c.
 *
 * No endpoint is ever stalled (the driver has no stall call for bulk endpoints): when the host asks for
 * more data than a command has, the data is padded with zeros and the CSW carries the residue (BOT
 * 6.7.2 case 4, "the device may pad"); when the host sends data for a refused command, it is received
 * and dropped (case 11 handled the same way). Every buffer the controller touches is in the driver's
 * non-cacheable DMA window. */
#include "msc.h"
#include "usb_desc.h"
#include "usb_dwc3.h"

#define P_IN  USB_PHYS(USB_EP_MSC_IN)
#define P_OUT USB_PHYS(USB_EP_MSC_OUT)

#define CBW_BUF  (USB_DMA_BASE + 0x8000UL)            /* 512 bytes */
#define CSW_BUF  (USB_DMA_BASE + 0x8400UL)            /* 64 bytes */
#define DATA_BUF (USB_DMA_BASE + 0x20000UL)           /* MSC_CHUNK bytes */

enum { S_IDLE, S_CBW, S_DIN, S_DOUT, S_CSW };

static struct {
    struct msc_lun *lun;
    unsigned n;
    int state;
    uint32_t tag, h, done;            /* dCBWTag, dCBWDataTransferLength, bytes moved so far */
    uint32_t chunk;                   /* the transfer in flight */
    uint8_t status;                   /* CSW status: 0 good, 1 failed, 2 phase error */
    uint8_t cdb[16];
    unsigned cur;                     /* LUN of the command */
    /* data-in from a reply (reply_len bytes, then zeros) or from the drive (blocks) */
    uint8_t reply[64];
    uint32_t reply_len;
    int io;                           /* 1: the data phase is block I/O on the drive */
    uint64_t lba;                     /* next block of block I/O */
    uint32_t io_bytes;                /* bytes of block I/O (<= h) */
    int drop;                         /* data-out to receive and drop */
} M;

struct msc_stats msc_stats;

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

static void mcopy(volatile uint8_t *d, const uint8_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        d[i] = s[i];
}

void msc_setup(struct msc_lun *luns, unsigned n)
{
    M.lun = luns;
    M.n = n;
    M.state = S_IDLE;
    usb_msc_max_lun = n ? n - 1 : 0;
    usb_msc_reset = 0;
}

static void sense(struct msc_lun *l, uint8_t key, uint8_t asc, uint8_t ascq)
{
    l->sense_key = key;
    l->asc = asc;
    l->ascq = ascq;
}

static void fail(struct msc_lun *l, uint8_t key, uint8_t asc, uint8_t ascq)
{
    sense(l, key, asc, ascq);
    M.status = 1;
    ++msc_stats.errors;
}

static void reply(const uint8_t *d, uint32_t n)
{
    if (n > sizeof(M.reply))
        n = sizeof(M.reply);
    for (uint32_t i = 0; i < n; ++i)
        M.reply[i] = d[i];
    M.reply_len = n;
}

static void pad_text(uint8_t *d, const char *s, unsigned n)
{
    unsigned i = 0;
    for (; s && s[i] && i < n; ++i)
        d[i] = (uint8_t)s[i];
    for (; i < n; ++i)
        d[i] = ' ';
}

/* The command in M.cdb for LUN l: sets M.status, the reply or the block I/O, the sense. */
static void scsi(struct msc_lun *l, int in)
{
    const uint8_t *c = M.cdb;
    uint8_t r[64];
    for (unsigned i = 0; i < sizeof(r); ++i)
        r[i] = 0;
    M.status = 0;
    M.reply_len = 0;
    M.io = 0;
    M.drop = 0;
    msc_stats.last_op = c[0];
    if (!l->present && c[0] != 0x12 && c[0] != 0x03) {
        fail(l, 0x02, 0x3a, 0x00);                         /* NOT READY: medium not present */
        return;
    }
    switch (c[0]) {
    case 0x00:                                             /* TEST UNIT READY */
        break;
    case 0x03: {                                           /* REQUEST SENSE */
        r[0] = 0x70;
        r[2] = l->sense_key;
        r[7] = 10;
        r[12] = l->asc;
        r[13] = l->ascq;
        reply(r, c[4] < 18 ? c[4] : 18);
        sense(l, 0, 0, 0);
        break;
    }
    case 0x12:                                             /* INQUIRY */
        if (c[1] & 1) {                                    /* EVPD */
            if (c[2] == 0x00) {
                const uint8_t p[] = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x80 };
                reply(p, sizeof(p));
            } else if (c[2] == 0x80) {
                r[1] = 0x80;
                r[3] = 8;
                pad_text(r + 4, l->serial, 8);
                reply(r, 12);
            } else {
                fail(l, 0x05, 0x24, 0x00);                 /* invalid field in CDB */
            }
            break;
        }
        r[0] = 0x00;                                       /* direct access block device */
        r[1] = 0x80;                                       /* removable */
        r[2] = 0x04;                                       /* SPC-2 */
        r[3] = 0x02;
        r[4] = 31;
        pad_text(r + 8, l->vendor, 8);
        pad_text(r + 16, l->product, 16);
        pad_text(r + 32, "0001", 4);
        reply(r, 36);
        break;
    case 0x1a:                                             /* MODE SENSE(6): header only */
        r[0] = 3;
        r[2] = l->ro ? 0x80 : 0x00;
        reply(r, 4);
        break;
    case 0x5a:                                             /* MODE SENSE(10) */
        r[1] = 6;
        r[3] = l->ro ? 0x80 : 0x00;
        reply(r, 8);
        break;
    case 0x1b:                                             /* START STOP UNIT */
    case 0x1e:                                             /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x35:                                             /* SYNCHRONIZE CACHE(10) */
    case 0x91:                                             /* SYNCHRONIZE CACHE(16) */
    case 0x2f:                                             /* VERIFY(10) without data */
        break;
    case 0x25: {                                           /* READ CAPACITY(10) */
        uint64_t last = l->blocks - 1;
        put32(r, last > 0xffffffffULL ? 0xffffffffU : (uint32_t)last);
        put32(r + 4, MSC_BLOCK);
        reply(r, 8);
        break;
    }
    case 0x9e:                                             /* SERVICE ACTION IN(16) */
        if ((c[1] & 0x1f) != 0x10) {                       /* READ CAPACITY(16) only */
            fail(l, 0x05, 0x20, 0x00);
            break;
        }
        put64(r, l->blocks - 1);
        put32(r + 8, MSC_BLOCK);
        reply(r, 32);
        break;
    case 0x23:                                             /* READ FORMAT CAPACITIES */
        r[3] = 8;
        put32(r + 4, l->blocks > 0xffffffffULL ? 0xffffffffU : (uint32_t)l->blocks);
        r[8] = 0x02;                                       /* formatted media */
        r[9] = 0;
        r[10] = (uint8_t)(MSC_BLOCK >> 8);
        r[11] = (uint8_t)MSC_BLOCK;
        reply(r, 12);
        break;
    case 0x28: case 0xa8: case 0x88:                       /* READ(10/12/16) */
    case 0x2a: case 0xaa: case 0x8a: {                     /* WRITE(10/12/16) */
        uint64_t lba;
        uint32_t n;
        if (c[0] == 0x28 || c[0] == 0x2a) {
            lba = be32(c + 2);
            n = be16(c + 7);
        } else if (c[0] == 0xa8 || c[0] == 0xaa) {
            lba = be32(c + 2);
            n = be32(c + 6);
        } else {
            lba = be64(c + 2);
            n = be32(c + 10);
        }
        int wr = c[0] == 0x2a || c[0] == 0xaa || c[0] == 0x8a;
        if (wr && l->ro) {
            fail(l, 0x07, 0x27, 0x00);                     /* DATA PROTECT: write protected */
            M.drop = !in;
            break;
        }
        if (lba > l->blocks || n > l->blocks - lba) {
            fail(l, 0x05, 0x21, 0x00);                     /* LBA out of range */
            M.drop = !in;
            break;
        }
        if (wr == in) {                                    /* direction mismatch (BOT cases 8, 10) */
            M.status = 2;
            M.drop = !in;
            break;
        }
        uint64_t want = (uint64_t)n * MSC_BLOCK;
        if (want != M.h) {
            /* host and command disagree: move what both allow, in whole blocks; the CSW carries the
             * residue. Host expects more (cases 4, 11): good; host expects less (cases 7, 13): phase error */
            uint64_t move = want < M.h ? want : (M.h - M.h % MSC_BLOCK);
            if (move) {
                M.io = 1;
                M.lba = lba;
                M.io_bytes = (uint32_t)move;
            }
            M.status = want < M.h ? 0 : 2;
            break;
        }
        if (n) {
            M.io = 1;
            M.lba = lba;
            M.io_bytes = n * MSC_BLOCK;
        }
        break;
    }
    default:
        fail(l, 0x05, 0x20, 0x00);                         /* ILLEGAL REQUEST: invalid opcode */
        M.drop = !in;
        break;
    }
}

static void send_csw(void)
{
    volatile uint8_t *p = (volatile uint8_t *)CSW_BUF;
    uint8_t b[13];
    b[0] = 'U'; b[1] = 'S'; b[2] = 'B'; b[3] = 'S';
    b[4] = (uint8_t)M.tag; b[5] = (uint8_t)(M.tag >> 8); b[6] = (uint8_t)(M.tag >> 16); b[7] = (uint8_t)(M.tag >> 24);
    /* residue: requested minus processed (padding is not processed data, BOT 6.7) */
    uint32_t real = M.io ? M.io_bytes : (M.reply_len < M.h ? M.reply_len : M.h);
    if (real > M.done)
        real = M.done;
    uint32_t res = M.h > real ? M.h - real : 0;
    b[8] = (uint8_t)res; b[9] = (uint8_t)(res >> 8); b[10] = (uint8_t)(res >> 16); b[11] = (uint8_t)(res >> 24);
    b[12] = M.status;
    mcopy(p, b, 13);
    if (usbd_queue(P_IN, CSW_BUF, 13) == 0)
        M.state = S_CSW;
}

/* The next data-in chunk: block I/O first, then the reply, then zeros up to dCBWDataTransferLength. */
static void next_in(void)
{
    struct msc_lun *l = &M.lun[M.cur];
    uint32_t left = M.h - M.done;
    uint32_t n = left < MSC_CHUNK ? left : MSC_CHUNK;
    volatile uint8_t *d = (volatile uint8_t *)DATA_BUF;
    uint32_t k = 0;
    if (M.io && M.done < M.io_bytes) {
        uint32_t io = M.io_bytes - M.done;
        if (io > n)
            io = n;
        if (l->read(l, M.lba, io / MSC_BLOCK, (uint8_t *)DATA_BUF)) {
            fail(l, 0x03, 0x11, 0x00);                     /* MEDIUM ERROR: unrecovered read error */
            M.io_bytes = M.done;                           /* the rest is padding */
        } else {
            M.lba += io / MSC_BLOCK;
            l->rd_bytes += io;
            k = io;
        }
    }
    if (!M.io) {
        for (; k < n && M.done + k < M.reply_len; ++k)
            d[k] = M.reply[M.done + k];
    }
    for (; k < n; ++k)
        d[k] = 0;
    M.chunk = n;
    if (usbd_queue(P_IN, DATA_BUF, n) == 0)
        M.state = S_DIN;
}

static void next_out(void)
{
    uint32_t left = M.h - M.done;
    uint32_t n = left < MSC_CHUNK ? left : MSC_CHUNK;
    uint32_t mps = usbd_full_speed() ? 64U : 512U;
    uint32_t q = (n + mps - 1) / mps * mps;                /* OUT transfers: whole packets */
    if (q > MSC_CHUNK)
        q = MSC_CHUNK;
    M.chunk = n;
    if (usbd_queue(P_OUT, DATA_BUF, q) == 0)
        M.state = S_DOUT;
}

static void start_data(int in)
{
    M.done = 0;
    if (!M.h) {
        send_csw();
        return;
    }
    if (in)
        next_in();
    else
        next_out();
}

static void on_cbw(uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)CBW_BUF;
    if (len != 31 || p[0] != 'U' || p[1] != 'S' || p[2] != 'B' || p[3] != 'C') {
        ++msc_stats.bad_cbw;
        M.state = S_IDLE;                                  /* not meaningful: wait for the next one */
        return;
    }
    M.tag = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    M.h = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
    int in = (p[12] & 0x80) != 0;
    unsigned lun = p[13] & 0x0f, clen = p[14] & 0x1f;
    for (unsigned i = 0; i < 16; ++i)
        M.cdb[i] = i < clen ? p[15 + i] : 0;
    ++msc_stats.cmds;
    if (lun >= M.n) {
        M.status = 1;
        M.reply_len = 0;
        M.io = 0;
        M.drop = !in;
        start_data(in);
        return;
    }
    M.cur = lun;
    scsi(&M.lun[lun], in);
    start_data(in);
}

static void on_in_done(uint32_t got)
{
    if (M.state == S_CSW) {
        M.state = S_IDLE;
        return;
    }
    if (M.state != S_DIN)
        return;
    M.done += got;
    if (got < M.chunk || M.done >= M.h)
        send_csw();
    else
        next_in();
}

static void on_out_done(uint32_t got)
{
    if (M.state == S_CBW) {
        on_cbw(got);
        return;
    }
    if (M.state != S_DOUT)
        return;
    struct msc_lun *l = &M.lun[M.cur];
    uint32_t use = got < M.chunk ? got : M.chunk;
    if (M.io && !M.drop && M.done < M.io_bytes) {
        uint32_t io = M.io_bytes - M.done;
        if (io > use)
            io = use;
        io -= io % MSC_BLOCK;
        if (io && l->write(l, M.lba, io / MSC_BLOCK, (const uint8_t *)DATA_BUF)) {
            fail(l, 0x03, 0x0c, 0x00);                     /* MEDIUM ERROR: write error */
            M.io_bytes = M.done;
        } else {
            M.lba += io / MSC_BLOCK;
            l->wr_bytes += io;
        }
    }
    M.done += use;
    if (got < M.chunk || M.done >= M.h)
        send_csw();
    else
        next_out();
}

void msc_event(const struct usbd_ev *e)
{
    if (e->type == USBD_EV_RESET || e->type == USBD_EV_DISCONNECT ||
        (e->type == USBD_EV_CONFIGURED && e->value != 1)) {
        M.state = S_IDLE;
        return;
    }
    if (e->type == USBD_EV_CONFIGURED) {
        M.state = S_IDLE;
        return;
    }
    if (e->type != USBD_EV_DONE)
        return;
    if (e->ep == P_OUT)
        on_out_done(e->value);
    else if (e->ep == P_IN)
        on_in_done(e->value);
}

void msc_service(void)
{
    if (usb_msc_reset) {                                   /* Bulk-Only Mass Storage Reset */
        usb_msc_reset = 0;
        ++msc_stats.resets;
        if (M.state != S_CBW)
            M.state = S_IDLE;
    }
    if (!usbd_configured() || !M.n)
        return;
    if (M.state == S_IDLE && !usbd_busy(P_OUT)) {
        if (usbd_queue(P_OUT, CBW_BUF, 512) == 0)
            M.state = S_CBW;
    }
}

int msc_busy(void)
{
    return M.state == S_DIN || M.state == S_DOUT || M.state == S_CSW;
}
