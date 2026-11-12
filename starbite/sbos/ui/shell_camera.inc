/* The Camera (iOS 26's, dark) on Track A's front-camera driver (shell.h sh_cam_*; included by shell_lock.c next to
 * shell_apps.inc).
 *
 * The page: the static controls are the app's strips (page_camera: the top row's circles, the mode strip with
 * PHOTO in yellow and VIDEO disabled, the shutter's white ring, the thumbnail's and the flip button's circles); the
 * ground is the app's own (black). The live parts are small layers of this file, linked into the page after the
 * strips (cam_layers):
 *   - campv:    the preview, CAM_PW x CAM_PH (0.7 MB) drawn 4x into the 3:4 viewfinder (390 x 520 pt under the top
 *               row), mirrored as the driver gives it; one copy of the driver's newest frame a display frame at most,
 *               only when its seq changed. Allocated when the camera starts, freed when it stops;
 *   - camblink: the shutter's blink (a solid: no bitmap) over the viewfinder;
 *   - cammsg:   the empty state (iOS's ContentUnavailableView): the camera refused or stopped, in sh_cam_why's words;
 *   - camflash: the flash button's glyph (off: struck through; on: yellow);
 *   - camshut:  the shutter's white disc (it shrinks on a shot);
 *   - camthumb: the newest photo (the store's at the open, then each shot's), a circle at the bottom left (it grows
 *               in after a shot); a tap opens Photos at that photo;
 *   - camwhite: the front flash: the whole screen white (and the panel at its brightest) while the still is taken.
 * The lifecycle: APPHOOK's on_show asks for the camera; the next turn after the page's strips are drawn opens it
 * (never in a tap); on_hide (the app closed, another app, the lock, sleep, a film's reset: all go through
 * app_finish_close) closes it and frees every bitmap. A shot: the tap asks sh_cam_still (or starts the front flash);
 * later turns poll sh_cam_still_get, then make the photo's 360 x 480 copy and the thumbnail from it; once the
 * thumbnail has grown in, the next turn keeps the photo in Track A's store (sh_photo_save blocks ~0.2-0.4 s on the
 * phone, so never in the tap, and only after the blink and the thumbnail are on the screen). A refusal says why in a
 * toast; that photo then stays in RAM until Camera closes and its thumbnail shows it in the viewfinder. ----------- */

#define CAM_PW 360                      /* the preview asked of the driver (its CAMERA.md's choice: each preview pixel  */
#define CAM_PH 480                      /* costs the kernel's render a non-cacheable read); drawn 4x: 1440 x 1920 px */
#define CAM_VF_Y 112.0f                 /* the viewfinder (3:4): 390 x 520 pt from here (iOS: under the top row) */
#define CAM_VF_H 520.0f
#define CAM_TOP_Y 66.0f                 /* the top row's circles' centre (36 pt) */
#define CAM_FLASH_X (MARGIN + 18)
#define CAM_LIVE_X (CONTENT_R - 18)
#define CAM_MODE_Y 642.0f               /* the mode strip's text top */
#define CAM_SH_X 195.0f                 /* the shutter: a 78 pt white ring (4 pt), a 62 pt disc */
#define CAM_SH_Y 718.0f
#define CAM_SH_R 31.0f
#define CAM_THUMB_X 48.0f               /* the thumbnail and the flip button: 44 pt glass circles */
#define CAM_FLIP_X 342.0f
#define CAM_YELLOW 0xffd60a             /* iOS's camera yellow */
#define CAM_STALL_MS 2500.0f            /* no new frame for this long: the empty state says why */

enum { CS_OFF, CS_RUN, CS_FAIL };
static tk_layer campv, camblink, cammsg, camflash, camshut, camthumb, camwhite;
static tk_bmp camrev;                   /* the last photo at the preview's size (cached: the review, the thumbnail) */
typedef struct {
    int want, st, flash, review, shot, have, stalled, nphotos;   /* shot: 0 none, 1 the white rising, 2 asked, 3 to save */
    int last, stored, photos;                                    /* last: the store's newest to show; stored: the
                                                                  * thumbnail is a kept photo; photos: go to Photos */
    unsigned seq;
    float t_open, t_last, t_ask, t_white, t_why;                       /* ms (sh_tick's clock) */
    float blink, press, white, thumb_in;                               /* the animations' clocks, s (< 0 idle) */
    int bright;                                                        /* the panel's level before the front flash */
    char why[96];
} cam_state;
#define CAM_STATE0 { .st = CS_OFF, .blink = -1, .press = -1, .white = 0, .thumb_in = -1 }
static cam_state CAM = CAM_STATE0;

/* the one place a photo is kept: Track A's store (sbos-photos: the newest 32). 0 kept, < 0 not (sh_photo_why) */
static int cam_save(const uint32_t *px, int w, int h) { return sh_photo_save(px, w, h); }

/* the circles: iOS's glass over the black ground reads as a dark grey disc with a faint rim. Drawn flat: the GPU's lens
 * (kit_shape) samples the wallpaper's backdrop, not the app's black ground (it showed the wallpaper's blue), and a
 * bake per circle would pause the open */
static void cam_glass(dc *d, float x, float y, float w, float h, float r)
{
    float by = PT(y) - d->oy;
    if (by + PT(h) < 0 || by > d->b->h) return;
    d_rrect(d, x, y, w, h, r, W(.14f));
    d_ring(d, x + w / 2, y + h / 2, r - .4f, .8f, 1, W(.07f), 0);
}
/* bolt.fill as three strokes (off: white with a slash, on: yellow) */
static void cam_bolt(dc *d, float cx, float cy, int on)
{
    tk_col c = on ? tk_hex(CAM_YELLOW, 1) : W(.95f);
    d_seg(d, cx + 2.5f, cy - 9, cx - 4, cy + 1, 2.2f, c);
    d_seg(d, cx - 4, cy + 1, cx + 4, cy - 1, 2.2f, c);
    d_seg(d, cx + 4, cy - 1, cx - 2.5f, cy + 9, 2.2f, c);
    if (!on) d_seg(d, cx - 8, cy - 8, cx + 8, cy + 8, 1.8f, W(.95f));
}
static int cam_ok(void) { return CAM.st == CS_RUN; }
/* mk / tk_layer_init on a layer that is linked: its place in the page's list kept (init clears next, which would cut
 * the layers after it loose until the next relink) */
static void cam_init(tk_layer *l, const char *name) { tk_layer *nx = l->next; tk_layer_init(l, name); l->next = nx; }
static dc cam_mk(tk_layer *l, const char *name, float x, float y, float w, float h, float mg, int glass)
{
    tk_layer *nx = l->next;
    dc d = mk(l, name, x, y, w, h, mg, glass);
    l->next = nx;
    return d;
}
/* an arc clockwise from a0 (0: the top) through sweep, and an arrow head at its end pointing on round */
static void cam_arc_head(dc *d, float cx, float cy, float r, float a0, float sweep, tk_col c)
{
    float sw = 1.8f;
    int bx = (int)tk_floorf(PT(cx - r - sw) - d->ox) - 2, by = (int)tk_floorf(PT(cy - r - sw) - d->oy) - 2;
    int n = (int)PT(2 * (r + sw)) + 5;
    tk_fm m;
    if (by + n < 0 || by > d->b->h || !tk_fm_new(&m, n, n)) return;
    tk_fm_arc(&m, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(r), PT(sw), a0, sweep, 1);
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &p, 1);
    tk_fm_free(&m);
    float a1 = a0 + sweep, tx = cx + r * tk_sinf(a1), ty = cy - r * tk_cosf(a1), dx = tk_cosf(a1), dy = tk_sinf(a1);
    float hx = tx + dx * 1.5f, hy = ty + dy * 1.5f, L = 4.6f;
    for (int k = -1; k <= 1; k += 2) {
        float ca = tk_cosf(2.5f * k), sa = tk_sinf(2.5f * k);
        d_seg(d, hx, hy, hx + (dx * ca - dy * sa) * L, hy + (dx * sa + dy * ca) * L, sw, c);
    }
}

/* ---- the page's static part (the strips): own_strip's page for "camera" -------------------------------------- */
static void page_camera(dc *d)
{
    /* the top row: flash (its glyph is camflash's) and Live Photos, off (it can't be had yet) */
    cam_glass(d, CAM_FLASH_X - 18, CAM_TOP_Y - 18, 36, 36, 18);
    cam_glass(d, CAM_LIVE_X - 18, CAM_TOP_Y - 18, 36, 36, 18);
    d_ring(d, CAM_LIVE_X, CAM_TOP_Y, 8, 1.6f, 1, W(.5f), 0);
    d_ring(d, CAM_LIVE_X, CAM_TOP_Y, 3.5f, 1.6f, 1, W(.5f), 0);
    d_seg(d, CAM_LIVE_X - 9, CAM_TOP_Y - 9, CAM_LIVE_X + 9, CAM_TOP_Y + 9, 1.6f, W(.5f));
    /* the modes: VIDEO (no video yet: dimmed) and PHOTO, centred, in yellow */
    float wv = tw(TK_IT600, 14, "VIDEO", .6f, 0), wp = tw(TK_IT600, 14, "PHOTO", .6f, 0);
    d_text(d, TK_IT600, 14, 195 - wp / 2 - 26 - wv, bl(TK_IT600, 14, CAM_MODE_Y), "VIDEO", .6f, 0, W(.3f), 0);
    d_text(d, TK_IT600, 14, 195 - wp / 2, bl(TK_IT600, 14, CAM_MODE_Y), "PHOTO", .6f, 0, tk_hex(CAM_YELLOW, 1), 0);
    /* the shutter's ring (its disc is camshut's), the thumbnail's circle, the flip button (only the front works: dim) */
    d_ring(d, CAM_SH_X, CAM_SH_Y, 37, 4, 1, W(1), 0);
    cam_glass(d, CAM_THUMB_X - 22, CAM_SH_Y - 22, 44, 44, 22);
    cam_glass(d, CAM_FLIP_X - 22, CAM_SH_Y - 22, 44, 44, 22);
    /* arrow.triangle.2.circlepath: two arcs round the middle, each with its head (only the front camera: dim) */
    cam_arc_head(d, CAM_FLIP_X, CAM_SH_Y, 9, -1.75f, 2.6f, W(.4f));
    cam_arc_head(d, CAM_FLIP_X, CAM_SH_Y, 9, 1.39f, 2.6f, W(.4f));
}

/* ---- the live layers ---------------------------------------------------------------------------------------- */
static void cam_draw_flash(void)
{
    relayer(&camflash);
    dc d = cam_mk(&camflash, "cam-flash", CAM_FLASH_X - 12, CAM_TOP_Y - 12, 24, 24, 1, 0);
    if (!d.b->px) return;
    cam_bolt(&d, CAM_FLASH_X, CAM_TOP_Y, CAM.flash);
    camflash.hidden = 0;
}
static void cam_draw_shutter(void)
{
    relayer(&camshut);
    dc d = cam_mk(&camshut, "cam-shutter", CAM_SH_X - CAM_SH_R, CAM_SH_Y - CAM_SH_R, 2 * CAM_SH_R, 2 * CAM_SH_R, 1, 0);
    if (!d.b->px) return;
    d_rrect(&d, CAM_SH_X - CAM_SH_R, CAM_SH_Y - CAM_SH_R, 2 * CAM_SH_R, 2 * CAM_SH_R, CAM_SH_R, W(cam_ok() ? 1 : .3f));
    camshut.ox = PT(CAM_SH_X);
    camshut.oy = PT(CAM_SH_Y);
    camshut.hidden = 0;
}
/* the empty state centred in the viewfinder: video.slash over iOS's title and the words (0: none) */
static void cam_msg(const char *title, const char *line)
{
    relayer(&cammsg);
    cammsg.hidden = 1;
    if (!title) return;
    char ln[4][96];
    float wm;
    int n = line && line[0] ? wrapn(ln, 3, TK_IT400, 15, line, 300, &wm) : 0;
    float h = 36 + 20.0f * n - (n ? 2 : 8), gh = 44, top = tk_floorf(CAM_VF_Y + (CAM_VF_H - h - gh) / 2) + gh;
    dc d = cam_mk(&cammsg, "cam-msg", 40, top - gh, 310, h + gh + 6, 0, 0);
    if (!d.b->px) return;
    /* the glyph: a camera body, its lens, struck through */
    float gx = 195, gy = top - gh + 12;
    tk_col g = W(.55f);
    d_seg(&d, gx - 17, gy - 9, gx + 8, gy - 9, 2.2f, g);
    d_seg(&d, gx - 17, gy + 9, gx + 8, gy + 9, 2.2f, g);
    d_seg(&d, gx - 17, gy - 9, gx - 17, gy + 9, 2.2f, g);
    d_seg(&d, gx + 8, gy - 9, gx + 8, gy + 9, 2.2f, g);
    d_seg(&d, gx + 8, gy - 2, gx + 17, gy - 7, 2.2f, g);
    d_seg(&d, gx + 8, gy + 2, gx + 17, gy + 7, 2.2f, g);
    d_seg(&d, gx + 17, gy - 7, gx + 17, gy + 7, 2.2f, g);
    d_seg(&d, gx - 20, gy - 14, gx + 20, gy + 14, 2.2f, g);
    d_empty(&d, top, top + h, title, n ? line : 0);
    trim(&cammsg);
    cammsg.hidden = 0;
}
static void cam_place(void)
{
    camblink.hidden = CAM.blink < 0;
    if (CAM.blink >= 0) camblink.alpha = CAM.blink < .05f ? CAM.blink / .05f : tk_maxf(0, 1 - (CAM.blink - .05f) / .2f);
    float s = 1;
    if (CAM.press >= 0) s = .86f + .14f * tk_bezier(.25f, .1f, .25f, 1, tk_minf(1, CAM.press / .28f));
    camshut.sx = camshut.sy = s;
    camwhite.hidden = CAM.white <= 0;
    camwhite.alpha = tk_minf(1, CAM.white);
    float ts = CAM.thumb_in >= 0 ? .4f + .6f * tk_bezier(.2f, .9f, .3f, 1.15f, tk_minf(1, CAM.thumb_in / .3f)) : 1;
    camthumb.sx = camthumb.sy = ts;
    camthumb.alpha = tk_minf(1, ts);
}
static int cam_layers(tk_layer **o)
{
    if (!campv.name) {
        tk_layer *L[] = { &campv, &camblink, &cammsg, &camflash, &camshut, &camthumb, &camwhite };
        static const char *N[] = { "cam-preview", "cam-blink", "cam-msg", "cam-flash", "cam-shutter", "cam-thumb", "cam-white" };
        for (int i = 0; i < 7; i++) { tk_layer_init(L[i], N[i]); L[i]->hidden = 1; }
    }
    if (!is_app("camera")) return 0;
    o[0] = &campv; o[1] = &camblink; o[2] = &cammsg; o[3] = &camflash; o[4] = &camshut; o[5] = &camthumb; o[6] = &camwhite;
    return 7;
}

/* ---- the camera on and off ------------------------------------------------------------------------------------ */
static void cam_white_end(void)
{
    if (CAM.bright > 0) sh_brightness(CAM.bright);     /* the panel back as it was */
    CAM.bright = 0;
}
static void cam_stop(void)
{
    if (CAM.st == CS_RUN) sh_cam_close();
    cam_white_end();
    tk_layer *L[] = { &campv, &camblink, &cammsg, &camflash, &camshut, &camthumb, &camwhite };
    for (int i = 0; i < 7; i++) { relayer(L[i]); L[i]->solid = 0; L[i]->hidden = 1; L[i]->sx = L[i]->sy = 1; }
    tk_bmp_free(&camrev);
    int flash = CAM.flash;
    static const cam_state zero = CAM_STATE0;
    CAM = zero;
    CAM.flash = flash;                                /* the flash setting stays (iOS keeps it) */
}
static void cam_fail(const char *why)
{
    CAM.st = CS_FAIL;
    campv.hidden = 1;
    cam_msg("Camera Not Available", why);
    cam_draw_shutter();
}
static void cam_start(float t)
{
    cam_draw_flash();
    CAM.last = 1;                                    /* the store's newest as the thumbnail, on the next turn */
    cam_init(&camblink, "cam-blink");          /* solids: no bitmaps */
    camblink.solid = 0xff000000u;
    camblink.x = 0; camblink.y = tk_floorf(PT(CAM_VF_Y));
    camblink.solid_w = TK_W; camblink.solid_h = 4 * CAM_PH;
    camblink.hidden = 1;
    cam_init(&camwhite, "cam-white");
    camwhite.solid = 0xffffffffu;
    camwhite.solid_w = TK_W; camwhite.solid_h = TK_H;
    camwhite.hidden = 1;
    int r = sh_cam_open(CAM_PW, CAM_PH);
    if (r < 0) { cam_fail(sh_cam_why()); return; }
    relayer(&campv);
    cam_init(&campv, "cam-preview");
    if (!tk_bmp_new(&campv.bmp, CAM_PW, CAM_PH)) { sh_cam_close(); cam_fail("There isn't enough memory for the preview."); return; }
    campv.x = campv.ox = 0;
    campv.y = campv.oy = tk_floorf(PT(CAM_VF_Y));
    campv.sx = campv.sy = (float)TK_W / CAM_PW;      /* 4: 1440 x 1920 px, the 3:4 viewfinder */
    campv.opaque = 1;
    campv.hidden = 1;                                /* until the first frame: the ground's black */
    CAM.st = CS_RUN;
    CAM.t_open = CAM.t_last = CAM.t_why = t;
    CAM.seq = 0;
    CAM.have = 0;
    cam_draw_shutter();
}
/* the newest frame into the preview's bitmap: once per new seq (the driver's memory is uncached: one bulk copy) */
static int cam_pull(float t)
{
    if (CAM.st != CS_RUN || CAM.review || !campv.bmp.px) return 0;
    int w = 0, h = 0;
    unsigned seq = 0;
    const uint32_t *px = sh_cam_frame(&w, &h, &seq);
    if (!px || (CAM.have && seq == CAM.seq)) return 0;
    tk_bmp *b = &campv.bmp;
    if (w == b->w && h == b->h && b->stride == w) tk_memcpy(b->px, px, (size_t)w * (size_t)h * 4);
    else if (w > 0 && h > 0) {                      /* another size than asked: nearest, row by row */
        for (int y = 0; y < b->h; y++) {
            const uint32_t *s = px + (size_t)(y * h / b->h) * (size_t)w;
            uint32_t *o = b->px + (size_t)y * b->stride;
            for (int x = 0; x < b->w; x++) o[x] = s[x * w / b->w] | 0xff000000u;
        }
    } else return 0;
    tk_layer_dirty(&campv);
    CAM.seq = seq;
    CAM.have = 1;
    CAM.t_last = t;
    if (CAM.stalled) { CAM.stalled = 0; cam_msg(0, 0); }
    campv.hidden = 0;
    return 1;
}
/* the still (612 x 816, uncached) into the review's 360 x 480 (2 x 2 means, two source rows copied at a time) and
 * the thumbnail's circle from that */
static uint32_t cam_rows[2][816];
static void cam_take(const uint32_t *px, int w, int h, int grow)
{
    if (w <= 1 || h <= 1 || w > 816) return;
    if (!camrev.px && !tk_bmp_new(&camrev, CAM_PW, CAM_PH)) return;
    for (int y = 0; y < CAM_PH; y++) {
        int sy = tk_mini(h - 2, y * h / CAM_PH);
        tk_memcpy(cam_rows[0], px + (size_t)sy * (size_t)w, (size_t)w * 4);
        tk_memcpy(cam_rows[1], px + (size_t)(sy + 1) * (size_t)w, (size_t)w * 4);
        uint32_t *o = camrev.px + (size_t)y * camrev.stride;
        for (int x = 0; x < CAM_PW; x++) {
            int sx = tk_mini(w - 2, x * w / CAM_PW);
            uint32_t a = cam_rows[0][sx], b = cam_rows[0][sx + 1], c = cam_rows[1][sx], e = cam_rows[1][sx + 1];
            uint32_t rb = ((a & 0xff00ffu) + (b & 0xff00ffu) + (c & 0xff00ffu) + (e & 0xff00ffu) + 0x20002u) >> 2 & 0xff00ffu;
            uint32_t g = ((a & 0xff00u) + (b & 0xff00u) + (c & 0xff00u) + (e & 0xff00u) + 0x200u) >> 2 & 0xff00u;
            o[x] = 0xff000000u | rb | g;
        }
    }
    /* the thumbnail: the photo's middle square in a 40 pt circle (2 x 2 means of the review, edge smoothed) */
    relayer(&camthumb);
    dc d = cam_mk(&camthumb, "cam-thumb", CAM_THUMB_X - 20, CAM_SH_Y - 20, 40, 40, 1, 0);
    if (!d.b->px) return;
    float cx = PT(CAM_THUMB_X) - d.ox, cy = PT(CAM_SH_Y) - d.oy, r = PT(20), sc = CAM_PW / (2 * r);
    int y0 = (CAM_PH - CAM_PW) / 2;
    for (int j = 0; j < d.b->h; j++)
        for (int i = 0; i < d.b->w; i++) {
            float dx = i + .5f - cx, dy = j + .5f - cy, cov = tk_clampf(r - tk_sqrtf(dx * dx + dy * dy) + .5f, 0, 1);
            if (cov <= 0) continue;
            int u = tk_mini(CAM_PW - 2, tk_maxi(0, (int)((dx + r) * sc))), v = tk_mini(CAM_PW - 2, tk_maxi(0, (int)((dy + r) * sc))) + y0;
            const uint32_t *s0 = camrev.px + (size_t)v * camrev.stride + u, *s1 = s0 + camrev.stride;
            uint32_t q = 0;
            for (int k = 0; k < 24; k += 8) {
                unsigned m = ((s0[0] >> k & 255) + (s0[1] >> k & 255) + (s1[0] >> k & 255) + (s1[1] >> k & 255) + 2) / 4;
                q |= (uint32_t)(m * cov + .5f) << k;
            }
            d.b->px[(size_t)j * d.b->stride + i] = q | (uint32_t)(255 * cov + .5f) << 24;
        }
    camthumb.ox = PT(CAM_THUMB_X);
    camthumb.oy = PT(CAM_SH_Y);
    camthumb.hidden = 0;
    CAM.thumb_in = grow ? 0 : -1;
    CAM.nphotos++;
}
/* the store's newest photo as the thumbnail (at the open; ~0.1 s read on the phone, a turn of its own) */
static void cam_last(void)
{
    int w = 0, h = 0;
    long long utc = 0;
    if (sh_photo_count() <= 0 || sh_photo_get(0, 0, 0, &w, &h, &utc) <= 0 || w > 816 || h < 2) return;
    uint32_t *px = tk_alloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    if (sh_photo_get(0, px, w * h, &w, &h, &utc) > 0 && w <= 816) { cam_take(px, w, h, 0); CAM.stored = 1; }
    tk_free(px);
}
static void cam_review(int on)
{
    if (on && (!camrev.px || !campv.bmp.px)) return;
    CAM.review = on;
    if (on) {
        tk_memcpy(campv.bmp.px, camrev.px, (size_t)CAM_PW * CAM_PH * 4);
        tk_layer_dirty(&campv);
        campv.hidden = 0;
        cam_msg(0, 0);
    } else
        CAM.have = 0;                                 /* the next frame whatever its seq */
}
/* a shot's step after the tap (a still asked: its blink) */
static void cam_ask(float t)
{
    if (sh_cam_still() < 0) { toast("The camera is busy"); CAM.shot = 0; return; }
    CAM.shot = 2;
    CAM.t_ask = t;
    if (!CAM.flash) CAM.blink = 0;
}

/* ---- the shell's hooks -------------------------------------------------------------------------------------- */
static void cam_show(int cold)
{
    (void)cold;
    if (CAM.st != CS_OFF) cam_stop();
    CAM.want = 1;                                     /* the next turn after the page's strips (never in the tap) */
}
static void cam_hide(void) { cam_stop(); }          /* (the layers stay linked, hidden, with no bitmaps) */
static int cam_tick(float dt, float t)
{
    if (!is_app("camera") || AP.closing) return 0;
    int mv = 0;
    if (CAM.want && !ap_pending) { CAM.want = 0; cam_start(t); relink(); return 1; }
    if (CAM.last) { CAM.last = 0; if (!CAM.nphotos) cam_last(); return 1; }
    if (CAM.photos) {                                /* the thumbnail: Photos at the newest photo (this app goes) */
        int a = -1;
        for (int k = 0; k < NAPPS; k++) if (!tk_strcmp(APPS[k].k, "photos")) a = k;
        CAM.photos = 0;
        if (a >= 0) { md_want_store = 0; app_finish_close(); app_open(a); }
        return 1;
    }
    if (CAM.st == CS_RUN) {
        mv |= cam_pull(t);
        /* no new frame for a while: say why (the driver's words), and again when they change */
        if (!CAM.review && t - CAM.t_last > CAM_STALL_MS && t - CAM.t_why > 500) {
            const char *w = sh_cam_why();
            CAM.t_why = t;
            if (!CAM.stalled || tk_strcmp(w, CAM.why)) {
                int n = 0;
                while (w[n] && n < (int)sizeof CAM.why - 1) { CAM.why[n] = w[n]; n++; }
                CAM.why[n] = 0;
                CAM.stalled = 1;
                campv.hidden = 1;
                cam_msg(CAM.have ? "The Camera Stopped" : "Waiting for the Camera", CAM.why);
            }
        }
        mv = 1;                                      /* frames keep coming: the turns go on while it runs */
    }
    /* a shot: the front flash's white (the panel at its brightest), the still's poll */
    if (CAM.shot == 1) {
        if (CAM.white < 1) { CAM.white = tk_minf(1, CAM.white + dt / .12f); CAM.t_white = t; }
        else if (t - CAM.t_white > 250) cam_ask(t);   /* the exposure follows the lit face for a few frames first */
    }
    if (CAM.shot == 2) {
        int w = 0, h = 0;
        const unsigned char *bmp = 0;
        unsigned len = 0;
        const uint32_t *px = CAM.st == CS_RUN ? sh_cam_still_get(&w, &h, &bmp, &len) : 0;
        if (px) { CAM.shot = 3; cam_take(px, w, h, 1); CAM.stored = 0; }   /* kept once the thumbnail has grown in */
        else if (t - CAM.t_ask > 4000) { CAM.shot = 0; toast("The photo didn't come"); }
        if (CAM.shot != 2) cam_white_end();
    } else if (CAM.shot == 3 && CAM.thumb_in < 0) {
        int w = 0, h = 0;
        const unsigned char *bmp = 0;
        unsigned len = 0;
        const uint32_t *px = CAM.st == CS_RUN ? sh_cam_still_get(&w, &h, &bmp, &len) : 0;   /* (still the driver's) */
        CAM.shot = 0;
        if (px && cam_save(px, w, h) == 0) CAM.stored = 1;
        else { char b[120]; int n = md_cat(b, 0, sizeof b, "Not saved: "); md_cat(b, n, sizeof b, px ? sh_photo_why() : "the photo was gone"); toast(b); }
    }
    if (!CAM.shot && CAM.white > 0) CAM.white = tk_maxf(0, CAM.white - dt / .3f);
    if (CAM.blink >= 0) { CAM.blink += dt; if (CAM.blink > .26f) CAM.blink = -1; }
    if (CAM.press >= 0) { CAM.press += dt; if (CAM.press > .3f) CAM.press = -1; }
    if (CAM.thumb_in >= 0) { CAM.thumb_in += dt; if (CAM.thumb_in > .32f) CAM.thumb_in = -1; }
    if (CAM.blink >= 0 || CAM.press >= 0 || CAM.thumb_in >= 0 || CAM.white > 0 || CAM.shot) mv = 1;
    cam_place();
    return mv;
}
/* the press under a finger: the shutter's disc darkens, a circle lights */
static int cam_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    float dx = x - CAM_SH_X, dy = y - CAM_SH_Y;
    if (dx * dx + dy * dy <= 44 * 44 && cam_ok()) {
        *r = *g = (tk_rect){ CAM_SH_X - CAM_SH_R, CAM_SH_Y - CAM_SH_R, 2 * CAM_SH_R, 2 * CAM_SH_R };
        *rad = CAM_SH_R; *c = APR_CELL;
        return 1;
    }
    /* cp6: the flash, and the thumbnail once there is a photo; Live, VIDEO and the flip are dimmed (a tap says why) */
    static const float B[2][3] = { { CAM_FLASH_X, CAM_TOP_Y, 18 }, { CAM_THUMB_X, CAM_SH_Y, 22 } };
    for (int k = 0; k < 2; k++) {
        float ex = x - B[k][0], ey = y - B[k][1], rr = B[k][2];
        if (k == 1 && !CAM.nphotos && !CAM.stored) break;
        if (ex * ex + ey * ey <= (rr + 8) * (rr + 8)) {
            *r = *g = (tk_rect){ B[k][0] - rr, B[k][1] - rr, 2 * rr, 2 * rr };
            *rad = rr; *c = APR_CIRC;
            return 1;
        }
    }
    return 0;
}
static void camera_tap(float x, float y)
{
    float dx = x - CAM_SH_X, dy = y - CAM_SH_Y, fx = x - CAM_FLASH_X, fy = y - CAM_TOP_Y, lx = x - CAM_LIVE_X;
    float tx = x - CAM_THUMB_X, px = x - CAM_FLIP_X;
    if (dx * dx + dy * dy <= 44 * 44) {
        if (CAM.review) cam_review(0);
        if (!cam_ok()) { sh_haptic(); toast(CAM.st == CS_FAIL ? "The camera isn't available" : "The camera is starting"); return; }
        if (CAM.shot) return;                         /* one at a time */
        CAM.press = 0;
        if (CAM.flash) {                              /* the front flash: white first, the still once it lights the face */
            CAM.shot = 1;
            CAM.white = .001f;
            CAM.bright = cc_bright > 0 ? cc_bright : 2;
            sh_brightness(4);
        } else
            cam_ask(now_ms);
    } else if (fx * fx + fy * fy <= 26 * 26) {
        CAM.flash = !CAM.flash;
        if (CAM.st != CS_OFF) cam_draw_flash();       /* a 24 pt glyph: nothing more */
        toast(CAM.flash ? "Flash On" : "Flash Off");
    } else if (lx * lx + fy * fy <= 26 * 26) {
        toast("Live Photos aren't available yet");
    } else if (y >= CAM_MODE_Y - 8 && y < CAM_MODE_Y + 26 && x < 195 - 30) {
        toast("Video isn't available yet");
    } else if (tx * tx + dy * dy <= 30 * 30) {
        if (CAM.stored && !CAM.shot) CAM.photos = 1;            /* Photos at it, on the next turn (never in the tap) */
        else if (CAM.nphotos) cam_review(!CAM.review);          /* not kept: shown in the viewfinder */
        else toast("No photos yet: tap the shutter");          /* cp6: never a silent tap */
    } else if (px * px + dy * dy <= 30 * 30) {
        toast("Only the front camera works yet");
    } else if (CAM.review && y >= CAM_VF_Y && y < CAM_VF_Y + CAM_VF_H) {
        cam_review(0);
    }
}
/* the host's stills: cam=:0 the preview (a frame in), cam=shot a photo taken (its thumbnail), cam=review the last
 * photo in the viewfinder, cam=flash:V the flash on and its white at V (0: only the lit glyph). SBTK_CAM=fail: refused */
static int cam_set(const char *what, const char *key, float v)
{
    if (tk_strcmp(what, "cam") || !is_app("camera")) return 0;
    if (CAM.want || CAM.st == CS_OFF) { CAM.want = 0; cam_start(now_ms); relink(); }
    if (CAM.last) { CAM.last = 0; if (!CAM.nphotos) cam_last(); }
    for (int i = 0; i < 40 && CAM.st == CS_RUN && !CAM.have; i++) cam_pull(now_ms);   /* the fake's clock moves per call */
    if (!tk_strcmp(key, "shot") || !tk_strcmp(key, "review")) {
        if (cam_ok() && !CAM.shot && sh_cam_still() == 0) {
            int w = 0, h = 0;
            const unsigned char *bmp = 0;
            unsigned len = 0;
            const uint32_t *px = 0;
            for (int i = 0; i < 40 && !px; i++) px = sh_cam_still_get(&w, &h, &bmp, &len);
            if (px) { cam_take(px, w, h, 0); CAM.stored = cam_save(px, w, h) == 0; }
        }
        if (!tk_strcmp(key, "review")) cam_review(1);
    } else if (!tk_strcmp(key, "flash")) {
        CAM.flash = 1;
        cam_draw_flash();
        CAM.white = v;
    }
    cam_place();
    return 1;
}
