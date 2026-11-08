/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd: viewer input -> abstract injection events (portable; host-tested by test_host.c).
 *
 * Keys go by physical position (HID usage -> PC/AT set-1 scan code, as Windows' own HID keyboard
 * driver does, table from Microsoft's "USB HID to PS/2 scan code translation"), so the Windows layout
 * decides the character. Pause and the consumer page (media, volume) go by virtual key.
 */
#include "sender.h"

#include <string.h>

#define E0 0x100

/* HID keyboard page (0x07) usage -> set-1 make code, 0x100 = E0 prefix */
static const uint16_t hid_scan[0xE8] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21,   /* A-F */
    [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,   /* G-L */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13,   /* M-R */
    [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,   /* S-X */
    [0x1C] = 0x15, [0x1D] = 0x2C,                                                               /* Y Z */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06,                  /* 1-5 */
    [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,                  /* 6-0 */
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39,   /* Enter Esc BS Tab Space */
    [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B,   /* - = [ ] \ */
    [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33,   /* NonUS# ; ' ` , */
    [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,                                 /* . / CapsLock */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40,   /* F1-F6 */
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,   /* F7-F12 */
    [0x46] = E0 | 0x37, [0x47] = 0x46,                                           /* PrintScreen ScrollLock */
    /* 0x48 Pause: E1 1D 45, sent as VK_PAUSE */
    [0x49] = E0 | 0x52, [0x4A] = E0 | 0x47, [0x4B] = E0 | 0x49,                  /* Insert Home PgUp */
    [0x4C] = E0 | 0x53, [0x4D] = E0 | 0x4F, [0x4E] = E0 | 0x51,                  /* Delete End PgDn */
    [0x4F] = E0 | 0x4D, [0x50] = E0 | 0x4B, [0x51] = E0 | 0x50, [0x52] = E0 | 0x48,   /* Right Left Down Up */
    [0x53] = 0x45,                                                               /* NumLock */
    [0x54] = E0 | 0x35, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = E0 | 0x1C,   /* KP / * - + Enter */
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C,   /* KP 1-5 */
    [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49, [0x62] = 0x52,   /* KP 6-9 0 */
    [0x63] = 0x53, [0x64] = 0x56, [0x65] = E0 | 0x5D,                            /* KP. NonUS\ Application */
    /* 0x66 Power: deliberately not mapped (a stray key must not power the phone off) */
    [0x67] = 0x59,                                                               /* KP = */
    [0x68] = 0x64, [0x69] = 0x65, [0x6A] = 0x66, [0x6B] = 0x67, [0x6C] = 0x68, [0x6D] = 0x69,   /* F13-F18 */
    [0x6E] = 0x6A, [0x6F] = 0x6B, [0x70] = 0x6C, [0x71] = 0x6D, [0x72] = 0x6E, [0x73] = 0x76,   /* F19-F24 */
    [0x7F] = E0 | 0x20, [0x80] = E0 | 0x30, [0x81] = E0 | 0x2E,                  /* Mute VolUp VolDown */
    [0x85] = 0x7E,                                                               /* KP , (Brazil) */
    [0x87] = 0x73, [0x88] = 0x70, [0x89] = 0x7D, [0x8A] = 0x79, [0x8B] = 0x7B,   /* Intl1-5 (JIS) */
    [0x90] = 0xF2, [0x91] = 0xF1,                                                /* Lang1 Lang2 */
    [0xE0] = 0x1D, [0xE1] = 0x2A, [0xE2] = 0x38, [0xE3] = E0 | 0x5B,             /* LCtrl LShift LAlt LGUI */
    [0xE4] = E0 | 0x1D, [0xE5] = 0x36, [0xE6] = E0 | 0x38, [0xE7] = E0 | 0x5C,   /* RCtrl RShift RAlt RGUI */
};

uint16_t sbl_hid_scan(uint16_t usage)
{
    return usage < sizeof hid_scan / sizeof hid_scan[0] ? hid_scan[usage] : 0;
}

uint8_t sbl_hid_vk(uint16_t page, uint16_t usage)
{
    if (page == SBLK_HID_PAGE_KEYBOARD) return usage == 0x48 ? 0x13 /* VK_PAUSE */ : 0;
    if (page != SBLK_HID_PAGE_CONSUMER) return 0;
    switch (usage) {
    case 0xE2:  return 0xAD;   /* Mute: VK_VOLUME_MUTE */
    case 0xE9:  return 0xAF;   /* VK_VOLUME_UP */
    case 0xEA:  return 0xAE;   /* VK_VOLUME_DOWN */
    case 0xCD:  return 0xB3;   /* Play/Pause: VK_MEDIA_PLAY_PAUSE */
    case 0xB5:  return 0xB0;   /* VK_MEDIA_NEXT_TRACK */
    case 0xB6:  return 0xB1;   /* VK_MEDIA_PREV_TRACK */
    case 0xB7:  return 0xB2;   /* VK_MEDIA_STOP */
    case 0x221: return 0xAA;   /* AC Search: VK_BROWSER_SEARCH */
    case 0x223: return 0xAC;   /* AC Home: VK_BROWSER_HOME */
    case 0x224: return 0xA6;   /* AC Back: VK_BROWSER_BACK */
    case 0x225: return 0xA7;   /* AC Forward: VK_BROWSER_FORWARD */
    case 0x18A: return 0xB4;   /* AL Email: VK_LAUNCH_MAIL */
    case 0x192: return 0xB7;   /* AL Calculator: VK_LAUNCH_APP2 */
    case 0x194: return 0xB6;   /* AL Local Browser (My Computer): VK_LAUNCH_APP1 */
    default:    return 0;      /* brightness (0x6F/0x70) etc.: no virtual key */
    }
}

/* ---- pointer --------------------------------------------------------------------------------------- */

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Windows turns a normalized coordinate n into a pixel by scaling with size / 65536; whether it then
 * truncates or rounds, aiming at p + 1/4 lands on p either way (truncate: [p, p+1), round:
 * [p-1/2, p+1/2)) for every size below 16384 pixels. */
static int32_t norm(int64_t p, int64_t size)
{
    if (size <= 0) return 0;
    int64_t n = ((4 * p + 1) * 65536) / (4 * size);
    return (int32_t)(n < 0 ? 0 : n > 65535 ? 65535 : n);
}

void sbl_map_abs(const sbl_geom *g, int32_t x, int32_t y, int32_t *nx, int32_t *ny)
{
    int32_t dw = g->disp_w ? (int32_t)g->disp_w : g->out_w, dh = g->disp_h ? (int32_t)g->disp_h : g->out_h;
    if (dw <= 0) dw = 1;
    if (dh <= 0) dh = 1;
    x = clamp32(x, 0, dw - 1);
    y = clamp32(y, 0, dh - 1);
    /* display pixel -> output pixel (equal unless the frame is scaled), then desktop pixel */
    int64_t ox = g->out_w > 0 && g->out_w != dw ? (int64_t)x * g->out_w / dw : x;
    int64_t oy = g->out_h > 0 && g->out_h != dh ? (int64_t)y * g->out_h / dh : y;
    int32_t vw = g->virt_w > 0 ? g->virt_w : g->out_w, vh = g->virt_h > 0 ? g->virt_h : g->out_h;
    int32_t vx = g->virt_w > 0 ? g->virt_x : g->out_x, vy = g->virt_h > 0 ? g->virt_y : g->out_y;
    *nx = norm(g->out_x + ox - vx, vw);
    *ny = norm(g->out_y + oy - vy, vh);
}

static const uint32_t btn_bits[5] = { SBLK_BTN_LEFT, SBLK_BTN_RIGHT, SBLK_BTN_MIDDLE, SBLK_BTN_BACK,
                                      SBLK_BTN_FORWARD };

int sbl_in_pointer(sbl_input *st, const sblk_input_pointer *p, const sbl_geom *g, sbl_ev *out, int max)
{
    int n = 0;
    if (max < SBL_MAX_EV_PTR) return 0;
    out[n].kind = SBL_EV_MOVE_ABS; out[n].flags = 0; out[n].code = 0; out[n].data = 0;
    sbl_map_abs(g, p->x, p->y, &out[n].x, &out[n].y);
    n++;
    for (int b = 0; b < 5; b++) {
        uint32_t bit = btn_bits[b];
        if ((p->buttons ^ st->buttons) & bit) {
            sbl_ev *e = &out[n++];
            memset(e, 0, sizeof *e);
            e->kind = SBL_EV_BUTTON;
            e->code = (uint16_t)b;
            e->flags = (p->buttons & bit) ? 0 : SBL_EVF_UP;
        }
    }
    st->buttons = p->buttons & 0x1Fu;
    if (p->wheel_dy) { memset(&out[n], 0, sizeof out[n]); out[n].kind = SBL_EV_WHEEL; out[n++].data = p->wheel_dy; }
    if (p->wheel_dx) { memset(&out[n], 0, sizeof out[n]); out[n].kind = SBL_EV_HWHEEL; out[n++].data = p->wheel_dx; }
    return n;
}

/* ---- keys -------------------------------------------------------------------------------------------- */

static int held(const sbl_input *st, uint16_t u) { return u < 256 && (st->keys[u >> 3] >> (u & 7)) & 1; }
static void set_held(sbl_input *st, uint16_t u, int on)
{
    if (u >= 256) return;
    if (on) st->keys[u >> 3] |= (uint8_t)(1u << (u & 7));
    else st->keys[u >> 3] &= (uint8_t)~(1u << (u & 7));
}

/* one key event for page 7 usage u (or consumer usage), 0 if unmapped */
static int key_ev(uint16_t page, uint16_t u, int up, sbl_ev *e)
{
    memset(e, 0, sizeof *e);
    uint8_t vk = sbl_hid_vk(page, u);
    if (vk) {
        e->kind = SBL_EV_KEY_VK;
        e->code = vk;
        /* media/volume VKs are "extended" keys (E0-prefixed on a real keyboard) */
        e->flags = (uint8_t)((up ? SBL_EVF_UP : 0) | (page == SBLK_HID_PAGE_CONSUMER ? SBL_EVF_EXT : 0));
        return 1;
    }
    if (page != SBLK_HID_PAGE_KEYBOARD) return 0;
    uint16_t sc = sbl_hid_scan(u);
    if (!sc) return 0;
    e->kind = SBL_EV_KEY_SCAN;
    e->code = (uint16_t)(sc & 0xFF);
    e->flags = (uint8_t)((up ? SBL_EVF_UP : 0) | ((sc & E0) ? SBL_EVF_EXT : 0));
    return 1;
}

int sbl_in_key(sbl_input *st, const sblk_input_key *k, sbl_ev *out, int max)
{
    int n = 0;
    if (max < SBL_MAX_EV_KEY) return 0;
    /* resync: release modifiers we hold that the viewer says are up (released while unfocused) */
    for (uint16_t i = 0; i < 8; i++) {
        uint16_t u = (uint16_t)(0xE0 + i);
        if (k->usage_page == SBLK_HID_PAGE_KEYBOARD && k->usage == u) continue;
        if (held(st, u) && !(k->modifiers & (1u << i))) {
            if (key_ev(SBLK_HID_PAGE_KEYBOARD, u, 1, &out[n])) n++;
            set_held(st, u, 0);
        }
    }
    if (k->usage_page == SBLK_HID_PAGE_KEYBOARD) {
        if (k->usage >= 256) return n;
        if (key_ev(SBLK_HID_PAGE_KEYBOARD, k->usage, !k->down, &out[n])) {
            n++;
            set_held(st, k->usage, k->down);
        }
    } else if (k->usage_page == SBLK_HID_PAGE_CONSUMER) {
        if (key_ev(SBLK_HID_PAGE_CONSUMER, k->usage, !k->down, &out[n])) {
            n++;
            int f = -1;
            for (int i = 0; i < st->ncons; i++) if (st->cons[i] == k->usage) f = i;
            if (k->down && f < 0 && st->ncons < 8) st->cons[st->ncons++] = k->usage;
            if (!k->down && f >= 0) st->cons[f] = st->cons[--st->ncons];
        }
    }
    return n;
}

int sbl_in_release_all(sbl_input *st, sbl_ev *out, int max)
{
    int n = 0;
    for (uint16_t u = 0; u < 256 && n < max; u++)
        if (held(st, u) && key_ev(SBLK_HID_PAGE_KEYBOARD, u, 1, &out[n])) n++;
    for (int i = 0; i < st->ncons && n < max; i++)
        if (key_ev(SBLK_HID_PAGE_CONSUMER, st->cons[i], 1, &out[n])) n++;
    for (int b = 0; b < 5 && n < max; b++)
        if (st->buttons & btn_bits[b]) {
            memset(&out[n], 0, sizeof out[n]);
            out[n].kind = SBL_EV_BUTTON; out[n].code = (uint16_t)b; out[n].flags = SBL_EVF_UP;
            n++;
        }
    memset(st, 0, sizeof *st);
    return n;
}

/* ---- text ---------------------------------------------------------------------------------------- */

/* decodes one code point; returns bytes used (>= 1), *cp = U+FFFD on malformed input */
static uint32_t utf8_next(const uint8_t *s, uint32_t n, uint32_t *cp)
{
    uint8_t c = s[0];
    uint32_t need, v, min;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; v = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; v = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; v = c & 0x07; min = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    if (n < need + 1) { *cp = 0xFFFD; return 1; }
    for (uint32_t i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return need + 1; }
    *cp = v;
    return need + 1;
}

static int push_pair(sbl_ev *out, int n, int max, uint8_t kind, uint16_t code)
{
    if (n + 2 > max) return n;
    memset(&out[n], 0, 2 * sizeof out[n]);
    out[n].kind = out[n + 1].kind = kind;
    out[n].code = out[n + 1].code = code;
    out[n + 1].flags = SBL_EVF_UP;
    return n + 2;
}

int sbl_in_text(const uint8_t *s, uint32_t len, sbl_ev *out, int max)
{
    int n = 0;
    uint32_t i = 0;
    while (i < len) {
        uint32_t cp;
        i += utf8_next(s + i, len - i, &cp);
        if (cp == '\r' && i < len && s[i] == '\n') i++;           /* CRLF = one Enter */
        if (cp == '\n' || cp == '\r') n = push_pair(out, n, max, SBL_EV_KEY_VK, 0x0D);   /* VK_RETURN */
        else if (cp == '\t') n = push_pair(out, n, max, SBL_EV_KEY_VK, 0x09);           /* VK_TAB */
        else if (cp < 0x20 || cp == 0x7F) continue;                                      /* other controls */
        else if (cp >= 0x10000) {
            cp -= 0x10000;
            n = push_pair(out, n, max, SBL_EV_UNICODE, (uint16_t)(0xD800 + (cp >> 10)));
            n = push_pair(out, n, max, SBL_EV_UNICODE, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else n = push_pair(out, n, max, SBL_EV_UNICODE, (uint16_t)cp);
    }
    return n;
}
