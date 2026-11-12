/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 * sbsnd (contract v3, sbshm.h SBSHM_SND_OFF (1 MiB), host_services & SBSHM_SVC_SND): the Linux guest's sound card
 * "Starbite" (one playback and one capture PCM) over the shared region.
 *
 * The format is FIXED: 48000 Hz, S16LE, 2 channels interleaved (L then R): one frame = 4 bytes = one 32-bit word
 * (L in the low half). Nothing is negotiated; a host whose speaker / mic run at another rate resamples in its mixer.
 *
 * Two byte rings, single-producer single-consumer, each index on its own 64-byte line:
 *   play: guest -> host. The guest's kernel (sbsnd.c) writes PCM at play_appl, the host's mixer (sbg_snd_pull)
 *         reads at play_hw.
 *   cap:  host -> guest. The host (sbg_snd_push) writes mic PCM at cap_hw, the guest reads at cap_appl.
 * Positions are free-running byte counts (uint32_t, wrap at 2^32; the ring size is a power of two, so the byte
 * of position p lives at ring[p % SBSHM_SND_RING]). Fill = appl - hw (play) / hw - appl (cap), never more than
 * SBSHM_SND_RING; both always multiples of 4 (whole frames). A side that finds an impossible fill (a bad peer)
 * resynchronises to the other side's position and counts it.
 * Producer: write the frames, dmb, then the position. Consumer: read the position, dmb, copy the frames, dmb, then
 * its own position. Plain 32-bit loads / stores only (the region is non-cacheable on the phone, no exclusives).
 *
 * Playback, the guest: on start it bumps play_starts and sets play_state RUN, and keeps the ring filled to a small
 * target (2 x host_block_frames + 4 of its timer ticks, growing on underruns) from the ALSA buffer, so the hw pointer
 * moves at the rate the host consumes; ALSA's delay = ring fill + host_latency_frames. On a drop / xrun it writes
 * play_discard = play_appl (the host skips whatever it has not played before that position), then STOP; at the
 * end of a drain it sets DRAIN (the host plays the rest, then idles). play_discard only ever names data that was
 * written before it, so the host may apply it at any time: when it CHANGED since the host last looked (a new drop;
 * the host remembers the last value, 0 at load) and lies between play_hw and play_appl, the host skips to it. The
 * guest writes play_appl, dmb, play_discard; the host reads play_discard, dmb, play_appl.
 * The host: an empty ring while play_state is RUN (after the first byte of this start) is an underrun: it plays
 * silence, counts play_underruns (episodes) and play_underrun_frames, and pends the guest's doorbell once per
 * episode so the guest refills at once instead of at its next tick.
 *
 * Capture, the guest: on start it sets cap_appl = cap_hw (old frames are not delivered), bumps cap_starts and sets
 * cap_state RUN; its timer moves whatever the ring holds into the ALSA buffer. The host pushes only while cap_state
 * is RUN; frames that do not fit (the guest stalled for SBSHM_SND_RING / 4 frames = 341 ms) are dropped and
 * counted in cap_overruns.
 *
 * Doorbells: the guest never rings for sound (the host's mixer pulls at its own pace); the host rings on an
 * underrun (above). The guest also polls with a jiffies timer while a stream runs.
 */
#ifndef SBSHM_SND_H
#define SBSHM_SND_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define SBSHM_SND_MAGIC     0x4e534253U         /* "SBSN" */
#define SBSHM_SND_RATE      48000U
#define SBSHM_SND_CHANNELS  2U
#define SBSHM_SND_FRAME     4U                  /* bytes per frame: S16LE x 2 */
#define SBSHM_SND_RING      0x10000U            /* bytes per ring, a power of two: 16384 frames = 341 ms */
#define SBSHM_SND_PLAY_OFF  0x10000U            /* from SBSHM_SND_OFF: the playback ring */
#define SBSHM_SND_CAP_OFF   0x20000U            /* from SBSHM_SND_OFF: the capture ring (.. 0x30000, in the 1 MiB) */

enum { SBSHM_SND_FMT_S16LE = 1 };
enum { SBSHM_SND_CAP_PLAY = 1U << 0, SBSHM_SND_CAP_CAPTURE = 1U << 1 };   /* struct sbshm_snd caps */
/* play_state / cap_state (guest) */
enum { SBSHM_SND_STOP = 0, SBSHM_SND_RUN = 1, SBSHM_SND_DRAIN = 2 };

struct sbshm_snd {
    /* ---- host, before CPU_ON (fixed for the boot) ---- */
    uint32_t magic;                 /* SBSHM_SND_MAGIC */
    uint32_t rate;                  /* SBSHM_SND_RATE */
    uint32_t channels;              /* SBSHM_SND_CHANNELS */
    uint32_t format;                /* SBSHM_SND_FMT_S16LE */
    uint32_t ring_bytes;            /* SBSHM_SND_RING */
    uint32_t play_off, cap_off;     /* SBSHM_SND_PLAY_OFF / SBSHM_SND_CAP_OFF */
    uint32_t caps;                  /* SBSHM_SND_CAP_*: the streams the host serves (the guest offers only these) */
    uint32_t host_block_frames;     /* the frames the host's mixer pulls / pushes at a time (its period) */
    uint32_t pad0[7];
    /* ---- host, any time ---- */
    uint32_t host_latency_frames;   /* from a frame the host pulled to the speaker (its queue + DMA + codec) */
    uint32_t host_volume;           /* the host's gain on the guest's playback, Q16 (0x10000 = unity; sbg_snd_pull
                                     * applies it); for the guest's information */
    uint32_t host_mute;             /* 1: the host plays the guest's playback as silence (still consumes it) */
    uint32_t play_underruns;        /* underrun episodes since the boot */
    uint32_t play_underrun_frames;  /* silence frames the host played in them */
    uint32_t cap_overruns;          /* capture frames the host dropped (ring full) */
    uint32_t pad1[10];
    /* ---- playback ---- */
    volatile uint32_t play_appl;    /* guest: bytes written */
    uint32_t play_state;            /* guest: SBSHM_SND_STOP / RUN / DRAIN */
    uint32_t play_period_frames;    /* guest: the ALSA period (information) */
    uint32_t play_discard;          /* guest: on a drop, play_appl at the stop (the host skips to it) */
    uint32_t play_starts;           /* guest: + 1 per start (before play_state = RUN) */
    uint32_t pad2[11];
    volatile uint32_t play_hw;      /* host: bytes consumed */
    uint32_t pad3[15];
    /* ---- capture ---- */
    volatile uint32_t cap_hw;       /* host: bytes written */
    uint32_t pad4[15];
    volatile uint32_t cap_appl;     /* guest: bytes consumed */
    uint32_t cap_state;             /* guest: SBSHM_SND_STOP / RUN */
    uint32_t cap_period_frames;     /* guest: the ALSA period (information) */
    uint32_t cap_starts;            /* guest: + 1 per start */
    uint32_t pad5[12];
};                                  /* 384 bytes; the rings at SBSHM_SND_PLAY_OFF / SBSHM_SND_CAP_OFF */

#endif
