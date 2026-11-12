/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the USB descriptors and chapter 9 / CDC-ACM control requests for the shared polling DWC3 driver
 * (starbite/utility/usb_dwc3.c calls usb_ch9_init / usb_ch9_setup / usb_ch9_out / usb_desc_eps / usb_ep_mps /
 * usb_ep_interval, declared in starbite/utility/usb_desc.h). Trimmed from the Utility OS's usb_desc.c (same
 * project, GPL-2.0-only): the same CDC-ACM function (IAD, PSTN 1.2, protocol 0 so modem managers leave the port
 * alone), and nothing else: no mass storage interface, no HID, no vendor interface. Any class request for another
 * interface is stalled. Built by code, not from pointer tables (the image keeps only relative relocations).
 * Pure code: kern/host/usb-test.c checks it. */
#include "usb_desc.h"
#include "usbacm.h"

struct usbacm_seen usbacm_seen;

void usbacm_seen_reset(void)
{
    struct usbacm_seen z = { 0 };
    usbacm_seen = z;
}

#define DT_DEVICE 1U
#define DT_CONFIG 2U
#define DT_STRING 3U
#define DT_INTERFACE 4U
#define DT_ENDPOINT 5U
#define DT_QUALIFIER 6U
#define DT_OTHER 7U
#define DT_IAD 11U
#define DT_CS_IF 0x24U

struct w {
    uint8_t *o;
    unsigned n;
};

static void b1(struct w *w, unsigned v) { w->o[w->n++] = (uint8_t)v; }
static void b2(struct w *w, unsigned v)
{
    b1(w, v & 0xffU);
    b1(w, (v >> 8) & 0xffU);
}

unsigned usb_ep_mps(unsigned addr, int fs)
{
    if (addr == USBACM_EP_NOTIFY)
        return 16U;
    return fs ? 64U : 512U;
}

unsigned usb_ep_interval(unsigned addr, int fs)
{
    if (addr == USBACM_EP_NOTIFY)
        return fs ? 32U : 9U;               /* 32 ms either way */
    return 0U;
}

unsigned usb_desc_eps(const uint8_t **list)
{
    static const uint8_t eps[3] = { USBACM_EP_NOTIFY, USBACM_EP_IN, USBACM_EP_OUT };
    *list = eps;
    return 3;
}

unsigned usb_desc_device(uint8_t *out)
{
    struct w w = { out, 0 };
    b1(&w, 18);
    b1(&w, DT_DEVICE);
    b2(&w, 0x0200);
    b1(&w, 0xef);                           /* miscellaneous / common / IAD */
    b1(&w, 0x02);
    b1(&w, 0x01);
    b1(&w, USB_EP0_MPS);
    b2(&w, USBACM_VID);
    b2(&w, USBACM_PID);
    b2(&w, USBACM_BCD);
    b1(&w, 1);                              /* manufacturer, product, serial */
    b1(&w, 2);
    b1(&w, 3);
    b1(&w, 1);                              /* one configuration */
    return w.n;
}

unsigned usb_desc_qualifier(uint8_t *out)
{
    struct w w = { out, 0 };
    b1(&w, 10);
    b1(&w, DT_QUALIFIER);
    b2(&w, 0x0200);
    b1(&w, 0xef);
    b1(&w, 0x02);
    b1(&w, 0x01);
    b1(&w, USB_EP0_MPS);
    b1(&w, 1);
    b1(&w, 0);
    return w.n;
}

static void ep(struct w *w, unsigned addr, unsigned attr, int fs)
{
    b1(w, 7);
    b1(w, DT_ENDPOINT);
    b1(w, addr);
    b1(w, attr);
    b2(w, usb_ep_mps(addr, fs));
    b1(w, usb_ep_interval(addr, fs));
}

unsigned usb_desc_config(uint8_t *out, int fs, uint8_t type)
{
    struct w w = { out, 0 };
    b1(&w, 9);
    b1(&w, type);
    b2(&w, USBACM_CONFIG_LEN);
    b1(&w, USBACM_IF_COUNT);
    b1(&w, 1);                              /* bConfigurationValue */
    b1(&w, 0);
    b1(&w, 0xc0);                           /* self-powered (the battery) */
    b1(&w, 50);                             /* 100 mA */
    b1(&w, 8);                              /* IAD: interfaces 0-1, CDC ACM */
    b1(&w, DT_IAD);
    b1(&w, USBACM_IF_CTRL);
    b1(&w, 2);
    b1(&w, 0x02);
    b1(&w, 0x02);
    b1(&w, 0x00);
    b1(&w, 4);
    b1(&w, 9);                              /* the control interface */
    b1(&w, DT_INTERFACE);
    b1(&w, USBACM_IF_CTRL);
    b1(&w, 0);
    b1(&w, 1);
    b1(&w, 0x02);
    b1(&w, 0x02);
    b1(&w, 0x00);
    b1(&w, 4);
    b1(&w, 5);                              /* header, CDC 1.10 */
    b1(&w, DT_CS_IF);
    b1(&w, 0x00);
    b2(&w, 0x0110);
    b1(&w, 5);                              /* call management: none, data interface 1 */
    b1(&w, DT_CS_IF);
    b1(&w, 0x01);
    b1(&w, 0x00);
    b1(&w, USBACM_IF_DATA);
    b1(&w, 4);                              /* ACM: line coding and line state */
    b1(&w, DT_CS_IF);
    b1(&w, 0x02);
    b1(&w, 0x02);
    b1(&w, 5);                              /* union: control 0, data 1 */
    b1(&w, DT_CS_IF);
    b1(&w, 0x06);
    b1(&w, USBACM_IF_CTRL);
    b1(&w, USBACM_IF_DATA);
    ep(&w, USBACM_EP_NOTIFY, 0x03, fs);
    b1(&w, 9);                              /* the data interface */
    b1(&w, DT_INTERFACE);
    b1(&w, USBACM_IF_DATA);
    b1(&w, 0);
    b1(&w, 2);
    b1(&w, 0x0a);
    b1(&w, 0x00);
    b1(&w, 0x00);
    b1(&w, 0);
    ep(&w, USBACM_EP_IN, 0x02, fs);
    ep(&w, USBACM_EP_OUT, 0x02, fs);
    return w.n;
}

unsigned usb_desc_string(uint8_t *out, unsigned i, const char *serial)
{
    const char *s;
    if (i == 0) {
        out[0] = 4;
        out[1] = DT_STRING;
        out[2] = 0x09;                      /* English (US) */
        out[3] = 0x04;
        return 4;
    }
    if (i == 1)
        s = "Project Starbite";
    else if (i == 2)
        s = "Starbite OS (Galaxy S9)";
    else if (i == 3)
        s = serial && serial[0] ? serial : USBACM_SERIAL;
    else if (i == 4)
        s = "Starbite OS console (read only)";
    else
        return 0;
    unsigned n = 0;
    for (; s[n] && n < 63; ++n) {
        out[2 + 2 * n] = (uint8_t)s[n];
        out[3 + 2 * n] = 0;
    }
    out[0] = (uint8_t)(2 + 2 * n);
    out[1] = DT_STRING;
    return out[0];
}

void usb_ch9_init(struct usb_dev_state *s, const char *serial)
{
    unsigned i = 0;
    s->full_speed = 0;
    s->config = 0;
    s->line_state = 0;
    s->line_coding[0] = 0x00;               /* 115200 8N1 until the host says otherwise */
    s->line_coding[1] = 0xc2;
    s->line_coding[2] = 0x01;
    s->line_coding[3] = 0x00;
    s->line_coding[4] = 0;
    s->line_coding[5] = 0;
    s->line_coding[6] = 8;
    for (; serial && serial[i] && i < sizeof(s->serial) - 1; ++i)
        s->serial[i] = serial[i];
    s->serial[i] = 0;
}

static void reply(struct ch9_res *r, const uint8_t *d, unsigned n)
{
    for (unsigned i = 0; i < n && i < sizeof(r->data); ++i)
        r->data[i] = d[i];
    r->len = n;
    r->action = CH9_IN;
}

void usb_ch9_setup(struct usb_dev_state *s, const uint8_t st[8], struct ch9_res *r)
{
    unsigned type = st[0], req = st[1];
    unsigned value = (unsigned)st[2] | (unsigned)st[3] << 8;
    unsigned index = (unsigned)st[4] | (unsigned)st[5] << 8;
    unsigned length = (unsigned)st[6] | (unsigned)st[7] << 8;
    unsigned rcpt = type & 0x1fU;
    r->action = CH9_STALL;
    r->len = 0;
    r->set_address = -1;
    r->set_config = -1;
    r->clear_halt = -1;
    r->line_changed = 0;
    ++usbacm_seen.setups;

    if ((type & 0x60U) == 0x00U) {          /* standard */
        switch (req) {
        case 0:                             /* GET_STATUS */
            if (!(type & 0x80U) || length < 2 || rcpt > 2)
                break;
            r->data[0] = rcpt == 0 ? 1 : 0; /* device: self-powered */
            r->data[1] = 0;
            r->len = 2;
            r->action = CH9_IN;
            break;
        case 1:                             /* CLEAR_FEATURE */
        case 3:                             /* SET_FEATURE */
            if (rcpt == 2 && value == 0 && req == 1) {
                r->clear_halt = (int)(index & 0x8fU);
                r->action = CH9_ACK;
            } else if (rcpt == 0 && value == 1) {
                r->action = CH9_ACK;        /* DEVICE_REMOTE_WAKEUP: nothing to do */
            }
            break;
        case 5:                             /* SET_ADDRESS */
            if (rcpt == 0 && value <= 127 && !length) {
                r->set_address = (int)value;
                r->action = CH9_ACK;
                ++usbacm_seen.set_address;
                usbacm_seen.address = value;
            }
            break;
        case 6: {                           /* GET_DESCRIPTOR */
            unsigned dt = value >> 8, di = value & 0xffU, n = 0;
            if (!(type & 0x80U))
                break;
            ++usbacm_seen.get_desc;
            if (dt == DT_DEVICE)
                n = usb_desc_device(r->data);
            else if (dt == DT_CONFIG || dt == DT_OTHER)
                n = usb_desc_config(r->data, dt == DT_OTHER ? !s->full_speed : s->full_speed, (uint8_t)dt);
            else if (dt == DT_QUALIFIER)
                n = usb_desc_qualifier(r->data);
            else if (dt == DT_STRING)
                n = usb_desc_string(r->data, di, s->serial);
            if (!n)
                break;                      /* BOS (bcdUSB 2.00: none), Microsoft OS strings and the rest */
            r->len = n;
            r->action = CH9_IN;
            break;
        }
        case 8:                             /* GET_CONFIGURATION */
            if (!(type & 0x80U))
                break;
            r->data[0] = s->config;
            r->len = 1;
            r->action = CH9_IN;
            break;
        case 9:                             /* SET_CONFIGURATION */
            if (rcpt == 0 && value <= 1) {
                r->set_config = (int)value;
                s->config = (uint8_t)value;
                r->action = CH9_ACK;
                ++usbacm_seen.set_config;
                usbacm_seen.config = value;
            }
            break;
        case 10:                            /* GET_INTERFACE */
            if ((type & 0x80U) && rcpt == 1 && index < USBACM_IF_COUNT && s->config) {
                r->data[0] = 0;
                r->len = 1;
                r->action = CH9_IN;
            }
            break;
        case 11:                            /* SET_INTERFACE: alternate 0 only */
            if (rcpt == 1 && index < USBACM_IF_COUNT && value == 0 && s->config)
                r->action = CH9_ACK;
            break;
        default:
            break;
        }
    } else if ((type & 0x60U) == 0x20U && rcpt == 1 && (index & 0xffU) == USBACM_IF_CTRL) {
        switch (req) {                      /* CDC PSTN class requests */
        case 0x20:                          /* SET_LINE_CODING */
            if (!(type & 0x80U) && length == 7) {
                r->len = 7;
                r->action = CH9_OUT;
            }
            break;
        case 0x21:                          /* GET_LINE_CODING */
            if (type & 0x80U)
                reply(r, s->line_coding, 7);
            break;
        case 0x22:                          /* SET_CONTROL_LINE_STATE */
            if (!(type & 0x80U)) {
                s->line_state = (uint16_t)value;
                r->line_changed = 1;
                r->action = CH9_ACK;
                ++usbacm_seen.line_sets;
                usbacm_seen.line_state = value;
            }
            break;
        case 0x23:                          /* SEND_BREAK */
            if (!(type & 0x80U))
                r->action = CH9_ACK;
            break;
        default:
            break;
        }
    }
    if (r->action == CH9_IN) {
        if (r->len > length)
            r->len = length;
        if (!r->len)
            r->action = CH9_ACK;            /* wLength 0: no data stage */
    }
    if (r->action == CH9_ACK && length && !(type & 0x80U))
        r->action = CH9_STALL;              /* the host would send data we did not ask for */
    if (r->action == CH9_STALL)
        ++usbacm_seen.stalls;
}

void usb_ch9_out(struct usb_dev_state *s, const uint8_t st[8], const uint8_t *d, unsigned len)
{
    if (st[0] == 0x21 && st[1] == 0x20 && len >= 7)
        for (unsigned i = 0; i < 7; ++i)
            s->line_coding[i] = d[i];
}
