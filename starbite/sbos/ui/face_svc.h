/* SPDX-License-Identifier: GPL-2.0-only
 * Face Unlock's service (face_svc.c): shell.h's sh_face_* on kern/face.c, for the kernel (kern_next.c) and the host
 * shell (host/main.c), each giving the platform below. The platform calls fsvc_tick from a timer every ~10 ms while
 * fsvc_busy() (and once soon after fsvc_passcode*: the key's derivation runs in a turn of its own, ~45 ms). */
#ifndef SBOS_FACE_SVC_H
#define SBOS_FACE_SVC_H
#include <stdint.h>
#include <stddef.h>
#include "facenet.h"

struct fsvc_plat {
    /* the face pack (tools/face-pack.py): 1 in memory (*p, *len), 0 being loaded (asking starts it), -1 not there.
     * The phone keeps it on the card (/starbite/face/sbface.pak) and loads it into the heap, checked, on demand */
    int (*pack_get)(const uint8_t **p, size_t *len);
    int (*pack_state)(void);            /* the same without starting anything: -1 not there, 0 not tried, 1 loading, 2 in */
    struct fn_plat fn;                  /* memory, the other cores, the clock in us */
    uint64_t (*now_ms)(void);
    /* the front camera: portrait 360 x 480, NOT mirrored; 0 started; the newest frame (stride = w) or 0 */
    int (*cam_open)(unsigned w, unsigned h);
    const uint32_t *(*cam_frame)(unsigned *seq);
    void (*cam_close)(void);
    int (*blob_get)(const char *name, void *p, unsigned cap);
    int (*blob_put)(const char *name, const void *p, unsigned n);   /* n 0 = delete */
    int (*pref_int)(const char *key, int def);
    void (*pref_set_int)(const char *key, int v);
    void (*random)(uint8_t *p, unsigned n);
    int (*pass_on)(void);
    int (*pass_wait)(void);             /* s of a passcode back-off still to run */
    void (*log)(const char *line);
    void (*arm)(unsigned ms);           /* fsvc_tick again in ms (the platform's timer) */
    int fake;                           /* the host without a pack: scans match after 0.3 s (SBTK_FACE=fail: fail) */
    const uint8_t *pak;                 /* (set by face_svc.c once pack_get gave it) */
    size_t pak_len;
};

void fsvc_init(const struct fsvc_plat *p);
int fsvc_busy(void);                    /* a scan, a set-up or a key job runs: keep ticking */
void fsvc_tick(void);                   /* one turn's work (<= ~35 ms of the face's own) */
/* the passcode was right (sh_pass_check): the key unwrapped (or made, the first time), the templates opened */
void fsvc_passcode(const char *digits);
/* the passcode was set, changed or removed (sh_pass_set succeeded): the key rewrapped, or everything forgotten */
void fsvc_passcode_set(const char *old, const char *nw);
/* the face test image: the networks on a made-up frame, timed (a log line); 1 started */
int fsvc_bench_start(void);
/* the face test image: the camera for ms with nobody enrolled (no unlock possible): the first frame's time, the
 * detections, the turns (a log line); 1 started */
int fsvc_camera_test(unsigned ms);
int fsvc_result_first_frame_ms(void);   /* the last job's first frame after the camera's start (0: none) */
void fsvc_host_enrolled(void);          /* the host's stills (fake platform only) */

#endif
