/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link end-to-end check: CPU cost of the sender's frame pipeline (win/sender/pipeline.c, the same
 * code sblinkd.exe runs on the phone) on scene.h, measured as thread CPU time per frame. Every encoded frame
 * is decoded and applied with the core (as the Mac viewer does) and the result must equal the desktop
 * exactly, so the numbers are for a correct stream.
 *
 *   encbench [--frames N] [--fps F] [--size WxH]...      default: 1280x720 and 1920x1080, 300 frames, 30 fps
 *
 * Per size it runs: "drag" (a window dragged at 240 px/s, the clock ticking), "drag-fast" (960 px/s),
 * "idle" (only the taskbar clock changes once a second). Frames with no OS dirty rects are not encoded, as
 * in the sender's loop (session.c), so an idle desktop costs nothing between clock ticks.
 */
#include "../win/sender/sender.h"
#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double thread_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* decodes one encoded frame and applies it to fb; returns rect count or < 0 */
static int apply_frame(const uint8_t *p, size_t n, uint8_t *fb, uint32_t w, uint32_t h, uint8_t *scratch)
{
    size_t off = 0;
    int rects = 0;
    while (off < n) {
        sblk_msg m;
        long used = sblk_decode(p + off, n - off, &m);
        if (used <= 0) return -1;
        if (m.type == SBLK_T_RECT) {
            if (sblk_apply_rect(fb, (size_t)w * 4, w, h, &m.u.rect, scratch) != SBLK_OK) return -2;
            rects++;
        }
        off += (size_t)used;
    }
    return rects;
}

static int run(uint32_t w, uint32_t h, int32_t speed, const char *label, int frames, int fps)
{
    scene sc;
    sbl_enc e;
    if (scene_init(&sc, w, h, speed) < 0 || sbl_enc_init(&e, 1) < 0) { fprintf(stderr, "init failed\n"); return 1; }
    uint8_t *fb = calloc((size_t)w * h, 4), *scratch = malloc((size_t)w * h * 4);
    double *ms = calloc((size_t)frames, sizeof *ms);
    if (!fb || !scratch || !ms) return 1;
    uint32_t seq = 1, caps = SBLK_CAP_ENC_RAW | SBLK_CAP_ENC_LZ4 | SBLK_CAP_ENC_SOLID;
    sbl_region dirty;

    /* keyframe */
    sbl_region_full(&dirty, w, h);
    double t0 = thread_ms();
    long kb = sbl_enc_frame(&e, sc.px, sc.stride, w, h, &dirty, 1, caps, SBLK_MAX_PAYLOAD, 1, 0, &seq);
    double kms = thread_ms() - t0;
    if (kb <= 0 || apply_frame(e.out, (size_t)kb, fb, w, h, scratch) < 0) { fprintf(stderr, "keyframe failed\n"); return 1; }

    uint64_t ticks_per_frame = SCENE_TICK_HZ / (uint64_t)fps, t = 0, bytes = 0, rects = 0, area = 0;
    int encoded = 0;
    double dec_ms = 0, sum = 0;
    for (int f = 0; f < frames; f++) {
        sbl_region_clear(&dirty);
        t += ticks_per_frame;
        scene_rect r[SCENE_MAX_DIRTY];
        /* the compositor ran at 60 Hz in between: dirty accumulates over the skipped ticks */
        for (uint64_t k = t - ticks_per_frame + 1; k <= t; k++) {
            int n = scene_step(&sc, k, r);
            for (int i = 0; i < n; i++) sbl_region_add(&dirty, r[i].x, r[i].y, r[i].w, r[i].h, w, h);
        }
        if (dirty.n == 0) continue;           /* the sender does not even map the capture */
        t0 = thread_ms();
        long len = sbl_enc_frame(&e, sc.px, sc.stride, w, h, &dirty, 0, caps, SBLK_MAX_PAYLOAD,
                                 (uint32_t)f + 2, 0, &seq);
        double dt = thread_ms() - t0;
        if (len < 0) { fprintf(stderr, "encode failed %ld\n", len); return 1; }
        ms[encoded++] = dt;
        sum += dt;
        if (len == 0) continue;
        bytes += (uint64_t)len;
        rects += e.nrects;
        area += e.area;
        double d0 = thread_ms();
        if (apply_frame(e.out, (size_t)len, fb, w, h, scratch) < 0) { fprintf(stderr, "decode failed\n"); return 1; }
        dec_ms += thread_ms() - d0;
    }
    int exact = 1;
    for (uint32_t y = 0; y < h && exact; y++)
        if (memcmp(fb + (size_t)y * w * 4, sc.px + (size_t)y * sc.stride, (size_t)w * 4)) exact = 0;

    double secs = (double)frames / fps;
    qsort(ms, (size_t)encoded, sizeof *ms, cmpd);
    double p50 = encoded ? ms[encoded / 2] : 0, p95 = encoded ? ms[(encoded * 95) / 100] : 0,
           mx = encoded ? ms[encoded - 1] : 0;
    printf("%-9s %4ux%-4u key %6.1f KiB %6.2f ms | %3d/%d frames encoded: cpu avg %5.2f p50 %5.2f p95 %5.2f max %5.2f ms"
           " | %6.1f KiB/frame %5.1f rects %4.1f%% px | %6.2f Mbit/s @%d fps | cpu %4.1f%% of a core | decode %4.2f ms | %s\n",
           label, w, h, (double)kb / 1024, kms, encoded, frames, encoded ? sum / encoded : 0, p50, p95, mx,
           encoded ? (double)bytes / encoded / 1024 : 0, encoded ? (double)rects / encoded : 0,
           encoded ? 100.0 * (double)area / encoded / ((double)w * h) : 0, (double)bytes * 8 / secs / 1e6, fps,
           100.0 * sum / (secs * 1000), encoded ? dec_ms / encoded : 0, exact ? "EXACT" : "MISMATCH");
    sbl_enc_free(&e);
    scene_free(&sc);
    free(fb); free(scratch); free(ms);
    return exact ? 0 : 1;
}

int main(int argc, char **argv)
{
    int frames = 300, fps = 30, nsizes = 0, fails = 0;
    uint32_t sw[8], sh[8];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && nsizes < 8) {
            if (sscanf(argv[++i], "%ux%u", &sw[nsizes], &sh[nsizes]) == 2) nsizes++;
        } else { fprintf(stderr, "usage: encbench [--frames N] [--fps F] [--size WxH]...\n"); return 2; }
    }
    if (fps < 1 || fps > 60 || 60 % fps) { fprintf(stderr, "fps must divide 60\n"); return 2; }
    if (!nsizes) { sw[0] = 1280; sh[0] = 720; sw[1] = 1920; sh[1] = 1080; nsizes = 2; }
    for (int i = 0; i < nsizes; i++) {
        fails += run(sw[i], sh[i], 4, "drag", frames, fps);
        fails += run(sw[i], sh[i], 16, "drag-fast", frames, fps);
        fails += run(sw[i], sh[i], 0, "idle", frames, fps);
    }
    printf("encbench: %s\n", fails ? "FAILED" : "all frames decoded exactly");
    return fails != 0;
}
