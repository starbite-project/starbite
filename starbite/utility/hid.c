/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS USB HID (UO3): keyboard (report 1), mouse (report 2), media keys (report 3) on interrupt
 * endpoint 0x84 (usb_desc.c interface 3). A small queue of reports; pointer motion is accumulated and sent
 * in steps of at most 127 so nothing is lost while the endpoint is busy. */
#include "hid.h"
#include "usb_desc.h"
#include "usb_dwc3.h"
#include "kbdmap.h"

#define P_HID   USB_PHYS(USB_EP_HID_IN)
#define HID_BUF (USB_DMA_BASE + 0xb000UL)            /* 16 bytes, in the non-cacheable window */
#define QN 64U

static struct { uint8_t b[9]; uint8_t n; } q[QN];
static unsigned qh, qt;
static int mdx, mdy, mwheel;
static uint8_t buttons;
struct hid_stats hid_stats;

static void push(const uint8_t *b, unsigned n)
{
    unsigned nt = (qt + 1) % QN;
    if (nt == qh) {                                   /* full: drop (a key press is never half sent: */
        ++hid_stats.dropped;                          /* press and release are pushed together) */
        return;
    }
    for (unsigned i = 0; i < n; ++i)
        q[qt].b[i] = b[i];
    q[qt].n = (uint8_t)n;
    qt = nt;
}

static unsigned room(void) { return (qh + QN - qt - 1) % QN; }
unsigned hid_room(void) { return room(); }

void hid_key(uint8_t mod, uint8_t key)
{
    if (room() < 2) {
        ++hid_stats.dropped;
        return;
    }
    const uint8_t down[9] = { 1, mod, 0, key, 0, 0, 0, 0, 0 };
    const uint8_t up[9] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    push(down, 9);
    push(up, 9);
    ++hid_stats.keys;
}

/* Text waits here and goes into the report queue as room frees up (hid_service), so a macro longer than
 * the queue is typed whole; 4 slots stay free for clicks and keys pressed meanwhile. UTF-8, typed as the
 * key presses that give each character on the computer's layout (kbdmap.c); characters it has none for
 * are skipped. */
int hid_layout = KL_FI_MAC;
static char pend[160];                                /* 0-terminated: a cut UTF-8 sequence stops at the 0 */
static unsigned pend_h, pend_t;

static void text_feed(void)
{
    while (pend_h < pend_t && room() >= 8) {
        uint32_t cp;
        pend_h += kbd_utf8(pend + pend_h, &cp);
        struct kstroke k[2];
        int n = kbd_char(hid_layout, cp, k);
        for (int i = 0; i < n; ++i)
            hid_key(k[i].mod, k[i].key);
    }
    if (pend_h >= pend_t)
        pend_h = pend_t = 0;
}

void hid_text(const char *s)
{
    if (pend_h) {                                     /* move what is still waiting to the front */
        for (unsigned i = pend_h; i <= pend_t; ++i)
            pend[i - pend_h] = pend[i];
        pend_t -= pend_h;
        pend_h = 0;
    }
    for (; s && *s; ++s) {
        if (pend_t + 1 >= sizeof(pend)) {
            ++hid_stats.dropped;                      /* more than 159 bytes waiting */
            break;
        }
        pend[pend_t++] = *s;
    }
    pend[pend_t] = 0;
    text_feed();
}

int hid_text_pending(void) { return pend_t > pend_h; }

/* The gamepad (report 4) is a state: the newest one is sent when the endpoint is free, before the queue. */
static uint8_t pad_r[8] = { 4, 0, 0, 8, 0, 0, 0, 0 };
static int pad_dirty, pad_last;                       /* pad_last: the last report sent was the gamepad's */

void hid_gamepad(const struct hid_pad *p)
{
    const uint8_t r[8] = { 4, (uint8_t)p->buttons, (uint8_t)(p->buttons >> 8), (uint8_t)(p->hat > 8 ? 8 : p->hat),
                           (uint8_t)p->x, (uint8_t)p->y, (uint8_t)p->z, (uint8_t)p->rz };
    for (unsigned i = 0; i < 8; ++i) {
        if (pad_r[i] != r[i])
            pad_dirty = 1;
        pad_r[i] = r[i];
    }
}

void hid_move(int dx, int dy)
{
    mdx += dx;
    mdy += dy;
}

void hid_wheel(int n)
{
    mwheel += n;
}

static void mouse_report(uint8_t btn, int dx, int dy, int wheel)
{
    const uint8_t r[5] = { 2, btn, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)(int8_t)wheel };
    push(r, 5);
}

void hid_click(uint8_t btn)
{
    if (room() < 2) {
        ++hid_stats.dropped;
        return;
    }
    mouse_report((uint8_t)(buttons | btn), 0, 0, 0);
    mouse_report(buttons, 0, 0, 0);
    ++hid_stats.clicks;
}

void hid_buttons(uint8_t b)
{
    buttons = b;
    mouse_report(buttons, 0, 0, 0);
}

void hid_media(uint16_t usage)
{
    if (room() < 2) {
        ++hid_stats.dropped;
        return;
    }
    const uint8_t d[3] = { 3, (uint8_t)usage, (uint8_t)(usage >> 8) };
    const uint8_t u[3] = { 3, 0, 0 };
    push(d, 3);
    push(u, 3);
}

static int clamp(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

void hid_service(void)
{
    text_feed();
    if (!usbd_configured() || usbd_busy(P_HID))
        return;
    volatile uint8_t *b = (volatile uint8_t *)HID_BUF;
    if (pad_dirty && (qh == qt || !pad_last)) {       /* the gamepad and the queue take turns */
        for (unsigned i = 0; i < 8; ++i)
            b[i] = pad_r[i];
        if (usbd_queue(P_HID, HID_BUF, 8) == 0) {
            pad_dirty = 0;
            pad_last = 1;
            ++hid_stats.reports;
        }
        return;
    }
    pad_last = 0;
    if (qh == qt && (mdx || mdy || mwheel)) {         /* motion only when no click/key is waiting */
        int dx = clamp(mdx), dy = clamp(mdy), w = clamp(mwheel);
        mdx -= dx;
        mdy -= dy;
        mwheel -= w;
        mouse_report(buttons, dx, dy, w);
    }
    if (qh == qt)
        return;
    for (unsigned i = 0; i < q[qh].n; ++i)
        b[i] = q[qh].b[i];
    if (usbd_queue(P_HID, HID_BUF, q[qh].n) == 0) {
        qh = (qh + 1) % QN;
        ++hid_stats.reports;
    }
}

/* Everything released: what was waiting is dropped, and if a press or a held button could still be down on
 * the computer (reports were waiting, a drag held a button, the gamepad was not idle), all-released reports
 * go out next, so leaving a screen in the middle of a macro or a drag leaves no key held. */
void hid_reset(void)
{
    const uint8_t idle[8] = { 4, 0, 0, 8, 0, 0, 0, 0 };
    int pad_busy = 0;
    for (unsigned i = 0; i < 8; ++i)
        pad_busy |= pad_r[i] != idle[i];
    int held = qh != qt || buttons || pad_busy;
    qh = qt = 0;
    pend_h = pend_t = 0;
    pend[0] = 0;
    for (unsigned i = 0; i < 8; ++i)
        pad_r[i] = idle[i];
    pad_dirty = pad_busy;
    pad_last = 0;
    mdx = mdy = mwheel = 0;
    buttons = 0;
    if (held) {
        const uint8_t k[9] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 }, m[5] = { 2, 0, 0, 0, 0 }, c[3] = { 3, 0, 0 };
        push(k, 9);
        push(m, 5);
        push(c, 3);
    }
}
