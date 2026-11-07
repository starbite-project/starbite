/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS UO3: the touchscreen as a trackpad + a macro pad + media keys, over USB HID (hid.c).
 * Pad: drag moves the pointer, a tap clicks, holding still 0.6 s right-clicks; Volume scrolls; Bixby leaves.
 * Buttons send Mac shortcuts (Command = HID GUI) and media keys. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "hid.h"
#include "ut.h"
#include "usb_dwc3.h"

#define PAD_Y0 (UI_VIEW_Y0 + 20)
#define PAD_Y1 1860
#define BTN_Y0 1930
#define BTN_H  290
#define BTN_GAP 30

struct mbtn { const char *label; uint8_t mod, key; uint16_t media; };
static const struct mbtn btns[12] = {
    { "Copy", HID_GUI, 0x06, 0 },      { "Paste", HID_GUI, 0x19, 0 },     { "Undo", HID_GUI, 0x1d, 0 },
    { "Cmd Tab", HID_GUI, 0x2b, 0 },   { "Space", 0, 0x2c, 0 },           { "Return", 0, 0x28, 0 },
    { "Esc", 0, 0x29, 0 },             { "Mission", HID_CTRL, 0x52, 0 },  { "Vol -", 0, 0, HID_VOL_DOWN },
    { "Play", 0, 0, HID_PLAY },        { "Vol +", 0, 0, HID_VOL_UP },     { "Mute", 0, 0, HID_MUTE },
};

static void btn_rect(int i, int *x, int *y, int *w, int *h)
{
    *w = (UI_W - 2 * 50 - 3 * BTN_GAP) / 4;
    *h = BTN_H - BTN_GAP;
    *x = 50 + (i % 4) * (*w + BTN_GAP);
    *y = BTN_Y0 + (i / 4) * BTN_H;
}

static void draw_btn(int i, int lit)
{
    int x, y, w, h;
    btn_rect(i, &x, &y, &w, &h);
    ui_round(x, y, w, h, 40, lit ? C_STAR : btns[i].media ? C_CARD3 : C_CARD2);
    int tw = (int)ui_text_fit(F_BTN, btns[i].label, w);
    (void)tw;
    ui_text_cut(F_BTN, x + 40, y + h / 2 + 20, btns[i].label, w - 60, lit ? C_BG : C_L1);
    ui_dirty(x, y, w, h);
}

static void draw_pad(int touching)
{
    ui_round(50, PAD_Y0, UI_W - 100, PAD_Y1 - PAD_Y0, 56, touching ? C_CARD2 : C_CARD);
    ui_text(F_DETAIL, 110, PAD_Y0 + 100, "Trackpad", C_L2);
    ui_text(F_SMALL, 110, PAD_Y1 - 150, "drag: move   tap: click   hold: right click", C_L3);
    ui_text(F_SMALL, 110, PAD_Y1 - 80, "Volume: scroll   Bixby: back", C_L3);
    ui_dirty(50, PAD_Y0, UI_W - 100, PAD_Y1 - PAD_Y0);
}

static void draw_all(void)
{
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_header_draw("Trackpad and keys", "USB", usbd_configured() ? "CONNECTED" : "NO HOST",
                   usbd_configured() ? C_GREEN : C_ORANGE);
    ut_status_bar();
    draw_pad(0);
    for (int i = 0; i < 12; ++i)
        draw_btn(i, 0);
    ui_dirty_all();
}

static int hit_btn(int x, int y)
{
    for (int i = 0; i < 12; ++i) {
        int bx, by, bw, bh;
        btn_rect(i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
            return i;
    }
    return -1;
}

static void press(int i)
{
    if (btns[i].media)
        hid_media(btns[i].media);
    else
        hid_key(btns[i].mod, btns[i].key);
    draw_btn(i, 1);
    ui_present();
    sb_mdelay(60);
    draw_btn(i, 0);
}

void ut_pad(void)
{
    hid_reset();
    ui_touch_raw(1);
    ui_input_reset();
    draw_all();
    ui_present();
    int touching = 0, in_pad = 0, moved = 0, held = 0, lx = 0, ly = 0;
    uint64_t t_down = 0, next = sb_ms() + 1000;
    int conf = usbd_configured();
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_HOME || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        if (e.type == EV_PRESS) {
            touching = 1;
            in_pad = e.y >= PAD_Y0 && e.y < PAD_Y1;
            moved = held = 0;
            lx = e.x;
            ly = e.y;
            t_down = sb_ms();
            if (in_pad)
                draw_pad(1);
        } else if (e.type == EV_DRAG && touching && in_pad) {
            int dx = e.x - lx, dy = e.y - ly;
            lx = e.x;
            ly = e.y;
            moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            hid_move(dx * 3 / 2, dy * 3 / 2);         /* 570 ppi panel: 1.5 Mac points per pixel */
        } else if (e.type == EV_TAP) {
            if (in_pad && !held && moved < 30)
                hid_click(HID_LEFT);
            else if (!in_pad) {
                int b = hit_btn(e.x, e.y);
                if (b >= 0)
                    press(b);
            }
        } else if (e.type == EV_TOUCH_UP) {
            if (touching && in_pad)
                draw_pad(0);
            touching = in_pad = 0;
        } else if (e.type == EV_UP) {
            hid_wheel(3);
        } else if (e.type == EV_DOWN) {
            hid_wheel(-3);
        } else if (e.type == EV_SELECT) {
            hid_click(HID_LEFT);
        }
        if (touching && in_pad && !held && moved < 30 && sb_ms() - t_down > 600) {
            hid_click(HID_RIGHT);                     /* held still: right click */
            held = 1;
        }
        if (sb_ms() >= next || usbd_configured() != conf) {
            conf = usbd_configured();
            ui_header_draw("Trackpad and keys", "USB", conf ? "CONNECTED" : "NO HOST", conf ? C_GREEN : C_ORANGE);
            ut_status_bar();
            next = sb_ms() + 1000;
        }
        ui_present();
    }
    ui_touch_raw(0);
    ui_input_reset();
}
