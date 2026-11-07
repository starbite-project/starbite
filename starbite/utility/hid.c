/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS USB HID (UO3): keyboard (report 1), mouse (report 2), media keys (report 3) on interrupt
 * endpoint 0x84 (usb_desc.c interface 3). A small queue of reports; pointer motion is accumulated and sent
 * in steps of at most 127 so nothing is lost while the endpoint is busy. */
#include "hid.h"
#include "usb_desc.h"
#include "usb_dwc3.h"

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

void hid_text(const char *s)
{
    for (; s && *s; ++s) {
        char c = *s;
        uint8_t mod = 0, k = 0;
        if (c >= 'a' && c <= 'z') {
            k = (uint8_t)(0x04 + c - 'a');
        } else if (c >= 'A' && c <= 'Z') {
            k = (uint8_t)(0x04 + c - 'A');
            mod = HID_SHIFT;
        } else if (c >= '1' && c <= '9') {
            k = (uint8_t)(0x1e + c - '1');
        } else if (c == '0') {
            k = 0x27;
        } else if (c == ' ') {
            k = 0x2c;
        } else if (c == '\n') {
            k = 0x28;
        } else if (c == '.') {
            k = 0x37;
        } else if (c == ',') {
            k = 0x36;
        } else if (c == '-') {
            k = 0x2d;
        } else if (c == '/') {
            k = 0x38;
        }
        if (k)
            hid_key(mod, k);
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
    if (!usbd_configured() || usbd_busy(P_HID))
        return;
    if (qh == qt && (mdx || mdy || mwheel)) {         /* motion only when no click/key is waiting */
        int dx = clamp(mdx), dy = clamp(mdy), w = clamp(mwheel);
        mdx -= dx;
        mdy -= dy;
        mwheel -= w;
        mouse_report(buttons, dx, dy, w);
    }
    if (qh == qt)
        return;
    volatile uint8_t *b = (volatile uint8_t *)HID_BUF;
    for (unsigned i = 0; i < q[qh].n; ++i)
        b[i] = q[qh].b[i];
    if (usbd_queue(P_HID, HID_BUF, q[qh].n) == 0) {
        qh = (qh + 1) % QN;
        ++hid_stats.reports;
    }
}

void hid_reset(void)
{
    qh = qt = 0;
    mdx = mdy = mwheel = 0;
    buttons = 0;
}
