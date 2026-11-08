/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link end-to-end check: a headless viewer (the Mac app's own session code, mac/sbv.c) that
 * connects to a sender over TCP, ACKs frames like the app, and measures what arrives.
 *
 *   viewbench --tcp HOST:PORT [--seconds S] [--mode WxH] [--hash-file F] [--settle S] [--input-hz N]
 *
 * Prints frames/s, Mbit/s, rects and bytes per frame, the viewer's CPU per frame (parse + LZ4 + apply), and
 * the PING round trip. Input: sends a key press/release and a pointer move N times a second (default 1, 0 =
 * none); the sender log shows them arriving. --hash-file: after S seconds, waits for the sender's scene to stop (the file appears) and the
 * stream to go quiet, then checks the composed image against the desktop hash, and counts the frames
 * and bytes that arrive while the desktop is static (should be 0). Exit 0 = connected, frames, image match.
 */
#include "../mac/sbv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fd = -1;
static uint64_t frames, frame_bytes, frame_rects, keyframes, last_frame_us;
static sbv_frame_info last;

static uint64_t mono_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u; }
static double thread_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int ev_send(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}
static uint64_t ev_now(void *ctx) { (void)ctx; return mono_us(); }
static void ev_frame(void *ctx, const sbv_frame_info *fi)
{
    (void)ctx;
    frames++;
    frame_bytes += fi->bytes;
    frame_rects += fi->nrects;
    keyframes += fi->keyframe != 0;
    last = *fi;
    last_frame_us = mono_us();
}
static void ev_event(void *ctx, int ev, const char *text)
{
    (void)ctx;
    if (ev != SBV_EV_PONG) fprintf(stderr, "viewbench: %s\n", text);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *hash_file = NULL;
    int port = SBLK_TCP_PORT;
    double seconds = 10, settle = 1.5, input_hz = 1;
    unsigned mw = 0, mh = 0;
    static char hostbuf[256];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--tcp") && v) {
            snprintf(hostbuf, sizeof hostbuf, "%s", v); i++;
            char *c = strrchr(hostbuf, ':');
            if (c) { *c = 0; port = atoi(c + 1); }
            host = hostbuf;
        } else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--settle") && v) { settle = atof(v); i++; }
        else if (!strcmp(a, "--input-hz") && v) { input_hz = atof(v); i++; }
        else if (!strcmp(a, "--mode") && v) { sscanf(v, "%ux%u", &mw, &mh); i++; }
        else if (!strcmp(a, "--hash-file") && v) { hash_file = v; i++; }
        else { fprintf(stderr, "usage: viewbench --tcp HOST:PORT [--seconds S] [--mode WxH] [--hash-file F] [--settle S] [--input-hz N]\n"); return 2; }
    }

    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { fprintf(stderr, "bad host %s\n", host); return 2; }
    for (int tries = 0;; tries++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) break;
        close(fd);
        if (tries > 50) { perror("connect"); return 1; }
        usleep(100000);
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    static sbv s;
    sbv_events ev = { NULL, ev_send, ev_now, ev_frame, ev_event };
    if (sbv_init(&s, &ev, "viewbench", (uint16_t)mw, (uint16_t)mh, 60000) < 0 || sbv_start(&s) < 0) return 1;

    static uint8_t buf[256 * 1024];
    uint64_t t_start = mono_us(), t_first = 0, f_first = 0, b_first = 0, r_first = 0, next_input = 0;
    double cpu_first = 0, rtt_sum = 0;
    int rtt_n = 0, input_sent = 0;
    uint64_t pongs_seen = 0;
    int phase = 0;            /* 0 = measuring, 1 = waiting for the static desktop, 2 = static window */
    uint64_t static_from = 0, static_frames0 = 0, static_bytes0 = 0, static_rx0 = 0;
    double meas_fps = 0, meas_mbps = 0, meas_cpu = 0, meas_rects = 0, meas_kib = 0;
    uint64_t meas_frames = 0;
    int rc = 0;

    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 50);
        if (r > 0) {
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n <= 0) { fprintf(stderr, "viewbench: connection closed\n"); rc = 1; break; }
            int e = sbv_feed(&s, buf, (size_t)n);
            if (e < 0) { fprintf(stderr, "viewbench: %s\n", sbv_strerror(e)); rc = 1; break; }
        }
        if (sbv_tick(&s) < 0) { fprintf(stderr, "viewbench: link dead\n"); rc = 1; break; }
        uint64_t now = mono_us();
        if (s.pongs != pongs_seen) { pongs_seen = s.pongs; rtt_sum += (double)s.rtt_us / 1000.0; rtt_n++; }
        if (!t_first && frames >= 2) {   /* measure from the first delta frame on (keyframe excluded) */
            t_first = now; f_first = frames; b_first = s.rx_bytes; r_first = frame_rects; cpu_first = thread_ms();
            next_input = now;
        }
        if (t_first && phase == 0 && input_hz > 0 && now >= next_input) {
            /* a key tap and a pointer move (the sender logs or injects them) */
            sbv_key(&s, SBLK_HID_PAGE_KEYBOARD, 0x04, 1, 0);
            sbv_key(&s, SBLK_HID_PAGE_KEYBOARD, 0x04, 0, 0);
            sbv_pointer(&s, (int32_t)(now / 1000 % 500), 100, 0, 0, 0);
            input_sent++;
            next_input = now + (uint64_t)(1e6 / input_hz);
        }
        if (phase == 0 && t_first && now - t_first >= (uint64_t)(seconds * 1e6)) {
            double secs = (double)(now - t_first) / 1e6;
            meas_frames = frames - f_first;
            meas_fps = (double)meas_frames / secs;
            meas_mbps = (double)(s.rx_bytes - b_first) * 8 / secs / 1e6;
            meas_cpu = meas_frames ? (thread_ms() - cpu_first) / (double)meas_frames : 0;
            meas_rects = meas_frames ? (double)(frame_rects - r_first) / (double)meas_frames : 0;
            meas_kib = meas_frames ? (double)(s.rx_bytes - b_first) / 1024.0 / (double)meas_frames : 0;
            if (!hash_file) break;
            phase = 1;
        }
        if (phase == 1) {
            FILE *f = fopen(hash_file, "r");
            if (f) {
                fclose(f);
                if (now - last_frame_us > (uint64_t)(settle * 1e6)) {
                    phase = 2; static_from = now; static_frames0 = frames; static_bytes0 = frame_bytes;
                    static_rx0 = s.rx_bytes;
                }
            }
            if (now - t_start > (uint64_t)((seconds + 60) * 1e6)) { fprintf(stderr, "viewbench: the scene never stopped\n"); rc = 1; break; }
        }
        if (phase == 2 && now - static_from >= 3000000u) break;
        if (now - t_start > (uint64_t)((seconds + 90) * 1e6)) { rc = 1; break; }
    }

    printf("viewbench: %llu frames in the measured %.0f s: %.1f frames/s, %.2f Mbit/s, %.1f KiB and %.1f rects per frame, "
           "viewer cpu %.3f ms/frame, PING rtt avg %.2f ms (%d), %d input bursts sent\n",
           (unsigned long long)meas_frames, seconds, meas_fps, meas_mbps, meas_kib, meas_rects, meas_cpu,
           rtt_n ? rtt_sum / rtt_n : 0.0, rtt_n, input_sent);
    if (!frames) rc = 1;
    if (hash_file && phase == 2) {
        unsigned w = 0, h = 0;
        unsigned long long want = 0;
        FILE *f = fopen(hash_file, "r");
        if (!f || fscanf(f, "%ux%u %llx", &w, &h, &want) != 3) { fprintf(stderr, "viewbench: bad hash file\n"); rc = 1; }
        if (f) fclose(f);
        uint64_t got = sbv_fb_hash(last.fb, last.stride, last.width, last.height);
        /* last.fb is the session's frame buffer, still valid (no frame since) */
        int match = w == last.width && h == last.height && got == want;
        printf("viewbench: static desktop for 3 s: %llu frames, %llu bytes of frames, %llu bytes in all (PING/PONG)\n",
               (unsigned long long)(frames - static_frames0), (unsigned long long)(frame_bytes - static_bytes0),
               (unsigned long long)(s.rx_bytes - static_rx0));
        printf("viewbench: final image %ux%u %016llx, sender desktop %ux%u %016llx: %s\n", last.width, last.height,
               (unsigned long long)got, w, h, want, match ? "MATCH" : "MISMATCH");
        if (!match) {
            rc = 1;
            char raw[1024];
            snprintf(raw, sizeof raw, "%s.viewer.bgra", hash_file);
            FILE *r = fopen(raw, "wb");
            if (r) {
                for (uint32_t y = 0; y < last.height; y++) fwrite(last.fb + (size_t)y * last.stride, 4, last.width, r);
                fclose(r);
                printf("viewbench: viewer image written to %s\n", raw);
            }
        }
    } else if (hash_file) rc = 1;
    sbv_bye(&s, SBLK_BYE_NORMAL, NULL);
    close(fd);
    sbv_free(&s);
    return rc;
}
