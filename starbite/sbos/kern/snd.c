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
#include <stdarg.h>

#ifdef SBOS_KERNEL
#include "sb.h"
#else
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#endif

#define W_AHEAD 3U                      /* periods mixed ahead of the one RDMA0 reads */
#define IDLE_STOP_US 2000000ULL
#define RETRY_US 2000000ULL             /* after a failed start, the next try */
#define LATE_US 15000ULL
#define STALL_US 250000ULL
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
static const struct { const struct seg *s; unsigned n, repeat; } tones[6] = {
    { 0, 0, 0 },
    { t_ring, sizeof t_ring / sizeof t_ring[0], 1 },
    { t_alarm, sizeof t_alarm / sizeof t_alarm[0], 1 },
    { t_notify, sizeof t_notify / sizeof t_notify[0], 0 },
    { t_click, 1, 0 },
    { t_test, 1, 1 },
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
    unsigned vol[4];
    uint32_t fill_idx, zero_idx, last_pos;
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

static void say(const char *fmt, ...)
{
    char b[220];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, "snd: ", fmt, ap);
    va_end(ap);
    if (inited && O.log)
        O.log(b);
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
static int call_has(void) { return O.call_owns_abox && O.call_owns_abox(); }

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
    memset(&G, 0, sizeof G);
    memset(st, 0, sizeof st);
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

int snd_tone(int kind)
{
    if (kind < 1 || kind > SND_TONE_TEST)
        return SND_EINVAL;
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
    int16_t *dst = (int16_t *)(void *)(apcm_play_buf() + idx * APCM_PERIOD_BYTES);
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
    memset(apcm_play_buf() + idx * APCM_PERIOD_BYTES, 0, APCM_PERIOD_BYTES);
}

/* ---- the engine ---------------------------------------------------------------------------------------------- */
static void engine_stop(const char *what)
{
    if (!G.eng)
        return;
    output_off();
    apcm_play_stop();
    if (!G.cap)
        apcm_down();
    G.eng = 0;
    G.stats.stops++;
    say("engine: stopped (%s)", what);
    why("Sound is ready (nothing playing)");
}

static int engine_start(void)
{
    int r, out = resolve();
    if (G.suspended || call_has()) {
        why("Sound is paused: a call has the audio hardware");
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
    G.eng = 1;
    G.cur_out = out;
    G.fill_idx = 1, G.zero_idx = 0;
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
    uint32_t stt = 0, pos = apcm_play_pos(&stt);
    if (pos != APCM_NOPOS && pos != G.last_pos)
        G.moved_at = t, G.last_pos = pos;
    if (t - G.moved_at > STALL_US) {    /* no position, or the same one, for 250 ms: the DMA does not run */
        say("engine: RDMA0 does not move (STATUS %x)", UL(stt));
        if (!G.dumped++)
            apcm_dump("engine");        /* once a boot: the registers for the log */
        engine_stop("RDMA0 did not run");
        why("Sound stopped: the audio DMA did not run");
        G.retry_at = t + RETRY_US;
        return;
    }
    if (pos == APCM_NOPOS)
        return;
    uint32_t c = pos / APCM_PERIOD_BYTES, dist = (G.fill_idx + APCM_PERIODS - c) % APCM_PERIODS;
    if (dist == 0 || dist > W_AHEAD + 1) {
        G.stats.xruns++;
        G.fill_idx = (c + 1) % APCM_PERIODS;
        dist = 1;
    }
    while (G.zero_idx != c) {
        zero_period(G.zero_idx);
        G.zero_idx = (G.zero_idx + 1) % APCM_PERIODS;
    }
    while (dist < W_AHEAD + 1) {
        mix_period(G.fill_idx);
        G.fill_idx = (G.fill_idx + 1) % APCM_PERIODS;
        dist++;
    }
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

int snd_cap_open(void)
{
    int r;
    if (!inited)
        return SND_ESTATE;
    if (G.cap)
        return 0;
    if (G.suspended || call_has()) {
        why("The microphone is busy: a call has the audio hardware");
        return SND_EBUSY;
    }
    if (!O.codec_mic) {
        why("No microphone: the codec driver is not linked");
        return SND_ESTATE;
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
    G.cap_last = 0, G.cap_head = G.cap_tail = 0, G.cap_n = 0, G.cap_sq = 0, G.cap_level = 0;
    say("capture: the main mic, 48 kHz mono");
    return 0;
}

static void cap_pull(void)
{
    uint32_t stt = 0, pos = apcm_cap_pos(&stt);
    const uint8_t *ring = apcm_cap_buf();
    if (pos == APCM_NOPOS || !ring)
        return;
    pos &= ~3U;
    unsigned guard = APCM_RING_BYTES / 4;
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
        G.cap_last = (G.cap_last + 4U) % APCM_RING_BYTES;
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
    if (!G.cap)
        return;
    apcm_cap_stop();
    if (G.cap_codec && O.codec_mic)
        (void)O.codec_mic(0);
    if (G.cap_codec && O.codec_off && !(G.eng && G.cur_out == SND_OUT_HEADPHONES))
        O.codec_off();
    G.cap_codec = 0;
    G.cap = 0;
    if (!G.eng)
        apcm_down();
    say("capture: closed");
}

/* ---- the loop ------------------------------------------------------------------------------------------------ */
int snd_busy(void) { return inited && !G.suspended && (G.eng || G.cap || need_play()); }

void snd_poll(uint64_t t)
{
    if (!inited)
        return;
    if ((G.eng || G.cap) && call_has()) {
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
    if (G.last_poll && t - G.last_poll > LATE_US && G.eng)
        G.stats.late_polls++;
    G.last_poll = t;
    int need = need_play();
    if (need && !G.eng && t >= G.retry_at) {
        if (engine_start()) {
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
    if (G.cap)
        cap_pull();
}

void snd_suspend_for_call(void)
{
    if (!inited)
        return;
    G.suspended = 1;
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

void snd_resume_after_call(void)
{
    if (!inited || !G.suspended)
        return;
    G.suspended = 0;
    G.retry_at = 0;
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
} P;

static const unsigned budget_of[3] = { 2000, 12000, 10000 };

static int pfinish(int ok, const char *line)
{
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
    if (G.cap)
        snd_cap_close();
    apcm_cap_stop();
    apcm_play_stop();
    apcm_down();
    G.eng = G.cap = 0;
    G.amp_override = -1;
    if (P.active && P.level == 2) {
        G.route = P.saved_route;
        for (int i = 1; i < 4; ++i)
            G.vol[i] = (unsigned)P.saved_vol[i];
    }
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
    }
    if (now() - P.t0 > (uint64_t)budget_of[level] * 1000ULL) {
        drv_audio_abort();
        psay("did not finish within %u ms", UL(budget_of[level]));
        return pfinish(0, "out of time: everything put back");
    }
    int done = level == 0 ? probe0() : level == 1 ? probe1() : probe2();
    if (!done && wait_ms)
        *wait_ms = level == 1 ? 50U : 5U;
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

static struct abox_hal k_hal;
void snd_init_libsb(void)
{
    if (inited)
        return;
    abox_hal_libsb(&k_hal, 0, 0);
    amp_hal_set(audio_hal_libsb());
    codec_hal_set(audio_hal_libsb());
    struct snd_ops o;
    memset(&o, 0, sizeof o);
    o.log = k_log, o.now_us = k_now, o.call_owns_abox = k_owns;
    o.amp_probe = amp_probe, o.amp_speaker = amp_speaker, o.amp_receiver = amp_receiver, o.amp_off = amp_off;
    o.codec_mic = codec_mic, o.codec_off = codec_off;
    o.amp_bus_ready = k_amp_bus_ready, o.codec_id_readonly = k_codec_id;
    (void)snd_init(&o, &k_hal);
    snd_set_hp(codec_hp);                   /* codec_media.c: headphones on HPOUT2 */
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
