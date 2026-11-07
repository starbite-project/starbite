/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS Remote: the phone as the computer's keyboard, trackpad, gamepad, media remote and presenter,
 * all over the one USB connection (hid.c: keyboard, mouse, media keys, gamepad reports). This file: the
 * Remote page (the screens and the computer's keyboard layout), the media remote and the presenter. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "hid.h"
#include "ut.h"
#include "usb_dwc3.h"
#include "kbdmap.h"

/* ---- big-button screens -------------------------------------------------------------------------------- */
struct bb { const char *label; uint8_t mod, key; uint16_t media; int x, y, w, h; uint32_t c; };

static void bb_draw(const struct bb *b, int lit)
{
    ui_fill(b->x - 4, b->y - 4, b->w + 8, b->h + 8, C_BG);
    ui_round(b->x, b->y, b->w, b->h, 48, lit ? C_L1 : b->c);
    ui_text_c(ui_text_w(F_TITLE_S, b->label) < b->w - 40 ? F_TITLE_S : F_BTN, b->x + b->w / 2,
              b->y + b->h / 2 + 26, b->label, lit ? C_BG : C_L1);
    ui_dirty(b->x - 4, b->y - 4, b->w + 8, b->h + 8);
}

static void bb_send(const struct bb *b)
{
    if (b->media)
        hid_media(b->media);
    else if (b->key)
        hid_key(b->mod, b->key);
}

static void bb_flash(const struct bb *b)
{
    bb_send(b);
    bb_draw(b, 1);
    ui_present();
    sb_mdelay(70);
    bb_draw(b, 0);
}

static int bb_hit(const struct bb *b, int n, int x, int y)
{
    for (int i = 0; i < n; ++i)
        if (x >= b[i].x && x < b[i].x + b[i].w && y >= b[i].y && y < b[i].y + b[i].h)
            return i;
    return -1;
}

static void head(const char *title)
{
    int c = usbd_configured();
    ui_header_draw(title, "Remote", c ? "CONNECTED" : "NO HOST", c ? C_GREEN : C_ORANGE);
    ut_status_bar();
}

/* ---- media remote ------------------------------------------------------------------------------------- */
static void media(void)
{
    int pc = hid_layout == KL_FI_PC;
    struct bb b[12] = {
        { "Prev", 0, 0, HID_PREV, 0, 0, 0, 0, C_CARD2 }, { "Play", 0, 0, HID_PLAY, 0, 0, 0, 0, C_BLUE },
        { "Next", 0, 0, HID_NEXT, 0, 0, 0, 0, C_CARD2 }, { "Vol -", 0, 0, HID_VOL_DOWN, 0, 0, 0, 0, C_CARD2 },
        { "Mute", 0, 0, HID_MUTE, 0, 0, 0, 0, C_CARD3 }, { "Vol +", 0, 0, HID_VOL_UP, 0, 0, 0, 0, C_CARD2 },
        { "Dim", 0, 0, HID_BRIGHT_DOWN, 0, 0, 0, 0, C_CARD2 }, { "Stop", 0, 0, HID_STOP, 0, 0, 0, 0, C_CARD3 },
        { "Bright", 0, 0, HID_BRIGHT_UP, 0, 0, 0, 0, C_CARD2 },
        { "Lock", pc ? HID_GUI : (HID_CTRL | HID_GUI), pc ? 0x0f : 0x14, 0, 0, 0, 0, 0, C_CARD3 },
        { "Space", 0, 0x2c, 0, 0, 0, 0, 0, C_CARD2 }, { "Esc", 0, 0x29, 0, 0, 0, 0, 0, C_CARD3 },
    };
    const int y0 = UI_VIEW_Y0 + 40, bh = 520, gap = 40, bw = (UI_W - 2 * 60 - 2 * gap) / 3;
    for (int i = 0; i < 12; ++i) {
        b[i].x = 60 + (i % 3) * (bw + gap);
        b[i].y = y0 + (i / 3) * (bh + gap);
        b[i].w = bw;
        b[i].h = bh;
    }
    hid_reset();
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    head("Media remote");
    for (int i = 0; i < 12; ++i)
        bb_draw(&b[i], 0);
    ui_text(F_SMALL, 60, UI_H - 90, "Volume keys: volume   Power: play / pause   Bixby: back", C_L3);
    ui_dirty_all();
    ui_present();
    uint64_t next = sb_ms() + 1000;
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_HOME || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        if (e.type == EV_UP)
            bb_flash(&b[5]);
        else if (e.type == EV_DOWN)
            bb_flash(&b[3]);
        else if (e.type == EV_SELECT)
            bb_flash(&b[1]);
        else if (e.type == EV_TAP) {
            int i = bb_hit(b, 12, e.x, e.y);
            if (i >= 0)
                bb_flash(&b[i]);
        }
        if (sb_ms() >= next) {
            head("Media remote");
            next = sb_ms() + 1000;
        }
        ui_present();
    }
    hid_reset();
}

/* ---- presenter: next / back as arrow keys, black screen, the talk's time ---------------------------------- */
static void presenter(void)
{
    struct bb b[4] = {
        { "Back", 0, 0x50, 0, 60, UI_VIEW_Y0 + 400, 640, 420, C_CARD2 },
        { "Black", 0, 0x05, 0, 740, UI_VIEW_Y0 + 400, 640, 420, C_CARD3 },
        { "Next", 0, 0x4f, 0, 60, UI_VIEW_Y0 + 860, UI_W - 120, 1240, C_BLUE },
        { "End show", 0, 0x29, 0, 60, UI_VIEW_Y0 + 2140, UI_W - 120, 280, C_CARD3 },
    };
    hid_reset();
    ut_awake = 1;                                     /* the talk's clock stays on screen */
    uint64_t t0 = sb_ms(), next = 0;
    unsigned slides = 0;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (int i = 0; i < 4; ++i)
        bb_draw(&b[i], 0);
    ui_dirty_all();
    for (;;) {
        uint64_t now = sb_ms();
        if (now >= next) {
            head("Presenter");
            struct ub u;
            ub_init(&u);
            uint64_t s = (now - t0) / 1000;
            ub_dec(&u, s / 60);
            ub_c(&u, ':');
            ub_dec2(&u, (unsigned)(s % 60));
            ui_fill(0, UI_VIEW_Y0, UI_W, 380, C_BG);
            ui_text(F_HEAD, 70, UI_VIEW_Y0 + 70, "TALK TIME (TAP TO RESTART)", C_L2);
            ui_text(F_BIG, 70, UI_VIEW_Y0 + 230, u.b, C_L1);
            ub_init(&u);
            ub_dec(&u, slides);
            ub_s(&u, slides == 1 ? " slide forward" : " slides forward");
            ui_text_r(F_DETAIL, UI_W - 70, UI_VIEW_Y0 + 230, u.b, C_L2);
            ui_text(F_SMALL, 70, UI_VIEW_Y0 + 330, "Volume Up: next   Volume Down: back   Bixby: leave", C_L3);
            ui_dirty(0, UI_VIEW_Y0, UI_W, 380);
            ui_present();
            next = now + 500;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_HOME || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        int i = -1;
        if (e.type == EV_UP || e.type == EV_SELECT)
            i = 2;
        else if (e.type == EV_DOWN)
            i = 0;
        else if (e.type == EV_TAP && e.y >= UI_VIEW_Y0 && e.y < UI_VIEW_Y0 + 380) {
            t0 = sb_ms();
            slides = 0;
            next = 0;
        } else if (e.type == EV_TAP)
            i = bb_hit(b, 4, e.x, e.y);
        if (i >= 0) {
            bb_flash(&b[i]);
            slides = i == 2 ? slides + 1 : i == 0 && slides ? slides - 1 : slides;
            next = 0;
        }
        ui_present();
    }
    ut_awake = 0;
    hid_reset();
}

/* ---- the Remote page ------------------------------------------------------------------------------------ */
static int r_kbd, r_pad, r_game, r_media, r_pres, r_lay[KL_N];

static void remote_fill(struct page *p)
{
    page_hdr(p, "Screens");
    r_kbd = page_row(p, "Keyboard and trackpad", "full keyboard, numpad, F-keys; 2-finger scroll", 0);
    r_pad = page_row(p, "Trackpad and macros", "big trackpad, your buttons from the card", 0);
    r_game = page_row(p, "Gamepad", "sticks, D-pad, 10 buttons; hold the phone sideways", 0);
    r_media = page_row(p, "Media remote", "play, volume, brightness, lock", 0);
    r_pres = page_row(p, "Presenter", "next / back with the volume keys, talk timer", 0);
    page_hdr(p, "The computer's keyboard layout");
    for (int l = 0; l < KL_N; ++l)
        r_lay[l] = page_row(p, kbd_layout_name[l],
                            l == KL_US ? "any computer set to US English"
                            : l == KL_FI_MAC ? "a Mac set to Finnish or Swedish"
                                             : "Windows or Linux set to Finnish or Swedish",
                            R_CHECK | R_NOCHEV | (hid_layout == l ? R_ON : 0));
    page_note(p, "Set the same layout as the computer: the phone sends key positions, the computer's layout turns "
                 "them into letters. Macros and the keyboard type with it.", C_L3);
    int c = usbd_configured();
    page_kv(p, "USB", c ? "connected: keyboard, mouse, media keys, gamepad" : "no computer", c ? C_GREEN : C_ORANGE);
}

void ut_remote(void)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    uint64_t next = 0;
    int focus = 0;
    for (;;) {
        if (sb_ms() >= next) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw("Remote", "Utility", "", 0);
            ut_status_bar();
            page_begin(&p);
            remote_fill(&p);
            page_end(&p);
            page_set_focus(&p, focus);
            page_redraw(&p);
            ui_present();
            next = sb_ms() + 1000;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (ut_back_event(&e))
            break;
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            int f = page_next_row(&p, focus, e.type == EV_UP ? -1 : 1);
            if (f >= 0)
                focus = f;
            page_set_focus(&p, focus);
            page_keep_visible(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick < 0) {
            page_fling_step(&p);
            continue;
        }
        focus = pick;
        for (int l = 0; l < KL_N; ++l)
            if (pick == r_lay[l])
                hid_layout = l;
        if (pick == r_kbd)
            ut_kbd();
        else if (pick == r_pad)
            ut_pad();
        else if (pick == r_game)
            ut_gamepad();
        else if (pick == r_media)
            media();
        else if (pick == r_pres)
            presenter();
        ui_input_reset();
        ui_fill(0, 0, UI_W, UI_H, C_BG);
        next = 0;
    }
    page_free(&p);
}
