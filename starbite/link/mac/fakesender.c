/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link: fake SBLINK v1 source for testing the Mac viewer without the phone.
 *
 * Serves TCP (default port 47474) like the phone's link service will: answers HELLO, honours MODE requests,
 * streams an animated desktop-like test pattern (wallpaper, taskbar with a clock, moving windows with text,
 * a spinner, a software cursor) with the same source rules as the phone (dirty 64x64 tiles via
 * sblk_dirty_rects, solid / LZ4 / raw via sblk_build_rect, at most 2 frames in flight), and logs every
 * input event it receives. Typed keys appear in the "Notepad" window, pointer drags move it, the wheel
 * scrolls it, and the "Input log" window shows the events: an interactive check of the whole loop.
 *
 *   fakesender [--port N] [--size WxH] [--fps N] [--crc-file F] [--log F]
 *              [--fault drop:N|stall:N|corrupt:N|rotate:N] [--exit-on-bye] [--verbose]
 *
 * --crc-file: after every frame "<id> <W>x<H> <hash>" (sbv_fb_hash of the composed image) so the viewer's
 * selftest can check its own composition. --fault: once, in the first session, after frame N: close the
 * socket (drop), go silent (stall), send garbage (corrupt), or turn the display a quarter (rotate: W x H becomes
 * H x W, announced with an unsolicited MODE APPLIED and a keyframe, like the phone after Windows' orientation
 * setting changed). --exit-on-bye: exit 0 when a viewer says BYE
 * with reason normal (0) or shutdown (3).
 * Mac only (CoreGraphics + CoreText for the drawing); uses the portable core for everything on the wire.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../proto/sblink.h"
#include "sbv.h"   /* sbv_fb_hash only */

#define MAX_RECTS 256
#define MAX_STRIPS 2048
#define LOG_LINES 18
#define NOTE_MAX 4096

static uint64_t mono_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u; }

/* ---- options / logging ------------------------------------------------------------------------------- */

static struct {
    int port, w, h, fps, verbose, exit_on_bye;
    const char *crc_file, *log_file;
    int fault_kind, fault_at;   /* 1 drop, 2 stall, 3 corrupt, 4 rotate */
} opt = { SBLK_TCP_PORT, 1280, 720, 60, 0, 0, NULL, NULL, 0, 0 };

static FILE *logf_, *crcf;

/* the on-screen "Input log" window */
static char screen_log[LOG_LINES][96];
static int screen_log_n;

static void say(int on_screen, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int on_screen, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(logf_, "%s\n", line);
    fflush(logf_);
    if (on_screen) {
        if (screen_log_n == LOG_LINES) {
            memmove(screen_log[0], screen_log[1], sizeof screen_log[0] * (LOG_LINES - 1));
            screen_log_n--;
        }
        snprintf(screen_log[screen_log_n++], sizeof screen_log[0], "%s", line);
    }
}

/* ---- scene --------------------------------------------------------------------------------------------- */

typedef struct {
    double x, y, w, h;
    const char *title;
    int kind;            /* 0 notepad, 1 input log, 2 stats, 3 files */
    int pinned;          /* user dragged it: no more automatic motion */
} win_t;

static struct {
    uint32_t W, H;
    uint8_t *cur, *prev, *wall;
    CGContextRef ctx;
    CGColorSpaceRef cs;
    CTFontRef ui, ui_bold, mono, big;
    win_t win[4];
    int nwin;
    char note[NOTE_MAX];
    int note_scroll;     /* lines scrolled */
    int wheel_acc;
    int32_t px, py;
    uint32_t buttons;
    int drag, drag_win;
    double drag_dx, drag_dy;
    uint64_t frames_sent;
    uint16_t mods;
    double t0;
} sc;

static CGColorRef rgba(double r, double g, double b, double a)
{
    CGFloat c[4] = { r, g, b, a };
    return CGColorCreate(sc.cs, c);
}

static void fill(CGContextRef c, double x, double y, double w, double h, double r, double g, double b, double a)
{
    CGContextSetRGBFillColor(c, r, g, b, a);
    CGContextFillRect(c, CGRectMake(x, y, w, h));
}

static void rounded(CGContextRef c, double x, double y, double w, double h, double rad)
{
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(x, y, w, h), rad, rad, NULL);
    CGContextAddPath(c, p);
    CGContextFillPath(c);
    CGPathRelease(p);
}

/* text with its top-left at (x, y) in the flipped (top-left origin) context */
static double text(CGContextRef c, CTFontRef font, double x, double y, double r, double g, double b, const char *s)
{
    CFStringRef str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    if (!str) return 0;
    CGColorRef col = rgba(r, g, b, 1);
    const void *keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    const void *vals[] = { font, col };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGContextSetTextMatrix(c, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(c, x, y + CTFontGetAscent(font));
    CTLineDraw(line, c);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    CFRelease(as);
    CFRelease(attrs);
    CGColorRelease(col);
    CFRelease(str);
    return w;
}

static CGContextRef make_ctx(uint8_t *buf)
{
    CGContextRef c = CGBitmapContextCreate(buf, sc.W, sc.H, 8, (size_t)sc.W * 4, sc.cs,
                                           kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextTranslateCTM(c, 0, sc.H);
    CGContextScaleCTM(c, 1, -1);   /* top-left origin, y down, like the display */
    return c;
}

static void draw_wallpaper(void)
{
    CGContextRef c = make_ctx(sc.wall);
    CGFloat comps[] = { 0.02, 0.10, 0.32, 1, 0.16, 0.42, 0.80, 1 };
    CGFloat locs[] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(sc.cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(sc.W, sc.H), 0);
    CGGradientRelease(g);
    /* the "bloom": two soft discs */
    CGFloat bl[] = { 0.55, 0.78, 1.0, 0.55, 0.30, 0.55, 0.95, 0.0 };
    g = CGGradientCreateWithColorComponents(sc.cs, bl, locs, 2);
    CGPoint ctr = CGPointMake(sc.W * 0.62, sc.H * 0.48);
    CGContextDrawRadialGradient(c, g, ctr, 0, ctr, sc.H * 0.45, 0);
    ctr = CGPointMake(sc.W * 0.70, sc.H * 0.56);
    CGContextDrawRadialGradient(c, g, ctr, 0, ctr, sc.H * 0.25, 0);
    CGGradientRelease(g);
    text(c, sc.big, 32, 28, 1, 1, 1, "Starbite Display");
    text(c, sc.ui, 34, 70, 0.85, 0.9, 1, "fake SBLINK source - Windows 11 on the S9 stands in here");
    CGContextRelease(c);
}

static void scene_init(uint32_t W, uint32_t H)
{
    if (sc.ctx) CGContextRelease(sc.ctx);
    free(sc.cur); free(sc.prev); free(sc.wall);
    sc.W = W;
    sc.H = H;
    size_t n = (size_t)W * H * 4;
    sc.cur = calloc(1, n);
    sc.prev = calloc(1, n);
    sc.wall = calloc(1, n);
    if (!sc.cur || !sc.prev || !sc.wall) { fprintf(stderr, "out of memory\n"); exit(1); }
    if (!sc.cs) {
        sc.cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        sc.ui = CTFontCreateWithName(CFSTR("Helvetica"), 13, NULL);
        sc.ui_bold = CTFontCreateWithName(CFSTR("Helvetica-Bold"), 13, NULL);
        sc.mono = CTFontCreateWithName(CFSTR("Menlo"), 12, NULL);
        sc.big = CTFontCreateWithName(CFSTR("Helvetica-Bold"), 34, NULL);
        sc.t0 = (double)mono_us() / 1e6;
        snprintf(sc.note, sizeof sc.note,
                 "Type on the Mac: the keys arrive here as HID usages.\n"
                 "Drag this window by its title bar; the wheel scrolls it.\n\n"
                 "The quick brown fox jumps over the lazy dog 0123456789\n");
    }
    draw_wallpaper();
    sc.ctx = make_ctx(sc.cur);
    sc.nwin = 4;
    sc.win[0] = (win_t){ W * 0.08, H * 0.18, 460, 300, "Notepad - notes.txt", 0, sc.win[0].pinned };
    sc.win[1] = (win_t){ W * 0.55, H * 0.12, 470, 330, "Input log", 1, 0 };
    sc.win[2] = (win_t){ W * 0.40, H * 0.58, 300, 170, "Frame stats", 2, 0 };
    sc.win[3] = (win_t){ W * 0.70, H * 0.62, 260, 190, "This PC", 3, 0 };
}

static void draw_window(CGContextRef c, win_t *wn, double t, int active)
{
    double x = floor(wn->x), y = floor(wn->y), w = wn->w, h = wn->h;
    /* shadow + frame */
    CGContextSetRGBFillColor(c, 0, 0, 0, 0.25);
    rounded(c, x + 4, y + 6, w, h, 8);
    CGContextSetRGBFillColor(c, active ? 0.93 : 0.97, active ? 0.95 : 0.97, active ? 0.98 : 0.97, 1);
    rounded(c, x, y, w, h, 8);
    fill(c, x, y + 32, w, 1, 0.82, 0.82, 0.85, 1);
    text(c, active ? sc.ui_bold : sc.ui, x + 12, y + 9, 0.1, 0.1, 0.12, wn->title);
    /* caption buttons */
    text(c, sc.ui, x + w - 108, y + 8, 0.2, 0.2, 0.2, "\xE2\x80\x94");
    text(c, sc.ui, x + w - 70, y + 8, 0.2, 0.2, 0.2, "\xE2\x96\xA1");
    fill(c, x + w - 46, y + 1, 45, 31, 0.86, 0.17, 0.17, active ? 1 : 0);
    text(c, sc.ui, x + w - 28, y + 8, active ? 1 : 0.2, active ? 1 : 0.2, active ? 1 : 0.2, "\xC3\x97");
    double cx = x + 1, cy = y + 33, cw = w - 2, ch = h - 34;
    char line[160];

    switch (wn->kind) {
    case 0: {   /* notepad: typed text, scrollable */
        fill(c, cx, cy, cw, ch, 1, 1, 1, 1);
        CGContextSaveGState(c);
        CGContextClipToRect(c, CGRectMake(cx, cy, cw, ch));
        const char *p = sc.note;
        int ln = 0;
        double ty = cy + 8;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n > 70) n = 70;
            if (ln >= sc.note_scroll) {
                memcpy(line, p, n);
                line[n] = 0;
                text(c, sc.mono, cx + 10, ty, 0.08, 0.08, 0.1, line);
                ty += 17;
            }
            ln++;
            p = e ? e + 1 : p + strlen(p);
        }
        /* caret blinks at 2 Hz */
        if (((int)(t * 2)) & 1) fill(c, cx + 10, ty, 8, 15, 0.1, 0.1, 0.1, 1);
        CGContextRestoreGState(c);
        break;
    }
    case 1:     /* the input log */
        fill(c, cx, cy, cw, ch, 0.07, 0.07, 0.09, 1);
        for (int i = 0; i < screen_log_n; i++)
            text(c, sc.mono, cx + 8, cy + 6 + i * 16, 0.55, 0.95, 0.55, screen_log[i]);
        snprintf(line, sizeof line, "pointer %d,%d buttons 0x%x mods 0x%03x", sc.px, sc.py, sc.buttons, sc.mods);
        fill(c, cx, cy + ch - 22, cw, 22, 0.15, 0.15, 0.2, 1);
        text(c, sc.mono, cx + 8, cy + ch - 19, 0.9, 0.9, 0.6, line);
        break;
    case 2: {   /* stats + spinner */
        fill(c, cx, cy, cw, ch, 0.98, 0.98, 1, 1);
        snprintf(line, sizeof line, "frames sent %llu", (unsigned long long)sc.frames_sent);
        text(c, sc.ui, cx + 12, cy + 12, 0.1, 0.1, 0.2, line);
        snprintf(line, sizeof line, "display %ux%u", sc.W, sc.H);
        text(c, sc.ui, cx + 12, cy + 32, 0.1, 0.1, 0.2, line);
        double a = t * 2 * M_PI, r = 40, ox = cx + cw - 70, oy = cy + ch / 2;
        CGContextSetRGBStrokeColor(c, 0.2, 0.45, 0.9, 1);
        CGContextSetLineWidth(c, 6);
        CGContextSetLineCap(c, kCGLineCapRound);
        CGContextAddArc(c, ox, oy, r, a, a + 4.2, 0);
        CGContextStrokePath(c);
        /* a progress bar */
        double f = fmod(t / 4, 1.0);
        fill(c, cx + 12, cy + ch - 30, cw - 140, 10, 0.85, 0.87, 0.92, 1);
        fill(c, cx + 12, cy + ch - 30, (cw - 140) * f, 10, 0.2, 0.6, 0.3, 1);
        break;
    }
    default:    /* "This PC": static icons, solid areas (SOLID encoding) */
        fill(c, cx, cy, cw, ch, 1, 1, 1, 1);
        for (int i = 0; i < 4; i++) {
            double ix = cx + 14 + (i % 2) * 120, iy = cy + 14 + (i / 2) * 70;
            fill(c, ix, iy, 40, 32, 0.95, 0.75, 0.25, 1);
            static const char *names[] = { "Local Disk (C:)", "Starbite (S:)", "Documents", "Downloads" };
            text(c, sc.ui, ix, iy + 38, 0.1, 0.1, 0.1, names[i]);
        }
        break;
    }
}

static void draw_cursor(CGContextRef c, double x, double y)
{
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, x, y);
    CGContextAddLineToPoint(c, x, y + 19);
    CGContextAddLineToPoint(c, x + 5, y + 14);
    CGContextAddLineToPoint(c, x + 9, y + 22);
    CGContextAddLineToPoint(c, x + 12, y + 21);
    CGContextAddLineToPoint(c, x + 8, y + 13);
    CGContextAddLineToPoint(c, x + 14, y + 13);
    CGContextClosePath(c);
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextSetRGBStrokeColor(c, 0, 0, 0, 1);
    CGContextSetLineWidth(c, 1);
    CGContextDrawPath(c, kCGPathFillStroke);
}

static void render(double t)
{
    memcpy(sc.cur, sc.wall, (size_t)sc.W * sc.H * 4);
    CGContextRef c = sc.ctx;
    /* automatic motion for windows nobody dragged */
    win_t *n = &sc.win[0], *s = &sc.win[2];
    if (!n->pinned) {
        n->x = sc.W * 0.08 + 120 * sin(t * 0.6);
        n->y = sc.H * 0.18 + 60 * sin(t * 0.9);
    }
    s->x = sc.W * 0.40 + 160 * sin(t * 0.35 + 1);
    s->y = sc.H * 0.58 + 30 * cos(t * 0.5);
    for (int i = sc.nwin - 1; i >= 0; i--) draw_window(c, &sc.win[i], t, i == 0);   /* notepad on top */
    /* taskbar */
    double tb = 48, ty = sc.H - tb;
    fill(c, 0, ty, sc.W, tb, 0.12, 0.13, 0.17, 1);
    double ix = sc.W / 2 - 3 * 44;
    const double cols[6][3] = { { 0.2, 0.55, 0.95 }, { 0.95, 0.75, 0.2 }, { 0.3, 0.75, 0.4 },
                                { 0.85, 0.3, 0.3 }, { 0.6, 0.4, 0.9 }, { 0.9, 0.9, 0.9 } };
    for (int i = 0; i < 6; i++) {
        CGContextSetRGBFillColor(c, cols[i][0], cols[i][1], cols[i][2], 1);
        rounded(c, ix + i * 44 + 6, ty + 8, 32, 32, 6);
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char clk[32];
    strftime(clk, sizeof clk, "%H:%M:%S", &tmv);
    text(c, sc.ui, sc.W - 90, ty + 8, 1, 1, 1, clk);
    strftime(clk, sizeof clk, "%Y-%m-%d", &tmv);
    text(c, sc.ui, sc.W - 90, ty + 26, 0.8, 0.8, 0.85, clk);
    draw_cursor(c, sc.px, sc.py);
    CGContextFlush(c);
}

/* ---- input from the viewer ------------------------------------------------------------------------- */

static void note_append(const char *s)
{
    size_t have = strlen(sc.note), add = strlen(s);
    if (have + add + 1 > sizeof sc.note) {   /* keep the tail */
        size_t drop = have + add + 1 - sizeof sc.note;
        memmove(sc.note, sc.note + drop, have - drop + 1);
        have -= drop;
    }
    memcpy(sc.note + have, s, add + 1);
}

static char hid_char(uint16_t u, int shift)
{
    if (u >= 0x04 && u <= 0x1D) return (char)((shift ? 'A' : 'a') + (u - 0x04));
    static const char dig[] = "1234567890", sdig[] = "!@#$%^&*()";
    if (u >= 0x1E && u <= 0x27) return shift ? sdig[u - 0x1E] : dig[u - 0x1E];
    static const char pun[] = "-=[]\\#;'`,./", spun[] = "_+{}|~:\"~<>?";
    if (u >= 0x2D && u <= 0x38) return shift ? spun[u - 0x2D] : pun[u - 0x2D];
    if (u == 0x2C) return ' ';
    return 0;
}

static void on_key(const sblk_input_key *k)
{
    say(1, "KEY page=0x%02x usage=0x%02x %s mods=0x%03x", k->usage_page, k->usage, k->down ? "down" : "up",
        k->modifiers);
    sc.mods = k->modifiers;
    if (!k->down || k->usage_page != SBLK_HID_PAGE_KEYBOARD) return;
    int shift = (k->modifiers & (SBLK_MOD_LSHIFT | SBLK_MOD_RSHIFT)) != 0;
    if (k->modifiers & SBLK_MOD_CAPS) shift = !shift;   /* letters only would be exact; close enough */
    if (k->modifiers & (SBLK_MOD_LCTRL | SBLK_MOD_RCTRL | SBLK_MOD_LGUI | SBLK_MOD_RGUI | SBLK_MOD_LALT)) return;
    if (k->usage == 0x28) { note_append("\n"); return; }
    if (k->usage == 0x2A) { size_t n = strlen(sc.note); if (n) sc.note[n - 1] = 0; return; }
    char ch = hid_char(k->usage, shift);
    if (ch) { char s[2] = { ch, 0 }; note_append(s); }
}

static int hit_title(int i, int32_t x, int32_t y)
{
    win_t *w = &sc.win[i];
    return x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + 32;
}

static void on_pointer(const sblk_input_pointer *p)
{
    int changed = p->buttons != sc.buttons || p->wheel_dx || p->wheel_dy;
    /* every event goes to the log file; the screen log only gets clicks and wheel (moves would flood it) */
    say(changed, "PTR x=%d y=%d buttons=0x%x wheel=%d,%d", p->x, p->y, p->buttons, p->wheel_dx, p->wheel_dy);
    int32_t x = p->x < 0 ? 0 : p->x >= (int32_t)sc.W ? (int32_t)sc.W - 1 : p->x;   /* the source clamps */
    int32_t y = p->y < 0 ? 0 : p->y >= (int32_t)sc.H ? (int32_t)sc.H - 1 : p->y;
    int left_now = p->buttons & SBLK_BTN_LEFT, left_was = sc.buttons & SBLK_BTN_LEFT;
    if (left_now && !left_was && hit_title(0, x, y)) {
        sc.drag = 1;
        sc.drag_win = 0;
        sc.drag_dx = x - sc.win[0].x;
        sc.drag_dy = y - sc.win[0].y;
        sc.win[0].pinned = 1;
    }
    if (!left_now) sc.drag = 0;
    if (sc.drag) {
        sc.win[sc.drag_win].x = x - sc.drag_dx;
        sc.win[sc.drag_win].y = y - sc.drag_dy;
    }
    if (p->wheel_dy) {
        sc.wheel_acc += p->wheel_dy;
        while (sc.wheel_acc >= 120) { sc.wheel_acc -= 120; if (sc.note_scroll > 0) sc.note_scroll--; }
        while (sc.wheel_acc <= -120) { sc.wheel_acc += 120; sc.note_scroll++; }
    }
    sc.px = x;
    sc.py = y;
    sc.buttons = p->buttons;
}

/* ---- the link ------------------------------------------------------------------------------------------- */

static struct {
    int fd;
    sblk_parser parser;
    uint8_t *pbuf;
    int hello;                /* viewer's HELLO received */
    sblk_hello peer;
    uint32_t seq, next_id, last_sent, last_acked;
    int keyframe;
    int faulted;              /* fault already used */
    int stalled;              /* stall/corrupt in progress: send nothing */
    int sessions;
    uint64_t blocked, bytes;
} L = { .fd = -1 };

static uint8_t *outbuf, *scratch;
static size_t outcap;
static _Alignas(8) uint8_t lz4state[SBLK_LZ4_STATE_SIZE];

static int send_all(const uint8_t *p, size_t n)
{
    L.bytes += n;
    while (n) {
        ssize_t k = send(L.fd, p, n, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void drop_client(const char *why)
{
    if (L.fd < 0) return;
    say(1, "viewer gone: %s", why);
    close(L.fd);
    L.fd = -1;
    L.hello = 0;
    L.stalled = 0;
}

static int send_msg(sblk_msg *m)
{
    uint8_t buf[SBLK_HDR_SIZE + 512];
    m->seq = L.seq++;
    long n = sblk_encode(m, buf, sizeof buf);
    if (n < 0) { say(0, "encode %s: %s", sblk_type_name(m->type), sblk_strerror((int)n)); return -1; }
    if (send_all(buf, (size_t)n) < 0) { drop_client("send failed"); return -1; }
    return 0;
}

static void outbuf_fit(void)
{
    size_t need = (size_t)sc.W * sc.H * 4 + (size_t)MAX_STRIPS * 64 + 4096;
    if (need <= outcap) return;
    free(outbuf);
    outbuf = malloc(need);
    outcap = need;
    if (!outbuf) { fprintf(stderr, "out of memory\n"); exit(1); }
}

static void send_hello(void)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello.role = SBLK_ROLE_SOURCE;
    m.u.hello.proto_major = SBLK_PROTO_MAJOR;
    m.u.hello.proto_minor = SBLK_PROTO_MINOR;
    m.u.hello.caps = SBLK_CAP_V1_ALL | SBLK_CAP_EXTEND;
    m.u.hello.width = (uint16_t)sc.W;
    m.u.hello.height = (uint16_t)sc.H;
    m.u.hello.refresh_mhz = (uint32_t)opt.fps * 1000;
    m.u.hello.pixel_format = SBLK_PIXFMT_BGRA8888;
    m.u.hello.max_payload = SBLK_MAX_PAYLOAD;
    snprintf(m.u.hello.name, sizeof m.u.hello.name, "%s", "Starbite S9 (fake)");
    send_msg(&m);
}

static void on_mode(const sblk_mode *md)
{
    uint32_t w = md->width, h = md->height;
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    if (w > 3840) w = 3840;
    if (h > 2160) h = 2160;
    w &= ~1u;
    h &= ~1u;
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_MODE;
    m.u.mode.width = (uint16_t)w;
    m.u.mode.height = (uint16_t)h;
    m.u.mode.refresh_mhz = (uint32_t)opt.fps * 1000;
    m.u.mode.status = SBLK_MODE_APPLIED;
    say(1, "MODE REQUEST %ux%u -> APPLIED %ux%u", md->width, md->height, w, h);
    if (w != sc.W || h != sc.H) {
        scene_init(w, h);
        outbuf_fit();
    }
    L.keyframe = 1;
    send_msg(&m);
}

static void handle(const sblk_msg *m)
{
    if (!L.hello && m->type != SBLK_T_HELLO && m->type != SBLK_T_BYE) {
        say(1, "protocol error: %s before HELLO", sblk_type_name(m->type));
        drop_client("protocol error");
        return;
    }
    switch (m->type) {
    case SBLK_T_HELLO: {
        char name[33];
        memcpy(name, m->u.hello.name, 32);
        name[32] = 0;
        say(1, "HELLO from \"%s\" role %u caps 0x%02x wants %ux%u", name, m->u.hello.role, m->u.hello.caps,
            m->u.hello.width, m->u.hello.height);
        if (m->u.hello.role != SBLK_ROLE_VIEWER || m->u.hello.proto_major != SBLK_PROTO_MAJOR) {
            drop_client("bad HELLO");
            return;
        }
        L.peer = m->u.hello;
        L.hello = 1;
        L.seq = 0;            /* a HELLO starts a new session, sequence numbers too */
        L.next_id = 1;
        L.last_sent = L.last_acked = 0;
        L.keyframe = 1;
        L.sessions++;
        send_hello();
        break;
    }
    case SBLK_T_MODE:
        if (m->u.mode.status == SBLK_MODE_REQUEST) on_mode(&m->u.mode);
        break;
    case SBLK_T_ACK:
        if (m->u.ack.frame_id == 0) { L.keyframe = 1; say(1, "ACK(0): keyframe requested"); break; }
        if ((int32_t)(m->u.ack.frame_id - L.last_acked) > 0) L.last_acked = m->u.ack.frame_id;
        break;
    case SBLK_T_INPUT_KEY: on_key(&m->u.key); break;
    case SBLK_T_INPUT_POINTER: on_pointer(&m->u.pointer); break;
    case SBLK_T_INPUT_TEXT: {
        char t[SBLK_TEXT_MAX + 1];
        memcpy(t, m->u.text.utf8, m->u.text.len);
        t[m->u.text.len] = 0;
        say(1, "TEXT \"%s\"", t);
        note_append(t);
        break;
    }
    case SBLK_T_PING: {
        if (L.stalled) break;
        sblk_msg r;
        memset(&r, 0, sizeof r);
        r.type = SBLK_T_PONG;
        r.u.pong.id = m->u.ping.id;
        r.u.pong.echo_us = m->u.ping.t_us;
        r.u.pong.t_us = mono_us();
        if (opt.verbose) say(0, "PING %u", m->u.ping.id);
        send_msg(&r);
        break;
    }
    case SBLK_T_PONG: break;
    case SBLK_T_BYE: {
        char t[SBLK_BYE_TEXT_MAX + 1];
        memcpy(t, m->u.bye.text.utf8 ? (const char *)m->u.bye.text.utf8 : "", m->u.bye.text.len);
        t[m->u.bye.text.len] = 0;
        say(1, "BYE received reason=%u text=\"%s\"", m->u.bye.reason, t);
        uint32_t reason = m->u.bye.reason;
        drop_client("BYE");
        if (opt.exit_on_bye && (reason == SBLK_BYE_NORMAL || reason == SBLK_BYE_SHUTDOWN)) {
            say(0, "exit on BYE: %d session(s)", L.sessions);
            exit(0);
        }
        break;
    }
    default: break;
    }
}

static void read_client(void)
{
    uint8_t buf[65536];
    ssize_t n = recv(L.fd, buf, sizeof buf, 0);
    if (n <= 0) { drop_client(n == 0 ? "closed" : strerror(errno)); return; }
    if (L.stalled && opt.fault_kind == 2) return;   /* stall: read and ignore until the viewer gives up */
    const uint8_t *p = buf;
    size_t left = (size_t)n;
    while (left && L.fd >= 0) {
        size_t used = 0;
        sblk_msg m;
        int r = sblk_parser_feed(&L.parser, p, left, &used, &m);
        p += used;
        left -= used;
        if (r < 0) {
            say(1, "protocol error from the viewer: %s", sblk_strerror(r));
            sblk_msg b;
            memset(&b, 0, sizeof b);
            b.type = SBLK_T_BYE;
            b.u.bye.reason = SBLK_BYE_PROTOCOL;
            send_msg(&b);
            drop_client("protocol error");
            return;
        }
        if (r == 0) break;
        handle(&m);
    }
}

static void maybe_fault(void)
{
    if (!opt.fault_kind || L.faulted || L.next_id - 1 != (uint32_t)opt.fault_at) return;
    L.faulted = 1;
    if (opt.fault_kind == 1) { say(1, "FAULT drop after frame %d", opt.fault_at); drop_client("fault: drop"); }
    else if (opt.fault_kind == 2) { say(1, "FAULT stall after frame %d", opt.fault_at); L.stalled = 1; }
    else if (opt.fault_kind == 4) {
        uint32_t w = sc.H, h = sc.W;
        say(1, "FAULT rotate after frame %d: %ux%u -> %ux%u", opt.fault_at, sc.W, sc.H, w, h);
        scene_init(w, h);
        outbuf_fit();
        L.keyframe = 1;
        sblk_msg m;
        memset(&m, 0, sizeof m);
        m.type = SBLK_T_MODE;
        m.u.mode.width = (uint16_t)w;
        m.u.mode.height = (uint16_t)h;
        m.u.mode.refresh_mhz = (uint32_t)opt.fps * 1000;
        m.u.mode.status = SBLK_MODE_APPLIED;
        send_msg(&m);
    } else {
        say(1, "FAULT corrupt after frame %d", opt.fault_at);
        uint8_t junk[64];
        for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 37 + 11);
        send_all(junk, sizeof junk);
        L.stalled = 1;
    }
}

static void send_frame(void)
{
    if (L.fd < 0 || !L.hello || L.stalled) return;
    if (sblk_in_flight(L.last_sent, L.last_acked) >= SBLK_MAX_IN_FLIGHT) { L.blocked++; return; }
    double t = (double)mono_us() / 1e6 - sc.t0;
    render(t);
    uint64_t cap_us = mono_us();

    sblk_rect rects[MAX_RECTS], strips[MAX_STRIPS];
    int nr;
    int key = L.keyframe;
    size_t stride = (size_t)sc.W * 4;
    if (key) {
        rects[0] = (sblk_rect){ 0, 0, (uint16_t)sc.W, (uint16_t)sc.H };
        nr = 1;
        memcpy(sc.prev, sc.cur, stride * sc.H);
    } else {
        nr = sblk_dirty_rects(sc.prev, sc.cur, stride, sc.W, sc.H, NULL, SBLK_DIRTY_UPDATE_PREV, rects, MAX_RECTS);
        if (nr <= 0) return;   /* nothing changed: no frame */
    }
    uint32_t maxp = L.peer.max_payload && L.peer.max_payload < SBLK_MAX_PAYLOAD ? L.peer.max_payload : SBLK_MAX_PAYLOAD;
    int ns = 0;
    for (int i = 0; i < nr; i++) {
        int k = sblk_split_rect(&rects[i], maxp, strips + ns, MAX_STRIPS - ns);
        if (k < 0) { say(0, "too many strips"); return; }
        ns += k;
    }
    uint32_t id = L.next_id;
    uint32_t seq_begin = L.seq++;
    size_t off = SBLK_HDR_SIZE + SBLK_FRAME_BEGIN_SIZE;
    uint32_t enc_n[3] = { 0, 0, 0 };
    for (int i = 0; i < ns; i++) {
        uint8_t enc = 0;
        long n = sblk_build_rect(outbuf + off, outcap - off, sc.cur, stride, &strips[i], L.peer.caps, 1, lz4state,
                                 scratch, L.seq++, &enc);
        if (n < 0) { say(0, "build_rect: %s", sblk_strerror((int)n)); drop_client("internal error"); return; }
        if (enc < 3) enc_n[enc]++;
        off += (size_t)n;
    }
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_FRAME_BEGIN;
    m.seq = seq_begin;
    m.u.frame_begin = (sblk_frame_begin){ id, (uint16_t)sc.W, (uint16_t)sc.H, (uint16_t)ns, key ? SBLK_FB_KEYFRAME : 0 };
    sblk_encode(&m, outbuf, SBLK_HDR_SIZE + SBLK_FRAME_BEGIN_SIZE);
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_FRAME_END;
    m.seq = L.seq++;
    m.u.frame_end.frame_id = id;
    m.u.frame_end.capture_us = cap_us;
    long n = sblk_encode(&m, outbuf + off, outcap - off);
    off += (size_t)n;

    if (crcf) {
        fprintf(crcf, "%u %ux%u %016llx\n", id, sc.W, sc.H,
                (unsigned long long)sbv_fb_hash(sc.cur, stride, sc.W, sc.H));
        fflush(crcf);
    }
    if (opt.verbose)
        say(0, "frame %u: %d rects (raw %u lz4 %u solid %u) %zu bytes%s", id, ns, enc_n[0], enc_n[1], enc_n[2], off,
            key ? " KEY" : "");
    if (send_all(outbuf, off) < 0) { drop_client("send failed"); return; }
    L.keyframe = 0;
    L.last_sent = id;
    L.next_id = id == 0xFFFFFFFFu ? 1 : id + 1;
    sc.frames_sent++;
    maybe_fault();
}

static int parse_fault(const char *s)
{
    const char *c = strchr(s, ':');
    if (!c) return -1;
    opt.fault_at = atoi(c + 1);
    if (!strncmp(s, "drop", 4)) opt.fault_kind = 1;
    else if (!strncmp(s, "stall", 5)) opt.fault_kind = 2;
    else if (!strncmp(s, "corrupt", 7)) opt.fault_kind = 3;
    else if (!strncmp(s, "rotate", 6)) opt.fault_kind = 4;
    else return -1;
    return opt.fault_at > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--port") && v) { opt.port = atoi(v); i++; }
        else if (!strcmp(a, "--size") && v && sscanf(v, "%dx%d", &opt.w, &opt.h) == 2) i++;
        else if (!strcmp(a, "--fps") && v) { opt.fps = atoi(v); i++; }
        else if (!strcmp(a, "--crc-file") && v) { opt.crc_file = v; i++; }
        else if (!strcmp(a, "--log") && v) { opt.log_file = v; i++; }
        else if (!strcmp(a, "--fault") && v && parse_fault(v) == 0) i++;
        else if (!strcmp(a, "--exit-on-bye")) opt.exit_on_bye = 1;
        else if (!strcmp(a, "--verbose")) opt.verbose = 1;
        else {
            fprintf(stderr, "usage: %s [--port N] [--size WxH] [--fps N] [--crc-file F] [--log F]\n"
                            "          [--fault drop:N|stall:N|corrupt:N|rotate:N] [--exit-on-bye] [--verbose]\n", argv[0]);
            return 2;
        }
    }
    if (opt.fps < 1) opt.fps = 1;
    signal(SIGPIPE, SIG_IGN);
    logf_ = opt.log_file ? fopen(opt.log_file, "w") : stderr;
    if (!logf_) { perror(opt.log_file); return 1; }
    if (opt.crc_file && !(crcf = fopen(opt.crc_file, "w"))) { perror(opt.crc_file); return 1; }

    scene_init((uint32_t)opt.w, (uint32_t)opt.h);
    outbuf_fit();
    scratch = malloc(SBLK_MAX_PAYLOAD);
    L.pbuf = malloc(SBLK_MAX_PAYLOAD);
    if (!scratch || !L.pbuf) return 1;
    sblk_parser_init(&L.parser, L.pbuf, SBLK_MAX_PAYLOAD);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)opt.port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* a test tool: loopback only */
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 4) < 0) { perror("bind/listen"); return 1; }
    say(0, "fakesender: listening on 127.0.0.1:%d, %ux%u @ %d fps", opt.port, sc.W, sc.H, opt.fps);

    uint64_t period = 1000000u / (uint64_t)opt.fps, next = mono_us() + period;
    for (;;) {
        struct pollfd pf[2] = { { ls, POLLIN, 0 }, { L.fd, POLLIN, 0 } };
        uint64_t t = mono_us();
        int to = next > t ? (int)((next - t + 999) / 1000) : 0;
        int r = poll(pf, L.fd >= 0 ? 2 : 1, to);
        if (r < 0 && errno != EINTR) { perror("poll"); return 1; }
        if (r > 0 && (pf[0].revents & POLLIN)) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
                if (L.fd >= 0) {
                    /* one viewer at a time (PROTOCOL.md section 1) */
                    uint8_t b[64];
                    sblk_msg m;
                    memset(&m, 0, sizeof m);
                    m.type = SBLK_T_BYE;
                    m.u.bye.reason = SBLK_BYE_BUSY;
                    long n = sblk_encode(&m, b, sizeof b);
                    if (n > 0) (void)!send(c, b, (size_t)n, 0);
                    close(c);
                    say(1, "second viewer refused: BYE(BUSY)");
                } else {
                    L.fd = c;
                    L.hello = 0;
                    L.stalled = 0;
                    sblk_parser_reset(&L.parser);
                    say(1, "viewer connected");
                }
            }
        }
        if (r > 0 && L.fd >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) read_client();
        t = mono_us();
        if (t >= next) {
            send_frame();
            next += period;
            if (next < t) next = t + period;   /* fell behind: do not burst */
        }
    }
}
