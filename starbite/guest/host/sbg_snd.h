/* SPDX-License-Identifier: GPL-2.0-only
 * sbg_snd: the host (Starbite OS) side of sbsnd, the guest Linux's sound card (../sbshm-snd.h). OS independent:
 * nothing here knows Starbite's audio code. The platform's mixer (Track I) is the clock: it PULLS the guest's
 * playback when it needs frames for the speaker and PUSHES mic frames when it has them; sbghost never calls it
 * except for the optional `active` hint.
 *
 * Format, fixed: 48000 Hz, S16LE, 2 channels interleaved (lr[2 * i] = left, lr[2 * i + 1] = right). A mixer
 * running at another rate resamples itself.
 *
 * Contexts: sbg_snd_pull may run in the mixer's own context (an audio interrupt / thread, on any host core), and so
 * may sbg_snd_push, each from ONE context at a time (each is its ring's single host-side user); everything else from
 * the sbghost tick (sbg_poll's context). Nothing blocks or allocates; pull / push cost one copy of the frames.
 */
#ifndef SBG_SND_H
#define SBG_SND_H
#include <stdint.h>
#include "../sbshm-snd.h"

enum { SBG_SND_PLAY = 1U << 0, SBG_SND_CAPTURE = 1U << 1 };

/* sbg_os.snd: what the platform gives (all fields optional except caps) */
struct sbg_snd {
    uint32_t caps;                  /* SBG_SND_PLAY | SBG_SND_CAPTURE: what the platform has (0: no sound at all) */
    uint32_t block_frames;          /* frames the mixer pulls / pushes per call, its period (0: 240 = 5 ms); the guest
                                     * keeps about two of these queued */
    uint32_t latency_frames;        /* from a pulled frame to the speaker (queue + DMA + codec); sbg_snd_set_latency
                                     * changes it later (e.g. a Bluetooth route) */
    /* optional, from sbg_poll: the guest started / stopped a stream (power the amp / the mic path up or down; the
     * mixer starts or stops pulling / pushing). Called once per change. */
    void (*active)(int playing, int capturing);
};

/* The mixer takes up to `frames` playback frames into lr (always all `frames` written: the guest's frames, then
 * silence). Returns how many were the guest's. An empty ring while the guest's stream runs is an underrun (counted,
 * the guest's doorbell rung once per episode from the next sbg_poll). The host's volume / mute are applied here. */
unsigned sbg_snd_pull(int16_t *lr, unsigned frames);
/* The mixer gives `frames` mic frames to the guest. Returns how many were taken: 0 while the guest does not
 * capture (nothing counted), fewer than `frames` when the guest's ring is full (the rest dropped, counted). */
unsigned sbg_snd_push(const int16_t *lr, unsigned frames);
/* 1 when the guest plays (a stream running, or draining with frames left) or captures; *playing / *capturing (each
 * may be NULL) say which. 0 when the service is off or the guest is not up. */
int sbg_snd_active(int *playing, int *capturing);
/* the host's gain on the guest's playback (Q16, 0x10000 = unity, at most 0x40000) and mute; unity, unmuted at load */
void sbg_snd_set_volume(uint32_t gain_q16, int mute);
/* the host's output latency in frames (reported to the guest: ALSA's delay, so A/V sync is right) */
void sbg_snd_set_latency(uint32_t frames);

struct sbg_snd_stats {
    uint64_t play_frames;           /* guest frames pulled since the load */
    uint64_t cap_frames;            /* mic frames the guest got */
    uint32_t underruns, underrun_frames, overrun_frames;
    uint32_t play_starts, cap_starts;   /* as the guest counted them */
    uint32_t fill_frames;           /* the playback ring's fill at the last pull (before it) */
    uint32_t fill_max;              /* the most seen at a pull */
    uint32_t resyncs;               /* impossible positions from the guest (resynchronised) */
    uint32_t discards;              /* drops that skipped unplayed frames */
};
const struct sbg_snd_stats *sbg_snd_stats(void);

#endif
