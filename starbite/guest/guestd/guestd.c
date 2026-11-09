/* SPDX-License-Identifier: GPL-2.0-only
 * guestd: the guest Linux's side of the Starbite OS 0.9 shared-memory contract (starbite/guest/sbshm.h,
 * docs/STARBITE-OS-LINUX-M3.md 5, 6). Static, started by the guest initramfs.
 *   - maps the shared region: /dev/sbshm (sbguest.ko: write-combine mmap, doorbells) or /dev/mem O_SYNC
 *     at /proc/device-tree/chosen/starbite,shm (stock kernel: polling);
 *   - says HELLO, then runs a demo "app": it draws into the back surface buffer and publishes it
 *     (surf_front, surf_seq); per-CPU load bars from /proc/stat show the four M3 cores working;
 *   - forwards Starbite's touch and key messages to a uinput device ("Starbite touch") and marks each touch
 *     in the app (a gold dot: sbsim checks the pixel);
 *   - on MSG_STOP: BYE, sync, power off. With sbguest.ko the power-off path is PSCI CPU_OFF on every guest
 *     CPU (the cluster goes dark, Starbite OS keeps running); without it a power off would be PSCI
 *     SYSTEM_OFF (the whole phone), so guestd only takes CPUs 1..3 offline and idles.
 * Messages go to /dev/kmsg, so they show in the guest's log (the ramoops zone Starbite OS reads).
 *   guestd [--burn N]   N threads spinning (heat / scheduler test on the M3s)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../sbshm.h"

static volatile struct sbshm_hdr *hdr;
static uint8_t *shm;
static struct sbshm_ring *h2g, *g2h;
static int sbfd = -1, uifd = -1, kfd = -1;

static void say(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + 8, sizeof(b) - 8, fmt, ap);
    va_end(ap);
    memcpy(b, "guestd: ", 8);
    n += 8;
    if (n > (int)sizeof(b) - 1)
        n = sizeof(b) - 1;
    if (kfd >= 0)
        (void)!write(kfd, b, (size_t)n);
    b[n] = 0;
    fprintf(stderr, "%s\n", b);
}

static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* ---- the shared region ----------------------------------------------------------------------------- */
static int map_shm(void)
{
    uint64_t base = 0, size = 0;
    unsigned char be[16];
    int fd = open("/proc/device-tree/chosen/starbite,shm", O_RDONLY);
    if (fd < 0 || read(fd, be, 16) != 16) {
        say("no /chosen/starbite,shm in the device tree");
        return -1;
    }
    close(fd);
    for (int i = 0; i < 8; ++i) {
        base = base << 8 | be[i];
        size = size << 8 | be[8 + i];
    }
    sbfd = open("/dev/sbshm", O_RDWR);
    if (sbfd >= 0) {
        shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, sbfd, 0);
        say("shared region %#llx + %#llx through /dev/sbshm (doorbells)", (unsigned long long)base,
            (unsigned long long)size);
    } else {
        fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) {
            say("/dev/mem: %s", strerror(errno));
            return -1;
        }
        shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
        say("shared region %#llx + %#llx through /dev/mem (polling)", (unsigned long long)base,
            (unsigned long long)size);
    }
    if (shm == MAP_FAILED) {
        say("mmap: %s", strerror(errno));
        return -1;
    }
    hdr = (volatile struct sbshm_hdr *)(shm + SBSHM_HDR_OFF);
    h2g = (struct sbshm_ring *)(shm + SBSHM_H2G_OFF);
    g2h = (struct sbshm_ring *)(shm + SBSHM_G2H_OFF);
    for (int i = 0; i < 50 && hdr->magic != SBSHM_MAGIC; ++i)
        usleep(100000);
    if (hdr->magic != SBSHM_MAGIC || hdr->version != SBSHM_VERSION) {
        say("bad header (magic %#llx version %u)", (unsigned long long)hdr->magic, hdr->version);
        return -1;
    }
    return 0;
}

static void g2h_send(uint16_t type, uint32_t a, uint32_t b)
{
    struct sbshm_msg m = {.type = type, .a = a, .b = b, .t_ms = (uint32_t)ms_now()};
    if (!sbshm_put(g2h, &m))
        say("g2h ring full");
    if (sbfd >= 0)
        (void)!write(sbfd, "1", 1);         /* sbguest.ko pends the host's doorbell SPI */
}

/* ---- uinput: Starbite's touch and keys as a Linux input device -------------------------------------- */
static void ui_setup(uint32_t w, uint32_t h)
{
    uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uifd < 0) {
        say("no /dev/uinput (%s): touches only reach this app", strerror(errno));
        return;
    }
    ioctl(uifd, UI_SET_EVBIT, EV_KEY);
    ioctl(uifd, UI_SET_EVBIT, EV_ABS);
    ioctl(uifd, UI_SET_EVBIT, EV_SYN);
    ioctl(uifd, UI_SET_KEYBIT, BTN_TOUCH);
    static const int keys[] = {KEY_BACK, KEY_HOMEPAGE, KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MENU};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        ioctl(uifd, UI_SET_KEYBIT, keys[i]);
    ioctl(uifd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    struct uinput_abs_setup a = {0};
    int codes[] = {ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_X, ABS_Y};
    int maxs[] = {9, 65535, (int)w - 1, (int)h - 1, (int)w - 1, (int)h - 1};
    for (int i = 0; i < 6; ++i) {
        ioctl(uifd, UI_SET_ABSBIT, codes[i]);
        a.code = (uint16_t)codes[i];
        a.absinfo.minimum = 0;
        a.absinfo.maximum = maxs[i];
        ioctl(uifd, UI_ABS_SETUP, &a);
    }
    struct uinput_setup s = {0};
    s.id.bustype = BUS_VIRTUAL;
    s.id.vendor = 0x5342;                   /* "SB" */
    s.id.product = 0x0009;
    snprintf(s.name, sizeof(s.name), "Starbite touch");
    if (ioctl(uifd, UI_DEV_SETUP, &s) < 0 || ioctl(uifd, UI_DEV_CREATE) < 0) {
        say("uinput setup: %s", strerror(errno));
        close(uifd);
        uifd = -1;
        return;
    }
    say("uinput device \"Starbite touch\" %ux%u + keys", w, h);
}

static void ui_ev(uint16_t type, uint16_t code, int32_t value)
{
    if (uifd < 0)
        return;
    struct input_event e = {.type = type, .code = code, .value = value};
    (void)!write(uifd, &e, sizeof(e));
}

/* ---- drawing ---------------------------------------------------------------------------------------- */
static uint32_t W, H, *fb;

static void rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < (int)H; ++j)
        for (int i = x < 0 ? 0 : x; i < x + w && i < (int)W; ++i)
            fb[(size_t)j * W + (size_t)i] = c;
}

static void dot(int cx, int cy, int r, uint32_t c)
{
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i)
            if (i * i + j * j <= r * r) {
                int x = cx + i, y = cy + j;
                if (x >= 0 && y >= 0 && x < (int)W && y < (int)H)
                    fb[(size_t)y * W + (size_t)x] = c;
            }
}

/* 5x7 glyphs, rows top to bottom, bit 4 = left */
static const char gl_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:%/+";
static const uint8_t gl_rows[][7] = {
    {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
    {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c},
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
    {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
    {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},
    {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
    {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},
    {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f},
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
    {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}, {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00},
    {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}, {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00},
};

static void text(int x, int y, int s, uint32_t c, const char *str)
{
    for (; *str; ++str, x += 6 * s) {
        char ch = *str >= 'a' && *str <= 'z' ? (char)(*str - 32) : *str;
        const char *p = strchr(gl_chars, ch);
        if (!p || ch == 0)
            continue;
        const uint8_t *g = gl_rows[p - gl_chars];
        for (int r = 0; r < 7; ++r)
            for (int b = 0; b < 5; ++b)
                if (g[r] & (0x10 >> b))
                    rect(x + b * s, y + r * s, s, s, c);
    }
}

/* ---- publishing (sbshm.h: damage per row) --------------------------------------------------------------
 * shadow[b] mirrors surface buffer b (both buffers are kept whole). A new frame: the rows that differ from the
 * back buffer's shadow are written there; the rows that differ from the front's get row_seq = the new seq (the
 * host copies only those); then the flip. Nothing changed: no frame. Returns 1 when a frame was published. */
static uint32_t *shadow[2];
static uint8_t *changed;
static uint64_t pub_frames, pub_rows;
static int publish(const uint32_t *src, uint32_t stride)
{
    volatile uint32_t *rows = (volatile uint32_t *)(shm + SBSHM_ROWS_OFF);
    if (!shadow[0]) {
        shadow[0] = malloc((size_t)W * H * 4);
        shadow[1] = malloc((size_t)W * H * 4);
        changed = malloc(H);
        if (!shadow[0] || !shadow[1] || !changed)
            return 0;
        memset(shadow[0], 0, (size_t)W * H * 4);       /* sbghost zeroed both buffers before the start */
        memset(shadow[1], 0, (size_t)W * H * 4);
        if (H <= SBSHM_ROWS_MAX)
            hdr->rows_valid = 1;
    }
    uint32_t front = hdr->surf_front & 1U, back = front ^ 1U, seq = hdr->surf_seq + 1U;
    size_t rb = (size_t)W * 4;
    uint32_t n = 0, y0 = H, y1 = 0;
    for (uint32_t y = 0; y < H; ++y) {
        changed[y] = memcmp(src + (size_t)y * W, shadow[front] + (size_t)y * W, rb) != 0;
        if (changed[y]) {
            ++n;
            if (y < y0)
                y0 = y;
            y1 = y + 1;
        }
    }
    if (!n && hdr->surf_seq)
        return 0;
    uint8_t *dst = shm + hdr->surf_off[back];
    for (uint32_t y = 0; y < H; ++y)
        if (memcmp(src + (size_t)y * W, shadow[back] + (size_t)y * W, rb)) {
            memcpy(dst + (size_t)y * stride, src + (size_t)y * W, rb);
            memcpy(shadow[back] + (size_t)y * W, src + (size_t)y * W, rb);
        }
    if (H <= SBSHM_ROWS_MAX)
        for (uint32_t y = 0; y < H; ++y)
            if (changed[y] || !hdr->surf_seq)
                rows[y] = seq;
    sbshm_dmb();
    hdr->surf_front = back;
    hdr->damage_x = 0;
    hdr->damage_w = W;
    hdr->damage_y = n ? y0 : 0;
    hdr->damage_h = n ? y1 - y0 : H;
    sbshm_dmb();
    hdr->surf_seq = seq;
    pub_frames++;
    pub_rows += n ? n : H;
    return 1;
}

/* ---- per-CPU load ----------------------------------------------------------------------------------- */
static unsigned long long prev_busy[8], prev_all[8];
static int cpu_load(int pct[8])
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned cpu;
        unsigned long long u, ni, sy, id, io, irq, sirq;
        if (sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu", &cpu, &u, &ni, &sy, &id, &io, &irq,
                   &sirq) != 8 || cpu >= 8)
            continue;
        unsigned long long busy = u + ni + sy + irq + sirq, all = busy + id + io;
        unsigned long long db = busy - prev_busy[cpu], da = all - prev_all[cpu];
        pct[cpu] = da ? (int)(db * 100 / da) : 0;
        prev_busy[cpu] = busy;
        prev_all[cpu] = all;
        if ((int)cpu + 1 > n)
            n = (int)cpu + 1;
    }
    fclose(f);
    return n;
}

static volatile uint64_t burn_until;          /* 0: forever (--burn), else ms_now() deadline (SBG_MSG_LOAD) */
static void *burn(void *arg)
{
    (void)arg;
    volatile uint64_t x = 1;
    uint64_t until = burn_until;
    for (unsigned i = 0;; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        if (until && !(i & 0xfffff) && ms_now() >= until)
            break;
    }
    return NULL;
}

/* ---- stop ------------------------------------------------------------------------------------------- */
static void stop(void)
{
    hdr->guest_state = SBG_GUEST_STOPPING;
    say("MSG_STOP: syncing and powering the M3 cluster off");
    g2h_send(SBG_MSG_BYE, 0, 0);
    sync();
    if (access("/sys/module/sbguest", F_OK) == 0) {
        reboot(RB_POWER_OFF);               /* sbguest.ko: CPU_OFF on every guest CPU */
        say("reboot(RB_POWER_OFF): %s", strerror(errno));
    }
    /* stock kernel: power off would be PSCI SYSTEM_OFF (the whole machine). Only the secondaries go. */
    for (int c = 1; c < 8; ++c) {
        char p[64];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/online", c);
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            (void)!write(fd, "0", 1);
            close(fd);
        }
    }
    hdr->guest_err = 1;
    say("no sbguest.ko: CPUs 1..3 offline, CPU 0 idles (the cluster stays powered)");
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    int nburn = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--burn"))
            nburn = atoi(argv[i + 1]);
    kfd = open("/dev/kmsg", O_WRONLY);
    if (map_shm() < 0)
        return 1;
    struct utsname u;
    uname(&u);
    for (unsigned i = 0; i < 63 && u.release[i]; ++i)
        hdr->guest_kver[i] = u.release[i];
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    hdr->guest_ncpus = (uint32_t)ncpu;
    hdr->guest_state = SBG_GUEST_UP;
    W = hdr->surf_w;
    H = hdr->surf_h;
    uint32_t stride = hdr->surf_stride;
    fb = calloc((size_t)W * H, 4);
    if (!fb || hdr->surf_format != SBSHM_FMT_XRGB8888 || stride < W * 4)
        return 1;
    ui_setup(W, H);
    for (int i = 0; i < nburn; ++i) {
        pthread_t t;
        pthread_create(&t, NULL, burn, NULL);
    }
    const char *title = "Linux on M3";
    for (unsigned i = 0; title[i]; ++i)
        hdr->title[i] = title[i];
    sbshm_dmb();
    hdr->guest_state = SBG_GUEST_RUNNING;
    g2h_send(SBG_MSG_HELLO, (uint32_t)ncpu, 1);
    say("HELLO: %d CPUs, kernel %s, surface %ux%u", ncpu, u.release, W, H);

    struct { int x, y; } dots[64];
    int ndots = 0, lastx = -1, lasty = -1, pct[8] = {0}, ncl = 0;
    char keyline[48] = "";
    uint64_t t0 = ms_now(), frame = 0, lastload = 0;
    int bx = 40, by = 400, vx = 7, vy = 5;
    for (;;) {
        /* 1. Starbite's messages */
        struct sbshm_msg m;
        while (sbshm_get(h2g, &m)) {
            if (m.type == SBG_MSG_TOUCH) {
                int x = m.x, y = m.y;
                if (m.b == SBG_TOUCH_MOVE && lastx >= 0)        /* the drag as a line of marks */
                    for (int k = 1; k <= 6 && ndots < 64; ++k)
                        dots[ndots].x = lastx + (x - lastx) * k / 6, dots[ndots++].y = lasty + (y - lasty) * k / 6;
                else if (m.b == SBG_TOUCH_DOWN && ndots < 64)
                    dots[ndots].x = x, dots[ndots++].y = y;
                lastx = m.b == SBG_TOUCH_UP ? -1 : x;
                lasty = y;
                ui_ev(EV_ABS, ABS_MT_SLOT, (int32_t)m.a);
                ui_ev(EV_ABS, ABS_MT_TRACKING_ID, m.b == SBG_TOUCH_UP ? -1 : (int32_t)m.a + 1);
                if (m.b != SBG_TOUCH_UP) {
                    ui_ev(EV_ABS, ABS_MT_POSITION_X, x);
                    ui_ev(EV_ABS, ABS_MT_POSITION_Y, y);
                    ui_ev(EV_ABS, ABS_X, x);
                    ui_ev(EV_ABS, ABS_Y, y);
                }
                ui_ev(EV_KEY, BTN_TOUCH, m.b != SBG_TOUCH_UP);
                ui_ev(EV_SYN, SYN_REPORT, 0);
                hdr->touches_seen++;
                if (m.b != SBG_TOUCH_MOVE)
                    say("touch %s at %d,%d", m.b == SBG_TOUCH_DOWN ? "down" : "up", x, y);
            } else if (m.type == SBG_MSG_KEY) {
                ui_ev(EV_KEY, (uint16_t)m.a, (int32_t)m.b);
                ui_ev(EV_SYN, SYN_REPORT, 0);
                snprintf(keyline, sizeof(keyline), "KEY %u %s", m.a, m.b ? "DOWN" : "UP");
                say("key %u %s", m.a, m.b ? "down" : "up");
            } else if (m.type == SBG_MSG_STOP) {
                stop();
            } else if (m.type == SBG_MSG_LOAD && m.a && m.a <= 16 && m.b && m.b <= 60) {
                burn_until = ms_now() + 1000ULL * m.b;
                for (uint32_t i = 0; i < m.a; ++i) {
                    pthread_t th;
                    if (pthread_create(&th, NULL, burn, NULL) == 0)
                        pthread_detach(th);
                }
                say("load: %u threads for %u s", m.a, m.b);
            }
        }
        /* 2. the frame */
        uint64_t t = ms_now();
        if (t - lastload >= 500) {
            ncl = cpu_load(pct);
            lastload = t;
        }
        for (uint32_t y = 0; y < H; ++y) {
            uint32_t g = 0x20 + y * 0x30 / H, c = (0x10U << 16) | (g << 8) | (0x30 + y * 0x40 / H);
            uint32_t *row = fb + (size_t)y * W;
            for (uint32_t x = 0; x < W; ++x)
                row[x] = c;
        }
        rect(0, 0, (int)W, 72, 0x001b5e20);
        text(24, 18, 6, 0x00ffffff, "LINUX ON M3");
        char line[96];
        snprintf(line, sizeof(line), "%s", u.release);
        line[30] = 0;
        text(24, 100, 3, 0x00e0f2f1, line);
        snprintf(line, sizeof(line), "CPUS %d  UP %llu.%llu S  FRAME %llu", ncpu,
                 (unsigned long long)((t - t0) / 1000), (unsigned long long)((t - t0) / 100 % 10),
                 (unsigned long long)frame);
        text(24, 140, 3, 0x00e0f2f1, line);
        for (int c = 0; c < ncl && c < 4; ++c) {
            int y = 200 + c * 56;
            snprintf(line, sizeof(line), "CPU%d %3d%%", c, pct[c]);
            text(24, y + 8, 3, 0x00ffffff, line);
            rect(220, y, (int)W - 260, 36, 0x00263238);
            rect(220, y, ((int)W - 260) * pct[c] / 100, 36, pct[c] > 80 ? 0x00ef6c00 : 0x0026a69a);
        }
        if (keyline[0])
            text(24, 440, 3, 0x00ffd180, keyline);
        bx += vx;
        by += vy;
        if (bx < 0 || bx > (int)W - 80) vx = -vx;
        if (by < 480 || by > (int)H - 80) vy = -vy;
        rect(bx, by, 80, 80, 0x00e91e63);
        for (int i = 0; i < ndots; ++i)
            dot(dots[i].x, dots[i].y, 14, 0x00ffd700);
        /* 3. publish: only the rows that changed (sbshm.h "damage per row") */
        if (publish(fb, stride))
            frame++;
        hdr->guest_heartbeat++;
        hdr->guest_uptime_ms = t - t0;
        /* 4. wait: the doorbell (sbguest.ko) or 50 ms */
        if (sbfd >= 0) {
            struct pollfd p = {.fd = sbfd, .events = POLLIN};
            if (poll(&p, 1, 50) > 0) {
                uint32_t n;
                (void)!read(sbfd, &n, sizeof(n));
            }
        } else {
            usleep(50000);
        }
    }
}
