/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the sound service (snd.c): media playback and capture outside calls, on ABOX in bypass (abox_pcm.c),
 * the MAX98512 pair (amp.c), the CS47L93's main mic (codec.c) and, when the codec agent's code is linked, its
 * headphone output (codec_media.c, through a hook).
 *
 * The model (single-threaded, no locks):
 *   - 4 PCM streams (48 kHz, stereo, S16, interleaved) + 1 tone generator, mixed in 32 bits, one master volume per
 *     output (0..15), mute, the speakers' guard (a peak limiter and a power budget), then written into RDMA0's ring a
 *     few periods ahead of where it reads.
 *   - Outputs: the loudspeaker (the 0x3A die, "Mono Right", amp_speaker: capped 15 dB under One UI, there is no DSM
 *     speaker protection here), the earpiece (the 0x39 die, amp_receiver: One UI's handset gain at most), the
 *     headphones (UAIF0 -> the codec's AIF1RX, codec_hp() when linked). The amp outputs get a mono mix on both slots.
 *     Route AUTO = headphones when plugged (and the hook exists), else the loudspeaker; the earpiece only on request.
 *   - Capture: the main mic (codec IN3L -> AIF1TX1 -> UAIF0 -> WDMA1), 48 kHz mono S16 for apps (the left slot).
 *     With use_calliope (the kernel's) both directions run through Calliope (abox_media.c: RDMA0 / WDMA1 on its PCM
 *     IPC), together if wanted, on one Calliope boot.
 *   - Calls win: snd_suspend_for_call() stops everything and gives ABOX back before abox_boot() (a Calliope boot of
 *     snd's still pending is finished there, so the modem's abox_boot() finds it running; a tone or capture that was
 *     waiting for it is dropped);
 *     snd_resume_after_call() after abox_off(). While suspended, streams keep their queued frames and nothing plays.
 *
 * Polling: snd_poll(now_us) from the kernel loop. While snd_busy(), every 5 ms. The mixer keeps 3 periods of 10.67 ms
 * queued ahead of the DMA with only tones (polls at most ~21 ms apart), 16 (~170 ms) while a stream is open (a tone
 * then starts after those); later than that the DMA plays zeroes, never stale sound, unless the kernel stalls longer
 * than the whole 341 ms ring. A Calliope boot (~1 s) runs a step per poll, never in one. Output latency: 21..32 ms
 * (tones), ~170 ms (streams) + the app's own queue. When idle it needs no poll (snd_busy() 0); a write, a tone or a capture makes it busy again. The engine stops 2 s after the last
 * sound (amps off first), so the AUD domain is off while nothing plays. */
#ifndef SBOS_SND_H
#define SBOS_SND_H
#include <stdint.h>
#include "abox_pcm.h"

#define SND_STREAMS 4
#define SND_VOL_MAX 15
#define SND_STREAM_FRAMES 8192U         /* each stream's queue: 170 ms (an app writing once per 120 ms turn keeps up) */
#define SND_CAP_SAMPLES 24000U          /* the capture queue: 0.5 s mono */

/* outputs (also the volume index) */
#define SND_OUT_AUTO 0
#define SND_OUT_SPEAKER 1
#define SND_OUT_EARPIECE 2
#define SND_OUT_HEADPHONES 3

/* tones: synthesized here (generic beeps, no recorded or copyrighted sounds) */
#define SND_TONE_RING 1                 /* a rising three-note figure, 1.2 s pause, repeating */
#define SND_TONE_ALARM 2                /* 4 x 1 kHz beeps, 0.6 s pause, repeating */
#define SND_TONE_NOTIFY 3               /* two notes, once */
#define SND_TONE_CLICK 4                /* a 6 ms 3 kHz tick, once */
#define SND_TONE_TEST 5                 /* 1 kHz steady (the probe), until snd_tone_stop() */
/* 8a (Track B, the top ten's sounds; generic synthesized sounds, nothing recorded): once each */
#define SND_TONE_CLICK_DEL 6            /* the delete key: a deeper 6 ms tick (iOS's keyboard has three clicks) */
#define SND_TONE_CLICK_MOD 7            /* shift, 123, space, return: a softer, lower 7 ms tick */
#define SND_TONE_LOCK 8                 /* the screen locks: a short two-part latch */
#define SND_TONE_SHUTTER 9              /* the Camera's shutter: two quick ticks */
#define SND_TONE_CHARGE 10              /* power connected: a bell with its octave, fading */
#define SND_TONE_MAX SND_TONE_CHARGE

#define SND_EBUSY (-16)                 /* no free stream / a call has the hardware */
#define SND_EINVAL (-22)
#define SND_ESTATE (-1)

/* What snd needs from the rest of the kernel; snd_ops_libsb() fills it from amp.c / codec.c / audio.c / abox.c.
 * NULL hooks are skipped (no headphones without codec_hp, no capture without codec_mic). */
struct snd_ops {
    void (*log)(const char *line);
    uint64_t (*now_us)(void);
    int (*call_owns_abox)(void);                /* nonzero: Calliope has ABOX (abox_state()) */
    int (*amp_probe)(void);                     /* amp.c */
    int (*amp_speaker)(int on, int level);
    int (*amp_receiver)(int on, int level);
    void (*amp_off)(void);
    int (*codec_mic)(int on);                   /* codec.c: probes the codec itself when needed */
    void (*codec_off)(void);
    int (*codec_hp)(int on, int level);         /* codec_media.c (AUDIO-CODEC): level 0..15 = snd volume */
    /* the probe's level 0, reads only: is the amps' HSI2C already set up (USI07 in I2C mode, gpp4-4/5 in function,
     * its gates open)? then amp_probe() only reads; and the codec's ID if it is out of reset (audio_probe()) */
    int (*amp_bus_ready)(void);
    int (*codec_id_readonly)(void);
    /* nonzero: play through Calliope (abox_media.c; the HAL's file hook reads its firmware from VENDOR), the bypass
     * engine only after Calliope failed to boot twice; 0: bypass only (as before). The kernel's snd_init_libsb: 1 */
    int use_calliope;
};

int snd_init(const struct snd_ops *ops, const struct abox_hal *hal);    /* 0, or <0 if a required hook is missing */
#ifdef SBOS_KERNEL
void snd_init_libsb(void);              /* the kernel's: libsb's ABOX HAL, amp.c, codec.c, audio.c, abox_state() */
#endif
void snd_set_hp(int (*codec_hp)(int on, int level));   /* the headphone hook, when codec_media.c is linked */
void snd_jack(int plugged);             /* from the jack detection: AUTO follows it */
int snd_jack_tick(void);                /* kernel only: codec_jack_poll() -> snd_jack(); every 50 ms */

/* streams */
int snd_open(void);                     /* a stream id 0..3, or SND_EBUSY */
unsigned snd_write(int id, const int16_t *frames, unsigned n);  /* stereo frames taken (0..n): never blocks */
unsigned snd_space(int id);             /* frames that fit now */
unsigned snd_queued(int id);
void snd_stream_gain(int id, unsigned q8);      /* 0..256 (256 = as written) */
void snd_close(int id);                 /* dropped at once */

/* tones */
int snd_tone(int kind);                 /* replaces a running tone */
void snd_tone_stop(void);
int snd_tone_playing(void);

/* outputs */
void snd_volume(int out, unsigned v);   /* 0..15 (out: SPEAKER, EARPIECE, HEADPHONES) */
unsigned snd_get_volume(int out);
void snd_mute(int on);
void snd_route(int out);                /* SND_OUT_AUTO (default), SPEAKER, EARPIECE, HEADPHONES */
int snd_output(void);                   /* where sound goes now (resolved AUTO) */

/* capture (main mic, 48 kHz mono S16) */
int snd_cap_open(void);                 /* 0 or <0 */
unsigned snd_cap_read(int16_t *mono, unsigned n);   /* samples taken */
unsigned snd_cap_level(void);           /* RMS of the last 100 ms, 0..32767 */
void snd_cap_close(void);

/* the kernel loop */
void snd_poll(uint64_t now_us);
int snd_busy(void);                     /* 1: poll every 5 ms */

/* calls: modem.c calls these two around abox_boot() / abox_off() */
void snd_suspend_for_call(void);
void snd_resume_after_call(void);
/* 9a (main 11-15: ringing calls and alarms with Cellular on): Cellular holds Calliope (the CP's VSS needs it booted
 * before the CP starts) for as long as it runs, not only during a call. snd_calliope_shared(1) when Cellular starts
 * (before the modem's abox_boot: as snd_suspend_for_call, media stopped and a staged boot of snd's finished), but
 * media is NOT paused: tones and streams play through the modem's Calliope (RDMA0, UAIF1 / UAIF0) while no call routes;
 * snd never boots it or switches it off then. The modem calls snd_suspend_for_call / snd_resume_after_call around a
 * call's routes only. snd_calliope_shared(0) when Cellular stops, before its abox_off(): media stopped, Calliope the
 * modem's to switch off; snd boots its own again on the next sound. */
void snd_calliope_shared(int on);

const char *snd_why(void);              /* plain words for the UI: what sound is doing or why it is not */

/* counters for tests and the probe */
struct snd_stats {
    uint32_t periods, xruns, late_polls, cap_dropped, guard_limited, starts, stops;
    int32_t guard_db10;                 /* the power budget's gain now, tenths of a dB (<= 0) */
    uint32_t peak_out;                  /* the last period's peak written to the ring */
};
void snd_stats(struct snd_stats *s);

/* ---- the Linux guest's shared ring (docs/drivers/AUDIO.md 6): one page the guest maps, a stream's source ------- */
struct snd_shring {
    volatile uint32_t magic;            /* 'SNDR' 0x52444E53 */
    volatile uint32_t frames;           /* ring size in frames, a power of two (<= 8192) */
    volatile uint32_t head;             /* written by the guest: frames produced (free-running) */
    volatile uint32_t tail;             /* written by snd: frames consumed (free-running) */
    volatile uint32_t xruns;            /* snd: periods the ring ran dry while open */
    volatile uint32_t pad[3];
    int16_t pcm[];                      /* frames x 2 (L, R), S16, 48 kHz */
};
#define SND_SHRING_MAGIC 0x52444E53U
int snd_attach_shring(int id, struct snd_shring *r);   /* the stream reads from r instead of snd_write; NULL detaches */

/* ---- the driver probe (drvprobe.h) ----------------------------------------------------------------------------
 * level 0: reads only. With the Calliope engine (use_calliope and the firmware's reader):
 * level 1: silent: snd's real engine through Calliope (zeroes, muted, the amps / codec output kept off) 1 s, measured
 *          from RDMA0 STATUS (PASS 182400..201600 B/s), then the main mic through Calliope 0.5 s (WDMA1's rate, RMS,
 *          peak); everything back, Calliope off after (abox_off) if snd booted it.
 * level 2: quiet, may run unattended: the mic's 1 s baseline, then snd_tone's 1 kHz on the earpiece (amp level 0,
 *          -20 dBFS) 2 s and the loudspeaker (amp level 0, -25 dBFS) 1 s while the mic records; a Goertzel filter
 *          (goertzel.h) says per output "heard by the mic: yes/no (1 kHz N dB over baseline)"; FAIL if neither was.
 * Without Calliope (bypass, the host tests): level 1 the RDMA0 / WDMA1 sweep, level 2 the tones then the mic. */
int drv_audio_probe(int level, unsigned *wait_ms);
void drv_audio_abort(void);
#define DRV_AUDIO_BUDGET_MS 20000U      /* level 0: 2 s, level 1: 20 s (the bypass trials), level 2: 14 s */

#ifdef SBOS_KERNEL
void snd_reserve(void);                     /* boot, before the UI's heap: the firmware cache's 3 MiB (no I/O) */
int snd_reserved(void);                     /* 1 when that cache's memory is held (Track A's self-test end line) */
#endif
#endif
