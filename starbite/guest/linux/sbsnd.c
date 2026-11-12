// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbsnd: the ALSA card "Starbite" over the shared region (../sbshm-snd.h, contract v3, hdr.host_services &
 * SBSHM_SVC_SND). One PCM device with one playback and one capture substream (each only when the host serves it),
 * 48000 Hz S16_LE stereo, nothing else (the host's mixer resamples). The rules of sbg_core.h apply.
 *
 * Like a DMA engine with a small FIFO: the PCM buffer is ordinary vmalloc memory (mmap works, so dmix / PipeWire do
 * too); a jiffies timer (no CONFIG_HIGH_RES_TIMERS in the guest kernel: 4 ms at HZ=250), and the host's doorbell,
 * move frames between it and the shared rings:
 *   playback: the ring is kept filled to `target` frames (2 x the host's block + 4 timer ticks, one tick more per
 *             host underrun up to 100 ms, or sbguest.snd_target_ms; and less than the buffer minus a period),
 *             so the ALSA hw pointer (= what went into the ring) moves at the rate the host's mixer consumes.
 *             runtime->delay = the ring's fill + the host's latency, so snd_pcm_delay is the real time to the
 *             speaker. In the normal mode (stop_threshold <= buffer_size) only frames the application
 *             wrote are moved, so an xrun stops the stream before stale frames reach the host; in the silence mode
 *             (dmix, PipeWire: stop_threshold = boundary) the buffer is read at the rate the host consumes.
 *   capture:  whatever the host pushed is moved into the buffer at each step.
 * A drop / xrun / pause tells the host to skip what it has not played (play_discard); the end of a drain lets it
 * play the rest (DRAIN).
 */
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include "sbg_core.h"
#include "../sbshm-snd.h"

#define SBSND_RMASK	(SBSHM_SND_RING - 1)
#define SBSND_FB	SBSHM_SND_FRAME		/* bytes per frame */

struct sbsnd_stream {
	struct snd_pcm_substream *ss;
	bool running;
	u32 pos;			/* the ALSA hw pointer: frames into the buffer */
	u32 period_acc;			/* frames since the last period boundary */
	u32 ring;			/* this side's ring position (bytes): play appl / capture appl */
	u32 fill;			/* frames in the ring (play: not yet consumed; capture: not yet moved) */
	bool pending;			/* a period ended outside the timer (trigger, doorbell): the timer reports it */
};

static struct sbshm_snd *sbsnd;		/* write-combine vmap: the control block and both rings */
static u8 *sbsnd_play_ring, *sbsnd_cap_ring;
static struct snd_card *sbsnd_card;
static struct sbsnd_stream sbsnd_play, sbsnd_cap;
static DEFINE_SPINLOCK(sbsnd_lock);	/* the streams' state above; taken inside the PCM stream lock, never around it */
static struct timer_list sbsnd_timer;
static bool sbsnd_ready;
static u32 sbsnd_target;		/* playback ring fill target (frames) before the buffer's limit */
static u32 sbsnd_tick;			/* frames per jiffy */
static u32 sbsnd_ur_seen;		/* the host's play_underruns at the last step */
#define SBSND_TARGET_MAX (SBSHM_SND_RATE / 10)	/* the adaptive target stops at 100 ms */

static unsigned int snd_target_ms;
module_param(snd_target_ms, uint, 0444);
MODULE_PARM_DESC(snd_target_ms, "sbsnd: the playback ring's fill target in ms (0: 2 host blocks + 4 timer ticks, grown by a tick per host underrun up to 100 ms)");
static u32 sbsnd_resyncs;

static const struct snd_pcm_hardware sbsnd_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_PAUSE,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = SBSHM_SND_RATE,
	.rate_max = SBSHM_SND_RATE,
	.channels_min = SBSHM_SND_CHANNELS,
	.channels_max = SBSHM_SND_CHANNELS,
	.buffer_bytes_max = 2 * SBSHM_SND_RING,		/* 682 ms */
	.period_bytes_min = 256 * SBSND_FB,		/* 5.3 ms: about a timer tick */
	.period_bytes_max = SBSHM_SND_RING,
	.periods_min = 2,
	.periods_max = 1024,
};

static struct sbsnd_stream *sbsnd_st(struct snd_pcm_substream *ss)
{
	return ss->stream == SNDRV_PCM_STREAM_PLAYBACK ? &sbsnd_play : &sbsnd_cap;
}

/* the frames from pos on, n of them, wrapping in the buffer (len frames) and in the ring (at byte position r) */
static void sbsnd_copy(u8 *buf, u32 len, u32 pos, u8 *ring, u32 r, u32 n, bool to_ring)
{
	while (n) {
		u32 a = min(len - pos, (SBSHM_SND_RING - (r & SBSND_RMASK)) / SBSND_FB);
		u32 k = min(n, a);

		if (to_ring)
			memcpy(ring + (r & SBSND_RMASK), buf + (size_t)pos * SBSND_FB, (size_t)k * SBSND_FB);
		else
			memcpy(buf + (size_t)pos * SBSND_FB, ring + (r & SBSND_RMASK), (size_t)k * SBSND_FB);
		pos = (pos + k) % len;
		r += k * SBSND_FB;
		n -= k;
	}
}

/* advance a stream by n frames; true when it crossed a period boundary */
static bool sbsnd_advance(struct sbsnd_stream *s, struct snd_pcm_runtime *rt, u32 n)
{
	s->pos = (s->pos + n) % rt->buffer_size;
	s->period_acc += n;
	if (s->period_acc < rt->period_size)
		return false;
	s->period_acc %= rt->period_size;
	return true;
}

/* sbsnd_lock held */
static bool sbsnd_play_step(struct sbsnd_stream *s)
{
	struct snd_pcm_runtime *rt = s->ss->runtime;
	u32 hw = READ_ONCE(sbsnd->play_hw), fill = s->ring - hw, tgt, n;
	/* draining: report every step, so ALSA sees the pointer reach the end (a last partial period) */
	bool drain = rt->state == SNDRV_PCM_STATE_DRAINING;

	if (fill > SBSHM_SND_RING || (fill & 3)) {	/* impossible: the host's position is wrong; treat as full */
		sbsnd_resyncs++;
		fill = SBSHM_SND_RING;
	}
	fill /= SBSND_FB;
	if (READ_ONCE(sbsnd->play_underruns) != sbsnd_ur_seen) {
		/* the host ran dry: this system's timer jitter is larger than the target allows; grow it */
		sbsnd_ur_seen = READ_ONCE(sbsnd->play_underruns);
		if (!snd_target_ms && sbsnd_target < SBSND_TARGET_MAX) {
			sbsnd_target = min_t(u32, sbsnd_target + sbsnd_tick, SBSND_TARGET_MAX);
			pr_info_ratelimited("sbsnd: host underrun %u: ring target now %u frames\n", sbsnd_ur_seen,
					    sbsnd_target);
		}
	}
	tgt = min3(sbsnd_target, (u32)(rt->buffer_size - rt->period_size), SBSHM_SND_RING / SBSND_FB);
	s->fill = fill;
	if (fill >= tgt)
		return drain;
	n = tgt - fill;
	if (rt->stop_threshold <= rt->buffer_size) {
		/* the normal mode: only what the application wrote. Where ALSA's own hw_ptr (it lags ours: it is
		 * updated through .pointer) puts our position, and how much is queued beyond it */
		snd_pcm_uframes_t hwp = READ_ONCE(rt->status->hw_ptr), ap = READ_ONCE(rt->control->appl_ptr);
		snd_pcm_uframes_t ahead = (s->pos + rt->buffer_size - hwp % rt->buffer_size) % rt->buffer_size;
		snd_pcm_sframes_t q = (snd_pcm_sframes_t)(ap - hwp - ahead);

		if (q < 0)
			q += rt->boundary;
		if (q < 0 || q > (snd_pcm_sframes_t)rt->buffer_size)
			q = 0;
		n = min_t(u32, n, q);
	}
	if (!n)
		return drain;
	sbsnd_copy(rt->dma_area, rt->buffer_size, s->pos, sbsnd_play_ring, s->ring, n, true);
	sbshm_dmb();					/* the frames, then the position */
	s->ring += n * SBSND_FB;
	WRITE_ONCE(sbsnd->play_appl, s->ring);
	s->fill += n;
	return sbsnd_advance(s, rt, n) || drain;
}

/* sbsnd_lock held */
static bool sbsnd_cap_step(struct sbsnd_stream *s)
{
	struct snd_pcm_runtime *rt = s->ss->runtime;
	u32 hw = READ_ONCE(sbsnd->cap_hw), avail = hw - s->ring, n;

	if (avail > SBSHM_SND_RING || (avail & 3)) {	/* impossible: start over at the host's position */
		sbsnd_resyncs++;
		s->ring = hw & ~3U;
		WRITE_ONCE(sbsnd->cap_appl, s->ring);
		return false;
	}
	sbshm_dmb();					/* the position, then the frames */
	n = min_t(u32, avail / SBSND_FB, rt->buffer_size);
	s->fill = avail / SBSND_FB - n;
	if (!n)
		return false;
	sbsnd_copy(rt->dma_area, rt->buffer_size, s->pos, sbsnd_cap_ring, s->ring, n, false);
	sbshm_dmb();					/* read them before the host may overwrite them */
	s->ring += n * SBSND_FB;
	WRITE_ONCE(sbsnd->cap_appl, s->ring);
	return sbsnd_advance(s, rt, n);
}

/*
 * One step of both streams. Only the timer reports the periods elapsed (outside sbsnd_lock: the core takes the
 * stream lock and calls .pointer): .sync_stop's timer_delete_sync then guarantees that no report for a stopped
 * stream is still on its way. The doorbell moves frames at once and leaves the report to the timer.
 */
static bool sbsnd_run(bool report)
{
	struct snd_pcm_substream *el[2];
	unsigned long flags;
	bool more;
	int i, k = 0;

	spin_lock_irqsave(&sbsnd_lock, flags);
	if (sbsnd_play.running && sbsnd_play_step(&sbsnd_play))
		sbsnd_play.pending = true;
	if (sbsnd_cap.running && sbsnd_cap_step(&sbsnd_cap))
		sbsnd_cap.pending = true;
	if (report) {
		if (sbsnd_play.running && sbsnd_play.pending)
			el[k++] = sbsnd_play.ss;
		if (sbsnd_cap.running && sbsnd_cap.pending)
			el[k++] = sbsnd_cap.ss;
		sbsnd_play.pending = sbsnd_cap.pending = false;
		more = sbsnd_play.running || sbsnd_cap.running;
	} else {
		more = sbsnd_play.pending || sbsnd_cap.pending;
	}
	spin_unlock_irqrestore(&sbsnd_lock, flags);
	for (i = 0; i < k; i++)
		snd_pcm_period_elapsed(el[i]);
	return more;
}

static void sbsnd_timer_fn(struct timer_list *t)
{
	if (sbsnd_run(true))
		mod_timer(&sbsnd_timer, jiffies + 1);
}

void sbsnd_kick(void)
{
	/* the host rings on a playback underrun: refill now instead of at the next tick */
	if (READ_ONCE(sbsnd_ready) && (READ_ONCE(sbsnd_play.running) || READ_ONCE(sbsnd_cap.running)) &&
	    sbsnd_run(false))
		mod_timer(&sbsnd_timer, jiffies);
}

/* ---- PCM ops --------------------------------------------------------------------------------------------- */
static int sbsnd_open(struct snd_pcm_substream *ss)
{
	struct sbsnd_stream *s = sbsnd_st(ss);
	unsigned long flags;

	ss->runtime->hw = sbsnd_hw;
	spin_lock_irqsave(&sbsnd_lock, flags);
	s->ss = ss;
	s->running = false;
	spin_unlock_irqrestore(&sbsnd_lock, flags);
	return 0;
}

static int sbsnd_close(struct snd_pcm_substream *ss)
{
	return 0;
}

static int sbsnd_prepare(struct snd_pcm_substream *ss)
{
	struct sbsnd_stream *s = sbsnd_st(ss);
	unsigned long flags;

	spin_lock_irqsave(&sbsnd_lock, flags);
	s->pos = 0;
	s->period_acc = 0;
	s->fill = 0;
	s->pending = false;
	spin_unlock_irqrestore(&sbsnd_lock, flags);
	return 0;
}

static int sbsnd_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct sbsnd_stream *s = sbsnd_st(ss);
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned long flags;

	spin_lock_irqsave(&sbsnd_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (play) {
			WRITE_ONCE(sbsnd->play_period_frames, ss->runtime->period_size);
			WRITE_ONCE(sbsnd->play_starts, READ_ONCE(sbsnd->play_starts) + 1);
			s->running = true;
			s->pending = sbsnd_play_step(s);	/* the ring filled before the host sees RUN */
			sbshm_dmb();
			WRITE_ONCE(sbsnd->play_state, SBSHM_SND_RUN);
		} else {
			s->ring = READ_ONCE(sbsnd->cap_hw) & ~3U;	/* nothing older than now */
			WRITE_ONCE(sbsnd->cap_appl, s->ring);
			WRITE_ONCE(sbsnd->cap_period_frames, ss->runtime->period_size);
			WRITE_ONCE(sbsnd->cap_starts, READ_ONCE(sbsnd->cap_starts) + 1);
			sbshm_dmb();
			WRITE_ONCE(sbsnd->cap_state, SBSHM_SND_RUN);
			s->running = true;
		}
		/* a prefill that crossed a period (two short periods) is reported at once by the timer: the
		 * core's period_elapsed takes the stream lock, which trigger runs under */
		mod_timer(&sbsnd_timer, jiffies + (s->pending ? 0 : 1));
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		s->running = false;
		if (play) {
			/* the end of a drain: the host plays the rest; else (drop, xrun, pause) it skips it */
			if (cmd == SNDRV_PCM_TRIGGER_STOP && ss->runtime->state == SNDRV_PCM_STATE_DRAINING) {
				WRITE_ONCE(sbsnd->play_state, SBSHM_SND_DRAIN);
			} else {
				WRITE_ONCE(sbsnd->play_discard, s->ring);
				sbshm_dmb();
				WRITE_ONCE(sbsnd->play_state, SBSHM_SND_STOP);
			}
		} else {
			WRITE_ONCE(sbsnd->cap_state, SBSHM_SND_STOP);
		}
		break;
	default:
		spin_unlock_irqrestore(&sbsnd_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&sbsnd_lock, flags);
	return 0;
}

/* no period_elapsed of a stopped stream may still be on its way when ALSA frees it */
static int sbsnd_sync_stop(struct snd_pcm_substream *ss)
{
	timer_delete_sync(&sbsnd_timer);
	if (READ_ONCE(sbsnd_play.running) || READ_ONCE(sbsnd_cap.running))
		mod_timer(&sbsnd_timer, jiffies + 1);
	return 0;
}

static snd_pcm_uframes_t sbsnd_pointer(struct snd_pcm_substream *ss)
{
	struct sbsnd_stream *s = sbsnd_st(ss);
	snd_pcm_uframes_t pos;
	unsigned long flags;

	spin_lock_irqsave(&sbsnd_lock, flags);
	pos = s->pos;
	/* the frames between the hw pointer and the speaker / the mic */
	ss->runtime->delay = s->fill + READ_ONCE(sbsnd->host_latency_frames);
	spin_unlock_irqrestore(&sbsnd_lock, flags);
	return pos;
}

static const struct snd_pcm_ops sbsnd_ops = {
	.open = sbsnd_open,
	.close = sbsnd_close,
	.prepare = sbsnd_prepare,
	.trigger = sbsnd_trigger,
	.sync_stop = sbsnd_sync_stop,
	.pointer = sbsnd_pointer,
};

void sbsnd_init(struct device *dev, bool irq_ok)
{
	struct snd_pcm *pcm;
	u32 caps, block, tick = SBSHM_SND_RATE / HZ;
	bool play, cap;
	int ret;

	if (!sbg_svc(SBSHM_SVC_SND))
		return;
	BUILD_BUG_ON(sizeof(struct sbshm_snd) > SBSHM_SND_PLAY_OFF);
	BUILD_BUG_ON(SBSHM_SND_CAP_OFF + SBSHM_SND_RING > SBSHM_CLIP_OFF - SBSHM_SND_OFF);
	if (shm_size < SBSHM_SND_OFF + SBSHM_SND_CAP_OFF + SBSHM_SND_RING) {
		dev_warn(dev, "sbsnd: the shared region is too small\n");
		return;
	}
	sbsnd = sbg_vmap_wc(SBSHM_SND_OFF, SBSHM_SND_CAP_OFF + SBSHM_SND_RING);
	if (!sbsnd)
		return;
	if (READ_ONCE(sbsnd->magic) != SBSHM_SND_MAGIC || sbsnd->rate != SBSHM_SND_RATE ||
	    sbsnd->channels != SBSHM_SND_CHANNELS || sbsnd->format != SBSHM_SND_FMT_S16LE ||
	    sbsnd->ring_bytes != SBSHM_SND_RING || sbsnd->play_off != SBSHM_SND_PLAY_OFF ||
	    sbsnd->cap_off != SBSHM_SND_CAP_OFF) {
		dev_warn(dev, "sbsnd: the host's layout is not this driver's (magic %#x, %u Hz, %u ch, ring %u)\n",
			 sbsnd->magic, sbsnd->rate, sbsnd->channels, sbsnd->ring_bytes);
		return;
	}
	sbsnd_play_ring = (u8 *)sbsnd + SBSHM_SND_PLAY_OFF;
	sbsnd_cap_ring = (u8 *)sbsnd + SBSHM_SND_CAP_OFF;
	caps = READ_ONCE(sbsnd->caps);
	play = caps & SBSHM_SND_CAP_PLAY;
	cap = caps & SBSHM_SND_CAP_CAPTURE;
	if (!play && !cap) {
		dev_info(dev, "sbsnd: the host offers no stream\n");
		return;
	}
	block = clamp_t(u32, READ_ONCE(sbsnd->host_block_frames), 1, SBSHM_SND_RING / SBSND_FB / 4);
	sbsnd_tick = tick;
	sbsnd_target = snd_target_ms ? clamp_t(u32, snd_target_ms * (SBSHM_SND_RATE / 1000), 1, SBSHM_SND_RING / SBSND_FB / 2)
				     : min_t(u32, 2 * block + 4 * tick, SBSND_TARGET_MAX);
	sbsnd_ur_seen = READ_ONCE(sbsnd->play_underruns);
	/* the ring positions continue from the host's (zero after a load) */
	sbsnd_play.ring = READ_ONCE(sbsnd->play_appl);
	sbsnd_cap.ring = READ_ONCE(sbsnd->cap_hw);
	timer_setup(&sbsnd_timer, sbsnd_timer_fn, 0);

	ret = snd_card_new(dev, -1, "Starbite", THIS_MODULE, 0, &sbsnd_card);
	if (ret) {
		dev_warn(dev, "sbsnd: snd_card_new: %d\n", ret);
		return;
	}
	strscpy(sbsnd_card->driver, "sbsnd", sizeof(sbsnd_card->driver));
	strscpy(sbsnd_card->shortname, "Starbite", sizeof(sbsnd_card->shortname));
	strscpy(sbsnd_card->longname, "Starbite OS speaker and microphone (sbsnd)", sizeof(sbsnd_card->longname));
	ret = snd_pcm_new(sbsnd_card, "Starbite", 0, play, cap, &pcm);
	if (ret)
		goto fail;
	strscpy(pcm->name, "Starbite", sizeof(pcm->name));
	if (play)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &sbsnd_ops);
	if (cap)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &sbsnd_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	ret = snd_card_register(sbsnd_card);
	if (ret)
		goto fail;
	WRITE_ONCE(sbsnd_ready, true);
	/* short: the host shows the guest's log in 160-character lines */
	dev_info(dev, "sbsnd: card %d: %s%s%s 48k S16 stereo, host block %u lat %u, target %u frames, %u ms tick%s\n",
		 sbsnd_card->number, play ? "play" : "", play && cap ? "+" : "", cap ? "rec" : "", block,
		 READ_ONCE(sbsnd->host_latency_frames), sbsnd_target, jiffies_to_msecs(1), irq_ok ? "+doorbell" : "");
	return;
fail:
	dev_warn(dev, "sbsnd: %d\n", ret);
	snd_card_free(sbsnd_card);
	sbsnd_card = NULL;
}
