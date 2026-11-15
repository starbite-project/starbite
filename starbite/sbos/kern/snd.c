/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the sound service (snd.h has the model and the interface; docs/drivers/AUDIO.md the wiring).
 *
 * What runs where, per poll (snd_poll, every 5 ms while busy):
 *   1  a call took ABOX without snd_suspend_for_call(): our state is dropped without touching anything;
 *   2  something to play and the engine off: apcm_up, RDMA0 started on silence, then the output switched on
 *      (amp_speaker / amp_receiver / codec_hp) while zeroes flow, so the amplifier starts without a pop;
 *   3  the periods RDMA0 has passed are zeroed, and the next 3 after the one it reads are mixed: the streams and the
 *      tone in 32 bits, the output's volume, then the guard (amp outputs: mono, a peak limiter to the output's
 *      ceiling, a short- and a long-term power budget; headphones: the limiter only), then S16 into the ring;
 *   4  2 s without anything to play: the output off first, then RDMA0, then (no capture) the AUD domain;
 *   5  capture: what WDMA1 wrote since the last poll is taken (the left slot: AIF1TX1, the main mic) into a 0.5 s
 *      mono queue for snd_cap_read.
 * With the Calliope engine (abox_media.c; the kernel's, since bypass moves neither RDMA0 nor WDMA1 on this phone)
 * playback is RDMA0 and capture WDMA1 through Calliope's PCM IPC; both may run at the same time on one Calliope boot,
 * which snd boots when nobody runs it and switches off 30 s after the last of the two stopped (cal_release). The boot
 * never holds the kernel loop: abox_boot_start(), then one abox_boot_step() per snd_poll (cal_tick: ~1 s on the phone,
 * the longest single step the calliope_dram.bin read), then 300 ms of abox_poll() by a deadline for Calliope's
 * post-boot messages (run 6: taken before our PCM messages). Meanwhile streams keep their queued frames, a requested
 * tone or capture waits and starts on the first poll after; a key click on a cold Calliope is dropped (it would come
 * ~1 s late) but starts the boot, so the next one plays.
 *
 * The guard stands in for Maxim's DSM speaker protection, which runs on ABOX's firmware and not here. It is ours:
 * a no-lookahead peak limiter (instant attack, ~40 ms release) to the ceiling, and a power budget on the mean square
 * of what leaves the limiter, averaged over ~1.4 s and ~22 s: above either limit the budget takes 0.1 dB off every
 * period (9.4 dB a second) down to -24 dB; below 70 % of both it gives back 0.05 dB a period. Ceilings and limits:
 *   loudspeaker  peak -3 dBFS, short -10 dBFS RMS, long -18 dBFS RMS (and amp.c's analog/digital cap under it)
 *   earpiece     peak -6 dBFS, short -12 dBFS RMS, long -18 dBFS RMS
 *   headphones   peak -1 dBFS, no budget (the codec's own level does the rest) */
#include "snd.h"
#include "codec_media.h"
#include "abox_media.h"
#include "goertzel.h"
#include <stdarg.h>

#ifdef SBOS_KERNEL
#include "sb.h"
#else
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#endif

#define W_AHEAD 3U                      /* periods mixed ahead of the one RDMA0 reads: tones / clicks only (~32 ms) */
#define W_DEEP 16U                      /* while a stream is open (~170 ms of the 341 ms ring): the shell's turns of
                                         * 20-45 ms, sometimes 100-176 ms, do not starve music; a tone that starts
                                         * then plays after what is already mixed (~170 ms later) */
#define IDLE_STOP_US 2000000ULL
#define RETRY_US 2000000ULL             /* after a failed start, the next try */
#define LATE_US 15000ULL
#define STALL_US 250000ULL
#define CAL_IDLE_OFF_US 30000000ULL     /* Calliope (booted by snd) off after 30 s without media */
#define AMP_SPK_LEVEL 5                 /* amp.c's top speaker level: still 15 dB under One UI (no DSM) */
#define AMP_RCV_LEVEL 3                 /* One UI's handset gain, never above */
#define PF APCM_PERIOD_FRAMES

static const int16_t qsin[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010,
    14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279,
    24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852,
    31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767
};
/* volume 0..15: 2.5 dB a step, 15 = 0 dB, 1 = -35 dB, 0 = mute (Q15) */
static const uint16_t vol_q15[16] = { 0, 583, 777, 1036, 1382, 1843, 2457, 3277, 4370, 5827, 7770, 10362, 13818,
                                      18426, 24572, 32767 };
/* the budget's gain: 1 dB steps and 0.05 dB steps (Q16) */
static const uint16_t att_db[25] = { 65535, 58409, 52057, 46396, 41350, 36854, 32846, 29274, 26090, 23253, 20724, 18471,
                                     16462, 14672, 13076, 11654, 10387, 9257, 8250, 7353, 6554, 5841, 5206, 4640, 4135 };
static const uint16_t att_frac[20] = { 65535, 65160, 64786, 64414, 64044, 63677, 63311, 62948, 62586, 62227, 61870, 61515,
                                       61162, 60811, 60462, 60115, 59770, 59426, 59085, 58746 };
#define ATT_MAX 480                     /* in 0.05 dB: -24 dB */

struct guard_par {
    int32_t ceil;
    uint64_t short_lim, long_lim;       /* mean squares; 0 = no budget */
};
static const struct guard_par gpar[4] = {
    { 23197, 107367629ULL, 17016622ULL },       /* (AUTO: not used) */
    { 23197, 107367629ULL, 17016622ULL },       /* speaker: -3 / -10 / -18 dBFS */
    { 16422, 67744394ULL, 17016622ULL },        /* earpiece: -6 / -12 / -18 dBFS */
    { 29204, 0, 0 },                            /* headphones: -1 dBFS */
};

/* ---- tones ------------------------------------------------------------------------------------------------- */
struct seg {
    uint16_t f1, f2, ms, level;         /* level Q15 (0 = a pause); f2: a second tone summed (each at half) */
    uint8_t decay;                      /* the level falls to 0 across the segment (a tick) */
};
#define L_RING 13107                    /* -8 dBFS */
#define L_NOTE 9830                     /* -10.5 dBFS */
static const struct seg t_ring[] = {
    { 659, 0, 120, L_RING, 0 }, { 0, 0, 30, 0, 0 }, { 831, 0, 120, L_RING, 0 }, { 0, 0, 30, 0, 0 },
    { 988, 0, 180, L_RING, 0 }, { 0, 0, 120, 0, 0 }, { 659, 0, 120, L_RING, 0 }, { 0, 0, 30, 0, 0 },
    { 831, 0, 120, L_RING, 0 }, { 0, 0, 30, 0, 0 }, { 988, 0, 180, L_RING, 0 }, { 0, 0, 1200, 0, 0 },
};
static const struct seg t_alarm[] = {
    { 1000, 0, 100, L_RING, 0 }, { 0, 0, 80, 0, 0 }, { 1000, 0, 100, L_RING, 0 }, { 0, 0, 80, 0, 0 },
    { 1000, 0, 100, L_RING, 0 }, { 0, 0, 80, 0, 0 }, { 1000, 0, 100, L_RING, 0 }, { 0, 0, 600, 0, 0 },
};
static const struct seg t_notify[] = { { 784, 0, 90, L_NOTE, 0 }, { 0, 0, 20, 0, 0 }, { 1175, 0, 160, L_NOTE, 0 } };
static const struct seg t_click[] = { { 3000, 0, 6, 6554, 1 } };
static const struct seg t_test[] = { { 1000, 0, 1000, 3277, 0 } };     /* -20 dBFS */
/* 8a (Track B): the keyboard's other two clicks, the lock, the shutter, the charging chime (generic, synthesized) */
static const struct seg t_click_del[] = { { 2000, 0, 6, 6554, 1 } };
static const struct seg t_click_mod[] = { { 1500, 0, 7, 5243, 1 } };
static const struct seg t_lock[] = { { 1400, 700, 14, 8192, 1 }, { 0, 0, 12, 0, 0 }, { 900, 450, 22, 6554, 1 } };
static const struct seg t_shutter[] = { { 2600, 1900, 9, 7864, 1 }, { 0, 0, 55, 0, 0 }, { 2200, 1500, 12, 7209, 1 } };
static const struct seg t_charge[] = { { 1319, 2637, 360, L_NOTE, 1 } };
#define NSEG(t) (sizeof t / sizeof t[0])
static const struct { const struct seg *s; unsigned n, repeat; } tones[SND_TONE_MAX + 1] = {
    { 0, 0, 0 },
    { t_ring, sizeof t_ring / sizeof t_ring[0], 1 },
    { t_alarm, sizeof t_alarm / sizeof t_alarm[0], 1 },
    { t_notify, sizeof t_notify / sizeof t_notify[0], 0 },
    { t_click, 1, 0 },
    { t_test, 1, 1 },
    { t_click_del, NSEG(t_click_del), 0 },
    { t_click_mod, NSEG(t_click_mod), 0 },
    { t_lock, NSEG(t_lock), 0 },
    { t_shutter, NSEG(t_shutter), 0 },
    { t_charge, NSEG(t_charge), 0 },
};
#define ENV_SAMPLES 192U                /* 4 ms ramps where a note starts or ends */

/* ---- state --------------------------------------------------------------------------------------------------- */
struct stream {
    int open;
    uint32_t head, tail;                /* frames, free-running */
    unsigned gain;                      /* Q8 */
    struct snd_shring *sh;
    int16_t pcm[SND_STREAM_FRAMES * 2];
};
static struct stream st[SND_STREAMS];

static struct snd_ops O;
static int inited;
static struct {
    int eng, out_on, cur_out, route, jack, muted, suspended, cap, cap_codec, amp_override, dumped;
    int ce, cal_own, cal_bad, cal_fails;    /* Calliope engine running, snd booted it, given up on it */
    int cal_boot, cal_settling, cap_want;   /* snd's staged boot pending; its 300 ms settle; a capture waits for it */
    uint64_t cal_t0, cal_settle_at;
    int cc;                                 /* capture runs through Calliope (WDMA1) */
    int modem;                              /* 9a: Cellular holds Calliope (snd_calliope_shared): snd never boots or
                                             * switches it off, and plays through it while no call routes */
    int silent;                             /* the probe's silent run: output_on() leaves the amps / codec off */
    uint64_t cal_off_at;
    unsigned vol[4];
    uint32_t fill_idx, zero_idx, last_pos;
    uint32_t lead;                          /* periods ahead of RDMA0 after the last fill (+1: the one it reads) */
    uint64_t last_poll, idle_since, retry_at, moved_at;
    /* tone */
    int tone;
    unsigned seg, seg_pos, seg_len;
    uint32_t ph1, ph2;
    /* guard */
    int32_t lim_q16;
    int64_t s_avg, l_avg;
    int att;
    /* capture */
    uint32_t cap_last, cap_head, cap_tail, cap_n;
    uint64_t cap_sq;
    unsigned cap_level;
    struct snd_stats stats;
} G;
static int16_t capq[SND_CAP_SAMPLES];
static char why_buf[160] = "Sound is off";

/* ---- log ----------------------------------------------------------------------------------------------------- */
static void vfmt(char *b, unsigned cap, const char *pre, const char *fmt, va_list ap)
{
    unsigned n = 0;
    for (const char *p = pre; *p && n + 1 < cap; ++p)
        b[n++] = *p;
    for (const char *f = fmt; *f && n + 1 < cap; ++f) {
        if (*f != '%') {
            b[n++] = *f;
            continue;
        }
        ++f;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "?"; *s && n + 1 < cap; ++s)
                b[n++] = *s;
        } else if (*f == 'u' || *f == 'x' || *f == 'd') {
            long sv = 0;
            unsigned long v;
            if (*f == 'd') {
                sv = va_arg(ap, long);
                v = (unsigned long)(sv < 0 ? -sv : sv);
                if (sv < 0 && n + 1 < cap)
                    b[n++] = '-';
            } else {
                v = va_arg(ap, unsigned long);
            }
            char t[24];
            unsigned i = 0, base = *f == 'x' ? 16U : 10U;
            if (base == 16 && n + 3 < cap)
                b[n++] = '0', b[n++] = 'x';
            do {
                t[i++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v && i < sizeof t);
            while (i && n + 1 < cap)
                b[n++] = t[--i];
        } else if (*f) {
            b[n++] = *f;
        } else {
            break;
        }
    }
    b[n] = 0;
}
#define UL(x) ((unsigned long)(x))

/* a + b + c into d (no libc) */
static void snprintf_like(char *d, unsigned cap, const char *a, const char *b, const char *c)
{
    unsigned n = 0;
    const char *parts[3] = { a, b, c };
    for (unsigned k = 0; k < 3; ++k)
        for (const char *p = parts[k]; p && *p && n + 1 < cap; ++p)
            d[n++] = *p;
    d[n] = 0;
}
#define SL(x) ((long)(x))

static struct abox_hal snd_hal;             /* snd_init's HAL, kept for snd's own boots */
static int snd_hal_ok;
/* the last Calliope boot snd made: its longest single step (the main-loop stall it can cause), stage, total */
static uint32_t boot_slow_ms, boot_slow_stage, boot_total_ms;
static int pfinish_raw(int ok, const char *line);
/* "longest boot step N ms (stage S), boot N ms; " into b; its length */
static unsigned snprintf_like3(char *b, unsigned cap, uint32_t slow, uint32_t stage, uint32_t total)
{
    char t[96];
    unsigned n = 0;
    const char *parts[3] = { "longest boot step ", " ms (stage ", ") of a "};
    uint32_t vals[3] = { slow, stage, total };
    for (unsigned k = 0; k < 3; ++k) {
        for (const char *q = parts[k]; *q; ++q)
            t[n++] = *q;
        char d[12];
        unsigned m = 0;
        uint32_t v = vals[k];
        do {
            d[m++] = (char)('0' + v % 10U);
            v /= 10U;
        } while (v && m < sizeof d);
        while (m)
            t[n++] = d[--m];
    }
    for (const char *q = " ms boot; "; *q; ++q)
        t[n++] = *q;
    unsigned i = 0;
    for (; i < n && i + 1 < cap; ++i)
        b[i] = t[i];
    b[i] = 0;
    return i;
}

/* the newest failure line snd said (for the probe's FAIL line: pmsg keeps only the run's tail) */
static char last_bad[112];
static int has_s(const char *h, const char *n)
{
    for (; *h; ++h) {
        unsigned k = 0;
        while (n[k] && h[k] == n[k])
            ++k;
        if (!n[k])
            return 1;
    }
    return 0;
}
static void note_bad(const char *l)
{
    if (!has_s(l, "did not") && !has_s(l, "did NOT") && !has_s(l, "fell over") && !has_s(l, "failed") &&
        !has_s(l, "refused") && !has_s(l, "stopped while") && !has_s(l, "took Calliope"))
        return;
    unsigned i = 0;
    for (; l[i] && i + 1 < sizeof last_bad; ++i)
        last_bad[i] = l[i];
    last_bad[i] = 0;
}
static void say(const char *fmt, ...)
{
    char b[220];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, "snd: ", fmt, ap);
    va_end(ap);
    if (inited && O.log)
        O.log(b);
    note_bad(b + 5);
}
/* the probe's lines ("drvprobe: audio: ...") */
static void psay(const char *fmt, ...)
{
    char b[220];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, "drvprobe: audio: ", fmt, ap);
    va_end(ap);
    if (inited && O.log)
        O.log(b);
}
static void why(const char *s)
{
    unsigned i = 0;
    for (; s && s[i] && i + 1 < sizeof why_buf; ++i)
        why_buf[i] = s[i];
    why_buf[i] = 0;
}

static uint64_t now(void) { return O.now_us(); }
/* Calliope has ABOX (abox.c in any running state): the bypass engine (apcm) cannot run */
static int call_has(void) { return O.call_owns_abox && O.call_owns_abox(); }
/* the Calliope engine (abox_media.c) when the HAL can read VENDOR (its firmware); else bypass only, as before */
static int cal_avail;
/* 9g: the modem's share told before snd_init (Cellular started before the shell's first sound): kept for snd_init */
static int modem_pending;
/* a call is on: with the Calliope engine only abox_call(1) counts (a booted Calliope is shared), else as before */
static int in_call(void) { return cal_avail ? abox_state() == ABOX_ST_CALL : call_has(); }
static uint8_t *eng_buf(void) { return G.ce ? cal_play_buf() : apcm_play_buf(); }

/* ---- set-up -------------------------------------------------------------------------------------------------- */
int snd_init(const struct snd_ops *ops, const struct abox_hal *hal)
{
    if (!ops || !ops->log || !ops->now_us || !ops->amp_speaker || !ops->amp_receiver || !ops->amp_off || !hal)
        return SND_EINVAL;
    O = *ops;
    inited = 1;
    if (apcm_hal_set(hal, ops->call_owns_abox)) {
        inited = 0;
        return SND_EINVAL;
    }
    /* the ring's window goes into every later abox_boot() (ours or the modem's), so either can carry media */
    if (ops->use_calliope && hal->file) {
        snd_hal = *hal;                     /* snd's own (its RAM copy of the files, file_part): see cal_up */
        snd_hal_ok = 1;
        if (!cal_avail)
            cal_avail = !cal_hal_set(hal) && !cal_map();
    } else {
        if (cal_avail)
            cal_unmap();
        cal_avail = 0;
    }
    memset(&G, 0, sizeof G);
    memset(st, 0, sizeof st);
    G.modem = modem_pending;                    /* 9g: Cellular already holds Calliope */
    G.vol[SND_OUT_SPEAKER] = 10, G.vol[SND_OUT_EARPIECE] = 10, G.vol[SND_OUT_HEADPHONES] = 8;
    G.cur_out = SND_OUT_SPEAKER;
    G.amp_override = -1;
    G.lim_q16 = 65536;
    why("Sound is ready (nothing playing)");
    say("ready: 4 streams + tones, 48 kHz stereo S16; speaker capped (no DSM): the guard limits it; headphones %s",
        O.codec_hp ? "via the codec" : "not available (no codec_hp)");
    return 0;
}

void snd_set_hp(int (*hp)(int on, int level))
{
    O.codec_hp = hp;
}

const char *snd_why(void) { return why_buf; }
void snd_stats(struct snd_stats *s)
{
    *s = G.stats;
    s->guard_db10 = -G.att / 2;
}

/* ---- streams ------------------------------------------------------------------------------------------------- */
int snd_open(void)
{
    for (int i = 0; i < SND_STREAMS; ++i)
        if (!st[i].open) {
            st[i].open = 1;
            st[i].head = st[i].tail = 0;
            st[i].gain = 256;
            st[i].sh = 0;
            return i;
        }
    return SND_EBUSY;
}

static int ok_id(int id) { return id >= 0 && id < SND_STREAMS && st[id].open; }

unsigned snd_queued(int id)
{
    if (!ok_id(id))
        return 0;
    if (st[id].sh) {
        struct snd_shring *r = st[id].sh;
        uint32_t q = r->head - r->tail;
        return q > r->frames ? r->frames : q;
    }
    return st[id].head - st[id].tail;
}

unsigned snd_space(int id)
{
    if (!ok_id(id) || st[id].sh)
        return 0;
    return SND_STREAM_FRAMES - (st[id].head - st[id].tail);
}

unsigned snd_write(int id, const int16_t *f, unsigned n)
{
    if (!ok_id(id) || !f || st[id].sh)
        return 0;
    unsigned room = snd_space(id), k = n < room ? n : room;
    for (unsigned i = 0; i < k; ++i) {
        uint32_t j = (st[id].head + i) % SND_STREAM_FRAMES;
        st[id].pcm[2 * j] = f[2 * i];
        st[id].pcm[2 * j + 1] = f[2 * i + 1];
    }
    st[id].head += k;
    return k;
}

void snd_stream_gain(int id, unsigned q8)
{
    if (ok_id(id))
        st[id].gain = q8 > 256 ? 256 : q8;
}

void snd_close(int id)
{
    if (id >= 0 && id < SND_STREAMS)
        st[id].open = 0, st[id].sh = 0, st[id].head = st[id].tail = 0;
}

int snd_attach_shring(int id, struct snd_shring *r)
{
    if (!ok_id(id))
        return SND_EINVAL;
    if (r && (r->magic != SND_SHRING_MAGIC || !r->frames || r->frames > 8192U || (r->frames & (r->frames - 1))))
        return SND_EINVAL;
    st[id].sh = r;
    st[id].head = st[id].tail = 0;
    return 0;
}

/* ---- tones --------------------------------------------------------------------------------------------------- */
static void seg_start(void)
{
    const struct seg *s = &tones[G.tone].s[G.seg];
    G.seg_pos = 0;
    G.seg_len = (unsigned)s->ms * (APCM_RATE / 1000U);
}

static int cal_cold(void);
static int cal_up(void);
int snd_tone(int kind)
{
    if (kind < 1 || kind > SND_TONE_MAX)
        return SND_EINVAL;
    if ((kind == SND_TONE_CLICK || kind == SND_TONE_CLICK_DEL || kind == SND_TONE_CLICK_MOD) && cal_cold()) {
        /* a 6 ms key click ~1 s late is worse than none: dropped, but Calliope boots so that the next one plays */
        if (!G.cal_boot && !G.cal_settling) {
            say("tone: a click while Calliope is off: dropped; Calliope boots for the next one");
            (void)cal_up();
        }
        return 0;
    }
    G.tone = kind;
    G.seg = 0;
    G.ph1 = G.ph2 = 0;
    seg_start();
    return 0;
}
void snd_tone_stop(void) { G.tone = 0; }
int snd_tone_playing(void) { return G.tone; }

static int32_t sine(uint32_t ph)
{
    unsigned q = ph >> 30, i = (ph >> 24) & 63U, fr = (ph >> 8) & 0xFFFFU;
    int32_t a, b;
    if (q & 1)                          /* falling quarter: mirror the index */
        a = qsin[64 - i], b = qsin[63 - i];
    else
        a = qsin[i], b = qsin[i + 1];
    int32_t v = a + (int32_t)(((int64_t)(b - a) * fr) >> 16);
    return q >= 2 ? -v : v;
}

static uint32_t inc_of(unsigned f) { return (uint32_t)(((uint64_t)f << 32) / APCM_RATE); }

/* the tone added into acc (stereo, n frames) */
static void tone_render(int32_t *acc, unsigned n)
{
    for (unsigned i = 0; i < n && G.tone; ++i) {
        const struct seg *tab = tones[G.tone].s, *s = &tab[G.seg];
        unsigned nseg = tones[G.tone].n;
        if (s->level) {
            int32_t amp = s->level;
            const struct seg *prev = G.seg ? &tab[G.seg - 1] : (tones[G.tone].repeat ? &tab[nseg - 1] : 0);
            const struct seg *next = G.seg + 1 < nseg ? &tab[G.seg + 1] : (tones[G.tone].repeat ? &tab[0] : 0);
            int join_prev = prev && prev->level == s->level && prev->f1 == s->f1 && prev->f2 == s->f2 && !s->decay;
            int join_next = next && next->level == s->level && next->f1 == s->f1 && next->f2 == s->f2 && !s->decay;
            if (s->decay)
                amp = (int32_t)((int64_t)amp * (G.seg_len - G.seg_pos) / (G.seg_len ? G.seg_len : 1));
            else if (!join_prev && G.seg_pos < ENV_SAMPLES)
                amp = amp * (int32_t)G.seg_pos / (int32_t)ENV_SAMPLES;
            else if (!join_next && G.seg_len - G.seg_pos <= ENV_SAMPLES)
                amp = amp * (int32_t)(G.seg_len - G.seg_pos) / (int32_t)ENV_SAMPLES;
            int32_t v = sine(G.ph1);
            if (s->f2)
                v = (v + sine(G.ph2)) / 2;
            G.ph1 += inc_of(s->f1);
            G.ph2 += inc_of(s->f2);
            v = (int32_t)(((int64_t)v * amp) >> 15);
            acc[2 * i] += v;
            acc[2 * i + 1] += v;
        } else {
            G.ph1 = G.ph2 = 0;          /* each note starts at phase 0 after a pause */
        }
        if (++G.seg_pos >= G.seg_len) {
            if (++G.seg >= nseg) {
                if (!tones[G.tone].repeat) {
                    G.tone = 0;
                    break;
                }
                G.seg = 0;
            }
            seg_start();
        }
    }
}

/* ---- outputs ------------------------------------------------------------------------------------------------- */
void snd_volume(int out, unsigned v)
{
    if (out < SND_OUT_SPEAKER || out > SND_OUT_HEADPHONES)
        return;
    G.vol[out] = v > SND_VOL_MAX ? SND_VOL_MAX : v;
    if (out == SND_OUT_HEADPHONES && G.out_on && G.cur_out == out && O.codec_hp)
        (void)O.codec_hp(1, (int)G.vol[out]);
}
unsigned snd_get_volume(int out) { return out >= 1 && out <= 3 ? G.vol[out] : 0; }
void snd_mute(int on) { G.muted = !!on; }
void snd_route(int out)
{
    if (out >= SND_OUT_AUTO && out <= SND_OUT_HEADPHONES)
        G.route = out;
}
void snd_jack(int plugged)
{
    if (G.jack != !!plugged)
        say("jack: headphones %s", plugged ? "plugged in" : "out");
    G.jack = !!plugged;
}

static int resolve(void)
{
    if (G.route == SND_OUT_HEADPHONES)
        return O.codec_hp ? SND_OUT_HEADPHONES : SND_OUT_SPEAKER;
    if (G.route == SND_OUT_SPEAKER || G.route == SND_OUT_EARPIECE)
        return G.route;
    return G.jack && O.codec_hp ? SND_OUT_HEADPHONES : SND_OUT_SPEAKER;
}
int snd_output(void) { return G.eng ? G.cur_out : resolve(); }

static int uaif_of(int out) { return out == SND_OUT_HEADPHONES ? APCM_UAIF0 : APCM_UAIF1; }

static const char *out_name(int out)
{
    return out == SND_OUT_EARPIECE ? "earpiece" : out == SND_OUT_HEADPHONES ? "headphones" : "loudspeaker";
}

static int output_on(int out)
{
    int r;
    int lvl = G.amp_override >= 0 ? G.amp_override : -1;
    if (G.silent) {
        G.out_on = 0;
        say("output: %s kept off (the probe's silent run)", out_name(out));
        return 0;
    }
    if (out == SND_OUT_HEADPHONES)
        r = O.codec_hp ? O.codec_hp(1, (int)G.vol[out]) : SND_ESTATE;
    else if (out == SND_OUT_EARPIECE)
        r = O.amp_receiver(1, lvl >= 0 ? lvl : AMP_RCV_LEVEL);
    else
        r = O.amp_speaker(1, lvl >= 0 ? lvl : AMP_SPK_LEVEL);
    G.out_on = r == 0;
    say("output: %s %s", out_name(out), r == 0 ? "on" : "did NOT start");
    return r;
}

static void output_off(void)
{
    if (!G.out_on)
        return;
    if (G.cur_out == SND_OUT_HEADPHONES) {
        if (O.codec_hp)
            (void)O.codec_hp(0, 0);
    } else {
        O.amp_off();
    }
    G.out_on = 0;
}

/* ---- the guard ----------------------------------------------------------------------------------------------- */
static int32_t budget_q16(void)
{
    return (int32_t)(((uint32_t)att_db[G.att / 20] * att_frac[G.att % 20]) >> 16);
}

static void guard_period(uint64_t ms, int out)
{
    const struct guard_par *p = &gpar[out];
    if (!p->short_lim)
        return;
    G.s_avg += ((int64_t)ms - G.s_avg) / 128;
    G.l_avg += ((int64_t)ms - G.l_avg) / 2048;
    int over = G.s_avg > (int64_t)p->short_lim || G.l_avg > (int64_t)p->long_lim;
    int under = G.s_avg < (int64_t)(p->short_lim * 7 / 10) && G.l_avg < (int64_t)(p->long_lim * 7 / 10);
    int was = G.att;
    if (over && G.att + 2 <= ATT_MAX)
        G.att += 2;
    else if (under && G.att > 0)
        G.att -= 1;
    if (was == 0 && G.att > 0)
        say("guard: the %s's power budget is reached: turning it down (%s)", out_name(out),
            G.s_avg > (int64_t)p->short_lim ? "loud for over a second" : "loud for a long time");
    if (was > 0 && G.att == 0)
        say("guard: the %s is back at its full level", out_name(out));
}

/* ---- the mixer ----------------------------------------------------------------------------------------------- */
static int32_t acc[2 * PF];

static int need_play(void)
{
    if (G.tone)
        return 1;
    for (int i = 0; i < SND_STREAMS; ++i)
        if (st[i].open && snd_queued(i))
            return 1;
    return 0;
}

static void mix_period(uint32_t idx)
{
    int16_t *dst = (int16_t *)(void *)(eng_buf() + idx * APCM_PERIOD_BYTES);
    int out = G.cur_out;
    memset(acc, 0, sizeof acc);
    for (int s = 0; s < SND_STREAMS; ++s) {
        struct stream *x = &st[s];
        if (!x->open)
            continue;
        unsigned q = snd_queued(s), n = q < PF ? q : PF;
        for (unsigned i = 0; i < n; ++i) {
            int32_t l, r;
            if (x->sh) {
                uint32_t j = (x->sh->tail + i) & (x->sh->frames - 1);
                l = x->sh->pcm[2 * j], r = x->sh->pcm[2 * j + 1];
            } else {
                uint32_t j = (x->tail + i) % SND_STREAM_FRAMES;
                l = x->pcm[2 * j], r = x->pcm[2 * j + 1];
            }
            acc[2 * i] += l * (int32_t)x->gain >> 8;
            acc[2 * i + 1] += r * (int32_t)x->gain >> 8;
        }
        if (x->sh) {
            x->sh->tail += n;
            if (n < PF && x->sh->head != x->sh->tail)
                x->sh->xruns++;
        } else {
            x->tail += n;
        }
    }
    tone_render(acc, PF);
    int32_t g = G.muted ? 0 : out == SND_OUT_HEADPHONES ? (G.vol[out] ? 32767 : 0) : vol_q15[G.vol[out]];
    int64_t gb = out == SND_OUT_HEADPHONES ? 65536 : budget_q16();
    int32_t ceil = gpar[out].ceil;
    uint64_t sq = 0;
    uint32_t peak = 0;
    for (unsigned i = 0; i < PF; ++i) {
        int32_t y[2];
        int mono = out != SND_OUT_HEADPHONES;
        int32_t in0 = mono ? (acc[2 * i] + acc[2 * i + 1]) / 2 : acc[2 * i];
        int32_t in1 = mono ? in0 : acc[2 * i + 1];
        for (int c = 0; c < (mono ? 1 : 2); ++c) {
            int64_t x = (int64_t)(c ? in1 : in0) * g >> 15;
            x = x * gb >> 16;
            int64_t a = x < 0 ? -x : x;
            int64_t v = x * G.lim_q16 >> 16, av = v < 0 ? -v : v;
            if (av > ceil) {
                G.lim_q16 = (int32_t)((int64_t)ceil * 65536 / a);
                v = x < 0 ? -ceil : ceil;
                G.stats.guard_limited++;
            } else if (c == 0 && G.lim_q16 < 65536) {
                G.lim_q16 += ((65536 - G.lim_q16) >> 11) + 1;     /* ~43 ms release; the +1 reaches unity */
            }
            if (v > 32767)
                v = 32767;
            if (v < -32768)
                v = -32768;
            y[c] = (int32_t)v;
        }
        if (mono)
            y[1] = y[0];
        dst[2 * i] = (int16_t)y[0];
        dst[2 * i + 1] = (int16_t)y[1];
        for (int c = 0; c < 2; ++c) {
            uint32_t a = (uint32_t)(y[c] < 0 ? -y[c] : y[c]);
            peak = a > peak ? a : peak;
        }
        sq += (uint64_t)((int64_t)y[0] * y[0]);
    }
    G.stats.peak_out = peak;
    G.stats.periods++;
    guard_period(sq / PF, out);
}

static void zero_period(uint32_t idx)
{
    memset(eng_buf() + idx * APCM_PERIOD_BYTES, 0, APCM_PERIOD_BYTES);
}

/* ---- the engine ---------------------------------------------------------------------------------------------- */
static void engine_stop(const char *what)
{
    if (!G.eng)
        return;
    output_off();
    if (G.ce) {
        cal_play_stop();
        G.ce = 0;
        G.cal_off_at = now() + CAL_IDLE_OFF_US;     /* Calliope stays booted a while: the next sound starts fast */
    } else {
        apcm_play_stop();
        if (!G.cap)
            apcm_down();
    }
    G.eng = 0;
    G.stats.stops++;
    say("engine: stopped (%s)", what);
    why("Sound is ready (nothing playing)");
}

/* Calliope off if snd booted it and nothing else uses it (neither our playback nor our capture) */
static void cal_release(const char *what)
{
    if (!G.cal_own || G.ce || G.cc)
        return;
    G.cal_own = 0;
    if (abox_vss_started()) {               /* the CP's voice path runs on it: Calliope is the modem's now */
        say("engine: Calliope left running (%s): the call audio (VSS) uses it", what);
        return;
    }
    if (abox_state() == ABOX_ST_READY || abox_state() == ABOX_ST_FAILED) {
        cal_dump_off("snd");
        abox_off();
        say("engine: Calliope off (%s)", what);
    }
}

/* the Calliope engine would have to boot (or is booting) before anything plays */
static int cal_cold(void)
{
    if (!inited || !cal_avail || G.cal_bad || G.eng || G.suspended || in_call() || apcm_state() != APCM_ST_OFF)
        return 0;
    return G.cal_boot || G.cal_settling || (abox_state() != ABOX_ST_READY && abox_state() != ABOX_ST_CALL);
}

/* a boot of snd's failed (r): Calliope off, counted; the second failure in a row gives up on it */
static void cal_failed(int r)
{
    char cause[160];
    const char *w = abox_why();
    unsigned i = 0;
    for (; w && w[i] && i + 1 < sizeof cause; ++i)          /* abox_off() replaces abox_why() with "ABOX off" */
        cause[i] = w[i];
    cause[i] = 0;
    (void)r;
    abox_off();
    say("engine: Calliope did not boot: %s", cause);
    if (++G.cal_fails >= 2) {
        G.cal_bad = 1;
        say("engine: Calliope failed twice: media uses the bypass engine from now on");
    }
}

/* Calliope running for media (playback or capture): 0 ready, 1 pending (snd's staged boot or its settle: the caller
 * starts on a later poll), < 0 failed. Booting it when nobody runs it only starts the boot (abox_boot_start); cal_tick
 * does the rest, one step per snd_poll. */
static int cal_up(void)
{
    int r, st = abox_state();
    if (G.cal_boot || G.cal_settling)
        return 1;
    if (st == ABOX_ST_BOOTING)
        return SND_EBUSY;
    if (st == ABOX_ST_READY || st == ABOX_ST_CALL)
        return 0;
    if (G.modem)                                    /* 9a: the modem's Calliope is not up (yet, or it failed) */
        return SND_EBUSY;
    G.cal_t0 = now();
    (void)cal_map();                                /* again: a probe may have taken its own windows away */
    /* snd's own HAL for its boot (its RAM copy, file_part), also when another user (the Calliope probe, the modem)
     * set one since (drivers run 13: the probe's HAL had neither, so the copy was not used) */
    if (snd_hal_ok)
        (void)cal_hal_set(&snd_hal);
    cal_abox_hal();                                 /* abox.c's HAL: snd may be the first to boot Calliope */
    r = abox_boot_start();
    if (r > 0) {
        G.cal_boot = 1;
        why("Sound is starting: the audio processor (Calliope) boots");
        return 1;
    }
    cal_unmap();
    if (r < 0)
        cal_failed(r);
    return r;
}

/* what waited for snd's boot learns that it failed: as a failed start did before (no stale burst later) */
static void cal_start_failed(int r)
{
    cal_failed(r);
    if (need_play()) {
        G.retry_at = now() + RETRY_US;
        for (int i = 0; i < SND_STREAMS; ++i)
            if (st[i].open && !st[i].sh)
                st[i].tail = st[i].head;
        G.tone = 0;
        why("Sound could not start: the audio processor (Calliope) did not start");
    }
    if (G.cap_want) {
        G.cap_want = 0;
        why("The microphone could not start: the audio processor (Calliope) did not start");
    }
}

/* snd's staged boot, one step per poll, then the settle by its deadline (abox_poll once a poll); never a delay loop */
static void cal_tick(void)
{
    int r;
    if (G.cal_boot) {
        if ((r = abox_boot_step()) > 0)
            return;
        G.cal_boot = 0;
        cal_unmap();
        if (r == 0 && abox_state() == ABOX_ST_READY) {
            G.cal_settling = 1;
            G.cal_settle_at = now() + CAL_SETTLE_MS * 1000ULL;
            return;
        }
        if (r < 0 && abox_state() == ABOX_ST_FAILED) {
            cal_start_failed(r);
            return;
        }
        /* someone else's abox_off() or abox_call() during our boot: not ours any more, nothing counted */
        say("engine: Calliope's boot for media was taken over (state %d)", SL(abox_state()));
        G.cap_want = 0;
        G.retry_at = now() + RETRY_US;
        return;
    }
    if (!G.cal_settling)
        return;
    if (abox_state() != ABOX_ST_READY) {
        G.cal_settling = 0;
        say("engine: Calliope was taken over while it settled (state %d)", SL(abox_state()));
        G.cap_want = 0;
        G.retry_at = now() + RETRY_US;
        return;
    }
    if ((r = abox_poll()) < 0) {
        G.cal_settling = 0;
        cal_start_failed(r);
        return;
    }
    if (now() < G.cal_settle_at)
        return;
    G.cal_settling = 0;
    G.cal_own = 1;
    G.cal_off_at = now() + CAL_IDLE_OFF_US;
    unsigned slow_stage = 0;
    uint32_t slow_us = abox_boot_slowest(&slow_stage);
    boot_slow_ms = slow_us / 1000U, boot_slow_stage = slow_stage, boot_total_ms = (uint32_t)((now() - G.cal_t0) / 1000U);
    say("engine: Calliope booted for media in %u ms (with %u ms for its post-boot messages); the longest step %u ms "
        "(stage %u)", UL((now() - G.cal_t0) / 1000U), UL(CAL_SETTLE_MS), UL(slow_us / 1000U), UL(slow_stage));
}

/* snd's pending boot dropped (the probe's abort / end): Calliope off unless a call has it */
static void cal_cancel(const char *what)
{
    if (!G.cal_boot && !G.cal_settling)
        return;
    if (G.cal_boot)
        cal_unmap();
    G.cal_boot = G.cal_settling = G.cap_want = 0;
    G.cal_own = 0;
    if (abox_vss_started()) {
        say("engine: Calliope's boot for media handed to the call audio (%s)", what);
        return;
    }
    if (abox_state() != ABOX_ST_CALL) {
        abox_off();
        say("engine: Calliope's boot for media cancelled (%s)", what);
    }
}

/* RDMA0 through Calliope (abox_media.c); 0 or <0 */
static int cal_engine_start(int out)
{
    int r;
    if ((r = cal_up()))
        return r;
    if ((r = cal_play_start(uaif_of(out), APCM_PERIOD_BYTES, APCM_PERIODS))) {
        say("engine: RDMA0 did not start through Calliope: %s", cal_why());
        cal_release("the stream did not start");
        return r;
    }
    G.ce = 1;
    G.cal_fails = 0;
    return 0;
}

static int engine_start(void)
{
    int r, out = resolve();
    if (G.suspended || in_call()) {
        why("Sound is paused: a call has the audio hardware");
        return SND_EBUSY;
    }
    if (!cal_avail && snd_hal_ok && abox_state() == ABOX_ST_OFF && apcm_state() == APCM_ST_OFF) {
        /* 9g: snd_init came while Calliope ran (cal_map refuses then): the Calliope engine again once it is off,
         * never the bypass engine (its RDMA0 does not move on this phone) when Calliope's files are there */
        cal_avail = !cal_hal_set(&snd_hal) && !cal_map();
        say("engine: Calliope's windows %s now that Calliope is off", cal_avail ? "mapped" : "still refused");
    }
    if (cal_avail && !G.cal_bad && apcm_state() == APCM_ST_OFF) {
        if ((r = cal_engine_start(out))) {
            if (r > 0)
                return r;                       /* Calliope boots: a later poll starts the stream */
            why("Sound could not start: the audio processor (Calliope) did not start");
            return r;
        }
        goto started;
    }
    if (call_has()) {
        why("Sound is paused: the audio processor is busy");
        return SND_EBUSY;
    }
    if ((r = apcm_up())) {
        why("Sound could not start: the audio block did not come up");
        say("engine: ABOX did not come up: %s", apcm_why());
        return r;
    }
    if ((r = apcm_play_start(uaif_of(out)))) {
        say("engine: RDMA0 did not start: %s", apcm_why());
        if (!G.cap)
            apcm_down();
        why("Sound could not start: the audio DMA did not start");
        return r;
    }
started:
    G.eng = 1;
    G.cur_out = out;
    G.fill_idx = 1, G.zero_idx = 0, G.lead = 1;
    G.moved_at = now(), G.last_pos = APCM_NOPOS;
    G.lim_q16 = 65536;
    G.s_avg = G.l_avg = 0;
    G.att = 0;
    G.idle_since = 0;
    G.stats.starts++;
    /* the output comes on while zeroes flow (no pop), then the first periods are mixed */
    if ((r = output_on(out))) {
        engine_stop("its output failed");
        why(out == SND_OUT_HEADPHONES ? "Sound could not start: the headphone output did not start"
                                      : "Sound could not start: the amplifier did not start");
        return r;
    }
    why(out == SND_OUT_EARPIECE ? "Playing on the earpiece" : out == SND_OUT_HEADPHONES ? "Playing on the headphones"
                                                                                       : "Playing on the loudspeaker");
    return 0;
}

static void engine_route(int out)
{
    if (G.ce && uaif_of(out) != uaif_of(G.cur_out)) {
        engine_stop("the output moves to the other UAIF");   /* the next poll starts it there */
        G.retry_at = 0;
        return;
    }
    output_off();
    if (uaif_of(out) != uaif_of(G.cur_out) && apcm_play_route(uaif_of(out))) {
        engine_stop("the route could not change");
        return;
    }
    G.cur_out = out;
    G.att = 0, G.s_avg = G.l_avg = 0, G.lim_q16 = 65536;
    if (output_on(out))
        engine_stop("the new output failed");
    else
        why(out == SND_OUT_EARPIECE ? "Playing on the earpiece" : out == SND_OUT_HEADPHONES ? "Playing on the headphones"
                                                                                           : "Playing on the loudspeaker");
}

static void engine_fill(uint64_t t)
{
    uint32_t stt = 0, pos = G.ce ? cal_play_pos(&stt) : apcm_play_pos(&stt);
    if (pos != APCM_NOPOS && pos != G.last_pos)
        G.moved_at = t, G.last_pos = pos;
    /* no position, or the same one, for 250 ms: the DMA does not run. t is the poll's start, older than moved_at when
     * this poll booted Calliope (~1 s on the phone, run 7b): no stall then (was an unsigned underflow) */
    if (t > G.moved_at && t - G.moved_at > STALL_US) {
        say("engine: RDMA0 does not move (STATUS %x)", UL(stt));
        if (!G.ce && !G.dumped++)
            apcm_dump("engine");        /* once a boot: the registers for the log */
        engine_stop("RDMA0 did not run");
        why("Sound stopped: the audio DMA did not run");
        G.retry_at = t + RETRY_US;
        return;
    }
    if (pos == APCM_NOPOS)
        return;
    uint32_t c = pos / APCM_PERIOD_BYTES, dist = (G.fill_idx + APCM_PERIODS - c) % APCM_PERIODS;
    /* dist only shrinks between fills (RDMA0 moves on, fill_idx does not): more than after the last fill, or 0, means
     * RDMA0 passed what was mixed (ran dry). The depth may change between fills: deeper is just mixing more; shallower
     * lets what is already mixed (the stream's own audio) play out, neither re-mixed nor zeroed */
    if (dist == 0 || dist > G.lead) {
        G.stats.xruns++;
        G.fill_idx = (c + 1) % APCM_PERIODS;
        dist = 1;
    }
    uint32_t depth = W_AHEAD;
    for (int i = 0; i < SND_STREAMS; ++i)
        if (st[i].open)
            depth = W_DEEP;
    while (G.zero_idx != c) {
        zero_period(G.zero_idx);
        G.zero_idx = (G.zero_idx + 1) % APCM_PERIODS;
    }
    while (dist < depth + 1) {
        mix_period(G.fill_idx);
        G.fill_idx = (G.fill_idx + 1) % APCM_PERIODS;
        dist++;
    }
    G.lead = dist;
    if (G.ce)
        cal_sync();
    else
        apcm_sync();
}

/* ---- capture ------------------------------------------------------------------------------------------------- */
static unsigned isqrt64(uint64_t m)
{
    uint64_t r = 0, b = 1ULL << 62;
    while (b > m)
        b >>= 2;
    while (b) {
        if (m >= r + b) {
            m -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (unsigned)r;
}

static void cap_reset_queue(void)
{
    G.cap_last = 0, G.cap_head = G.cap_tail = 0, G.cap_n = 0, G.cap_sq = 0, G.cap_level = 0;
}

/* the main mic through Calliope: WDMA1 <- UAIF0 (abox_media.c), booting Calliope if nobody runs it; 0 or <0 */
static int cal_cap_open(void)
{
    int r;
    if ((r = cal_up())) {
        if (r > 0) {
            G.cap_want = 1;                     /* Calliope boots: snd_poll opens the mic after it */
            return 0;
        }
        why("The microphone could not start: the audio processor (Calliope) did not start");
        return r;
    }
    if ((r = O.codec_mic(1))) {
        say("capture: the codec's mic did not start (%d)", SL(r));
        cal_release("the mic did not start");
        why("The microphone could not start: the codec did not answer");
        return r;
    }
    G.cap_codec = 1;
    if ((r = cal_cap_start(CAL_UAIF0, APCM_PERIOD_BYTES, APCM_PERIODS))) {
        say("capture: WDMA1 did not start through Calliope: %s", cal_why());
        (void)O.codec_mic(0);
        G.cap_codec = 0;
        cal_release("the capture did not start");
        why("The microphone could not start: the capture DMA did not start");
        return r;
    }
    G.cc = 1;
    G.cap = 1;
    cap_reset_queue();
    say("capture: the main mic through Calliope (WDMA1), 48 kHz mono");
    return 0;
}

int snd_cap_open(void)
{
    int r;
    if (!inited)
        return SND_ESTATE;
    if (G.cap || G.cap_want)
        return 0;
    if (G.suspended || in_call()) {
        why("The microphone is busy: a call has the audio hardware");
        return SND_EBUSY;
    }
    if (!O.codec_mic) {
        why("No microphone: the codec driver is not linked");
        return SND_ESTATE;
    }
    if (cal_avail && !G.cal_bad && apcm_state() == APCM_ST_OFF)
        return cal_cap_open();
    if (call_has()) {
        why("The microphone is busy: the audio processor plays media");
        return SND_EBUSY;
    }
    if ((r = apcm_up())) {
        why("The microphone could not start: the audio block did not come up");
        return r;
    }
    if ((r = O.codec_mic(1))) {
        say("capture: the codec's mic did not start (%d)", SL(r));
        if (!G.eng)
            apcm_down();
        why("The microphone could not start: the codec did not answer");
        return r;
    }
    G.cap_codec = 1;
    if ((r = apcm_cap_start())) {
        (void)O.codec_mic(0);
        G.cap_codec = 0;
        if (!G.eng)
            apcm_down();
        why("The microphone could not start: the capture DMA did not start");
        return r;
    }
    G.cap = 1;
    cap_reset_queue();
    say("capture: the main mic, 48 kHz mono");
    return 0;
}

static void cap_pull(void)
{
    uint32_t stt = 0, pos = G.cc ? cal_cap_pos(&stt) : apcm_cap_pos(&stt);
    const uint8_t *ring = G.cc ? cal_cap_buf() : apcm_cap_buf();
    uint32_t rb = G.cc ? cal_cap_ring_bytes() : APCM_RING_BYTES;
    if (pos == APCM_NOPOS || !ring || !rb)
        return;
    pos &= ~3U;
    unsigned guard = rb / 4;
    while (G.cap_last != pos && guard--) {
        int16_t l = (int16_t)(ring[G.cap_last] | ring[G.cap_last + 1] << 8);
        if (G.cap_head - G.cap_tail < SND_CAP_SAMPLES) {
            capq[G.cap_head % SND_CAP_SAMPLES] = l;
            G.cap_head++;
        } else {
            G.stats.cap_dropped++;
        }
        G.cap_sq += (uint64_t)((int32_t)l * l);
        if (++G.cap_n == 4800U) {
            G.cap_level = isqrt64(G.cap_sq / 4800U);
            G.cap_n = 0, G.cap_sq = 0;
        }
        G.cap_last = (G.cap_last + 4U) % rb;
    }
}

unsigned snd_cap_read(int16_t *mono, unsigned n)
{
    unsigned k = 0;
    while (k < n && G.cap_tail != G.cap_head)
        mono[k++] = capq[G.cap_tail++ % SND_CAP_SAMPLES];
    return k;
}

unsigned snd_cap_level(void) { return G.cap_level; }

void snd_cap_close(void)
{
    G.cap_want = 0;                             /* a capture still waiting for Calliope's boot never starts */
    if (!G.cap)
        return;
    if (G.cc)
        cal_cap_stop();
    else
        apcm_cap_stop();
    if (G.cap_codec && O.codec_mic)
        (void)O.codec_mic(0);
    if (G.cap_codec && O.codec_off && !(G.eng && G.cur_out == SND_OUT_HEADPHONES))
        O.codec_off();
    G.cap_codec = 0;
    G.cap = 0;
    if (G.cc) {
        G.cc = 0;
        G.cal_off_at = now() + CAL_IDLE_OFF_US;     /* Calliope stays booted a while, as after playback */
    } else if (!G.eng) {
        apcm_down();
    }
    say("capture: closed");
}

/* ---- the loop ------------------------------------------------------------------------------------------------ */
int snd_busy(void)
{
    return inited && !G.suspended &&
           (G.eng || G.cap || G.cal_own || G.cal_boot || G.cal_settling || G.cap_want || need_play());
}

void snd_poll(uint64_t t)
{
    if (!inited)
        return;
    if ((G.ce || G.cc) && (in_call() || (abox_state() != ABOX_ST_READY && abox_state() != ABOX_ST_CALL))) {
        /* a call came without snd_suspend_for_call(), or Calliope stopped under us (the modem's abox_off) */
        int c = in_call();
        if (G.ce)
            say(c ? "a call took Calliope while media played (snd_suspend_for_call was not called): stream stopped"
                  : "Calliope stopped while media played: stream dropped");
        if (G.cc)
            say(c ? "a call took Calliope while the mic recorded (snd_suspend_for_call was not called): capture "
                    "stopped" : "Calliope stopped while the mic recorded: capture dropped");
        if (G.ce) {
            output_off();
            cal_play_stop();
        }
        if (G.cc) {
            cal_cap_stop();                     /* during a call it leaves UAIF0 MIC (the call's) alone */
            if (G.cap_codec && O.codec_mic && !c)
                (void)O.codec_mic(0);           /* the call's codec path is the call's */
        }
        G.eng = G.ce = G.cap = G.cc = G.cap_codec = 0;
        G.cal_own = 0;                          /* the call's (or nobody's) now */
        G.retry_at = t + RETRY_US;
        why(c ? "Sound is paused: a call has the audio hardware" : "Sound stopped: the audio processor stopped");
        return;
    }
    if ((G.ce || G.cc || G.cal_own) && abox_poll() < 0) {
        say("Calliope fell over: %s", abox_why());
        output_off();
        cal_play_stop();
        cal_cap_stop();
        if (G.cc && G.cap_codec && O.codec_mic)
            (void)O.codec_mic(0);
        if (!G.modem)                           /* 9a: the modem's Calliope: its own poll sees it and stops */
            abox_off();
        G.eng = G.ce = G.cal_own = 0;
        if (G.cc)
            G.cap = G.cc = G.cap_codec = 0;
        G.retry_at = t + RETRY_US;
        why("Sound stopped: the audio processor stopped");
        return;
    }
    if (!G.eng && G.cal_own && t >= G.cal_off_at)
        cal_release("30 s without media");
    if (((G.eng && !G.ce) || (G.cap && !G.cc)) && call_has()) {
        /* a missed snd_suspend_for_call(): ABOX is Calliope's now; nothing of ours is written to it */
        say("a call took ABOX while media sound ran (snd_suspend_for_call was not called): sound state dropped");
        apcm_forget();
        if (G.out_on && G.cur_out == SND_OUT_HEADPHONES && O.codec_hp)
            (void)O.codec_hp(0, 0);
        G.eng = G.cap = G.out_on = G.cap_codec = 0;
        why("Sound is paused: a call has the audio hardware");
        return;
    }
    if (G.suspended)
        return;
    cal_tick();
    if (G.last_poll && t - G.last_poll > LATE_US && G.eng)
        G.stats.late_polls++;
    G.last_poll = t;
    int need = need_play();
    if (need && !G.eng && t >= G.retry_at) {
        if (engine_start() < 0) {
            G.retry_at = t + RETRY_US;
            for (int i = 0; i < SND_STREAMS; ++i)       /* no stale burst later */
                if (st[i].open && !st[i].sh)
                    st[i].tail = st[i].head;
            G.tone = 0;
        }
    }
    if (G.eng) {
        int want = resolve();
        if (want != G.cur_out)
            engine_route(want);
    }
    if (G.eng) {
        engine_fill(t);
        if (need)
            G.idle_since = 0;
        else if (!G.idle_since)
            G.idle_since = t;
        else if (t - G.idle_since >= IDLE_STOP_US)
            engine_stop("2 s of silence");
    }
    if (G.cap_want && !G.cal_boot && !G.cal_settling) {
        G.cap_want = 0;
        if (!in_call())
            (void)cal_cap_open();               /* Calliope is up now; its failures are said there */
    }
    if (G.cap)
        cap_pull();
}

void snd_suspend_for_call(void)
{
    if (!inited)
        return;
    G.suspended = 1;
    if (G.cal_boot || G.cal_settling) {
        /* snd's staged boot is finished here (blocking, as the modem's own abox_boot would be): the modem's
         * abox_boot() then finds Calliope running ("already runs"), or boots afresh after a failure (Calliope off) */
        int r = 0;
        if (G.cal_boot) {
            r = abox_boot_finish();
            cal_unmap();
        }
        G.cal_boot = G.cal_settling = G.cap_want = 0;
        G.tone = 0;                             /* it never started: no tone after the call */
        if (r < 0)
            cal_failed(r);
        else
            say("call: Calliope's boot for media finished here; Calliope left running for the call");
    }
    if (G.ce) {
        /* Calliope stays booted: the modem's abox_boot() then finds it running and uses it (abox_call) */
        G.tone = 0;
        output_off();
        cal_play_stop();
        G.eng = G.ce = 0;
        say("call: media sound stopped; Calliope left running for the call");
    }
    if (G.cc) {
        /* the mic before the call takes UAIF0 MIC and the codec */
        cal_cap_stop();
        if (G.cap_codec && O.codec_mic)
            (void)O.codec_mic(0);
        G.cap = G.cc = G.cap_codec = 0;
        say("call: the mic's capture stopped; Calliope left running for the call");
    }
    G.cal_own = 0;                              /* the modem's now: its audio_stop() turns it off */
    if (G.eng || G.cap || apcm_state() != APCM_ST_OFF) {
        G.tone = 0;
        output_off();
        if (G.cap) {
            apcm_cap_stop();
            if (G.cap_codec && O.codec_mic)
                (void)O.codec_mic(0);
            G.cap = G.cap_codec = 0;
        }
        apcm_play_stop();
        apcm_down();
        G.eng = 0;
        say("call: media sound stopped and ABOX given back");
    }
    why("Sound is paused for a call");
}

void snd_calliope_shared(int on)
{
    modem_pending = !!on;                       /* 9g: snd_init takes it if snd is not up yet */
    if (!inited)
        return;
    if (on) {
        snd_suspend_for_call();                 /* media stopped, a staged boot finished, Calliope not ours */
        G.suspended = 0;                        /* ... but media plays through the modem's Calliope */
        G.modem = 1;
        G.retry_at = 0;
        say("cellular: Calliope is the modem's while Cellular runs: media plays through it while no call routes");
        why("Sound is ready (nothing playing)");
        return;
    }
    if (!G.modem)
        return;
    if (G.ce || G.cc || G.eng || G.cap)
        snd_suspend_for_call();                 /* before the modem's abox_off(): stopped cleanly, not dropped */
    G.suspended = 0;
    G.modem = 0;
    G.cal_own = 0;
    G.retry_at = 0;
    say("cellular: off: Calliope goes with the modem; media boots its own again");
    why("Sound is ready (nothing playing)");
}

void snd_resume_after_call(void)
{
    if (!inited || !G.suspended)
        return;
    G.suspended = 0;
    G.retry_at = 0;
    if (cal_avail && abox_state() == ABOX_ST_READY)
        say("call: Calliope still runs (the modem's while Cellular is on): media plays through it");
    why("Sound is ready (nothing playing)");
    say("call: over, media sound may play again");
}

/* ---- the driver probe (drvprobe.h) --------------------------------------------------------------------------- */
static struct {
    int active, level, step, aud_was_on, fail;
    uint64_t t0, ts;
    uint32_t last, moved, cap_moved, rate, cap_rate, peak, uaif1_st;
    unsigned rms;
    uint64_t sq;
    uint32_t n;
    int saved_route, saved_vol[4];
    unsigned trial, found, trials_run;
    uint32_t found_moved, rate2;
    char found_desc[48];
    /* through Calliope */
    int sid, sid_open, saved, saved_muted;
    uint64_t tm;                                /* when the measured window began (0: not yet) */
    uint32_t skip;
    struct gz gb, gt;                           /* the mic: baseline, the tone now */
    struct gz_verdict v[2];                     /* the earpiece, the loudspeaker */
} P;

/* level 1: the combined RDMA0 sweep (bypass), ~1100 trials of ~8 ms; level 2: Calliope's boot (<= 3.5 s) + 0.3 s +
 * the mic's 1.1 s baseline + 2.1 s earpiece + 1.1 s loudspeaker + the starts and stops */
static const unsigned budget_of[3] = { 2000, 20000, 14000 };
#define ENGINE_WAIT_US 4000000ULL               /* the probes' wait for snd's staged Calliope boot + settle */

static int pfinish(int ok, const char *line)
{
    if (boot_total_ms) {                /* the boot's timing first: pmsg / the run's summary keep ~100 characters */
        char b[240];
        unsigned n = 0;
        n = (unsigned)snprintf_like3(b, sizeof b, boot_slow_ms, boot_slow_stage, boot_total_ms);
        for (const char *q = line; *q && n + 1 < sizeof b; ++q)
            b[n++] = *q;
        b[n] = 0;
        boot_total_ms = 0;
        return pfinish_raw(ok, b);
    }
    return pfinish_raw(ok, line);
}
static int pfinish_raw(int ok, const char *line)
{
    if (P.level > 0 && !G.eng && !G.cap)
        cal_cancel("the probe is done");        /* a boot the probe started and no longer waits for */
    if (!G.eng)
        cal_release("the probe is done");       /* a probe leaves Calliope off (snd booted it for the tones) */
    psay("%s %s", ok ? "PASS" : "FAIL", line);
    P.active = 0;
    return 1;
}

void drv_audio_abort(void)
{
    if (!inited)
        return;
    G.tone = 0;
    output_off();
    G.silent = 0;
    if (P.active && P.sid_open)
        snd_close(P.sid);
    P.sid_open = 0;
    if (G.cap)
        snd_cap_close();
    if (G.ce)
        cal_play_stop();
    G.ce = 0;
    cal_cancel("the probe was aborted");
    cal_release("the probe was aborted");
    apcm_cap_stop();
    apcm_play_stop();
    apcm_down();
    G.eng = G.cap = 0;
    G.amp_override = -1;
    if (P.active && (P.level == 2 || P.saved)) {
        G.route = P.saved_route;
        for (int i = 1; i < 4; ++i)
            G.vol[i] = (unsigned)P.saved_vol[i];
    }
    if (P.active && P.saved)
        G.muted = P.saved_muted;
    P.saved = 0;
    P.active = 0;
}

static uint32_t moved_since(uint32_t *last, uint32_t pos)
{
    if (pos == APCM_NOPOS)
        return 0;
    uint32_t d = (pos + APCM_RING_BYTES - *last) % APCM_RING_BYTES;
    *last = pos;
    return d;
}

static int probe0(void)
{
    struct apcm_look l;
    apcm_look(&l);
    psay("AUD domain %s (PMU 0x14064004 = %x), ABOX CPU %s", l.aud_on ? "on" : "off", UL(l.aud_stat),
         (l.cpu_stat & 1U) ? "on" : "off");
    if (l.aud_on)
        psay("PLL_AUD CON0 %x CON3 %x = %u Hz, SysMMU CTRL %x, IP_INDEX %x", UL(l.pll_con0), UL(l.pll_con3),
             UL(l.pll_hz), UL(l.mmu_ctrl), UL(l.ip_index));
    else
        psay("PLL_AUD not read: CMU_AUD is in the AUD domain, which is off");
    const char *amps = "not read (their I2C bus is not set up yet; reading needs set-up writes)";
    int amp_bad = 0;
    if (O.amp_bus_ready && O.amp_bus_ready() && O.amp_probe) {
        int r = O.amp_probe();
        amps = r == 0 ? "both MAX98512 dies answer (REV_ID read)" : "did NOT answer on their bus";
        amp_bad = r != 0;
    }
    psay("amplifiers: %s", amps);
    if (O.codec_id_readonly)
        (void)O.codec_id_readonly();    /* audio_probe(): the ID only if the codec is powered and out of reset */
    else
        psay("codec: not read (no read-only hook)");
    return pfinish(!amp_bad, amp_bad ? "the amplifiers did not answer (0x39 / 0x3A on HSI2C 0x108C0000)"
                                     : l.aud_on ? "read only: AUD domain on, PLL_AUD read, amps/codec as logged"
                                                : "read only: AUD domain off (nothing in it read), amps/codec as logged");
}

static int probe1(void)
{
    uint64_t t = now();
    switch (P.step) {
    case 0: {
        struct apcm_look l;
        if (call_has())
            return pfinish(0, "a call has ABOX: not run (nothing touched)");
        if (G.eng || G.cap)
            return pfinish(0, "media sound is in use: not run (nothing touched)");
        apcm_look(&l);
        P.aud_was_on = l.aud_on;
        psay("level 1: RDMA0 zeroes -> UAIF1 1 s (amps off), WDMA1 <- UAIF0 0.5 s (codec off); a stall: dump + RDMA0 "
             "trials; all back as found");
        if (apcm_up()) {
            psay("ABOX did not come up: %s", apcm_why());
            return pfinish(0, "ABOX did not come up in bypass (see the apcm lines): everything put back");
        }
        if (apcm_play_start(APCM_UAIF1)) {
            apcm_down();
            return pfinish(0, "RDMA0 did not start (see the apcm lines): everything put back");
        }
        P.last = 0, P.moved = 0, P.ts = t, P.step = 1;
        return 0;
    }
    case 1:
        P.moved += moved_since(&P.last, apcm_play_pos(0));
        if (t - P.ts < 1000000ULL)
            return 0;
        P.rate = (uint32_t)((uint64_t)P.moved * 1000000ULL / (t - P.ts));
        P.uaif1_st = apcm_uaif_status(APCM_UAIF1);
        psay("RDMA0 read %u bytes a second (192000 = 48 kHz 16-bit stereo); UAIF1 STATUS %x%s", UL(P.rate),
             UL(P.uaif1_st), (P.uaif1_st & 1U) ? " (ERROR_OF_SPK)" : "");
        if (!P.moved) {
            /* the read-only dump, then RDMA0-only trials (silent: the amps are off, RDMA only reads its ring) */
            apcm_dump("RDMA0 still");
            P.step = 10, P.trial = 0;
            return 0;
        }
        goto to_capture;
    case 10: {
        char d[48];
        for (unsigned k = 0; k < 8; ++k) {
            uint32_t mv = 0;
            int r = apcm_trial(P.trial, &mv, d, sizeof d);
            if (r < 0) {
                apcm_down();
                return pfinish(0, "RDMA0 did not stop during the trials: everything put back");
            }
            if (r == 0) {
                psay("trials: %u configurations, none moved RDMA0 (see AUDIO.md 8)", UL(P.trials_run));
                goto to_capture;
            }
            P.trials_run++;
            if (mv >= 64U) {
                P.found = P.trial + 1, P.found_moved = mv;
                memcpy(P.found_desc, d, sizeof d);
                psay("trial %u (%s): RDMA0 moved %u bytes in 5 ms: adopted; measuring 0.5 s", UL(P.trial), d, UL(mv));
                (void)apcm_trial_adopt(P.trial);
                apcm_play_stop();
                apcm_down();
                if (apcm_up() || apcm_play_start(APCM_UAIF1)) {
                    apcm_down();
                    return pfinish(0, "ABOX did not come up again with the found setting");
                }
                P.last = 0, P.moved = 0, P.ts = now(), P.step = 11;
                return 0;
            }
            P.trial++;
        }
        return 0;
    }
    case 11:
        P.moved += moved_since(&P.last, apcm_play_pos(0));
        if (t - P.ts < 500000ULL)
            return 0;
        P.rate2 = (uint32_t)((uint64_t)P.moved * 1000000ULL / (t - P.ts));
        psay("with %s: RDMA0 read %u bytes a second", P.found_desc, UL(P.rate2));
        /* fall through */
    to_capture:
        apcm_play_stop();
        if (apcm_state() != APCM_ST_UP || apcm_cap_start()) {
            apcm_down();
            return pfinish(0, "WDMA1 did not start (see the apcm lines): everything put back");
        }
        P.last = 0, P.cap_moved = 0, P.ts = t, P.step = 2;
        return 0;
    default: {
        P.cap_moved += moved_since(&P.last, apcm_cap_pos(0));
        if (t - P.ts < 500000ULL)
            return 0;
        P.cap_rate = (uint32_t)((uint64_t)P.cap_moved * 1000000ULL / (t - P.ts));
        const uint8_t *ring = apcm_cap_buf();
        P.peak = 0;
        for (uint32_t i = 0; ring && i + 1 < APCM_RING_BYTES; i += 2) {
            int16_t v = (int16_t)(ring[i] | ring[i + 1] << 8);
            uint32_t a = (uint32_t)(v < 0 ? -(int32_t)v : v);
            P.peak = a > P.peak ? a : P.peak;
        }
        uint32_t ust = apcm_uaif_status(APCM_UAIF0);
        if (!P.cap_moved)
            apcm_dump("WDMA1 still");
        apcm_cap_stop();
        apcm_down();
        struct apcm_look l;
        apcm_look(&l);
        psay("WDMA1 wrote %u bytes a second, peak %u of 32767 (codec in reset: near 0 expected); UAIF0 STATUS %x",
             UL(P.cap_rate), UL(P.peak), UL(ust));
        int play_ok = P.rate >= 182400U && P.rate <= 201600U, cap_ok = P.cap_rate >= 182400U && P.cap_rate <= 201600U;
        if (l.aud_on != P.aud_was_on)
            return pfinish(0, "the AUD domain is not back as found");
        if (P.found) {
            char line[160];
            int ok2 = P.rate2 >= 182400U && P.rate2 <= 201600U;
            snprintf_like(line, sizeof line, ok2 ? "FOUND: RDMA0 runs at 48 kHz only with " : "FOUND: RDMA0 moves (wrong rate) only with ",
                          P.found_desc, P.cap_moved ? "; WDMA1 moved with it too" : "; WDMA1 still did not move");
            return pfinish(0, line);
        }
        if (!P.moved)
            return pfinish(0, "RDMA0 did not move, nor with any trial setting (dump above): media needs Calliope here");
        if (!play_ok)
            return pfinish(0, "RDMA0 moved at the wrong rate: its CTRL0 format bits are needed (AUDIO.md, risks)");
        if (!P.cap_moved)
            return pfinish(0, "playback ran at 48 kHz but WDMA1 did not move (the capture path)");
        if (!cap_ok)
            return pfinish(0, "playback ran at 48 kHz but WDMA1 wrote at the wrong rate");
        return pfinish(1, "RDMA0 -> UAIF1 and UAIF0 -> WDMA1 ran at 48 kHz 16-bit stereo; all back as found");
    }
    }
}

/* ---- the probe through Calliope (cal_avail): snd's real engine end to end ------------------------------------ */
static void lfmt(char *b, unsigned cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, cap, "", fmt, ap);
    va_end(ap);
}
/* tenths of a dB as "-12.3" */
static const char *db_str(char *b, int32_t d10)
{
    unsigned a = (unsigned)(d10 < 0 ? -d10 : d10);
    lfmt(b, 16, "%s%u.%u", d10 < 0 ? "-" : "", UL(a / 10U), UL(a % 10U));
    return b;
}
static void cprobe_save(void)
{
    P.saved = 1;
    P.saved_route = G.route;
    P.saved_muted = G.muted;
    for (int i = 1; i < 4; ++i)
        P.saved_vol[i] = (int)G.vol[i];
}
/* the user's settings back (drv_audio_abort does the same on its way) */
static void cprobe_restore(void)
{
    if (P.sid_open)
        snd_close(P.sid);
    P.sid_open = 0;
    G.silent = 0;
    G.amp_override = -1;
    if (P.saved) {
        G.route = P.saved_route;
        G.muted = P.saved_muted;
        for (int i = 1; i < 4; ++i)
            G.vol[i] = (unsigned)P.saved_vol[i];
    }
    P.saved = 0;
}
/* every failure: the tone, the output (amps first), the streams, the mic, Calliope if snd booted it, the settings */
static int cfail(const char *line)
{
    char b[240];
    unsigned n = 0;
    psay("%s", why_buf);
    psay("failed: %s", line);
    /* the cause first (snd's newest failure line), then the probe's step: the summary keeps ~100 characters */
    for (const char *q = last_bad[0] ? last_bad : line; *q && n + 1 < sizeof b; ++q)
        b[n++] = *q;
    if (last_bad[0])
        for (const char *q = " | step: "; *q && n + 1 < sizeof b; ++q)
            b[n++] = *q;
    for (const char *q = last_bad[0] ? line : ""; *q && n + 1 < sizeof b; ++q)
        b[n++] = *q;
    b[n] = 0;
    drv_audio_abort();
    return pfinish(0, b);
}
static void feed_zeros(void)
{
    static const int16_t z[2 * 256];
    while (P.sid_open && snd_space(P.sid) && snd_write(P.sid, z, 256))
        ;
}
/* the mic's queued samples into g (the first P.skip dropped) */
static void take_mic(struct gz *g)
{
    int16_t b[256];
    unsigned k;
    while ((k = snd_cap_read(b, 256)) > 0)
        for (unsigned i = 0; i < k; ++i) {
            P.n++;
            if (P.skip) {
                P.skip--;
                continue;
            }
            gz_put(g, b[i]);
        }
}
/* bytes a position moved since the last; the window starts at the first valid position. 1 once ms have passed. */
static int window(uint32_t pos, uint32_t *moved, uint32_t ms)
{
    uint64_t t = now();
    if (!P.tm) {
        if (pos != CAL_NOPOS)
            P.tm = t, P.last = pos;
        return 0;
    }
    *moved += moved_since(&P.last, pos);
    return t - P.tm >= ms * 1000ULL;
}

/* Level 1 (silent): zeroes through snd's engine (Calliope's RDMA0, output kept off, muted) 1 s, then the main mic
 * through Calliope's WDMA1 0.5 s; Calliope off after if snd booted it (abox_off: the AUD domain ends off). */
static int probe1c(void)
{
    char line[200];
    switch (P.step) {
    case 0:
        if (in_call())
            return pfinish(0, "a call has ABOX: not run (nothing touched)");
        if (G.eng || G.cap)
            return pfinish(0, "media sound is in use: not run (nothing touched)");
        psay("level 1 (silent, through Calliope): snd's engine plays zeroes 1 s (output kept off), then the main mic "
             "0.5 s; Calliope off after if snd booted it");
        cprobe_save();
        G.silent = 1, G.muted = 1;
        if ((P.sid = snd_open()) < 0)
            return cfail("no free stream for the probe");
        P.sid_open = 1;
        feed_zeros();
        G.retry_at = 0;
        P.ts = now(), P.step = 5;
        /* fall through */
    case 5:                                     /* snd boots Calliope a step per poll (~1 s), then starts: <= 4 s */
        feed_zeros();
        snd_poll(now());
        if (!G.eng) {
            if (!G.cal_boot && !G.cal_settling)
                return cfail("the engine did not start through Calliope (see the snd / cal / abox lines)");
            if (now() - P.ts > ENGINE_WAIT_US)
                return cfail("the engine through Calliope did not start within 4 s");
            return 0;
        }
        if (!G.ce)
            return cfail("the engine started in bypass, not through Calliope (Calliope failed before?)");
        P.tm = 0, P.moved = 0, P.ts = now(), P.step = 1;
        return 0;
    case 1:
        feed_zeros();
        snd_poll(now());
        if (!G.eng || !G.ce)
            return cfail("the engine stopped during its 1 s (see the snd lines)");
        if (!P.tm && now() - P.ts > 1000000ULL)
            return cfail("RDMA0 shows no position through Calliope (STATUS not in progress for 1 s)");
        if (!window(cal_play_pos(0), &P.moved, 1000))
            return 0;
        P.rate = (uint32_t)((uint64_t)P.moved * 1000000ULL / (now() - P.tm));
        {
            struct snd_stats ss;
            snd_stats(&ss);
            psay("engine: RDMA0 read %u B/s through Calliope (%u periods mixed, %u xruns); output stayed off",
                 UL(P.rate), UL(ss.periods), UL(ss.xruns));
        }
        snd_close(P.sid);
        P.sid_open = 0;
        engine_stop("the probe's 1 s is done");
        if (snd_cap_open())
            return cfail("the main mic did not start through Calliope (codec or WDMA1; see the lines above)");
        if (!G.cc)
            return cfail("the mic opened in bypass, not through Calliope");
        gz_reset(&P.gb);
        P.tm = 0, P.cap_moved = 0, P.n = 0, P.skip = 0, P.ts = now(), P.step = 2;
        return 0;
    default: {
        snd_poll(now());
        if (!G.cap || !G.cc)
            return cfail("the capture stopped during its 0.5 s (see the lines above)");
        take_mic(&P.gb);
        if (!P.tm && now() - P.ts > 1000000ULL)
            return cfail("WDMA1 shows no position through Calliope (STATUS not in progress for 1 s)");
        if (!window(cal_cap_pos(0), &P.cap_moved, 500))
            return 0;
        P.cap_rate = (uint32_t)((uint64_t)P.cap_moved * 1000000ULL / (now() - P.tm));
        snd_cap_close();
        cprobe_restore();
        psay("mic: WDMA1 wrote %u B/s through Calliope; %u samples, RMS %u, peak %u (of 32767)", UL(P.cap_rate),
             UL(P.n), UL(gz_rms(&P.gb)), UL(P.gb.peak));
        int play_ok = P.rate >= 182400U && P.rate <= 201600U, cap_ok = P.cap_rate >= 182400U && P.cap_rate <= 201600U;
        if (!play_ok) {
            lfmt(line, sizeof line, "snd's engine through Calliope: RDMA0 %u B/s (192000 expected); WDMA1 %u B/s",
                 UL(P.rate), UL(P.cap_rate));
            return pfinish(0, line);
        }
        if (!cap_ok) {
            lfmt(line, sizeof line, "the mic through Calliope: WDMA1 %u B/s (192000 expected); RDMA0 %u B/s ok",
                 UL(P.cap_rate), UL(P.rate));
            return pfinish(0, line);
        }
        lfmt(line, sizeof line, "snd's engine through Calliope: RDMA0 %u B/s, WDMA1 %u B/s, mic RMS %u peak %u; "
             "output stayed off", UL(P.rate), UL(P.cap_rate), UL(gz_rms(&P.gb)), UL(P.gb.peak));
        return pfinish(1, line);
    }
    }
}

/* one output's loopback result, for the logs and the last line */
static void heard_line(const char *out, const struct gz_verdict *v, const struct gz *g)
{
    char a[16], b[16];
    psay("%s: 1 kHz %s dB over the baseline, %s dB over 700/1300 Hz (%u x 20 ms, RMS %u): heard %s", out,
         db_str(a, v->over_base), db_str(b, v->over_nb), UL(g->blocks), UL(gz_rms(g)), v->heard ? "yes" : "no");
}

/* Level 2 (quiet, may run unattended): the mic's 1 s baseline through Calliope, then snd_tone's 1 kHz on the earpiece
 * (amp level 0, -20 dBFS: the tone's own level) 2 s and on the loudspeaker (amp level 0, volume 13: -25 dBFS) 1 s while
 * the mic records; a Goertzel filter judges each (goertzel.h). Output and amps off on every way out. */
#define CP_SKIP 4800U                   /* the first 100 ms of the mic / of each output not judged */
static int probe2c(void)
{
    char line[220], a[16], b[16];
    switch (P.step) {
    case 0:
        if (in_call())
            return pfinish(0, "a call has ABOX: not run (nothing touched)");
        if (G.eng || G.cap)
            return pfinish(0, "media sound is in use: not run (nothing touched)");
        psay("level 2 (quiet, through Calliope): the mic's baseline 1 s, then 1 kHz on the earpiece (-20 dBFS, amp "
             "level 0) 2 s and the loudspeaker (-25 dBFS, amp level 0) 1 s while the mic listens");
        cprobe_save();
        G.amp_override = 0;
        G.route = SND_OUT_EARPIECE;
        G.muted = 0;
        G.vol[SND_OUT_EARPIECE] = SND_VOL_MAX;      /* the test tone as made: -20 dBFS */
        G.vol[SND_OUT_SPEAKER] = 13;                /* -5 dB: -25 dBFS */
        if (snd_cap_open())
            return cfail("the main mic did not start through Calliope (codec or WDMA1; see the lines above)");
        P.ts = now(), P.step = 5;
        /* fall through */
    case 5:                                     /* the mic waits for snd's staged Calliope boot: <= 4 s */
        if (!G.cap) {
            snd_poll(now());
            if (!G.cap) {
                if (!G.cap_want)
                    return cfail("the main mic did not start through Calliope (codec or WDMA1; see the lines above)");
                if (now() - P.ts > ENGINE_WAIT_US)
                    return cfail("the main mic through Calliope did not start within 4 s");
                return 0;
            }
        }
        if (!G.cc)
            return cfail("the mic opened in bypass, not through Calliope");
        gz_reset(&P.gb);
        P.n = 0, P.skip = CP_SKIP, P.ts = now(), P.step = 1;
        return 0;
    case 1:
        snd_poll(now());
        if (!G.cap || !G.cc)
            return cfail("the capture stopped during the baseline (see the lines above)");
        take_mic(&P.gb);
        if (P.gb.blocks < 50U) {                    /* 1 s */
            if (now() - P.ts > 3000000ULL)
                return cfail("the mic gave less than 1 s of samples in 3 s (WDMA1 through Calliope)");
            return 0;
        }
        psay("baseline: the mic's RMS %u, peak %u over %u ms", UL(gz_rms(&P.gb)), UL(P.gb.peak),
             UL(P.gb.blocks * 20U));
        (void)snd_tone(SND_TONE_TEST);
        G.retry_at = 0;
        snd_poll(now());
        if (!G.eng || !G.ce || G.cur_out != SND_OUT_EARPIECE)
            return cfail("the earpiece path did not start through Calliope (see the snd / cal / amp lines)");
        gz_reset(&P.gt);
        P.skip = CP_SKIP, P.ts = now(), P.step = 2;
        return 0;
    case 2:
        snd_poll(now());
        if (!G.eng || !G.cap)
            return cfail("the earpiece path or the mic stopped (see the lines above)");
        take_mic(&P.gt);
        if (now() - P.ts < 2000000ULL)
            return 0;
        gz_judge(&P.gt, &P.gb, &P.v[0]);
        heard_line("earpiece", &P.v[0], &P.gt);
        G.route = SND_OUT_SPEAKER;
        snd_poll(now());
        if (!G.eng || G.cur_out != SND_OUT_SPEAKER)
            return cfail("the loudspeaker path did not start (see the lines above)");
        gz_reset(&P.gt);
        P.skip = CP_SKIP, P.ts = now(), P.step = 3;
        return 0;
    default: {
        snd_poll(now());
        if (!G.eng || !G.cap)
            return cfail("the loudspeaker path or the mic stopped (see the lines above)");
        take_mic(&P.gt);
        if (now() - P.ts < 1000000ULL)
            return 0;
        gz_judge(&P.gt, &P.gb, &P.v[1]);
        heard_line("loudspeaker", &P.v[1], &P.gt);
        snd_tone_stop();
        engine_stop("the probe's tones are done");  /* the amp off first */
        snd_cap_close();
        cprobe_restore();
        int any = P.v[0].heard || P.v[1].heard;
        lfmt(line, sizeof line, "%searpiece: heard by the mic: %s (1 kHz %s dB over baseline); loudspeaker: heard by "
             "the mic: %s (1 kHz %s dB over baseline)", any ? "" : "no tone heard: ", P.v[0].heard ? "yes" : "no",
             db_str(a, P.v[0].over_base), P.v[1].heard ? "yes" : "no", db_str(b, P.v[1].over_base));
        return pfinish(any, line);
    }
    }
}

static void probe2_restore(void)
{
    G.amp_override = -1;
    G.route = P.saved_route;
    for (int i = 1; i < 4; ++i)
        G.vol[i] = (unsigned)P.saved_vol[i];
}

static int probe2(void)
{
    uint64_t t = now();
    switch (P.step) {
    case 0:
        if (call_has())
            return pfinish(0, "a call has ABOX: not run (nothing touched)");
        if (G.eng || G.cap)
            return pfinish(0, "media sound is in use: not run (nothing touched)");
        psay("level 2 (attended): 1 kHz at -20 dBFS: 1 s earpiece (amp level 0), 1 s loudspeaker (amp level 0), "
             "then 2 s of the main mic");
        P.saved_route = G.route;
        for (int i = 1; i < 4; ++i)
            P.saved_vol[i] = (int)G.vol[i];
        G.amp_override = 0;
        G.route = SND_OUT_EARPIECE;
        G.vol[SND_OUT_EARPIECE] = G.vol[SND_OUT_SPEAKER] = SND_VOL_MAX;
        G.retry_at = 0;
        (void)snd_tone(SND_TONE_TEST);
        snd_poll(t);
        if (!G.eng) {
            psay("%s", why_buf);
            drv_audio_abort();
            return pfinish(0, "the earpiece path did not start (see the snd / apcm / amp lines)");
        }
        P.ts = t, P.step = 1;
        return 0;
    case 1:
        snd_poll(t);
        if (!G.eng) {
            drv_audio_abort();
            return pfinish(0, "the earpiece path stopped (see the lines above)");
        }
        if (t - P.ts < 1000000ULL)
            return 0;
        psay("earpiece: 1 s sent (%u periods, last peak %u)", UL(G.stats.periods), UL(G.stats.peak_out));
        G.route = SND_OUT_SPEAKER;
        snd_poll(t);
        if (!G.eng || G.cur_out != SND_OUT_SPEAKER) {
            drv_audio_abort();
            return pfinish(0, "the loudspeaker path did not start (see the lines above)");
        }
        P.ts = t, P.step = 2;
        return 0;
    case 2:
        snd_poll(t);
        if (t - P.ts < 1000000ULL)
            return 0;
        snd_tone_stop();
        engine_stop("the probe's tones are done");
        cal_release("the mic test uses the bypass engine");   /* capture runs on apcm, not while Calliope runs */
        psay("loudspeaker: 1 s sent; output off");
        if (snd_cap_open()) {
            psay("%s", why_buf);
            drv_audio_abort();
            return pfinish(0, "the main mic did not start (codec or WDMA1; see the lines above)");
        }
        P.ts = t, P.step = 3, P.sq = 0, P.n = 0, P.peak = 0;
        return 0;
    default: {
        int16_t b[256];
        unsigned k;
        snd_poll(t);
        while ((k = snd_cap_read(b, 256)) > 0)
            for (unsigned i = 0; i < k; ++i) {
                uint32_t a = (uint32_t)(b[i] < 0 ? -(int32_t)b[i] : b[i]);
                P.peak = a > P.peak ? a : P.peak;
                P.sq += (uint64_t)((int32_t)b[i] * b[i]);
                P.n++;
            }
        if (t - P.ts < 2000000ULL)
            return 0;
        snd_cap_close();
        probe2_restore();
        apcm_down();
        P.rms = P.n ? isqrt64(P.sq / P.n) : 0;
        psay("mic: %u samples in 2 s, RMS %u, peak %u (of 32767)", UL(P.n), UL(P.rms), UL(P.peak));
        if (P.n < 48000U)
            return pfinish(0, "the main mic gave fewer than 1 s of samples in 2 s");
        return pfinish(1, "1 kHz sent to the earpiece and the loudspeaker (ask: both heard, quietly?); mic recorded");
    }
    }
}

#ifdef SBOS_KERNEL
void snd_init_libsb(void);
#endif

int drv_audio_probe(int level, unsigned *wait_ms)
{
    if (wait_ms)
        *wait_ms = 0;
#ifdef SBOS_KERNEL
    if (!inited)
        snd_init_libsb();
#endif
    if (!inited)
        return 1;
    if (level < 0 || level > 2)
        return pfinish(0, "no such level");
    if (!P.active || P.level != level) {
        memset(&P, 0, sizeof P);
        P.active = 1, P.level = level, P.t0 = now();
        last_bad[0] = 0;
    }
    if (now() - P.t0 > (uint64_t)budget_of[level] * 1000ULL) {
        drv_audio_abort();
        psay("did not finish within %u ms", UL(budget_of[level]));
        return pfinish(0, "out of time: everything put back");
    }
    int done = level == 0 ? probe0() : level == 1 ? (cal_avail ? probe1c() : probe1()) : cal_avail ? probe2c() : probe2();
    if (!done && wait_ms)
        *wait_ms = level == 1 && !cal_avail ? 50U : 5U;
    return done;
}

/* ---- libsb glue ---------------------------------------------------------------------------------------------- */
#ifdef SBOS_KERNEL
#include "amp.h"
#include "codec.h"
int audio_probe(void);

static void k_log(const char *s) { sb_log(s); }
static uint64_t k_now(void) { return sb_us(); }
static int k_owns(void)
{
    int s = abox_state();
    return s == ABOX_ST_BOOTING || s == ABOX_ST_READY || s == ABOX_ST_CALL;
}
/* amp.c's bus_up() writes nothing when the boot left USI07 in I2C mode, gpp4-4/5 in function 2 without pulls, and
 * the gates open (its own checks, read here first) */
static int k_amp_bus_ready(void)
{
    const struct audio_hal *h = audio_hal_libsb();
    static const uint16_t gates[4] = { 0x203C, 0x2094, 0x20E4, 0x20E8 };
    for (unsigned i = 0; i < 4; ++i)
        if ((h->rd32(0x10800000UL + gates[i]) & 0x00300000U) == 0x00100000U)
            return 0;
    return (h->rd32(0x10811024UL) & 7U) == 4U && ((h->rd32(0x10830000UL) >> 16) & 0xFFU) == 0x22U &&
           !((h->rd32(0x10830008UL) >> 16) & 0xFFU);
}
static int k_codec_id(void) { return audio_probe(); }

/* Calliope's firmware from VENDOR (as modem.c reads it): with it the Calliope engine is used (abox_media.c) */
long bcmw_sb_vendor_read(const char *path, void *dst, uint32_t cap);

/* Calliope's files kept in RAM after their first read (run 8: a cold start still stalled the main loop 0.3-0.4 s,
 * mostly calliope_dram.bin's 2.2 MB VENDOR read into non-cacheable abox_rmem). The VENDOR partition does not change
 * while the OS runs, so a later boot copies from here (cacheable, ~ms). One sb_alloc of FWC_BYTES, never freed; if it
 * fails or is full, the files are read from VENDOR as before. snd_init_libsb fills it once (snd_fw_prefetch). */
#define FWC_BYTES 0x300000U                 /* the 10 files: ~2.67 MB */
#define FWC_N 12U
static struct { char path[48]; uint32_t off, len; } fwc[FWC_N];
static unsigned fwc_n;
static uint8_t *fwc_buf;
static uint32_t fwc_used;
static int fwc_find(const char *p)
{
    for (unsigned i = 0; i < fwc_n; ++i) {
        unsigned k = 0;
        while (p[k] && fwc[i].path[k] == p[k])
            ++k;
        if (!p[k] && !fwc[i].path[k])
            return (int)i;
    }
    return -1;
}
static int fwc_room(const char *path)
{
    unsigned k = 0;
    while (path[k])
        ++k;
    if (fwc_n >= FWC_N || k >= sizeof fwc[0].path)
        return 0;
    if (!fwc_buf)
        fwc_buf = (uint8_t *)sb_alloc(FWC_BYTES);
    return fwc_buf != 0;
}
static void fwc_add(const char *path, uint32_t len)
{
    unsigned k = 0;
    for (; path[k]; ++k)
        fwc[fwc_n].path[k] = path[k];
    fwc[fwc_n].path[k] = 0;
    fwc[fwc_n].off = fwc_used, fwc[fwc_n].len = len;
    fwc_used += (len + 63U) & ~63U;
    fwc_n++;
}
int snd_reserved(void)
{
    return fwc_buf != 0;
}
/* boot, before the UI's heap takes the arena (main.c, beside kmodem_reserve): the cache's 3 MiB, no I/O. Without it
 * the arena can be full by snd_init_libsb (drivers runs 13-14: snd's cache did not fit in the probe images). */
void snd_reserve(void)
{
    if (!fwc_buf)
        fwc_buf = (uint8_t *)sb_alloc(FWC_BYTES);
}

static long k_vendor(void *ctx, const char *path, void *dst, uint32_t cap)
{
    (void)ctx;
    int i = fwc_find(path);
    if (i >= 0) {
        if (fwc[i].len > cap)
            return -1;
        memcpy(dst, fwc_buf + fwc[i].off, fwc[i].len);
        return (long)fwc[i].len;
    }
    long got = bcmw_sb_vendor_read(path, dst, cap);
    if (got > 0 && fwc_room(path) && fwc_used + (uint32_t)got <= FWC_BYTES) {
        memcpy(fwc_buf + fwc_used, dst, (uint32_t)got);
        fwc_add(path, (uint32_t)got);
    }
    return got;
}

/* abox.c's file_part: the cached file's size (len 0) or a piece of it; < 0 when not cached (abox.c then reads it whole) */
static long k_file_part(void *ctx, const char *path, void *dst, uint32_t off, uint32_t len)
{
    (void)ctx;
    int i = fwc_find(path);
    if (i < 0)
        return -1;
    if (!len)
        return (long)fwc[i].len;
    if (!dst || off > fwc[i].len || len > fwc[i].len - off)
        return -1;
    memcpy(dst, fwc_buf + fwc[i].off + off, len);
    return (long)len;
}

/* once, at snd_init_libsb (the OS's start, before its main loop): Calliope's files straight into the cache, so even
 * the first sound after power-on needs no VENDOR read on the main loop. Names as abox.c loads them. */
static void snd_fw_prefetch(void)
{
    static const char *const names[] = {
        "firmware/calliope_sram.bin", "firmware/calliope_dram.bin", "firmware/calliope_iva.bin",
        "firmware/dsm.bin", "firmware/AP_AUDIO_SLSI.bin", "firmware/APBargeIn_AUDIO_SLSI.bin",
        "firmware/SoundBoosterParam.bin", "firmware/APDV_AUDIO_SLSI.bin", "firmware/APBiBF_AUDIO_SLSI.bin",
        "firmware/dsm_tune.bin",
    };
    uint64_t t0 = sb_us();
    unsigned got_n = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (fwc_find(names[i]) >= 0 || !fwc_room(names[i]) || fwc_used >= FWC_BYTES)
            continue;
        long got = bcmw_sb_vendor_read(names[i], fwc_buf + fwc_used, FWC_BYTES - fwc_used);
        if (got > 0) {
            fwc_add(names[i], (uint32_t)got);
            got_n++;
        }
    }
    say("engine: Calliope's %u files kept in RAM (%u KiB, %u ms at start-up): later starts skip VENDOR",
        UL(got_n), UL(fwc_used / 1024U), UL((sb_us() - t0) / 1000U));
}

static struct abox_hal k_hal;
void snd_init_libsb(void)
{
    if (inited)
        return;
    abox_hal_libsb(&k_hal, k_vendor, 0);
    k_hal.file_part = k_file_part;          /* calliope_dram.bin from the RAM copy in 256 KiB steps */
    amp_hal_set(audio_hal_libsb());
    codec_hal_set(audio_hal_libsb());
    struct snd_ops o;
    memset(&o, 0, sizeof o);
    o.log = k_log, o.now_us = k_now, o.call_owns_abox = k_owns;
    o.amp_probe = amp_probe, o.amp_speaker = amp_speaker, o.amp_receiver = amp_receiver, o.amp_off = amp_off;
    o.codec_mic = codec_mic, o.codec_off = codec_off;
    o.amp_bus_ready = k_amp_bus_ready, o.codec_id_readonly = k_codec_id;
    o.use_calliope = 1;                     /* bypass does not move ABOX's DMAs on this phone (AUDIO.md 8, 9) */
    (void)snd_init(&o, &k_hal);
    snd_set_hp(codec_hp);                   /* codec_media.c: headphones on HPOUT2 */
    snd_fw_prefetch();
}

/* The kernel's jack tick (every CODEC_JACK_POLL_MS = 50 ms while the screen is on or sound plays): the jack state into
 * AUTO routing. The codec stays out of reset while this runs (codec_media.c), so the OS stops it when idle with the
 * screen off. Returns CODEC_JACK_*. */
int snd_jack_tick(void)
{
    int j = codec_jack_poll();
    if (j >= 0)
        snd_jack(j > 0);
    return j;
}
#endif
