/* SPDX-License-Identifier: GPL-2.0-only */
/* sbg_snd: the host side of sbsnd (../sbshm-snd.h, sbg_snd.h). Built inside sbghost.c (#include at its end), so it
 * may use sbghost.c's text helpers (struct tb, t_s, t_u); the rest goes through sbghost_int.h.
 * The host keeps its own ring positions (p_hw, c_hw): it never trusts the copies in the region, which the guest can
 * write. The guest's positions are checked before use (a fill beyond the ring resynchronises). */
#include "sbghost_int.h"
#include "sbg_snd.h"

#define SND ((volatile struct sbshm_snd *)sbg_shm(SBSHM_SND_OFF))
#define SND_RING_MASK (SBSHM_SND_RING - 1U)

static struct sbg_snd_stats snd_st;
static uint32_t p_hw, c_hw;                 /* the host's ring positions (bytes) */
static uint32_t p_starts_seen;              /* the guest's play_starts at the last pull */
static uint32_t p_discard_seen;             /* the guest's play_discard last applied (a new value is a new drop) */
static int p_primed;                        /* a byte of this start was pulled: an empty ring is an underrun now */
static int p_in_underrun;
static volatile int snd_ring_want;          /* pull -> poll: pend the guest's doorbell */
static uint32_t snd_gain = 0x10000U;
static int snd_mute;
static int snd_was_play, snd_was_cap;       /* poll: the last state the platform was told */

static int snd_on(void)
{
    const struct sbg_os *o = sbg_os_();
    uint32_t s;

    if (!o || !o->snd || !(sbg_services() & SBSHM_SVC_SND))
        return 0;
    s = sbg_stats()->state;
    return s == SBG_BOOTING || s == SBG_RUNNING || s == SBG_STOPPING;
}

void sbg_snd_load(void)
{
    const struct sbg_snd *p = sbg_os_()->snd;
    volatile struct sbshm_snd *s = SND;

    p_hw = c_hw = 0;
    p_starts_seen = p_discard_seen = 0;
    p_primed = p_in_underrun = 0;
    snd_ring_want = 0;
    snd_was_play = snd_was_cap = 0;
    for (unsigned i = 0; i < sizeof(snd_st); ++i)
        ((volatile uint8_t *)&snd_st)[i] = 0;
    if (!(sbg_services() & SBSHM_SVC_SND) || !p)
        return;
    /* the region is zero (sbg_load cleared the v3 regions): positions 0, states STOP */
    s->rate = SBSHM_SND_RATE;
    s->channels = SBSHM_SND_CHANNELS;
    s->format = SBSHM_SND_FMT_S16LE;
    s->ring_bytes = SBSHM_SND_RING;
    s->play_off = SBSHM_SND_PLAY_OFF;
    s->cap_off = SBSHM_SND_CAP_OFF;
    s->caps = ((p->caps & SBG_SND_PLAY) ? SBSHM_SND_CAP_PLAY : 0U) |
              ((p->caps & SBG_SND_CAPTURE) ? SBSHM_SND_CAP_CAPTURE : 0U);
    s->host_block_frames = p->block_frames ? p->block_frames : 240U;
    s->host_latency_frames = p->latency_frames;
    s->host_volume = snd_gain;
    s->host_mute = (uint32_t)snd_mute;
    sbshm_dmb();
    s->magic = SBSHM_SND_MAGIC;
}

static int16_t snd_scale(int16_t v)
{
    int32_t x = (int32_t)(((int64_t)v * snd_gain) >> 16);
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

unsigned sbg_snd_pull(int16_t *lr, unsigned frames)
{
    unsigned got = 0;

    if (snd_on() && (SND->caps & SBSHM_SND_CAP_PLAY)) {
        volatile struct sbshm_snd *s = SND;
        uintptr_t ring = sbg_shm(SBSHM_SND_OFF + SBSHM_SND_PLAY_OFF);
        uint32_t d = s->play_discard, starts = s->play_starts, state = s->play_state;
        uint32_t appl, fill;

        sbshm_dmb();                        /* the guest wrote play_appl before play_discard */
        appl = s->play_appl;

        if (starts != p_starts_seen) {      /* a new start: the first underrun counts once a byte came */
            p_starts_seen = starts;
            p_primed = p_in_underrun = 0;
        }
        if (d != p_discard_seen) {          /* a drop: skip what was not played (when it is ahead, up to appl) */
            p_discard_seen = d;
            if ((int32_t)(d - p_hw) > 0 && d - p_hw <= SBSHM_SND_RING && (int32_t)(appl - d) >= 0) {
                p_hw = d;
                snd_st.discards++;
            }
        }
        fill = appl - p_hw;
        if (fill > SBSHM_SND_RING || (fill & 3U)) {     /* impossible: the guest's position is wrong */
            snd_st.resyncs++;
            p_hw = appl & ~3U;
            fill = 0;
        }
        sbshm_dmb();
        snd_st.fill_frames = fill / SBSHM_SND_FRAME;
        if (snd_st.fill_frames > snd_st.fill_max)
            snd_st.fill_max = snd_st.fill_frames;
        got = fill / SBSHM_SND_FRAME;
        if (got > frames)
            got = frames;
        for (unsigned i = 0; i < got; ++i) {
            uint32_t w = *(volatile uint32_t *)(ring + ((p_hw + 4U * i) & SND_RING_MASK));
            int16_t l = (int16_t)(w & 0xffffU), r = (int16_t)(w >> 16);
            if (snd_mute) {
                l = r = 0;
            } else if (snd_gain != 0x10000U) {
                l = snd_scale(l);
                r = snd_scale(r);
            }
            lr[2 * i] = l;
            lr[2 * i + 1] = r;
        }
        sbshm_dmb();
        p_hw += got * SBSHM_SND_FRAME;
        s->play_hw = p_hw;
        snd_st.play_frames += got;
        if (got)
            p_primed = 1;
        if (state == SBSHM_SND_RUN && p_primed && got < frames) {
            snd_st.underrun_frames += frames - got;
            if (!p_in_underrun) {
                p_in_underrun = 1;
                snd_st.underruns++;
                snd_ring_want = 1;
            }
            s->play_underruns = snd_st.underruns;
            s->play_underrun_frames = snd_st.underrun_frames;
        } else if (got == frames) {
            p_in_underrun = 0;
        }
    }
    for (unsigned i = got; i < frames; ++i)
        lr[2 * i] = lr[2 * i + 1] = 0;
    return got;
}

unsigned sbg_snd_push(const int16_t *lr, unsigned frames)
{
    volatile struct sbshm_snd *s = SND;
    uintptr_t ring;
    uint32_t used, n;

    if (!snd_on() || !(s->caps & SBSHM_SND_CAP_CAPTURE) || s->cap_state != SBSHM_SND_RUN)
        return 0;
    ring = sbg_shm(SBSHM_SND_OFF + SBSHM_SND_CAP_OFF);
    used = c_hw - s->cap_appl;
    if (used > SBSHM_SND_RING || (used & 3U)) {         /* the guest's position is wrong: as if it read all */
        snd_st.resyncs++;
        used = SBSHM_SND_RING;
    }
    n = (SBSHM_SND_RING - used) / SBSHM_SND_FRAME;
    if (n > frames)
        n = frames;
    sbshm_dmb();                                        /* the guest read those bytes before it moved cap_appl */
    for (uint32_t i = 0; i < n; ++i)
        *(volatile uint32_t *)(ring + ((c_hw + 4U * i) & SND_RING_MASK)) =
            (uint32_t)(uint16_t)lr[2 * i] | (uint32_t)(uint16_t)lr[2 * i + 1] << 16;
    sbshm_dmb();
    c_hw += n * SBSHM_SND_FRAME;
    s->cap_hw = c_hw;
    snd_st.cap_frames += n;
    if (n < frames) {
        snd_st.overrun_frames += frames - n;
        s->cap_overruns = snd_st.overrun_frames;
    }
    return n;
}

int sbg_snd_active(int *playing, int *capturing)
{
    int p = 0, c = 0;

    if (snd_on()) {
        volatile struct sbshm_snd *s = SND;
        uint32_t ps = s->play_state;
        p = ps == SBSHM_SND_RUN || (ps == SBSHM_SND_DRAIN && s->play_appl != p_hw);
        c = s->cap_state == SBSHM_SND_RUN;
    }
    if (playing)
        *playing = p;
    if (capturing)
        *capturing = c;
    return p || c;
}

void sbg_snd_set_volume(uint32_t gain_q16, int mute)
{
    snd_gain = gain_q16 > 0x40000U ? 0x40000U : gain_q16;
    snd_mute = mute != 0;
    if (sbg_os_() && (sbg_services() & SBSHM_SVC_SND)) {
        SND->host_volume = snd_gain;
        SND->host_mute = (uint32_t)snd_mute;
    }
}

void sbg_snd_set_latency(uint32_t frames)
{
    if (sbg_os_() && (sbg_services() & SBSHM_SVC_SND))
        SND->host_latency_frames = frames;
}

const struct sbg_snd_stats *sbg_snd_stats(void)
{
    if (snd_on()) {
        snd_st.play_starts = SND->play_starts;
        snd_st.cap_starts = SND->cap_starts;
    }
    return &snd_st;
}

void sbg_snd_poll(void)
{
    const struct sbg_os *o = sbg_os_();
    int p, c;

    if (!o || !o->snd)
        return;
    if (snd_ring_want) {
        snd_ring_want = 0;
        if (snd_on())
            sbg_ring_guest();
    }
    sbg_snd_active(&p, &c);                 /* both 0 once the guest is down: the platform hears "stopped" */
    if (p == snd_was_play && c == snd_was_cap)
        return;
    struct tb t = {.n = 0};
    if (p != snd_was_play) {
        t_s(&t, p ? "guest: snd: playback started, period " : "guest: snd: playback stopped, ");
        if (p) {
            t_u(&t, SND->play_period_frames);
            t_s(&t, " frames");
        } else {
            t_u(&t, snd_st.play_frames);
            t_s(&t, " frames played so far, underruns ");
            t_u(&t, snd_st.underruns);
            t_s(&t, " (");
            t_u(&t, snd_st.underrun_frames);
            t_s(&t, " frames)");
        }
    }
    if (c != snd_was_cap) {
        t_s(&t, t.n ? "; capture " : "guest: snd: capture ");
        t_s(&t, c ? "started" : "stopped");
    }
    say(&t);
    snd_was_play = p;
    snd_was_cap = c;
    if (o->snd->active)
        o->snd->active(p, c);
}
