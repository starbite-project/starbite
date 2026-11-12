/* SPDX-License-Identifier: GPL-2.0-only
 * test-snd: sbg_snd (the host side of sbsnd) on the Mac / any host, with a fake guest that writes the region the way
 * linux/sbsnd.c does. Builds the real sbghost.c (sbg_snd.c is included at its end) over a malloc'd shared region:
 *   cd starbite/guest/host && cc -O2 -Wall -Wextra -I.. test-snd.c -o /tmp/test-snd -lpthread && /tmp/test-snd
 * Covers: the layout at load, pull (data, silence, underrun episodes + the doorbell, priming per start), the ring
 * wrap and the 2^32 position wrap, drop (play_discard) and drain, a guest with impossible positions, volume / mute /
 * saturation, push (only while capturing, overrun, wrap), the `active` hint, and a two-thread run (guest writer and
 * host puller / pusher on different cores) checking every frame's sequence number. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbghost.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                     printf("\n"); } } while (0)

static uint8_t *shm;
static uint32_t gicd_fake[1024];
static uint64_t fake_ms;
static int act_calls, act_play, act_cap;

static uint64_t t_ms(void) { return fake_ms; }
static void t_log(const char *l) { printf("  log: %s\n", l); }
static void t_active(int p, int c) { act_calls++; act_play = p; act_cap = c; }
static const struct sbg_snd plat = { .caps = SBG_SND_PLAY | SBG_SND_CAPTURE, .block_frames = 96,
                                     .latency_frames = 480, .active = t_active };
static const struct sbg_os tos = { .ms = t_ms, .log = t_log, .snd = &plat };

/* ---- the fake guest (what linux/sbsnd.c does) ------------------------------------------------------------ */
static volatile struct sbshm_snd *S;
static uint32_t g_appl, g_cap_appl;
static uint8_t *ring_p, *ring_c;

static void g_play_start(void)
{
    S->play_starts++;
    S->play_state = SBSHM_SND_RUN;
}
static void g_write(uint32_t frames, uint32_t seq0)     /* frame i = (seq0 + i) in both halves' sum */
{
    for (uint32_t i = 0; i < frames; ++i)
        *(volatile uint32_t *)(ring_p + ((g_appl + 4 * i) & (SBSHM_SND_RING - 1))) = seq0 + i;
    sbshm_dmb();
    g_appl += frames * 4;
    S->play_appl = g_appl;
}
static uint32_t frame_of(const int16_t *lr, unsigned i) { return (uint16_t)lr[2 * i] | (uint32_t)(uint16_t)lr[2 * i + 1] << 16; }

static unsigned g_read(uint32_t *out, unsigned max)       /* capture: what the guest's timer would move */
{
    uint32_t hw = S->cap_hw, n = (hw - g_cap_appl) / 4;
    sbshm_dmb();
    if (n > max)
        n = max;
    for (uint32_t i = 0; i < n; ++i)
        out[i] = *(volatile uint32_t *)(ring_c + ((g_cap_appl + 4 * i) & (SBSHM_SND_RING - 1)));
    sbshm_dmb();
    g_cap_appl += n * 4;
    S->cap_appl = g_cap_appl;
    return n;
}

static void boot(void)
{
    memset(shm, 0, SBSHM_V3_FREE_OFF);
    struct sbg_cfg c = { .shm_base = (uintptr_t)shm, .shm_size = 0x4000000, .gicd = (uintptr_t)gicd_fake,
                         .db_guest = 132, .db_host = 133, .v2 = 1, .services = SBSHM_SVC_SND };
    sbg_init(&tos, &c, 1);
    HDR->version = SBSHM_VERSION_V3;            /* what load() writes */
    HDR->host_services = SBSHM_SVC_SND;
    sbg_snd_load();
    st.state = SBG_RUNNING;
    S = (volatile struct sbshm_snd *)(shm + SBSHM_SND_OFF);
    ring_p = shm + SBSHM_SND_OFF + SBSHM_SND_PLAY_OFF;
    ring_c = shm + SBSHM_SND_OFF + SBSHM_SND_CAP_OFF;
    g_appl = g_cap_appl = 0;
    act_calls = act_play = act_cap = 0;
    memset(gicd_fake, 0, sizeof(gicd_fake));
}

static int16_t buf[2 * 20000];
static uint32_t cbuf[20000];

static void t_layout(void)
{
    boot();
    CHECK(sizeof(struct sbshm_snd) == 384, "struct size %zu", sizeof(struct sbshm_snd));
    CHECK(SBSHM_SND_CAP_OFF + SBSHM_SND_RING <= 0x100000, "the rings leave the 1 MiB");
    CHECK(S->magic == SBSHM_SND_MAGIC && S->rate == 48000 && S->channels == 2 && S->format == SBSHM_SND_FMT_S16LE,
          "header %#x %u %u %u", S->magic, S->rate, S->channels, S->format);
    CHECK(S->ring_bytes == SBSHM_SND_RING && S->caps == 3 && S->host_block_frames == 96 && S->host_latency_frames == 480
          && S->host_volume == 0x10000, "caps %u block %u lat %u vol %#x", S->caps, S->host_block_frames,
          S->host_latency_frames, S->host_volume);
    /* the service off: nothing written, pull is silence */
    HDR->host_services = 0;
    memset(buf, 0x55, sizeof(buf));
    CHECK(sbg_snd_pull(buf, 100) == 0 && buf[0] == 0 && buf[199] == 0, "pull with the service off");
    HDR->host_services = SBSHM_SVC_SND;
}

static void t_pull_underrun(void)
{
    boot();
    memset(buf, 0x55, sizeof(buf));
    CHECK(sbg_snd_pull(buf, 64) == 0 && buf[127] == 0, "idle pull");
    g_play_start();
    CHECK(sbg_snd_pull(buf, 64) == 0 && sbg_snd_stats()->underruns == 0, "an empty ring before the first byte of a "
          "start is not an underrun (%u)", sbg_snd_stats()->underruns);
    g_write(1000, 7);
    unsigned got = 0, n;
    for (int k = 0; k < 3; ++k) {
        n = sbg_snd_pull(buf, 300);
        CHECK(n == 300, "pull %d got %u", k, n);
        for (unsigned i = 0; i < n; ++i)
            if (frame_of(buf, i) != 7 + got + i) {
                CHECK(0, "frame %u = %#x", got + i, frame_of(buf, i));
                break;
            }
        got += n;
    }
    CHECK(sbg_snd_stats()->fill_frames == 400 && sbg_snd_stats()->fill_max == 1000, "fill %u max %u",
          sbg_snd_stats()->fill_frames, sbg_snd_stats()->fill_max);
    n = sbg_snd_pull(buf, 300);
    CHECK(n == 100 && frame_of(buf, 99) == 7 + 999 && buf[200] == 0 && buf[599] == 0, "short pull %u", n);
    CHECK(sbg_snd_stats()->underruns == 1 && sbg_snd_stats()->underrun_frames == 200, "underrun %u / %u",
          sbg_snd_stats()->underruns, sbg_snd_stats()->underrun_frames);
    CHECK(S->play_hw == 4000 && S->play_underruns == 1 && S->play_underrun_frames == 200, "region hw %u ur %u/%u",
          S->play_hw, S->play_underruns, S->play_underrun_frames);
    sbg_snd_poll();
    CHECK(gicd_fake[(0x200 + 4 * (132 / 32)) / 4] == 1U << (132 % 32), "the doorbell for the underrun");
    CHECK(act_calls == 1 && act_play == 1 && act_cap == 0, "active hint %d %d %d", act_calls, act_play, act_cap);
    gicd_fake[(0x200 + 16) / 4] = 0;
    sbg_snd_pull(buf, 300);                     /* the same episode: no second count, no second doorbell */
    sbg_snd_poll();
    CHECK(sbg_snd_stats()->underruns == 1 && sbg_snd_stats()->underrun_frames == 500 && !gicd_fake[(0x200 + 16) / 4],
          "one episode %u, %u frames", sbg_snd_stats()->underruns, sbg_snd_stats()->underrun_frames);
    g_write(300, 5000);
    CHECK(sbg_snd_pull(buf, 300) == 300, "refilled");
    g_write(10, 6000);
    sbg_snd_pull(buf, 300);
    CHECK(sbg_snd_stats()->underruns == 2, "a second episode %u", sbg_snd_stats()->underruns);
    /* a new start: not primed again until a byte comes */
    S->play_state = SBSHM_SND_STOP;
    g_play_start();
    sbg_snd_pull(buf, 300);
    CHECK(sbg_snd_stats()->underruns == 2, "a fresh start is not primed %u", sbg_snd_stats()->underruns);
}

static void t_wrap(void)
{
    boot();
    /* positions just below 2^32: both sides there (as after a long run) */
    p_hw = g_appl = 0xfffff000U;
    S->play_appl = S->play_hw = g_appl;
    g_play_start();
    uint32_t seq = 100, want = 100, bad = 0;
    unsigned total = 0;
    for (int k = 0; k < 2000; ++k) {            /* odd sizes: every offset of the ring, many wraps */
        uint32_t w = 37 + (uint32_t)(k * 7919) % 3000;
        uint32_t fill = (g_appl - p_hw) / 4;
        if (fill + w > SBSHM_SND_RING / 4)
            w = SBSHM_SND_RING / 4 - fill;
        g_write(w, seq);
        seq += w;
        unsigned r = 11 + (unsigned)(k * 104729) % 3100;
        unsigned n = sbg_snd_pull(buf, r);
        for (unsigned i = 0; i < n; ++i)
            bad += frame_of(buf, i) != want + i;
        want += n;
        total += n;
    }
    CHECK(bad == 0 && total > 2000000 && p_hw < 0xfffff000U, "wrap: %u bad of %u frames, hw %#x",
          bad, total, p_hw);
    CHECK(sbg_snd_stats()->resyncs == 0, "resyncs %u", sbg_snd_stats()->resyncs);
    printf("  wrap: %u frames through the ring (%u ring wraps, position %#x after 2^32)\n", total,
           total / (SBSHM_SND_RING / 4), p_hw);
}

static void t_discard_drain_bad(void)
{
    boot();
    g_play_start();
    g_write(2000, 0);
    CHECK(sbg_snd_pull(buf, 500) == 500, "before the drop");
    S->play_discard = g_appl;                    /* the guest's drop */
    S->play_state = SBSHM_SND_STOP;
    int p = 9, c = 9;
    CHECK(sbg_snd_active(&p, &c) == 0 && p == 0 && c == 0, "stopped: not active");
    CHECK(sbg_snd_pull(buf, 500) == 0 && sbg_snd_stats()->discards == 1 && S->play_hw == g_appl, "drop skipped "
          "(%u discards, hw %u appl %u)", sbg_snd_stats()->discards, S->play_hw, g_appl);
    CHECK(sbg_snd_stats()->underruns == 0, "a stopped stream does not underrun");
    /* a discard of an old position does nothing */
    g_play_start();
    g_write(100, 50);
    unsigned n = sbg_snd_pull(buf, 60);
    CHECK(n == 60 && frame_of(buf, 0) == 50, "after a restart %u %#x", n, frame_of(buf, 0));
    /* drain: the rest is played, then idle, no underrun */
    S->play_state = SBSHM_SND_DRAIN;
    CHECK(sbg_snd_active(&p, NULL) == 1 && p == 1, "draining with frames left is active");
    n = sbg_snd_pull(buf, 100);
    CHECK(n == 40 && frame_of(buf, 39) == 50 + 99 && sbg_snd_stats()->underruns == 0, "drain end %u", n);
    CHECK(sbg_snd_active(&p, NULL) == 0, "drained: idle");
    /* a guest that claims more than the ring holds */
    S->play_state = SBSHM_SND_RUN;
    S->play_appl = g_appl + SBSHM_SND_RING + 4;
    CHECK(sbg_snd_pull(buf, 100) == 0 && sbg_snd_stats()->resyncs == 1 && p_hw == g_appl + SBSHM_SND_RING + 4,
          "resync %u", sbg_snd_stats()->resyncs);
    S->play_appl = p_hw + 6;                     /* not whole frames */
    CHECK(sbg_snd_pull(buf, 100) == 0 && sbg_snd_stats()->resyncs == 2, "odd fill");
}

static void t_volume(void)
{
    boot();
    g_play_start();
    g_write(1, (uint32_t)(uint16_t)-20000 << 16 | 20000);
    g_write(1, (uint32_t)(uint16_t)-20000 << 16 | 20000);
    g_write(1, (uint32_t)(uint16_t)-20000 << 16 | 20000);
    sbg_snd_set_volume(0x8000, 0);
    CHECK(S->host_volume == 0x8000 && S->host_mute == 0, "the region shows the volume");
    sbg_snd_pull(buf, 1);
    CHECK(buf[0] == 10000 && buf[1] == -10000, "half %d %d", buf[0], buf[1]);
    sbg_snd_set_volume(0x30000, 0);
    sbg_snd_pull(buf, 1);
    CHECK(buf[0] == 32767 && buf[1] == -32768, "saturated %d %d", buf[0], buf[1]);
    sbg_snd_set_volume(0x10000, 1);
    sbg_snd_pull(buf, 1);
    CHECK(buf[0] == 0 && buf[1] == 0 && S->host_mute == 1, "muted %d %d", buf[0], buf[1]);
    sbg_snd_set_volume(0x10000, 0);
    sbg_snd_set_latency(960);
    CHECK(S->host_latency_frames == 960, "latency");
}

static void t_capture(void)
{
    boot();
    int16_t mic[2 * 10000];
    for (int i = 0; i < 10000; ++i) {
        mic[2 * i] = (int16_t)i;
        mic[2 * i + 1] = (int16_t)~i;
    }
    CHECK(sbg_snd_push(mic, 100) == 0 && S->cap_hw == 0 && S->cap_overruns == 0, "no capture: nothing taken");
    /* the guest starts capturing (cap_appl = cap_hw: no old frames) */
    g_cap_appl = S->cap_hw;
    S->cap_appl = g_cap_appl;
    S->cap_starts++;
    S->cap_state = SBSHM_SND_RUN;
    sbg_snd_poll();
    CHECK(act_calls == 1 && act_cap == 1 && act_play == 0, "capture hint");
    CHECK(sbg_snd_push(mic, 10000) == 10000, "first push");
    unsigned n = g_read(cbuf, 3000);
    CHECK(n == 3000 && cbuf[0] == 0xffff0000U && cbuf[2999] == ((uint32_t)(uint16_t)~2999 << 16 | 2999), "read %u %#x",
          n, cbuf[0]);
    n = sbg_snd_push(mic, 10000);
    CHECK(n == 16384 - 7000 && sbg_snd_stats()->overrun_frames == 10000 - n && S->cap_overruns == 10000 - n,
          "overrun: took %u, dropped %u", n, sbg_snd_stats()->overrun_frames);
    /* the guest reads everything: 7000 left of push 1, then push 2's first n */
    n = g_read(cbuf, 20000);
    uint32_t bad = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned j = i < 7000 ? 3000 + i : i - 7000;
        bad += cbuf[i] != ((uint32_t)(uint16_t)~j << 16 | (uint16_t)j);
    }
    CHECK(n == 16384 && bad == 0, "read back %u frames, %u bad", n, bad);
    /* the 2^32 wrap on the capture side */
    c_hw = 0xffffff00U;
    S->cap_hw = c_hw;
    g_cap_appl = c_hw;
    S->cap_appl = g_cap_appl;
    bad = 0;
    for (int k = 0; k < 500; ++k) {
        unsigned w = 1 + (unsigned)(k * 7919) % 5000, r;
        r = sbg_snd_push(mic, w);
        bad += r != w;
        n = g_read(cbuf, 20000);
        bad += n != r;
        for (unsigned i = 0; i < n; ++i)
            bad += (cbuf[i] & 0xffff) != i;
    }
    CHECK(bad == 0 && c_hw < 0x10000000U, "capture wrap: %u bad, hw %#x", bad, c_hw);
    /* a guest position beyond the ring: nothing pushed, counted */
    S->cap_appl = c_hw + 8;
    CHECK(sbg_snd_push(mic, 10) == 0 && sbg_snd_stats()->resyncs == 1, "bad cap_appl");
    S->cap_appl = g_cap_appl;
    S->cap_state = SBSHM_SND_STOP;
    CHECK(sbg_snd_push(mic, 10) == 0, "stopped");
    sbg_snd_poll();
    CHECK(act_calls == 2 && act_cap == 0, "capture stopped hint");
    /* the guest goes away: playing / capturing hints drop */
    S->cap_state = SBSHM_SND_RUN;
    sbg_snd_poll();
    st.state = SBG_OFF;
    sbg_snd_poll();
    CHECK(act_calls == 4 && act_cap == 0 && !sbg_snd_active(NULL, NULL), "guest off: stopped (%d calls)", act_calls);
}

/* ---- two threads: the guest's writer / reader on one core, the host's mixer on another ------------------------- */
static volatile int stop_threads;
static uint64_t th_pulled, th_bad, th_cap_bad, th_cap_read;
static void *guest_thread(void *a)
{
    (void)a;
    uint32_t seq = 1, cap_want = 1;
    unsigned k = 0;
    while (!stop_threads) {
        uint32_t fill = (g_appl - S->play_hw) / 4, w = 1 + (k++ * 7919U) % 700;
        if (fill + w <= SBSHM_SND_RING / 4) {
            g_write(w, seq);
            seq += w;
        }
        unsigned n = g_read(cbuf, 20000);
        for (unsigned i = 0; i < n; ++i)
            th_cap_bad += cbuf[i] != cap_want + i;
        cap_want += n;
        th_cap_read += n;
    }
    return NULL;
}
static void *host_thread(void *a)
{
    (void)a;
    static int16_t hb[2 * 2000], mb[2 * 2000];
    uint32_t want = 1, cseq = 1;
    unsigned k = 0;
    while (!stop_threads) {
        unsigned n = sbg_snd_pull(hb, 1 + (k * 104729U) % 1500);
        for (unsigned i = 0; i < n; ++i)
            th_bad += frame_of(hb, i) != want + i;
        want += n;
        th_pulled += n;
        unsigned m = 1 + (k++ * 31U) % 900;
        for (unsigned i = 0; i < m; ++i) {
            mb[2 * i] = (int16_t)(cseq + i);
            mb[2 * i + 1] = (int16_t)((cseq + i) >> 16);
        }
        cseq += sbg_snd_push(mb, m);            /* dropped frames are not part of the sequence: push again */
    }
    return NULL;
}
static void t_threads(void)
{
    boot();
    g_cap_appl = S->cap_hw;
    S->cap_appl = g_cap_appl;
    S->cap_state = SBSHM_SND_RUN;
    g_play_start();
    pthread_t g, h;
    stop_threads = 0;
    pthread_create(&g, NULL, guest_thread, NULL);
    pthread_create(&h, NULL, host_thread, NULL);
    struct timespec ts = { 1, 500000000 };
    nanosleep(&ts, NULL);
    stop_threads = 1;
    pthread_join(g, NULL);
    pthread_join(h, NULL);
    CHECK(th_bad == 0 && th_cap_bad == 0 && th_pulled > 100000 && th_cap_read > 100000 && sbg_snd_stats()->resyncs == 0,
          "threads: %llu bad of %llu pulled, %llu bad of %llu captured, %u resyncs", (unsigned long long)th_bad,
          (unsigned long long)th_pulled, (unsigned long long)th_cap_bad, (unsigned long long)th_cap_read,
          sbg_snd_stats()->resyncs);
    printf("  threads: %llu frames pulled, %llu captured in 1.5 s, every sequence number in order (underruns %u, "
           "overrun frames %u)\n", (unsigned long long)th_pulled, (unsigned long long)th_cap_read,
           sbg_snd_stats()->underruns, sbg_snd_stats()->overrun_frames);
}

int main(void)
{
    shm = aligned_alloc(4096, 0x4000000);
    if (!shm)
        return 2;
    t_layout();
    t_pull_underrun();
    t_wrap();
    t_discard_drain_bad();
    t_volume();
    t_capture();
    t_threads();
    printf(fails ? "test-snd: FAIL (%d)\n" : "test-snd: PASS\n", fails);
    return fails != 0;
}
