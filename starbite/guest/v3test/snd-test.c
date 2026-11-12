/* SPDX-License-Identifier: GPL-2.0-only
 * snd-test: sbsnd's guest-side test (no alsa-lib: the raw ALSA PCM ioctls of <sound/asound.h>). On the card whose id
 * is "Starbite" (/proc/asound/Starbite -> cardN): plays 3 s of a 440 Hz sine (amplitude 16384, both channels) while
 * a child records 2 s, then checks that the recording is the 1 kHz tone sbsim pushes (Goertzel against the energy
 * per 100-ms block: 90 % of the blocks after the first 0.25 s).
 * Prints to /dev/kmsg (the host reads the guest's log), each line short (sbghost cuts at 160 characters):
 *   v3-test: snd: play ... / v3-test: snd: rec ... / v3-test: snd: PASS|FAIL ...
 * sbsim (sbsim/sim_snd.c) checks the 440 Hz it pulled on its side. Static build in the Lima VM:
 *   gcc -O2 -static -Wall -Wextra snd-test.c -o snd-test -lm
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define RATE 48000
#define PERIOD 1024
#define PERIODS 4

static void klog(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, b, strlen(b)) < 0)
            perror("kmsg");
        close(fd);
    }
    printf("%s\n", b);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void p_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof(*p));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++)
        memset(&p->masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof(struct snd_mask));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
        struct snd_interval *v = &p->intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        v->min = 0;
        v->max = UINT_MAX;
    }
    p->rmask = ~0U;
    p->info = ~0U;
}
static void p_mask(struct snd_pcm_hw_params *p, int k, unsigned v)
{
    struct snd_mask *m = &p->masks[k - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[v >> 5] |= 1U << (v & 31);
}
static void p_int(struct snd_pcm_hw_params *p, int k, unsigned v)
{
    struct snd_interval *i = &p->intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->openmin = i->openmax = 0;
    i->integer = 1;
}

/* open + hw_params (S16_LE, 2 ch, 48 kHz, PERIODS x PERIOD frames) + prepare; the fd or -errno */
static int pcm_open(int card, int capture, char *why, size_t wn)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0%c", card, capture ? 'c' : 'p');
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        snprintf(why, wn, "open %s: %s", path, strerror(errno));
        return -1;
    }
    struct snd_pcm_hw_params hp;
    p_any(&hp);
    p_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    p_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    p_mask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    p_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    p_int(&hp, SNDRV_PCM_HW_PARAM_RATE, RATE);
    p_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
    p_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
        snprintf(why, wn, "%s HW_PARAMS: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        snprintf(why, wn, "%s PREPARE: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

struct rec_result {
    int ok;
    char why[160];
    unsigned frames, overruns, blocks, tone;
    double ratio, rms, secs;
};

/* the child: record 2 s, analyse the last 1.75 s for 1 kHz */
static void record(int card, struct rec_result *r)
{
    static int16_t buf[2 * RATE * 2];
    const unsigned want = 2 * RATE;
    memset(r, 0, sizeof(*r));
    int fd = pcm_open(card, 1, r->why, sizeof(r->why));
    if (fd < 0)
        return;
    double t0 = now();
    if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
        snprintf(r->why, sizeof(r->why), "capture START: %s", strerror(errno));
        return;
    }
    while (r->frames < want) {
        struct snd_xferi x = { .buf = buf + 2 * r->frames, .frames = want - r->frames > PERIOD ? PERIOD : want - r->frames };
        if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) < 0) {
            if (errno == EPIPE) {
                r->overruns++;
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
                ioctl(fd, SNDRV_PCM_IOCTL_START);
                continue;
            }
            snprintf(r->why, sizeof(r->why), "READI: %s", strerror(errno));
            close(fd);
            return;
        }
        r->frames += x.result;
    }
    r->secs = now() - t0;
    ioctl(fd, SNDRV_PCM_IOCTL_DROP);
    close(fd);
    /* Goertzel at 1 kHz per 100-ms block (bin 100 exactly) over [0.25 s, 2 s) of the mono mix: an overrun's restart
     * jumps the phase, so whole-recording sums would mix it up; blocks see it as one bad block */
    const unsigned B = RATE / 10;
    double w = 2 * M_PI * 1000.0 / RATE, c = 2 * cos(w), rsum = 0, esum = 0;
    for (unsigned a = RATE / 4; a + B <= want; a += B) {
        double s1 = 0, s2 = 0, e = 0;
        for (unsigned i = a; i < a + B; ++i) {
            double m = (buf[2 * i] + buf[2 * i + 1]) / 2.0, s0 = m + c * s1 - s2;
            s2 = s1;
            s1 = s0;
            e += m * m;
        }
        double p = s1 * s1 + s2 * s2 - c * s1 * s2, q = e > 0 ? 2 * p / (B * e) : 0;
        r->blocks++;
        r->tone += q > 0.9;
        rsum += q;
        esum += e;
    }
    r->ratio = r->blocks ? rsum / r->blocks : 0;
    r->rms = r->blocks ? sqrt(esum / ((double)B * r->blocks)) : 0;
    r->ok = r->blocks >= 15 && r->tone * 10 >= r->blocks * 9 && r->rms > 8000 && r->rms < 16000;
    if (!r->ok)
        snprintf(r->why, sizeof(r->why), "the recording is not the 1 kHz tone");
}

int main(void)
{
    char link[64] = "", why[160] = "";
    ssize_t ln = readlink("/proc/asound/Starbite", link, sizeof(link) - 1);
    int card = -1;
    if (ln > 4 && !strncmp(link, "card", 4))
        card = atoi(link + 4);
    if (card < 0) {
        klog("v3-test: snd: FAIL no ALSA card \"Starbite\" (/proc/asound/Starbite missing: sbsnd not offered?)");
        return 1;
    }
    /* the child records while the parent plays (full duplex) */
    int pfd[2];
    if (pipe(pfd) < 0)
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        struct rec_result r;
        close(pfd[0]);
        usleep(300000);                         /* start inside the playback */
        record(card, &r);
        if (write(pfd[1], &r, sizeof(r)) != sizeof(r))
            _exit(1);
        _exit(0);
    }
    close(pfd[1]);

    int fd = pcm_open(card, 0, why, sizeof(why));
    unsigned played = 0, xruns = 0, nd = 0;
    double dsum = 0, dmin = 1e9, dmax = 0, t0 = now(), t_drain = 0;
    const unsigned total = 3 * RATE;
    static int16_t buf[2 * PERIOD];
    if (fd >= 0) {
        double ph = 0, dph = 2 * M_PI * 440.0 / RATE;
        while (played < total) {
            unsigned n = total - played > PERIOD ? PERIOD : total - played;
            for (unsigned i = 0; i < n; ++i) {
                buf[2 * i] = buf[2 * i + 1] = (int16_t)lrint(16384 * sin(ph));
                ph += dph;
                if (ph > 2 * M_PI)
                    ph -= 2 * M_PI;
            }
            unsigned off = 0;
            while (off < n) {
                struct snd_xferi x = { .buf = buf + 2 * off, .frames = n - off };
                if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
                    if (errno == EPIPE) {
                        xruns++;
                        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
                        continue;
                    }
                    snprintf(why, sizeof(why), "WRITEI: %s", strerror(errno));
                    goto out;
                }
                off += x.result;
            }
            played += n;
            snd_pcm_sframes_t d;
            if (played > RATE / 2 && ioctl(fd, SNDRV_PCM_IOCTL_DELAY, &d) == 0) {   /* steady state */
                double ms = d * 1000.0 / RATE;
                dsum += ms;
                nd++;
                dmin = ms < dmin ? ms : dmin;
                dmax = ms > dmax ? ms : dmax;
            }
        }
        t_drain = now();
        if (ioctl(fd, SNDRV_PCM_IOCTL_DRAIN) < 0)
            snprintf(why, sizeof(why), "DRAIN: %s", strerror(errno));
    out:
        close(fd);
    }
    double t1 = now();
    struct rec_result r;
    memset(&r, 0, sizeof(r));
    if (read(pfd[0], &r, sizeof(r)) != sizeof(r))
        snprintf(r.why, sizeof(r.why), "the recorder died");
    waitpid(pid, NULL, 0);

    int play_ok = fd >= 0 && !why[0] && played == total && xruns == 0 && t1 - t0 > 2.9 && t1 - t0 < 3.6;
    /* short lines: the host shows the guest's log in 160-character lines */
    klog("v3-test: snd: play %.2f s of 440 Hz in %.2f s (drain %.0f ms), xruns %u, delay avg %.1f min %.1f max %.1f ms",
         played / (double)RATE, t1 - t0, (t1 - t_drain) * 1000, xruns, nd ? dsum / nd : 0.0, nd ? dmin : 0.0, dmax);
    klog("v3-test: snd: rec %.2f s in %.2f s: 1 kHz in %u of %u blocks (%.1f %% of the energy), rms %.0f, overruns %u",
         r.frames / (double)RATE, r.secs, r.tone, r.blocks, r.ratio * 100, r.rms, r.overruns);
    if (play_ok && r.ok)
        klog("v3-test: snd: PASS full duplex on card %d: 3 s of 440 Hz played, sbsim's 1 kHz recorded", card);
    else
        klog("v3-test: snd: FAIL %s%.70s%s%.70s", play_ok ? "" : "play: ", play_ok ? "" : (why[0] ? why : "time / xruns"),
             r.ok ? "" : " rec: ", r.ok ? "" : r.why);
    return play_ok && r.ok ? 0 : 1;
}
