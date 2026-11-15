/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS Face Unlock (docs/FACE-UNLOCK.md): the pipeline on the front camera's frames, the unlock and enrolment
 * rules, and the encrypted template record. Pure C, no libc, no hardware: the kernel (kern_next.c drives it from the
 * camera's timer), the host shell and kern/host/face-test.c build this file as it is.
 *
 * Engine (one per camera session): face_eng_open, then per camera frame face_eng_feed (copies it: the camera's memory
 * is uncached and reused), then face_eng_step(budget) once a loop turn until it says what it found:
 *   FE_DET   a frame's best face (or none): eng.det, the liveness signals (yaw ratio, eye openness);
 *   FE_EMB   the 128-D embedding of the face face_eng_embed was given (eng.emb, unit length).
 * Detection (YuNet, 17 M multiply-adds) runs on every frame fed; an embedding (SFace, 574 M) runs on the frames the
 * caller picks, beside the next detections. Each step runs whole ops until the budget is spent.
 *
 * Policy (pure, no engine): face_scan_* (one unlock attempt: Speed or Security), face_enrol_* (5 poses).
 * Record: face_tpl_seal / face_tpl_open (SHA-256 CTR + HMAC under a 256-bit key K) and face_key_wrap / unwrap (K
 * under a key from the passcode, salted and iterated as passcode.c): after a restart the templates stay sealed until
 * the passcode is typed once. Nothing here logs an embedding, a key or the digits.
 */
#ifndef SBOS_FACE_H
#define SBOS_FACE_H
#include <stdint.h>
#include <stddef.h>
#include "facenet.h"

#define FACE_DIM 128
#define FACE_NTPL 5                     /* the enrolment's poses: centre, left, right, up, down */
#define FACE_FRAME_W 360U               /* the camera's preview for Face Unlock: portrait, not mirrored */
#define FACE_FRAME_H 480U
#define FACE_DET_SCALE 3U
/* face size = eye distance / frame width. The S9's front camera (~80 degrees wide): ~0.12 at 30 cm, ~0.09 at 40 cm */
#define FACE_MIN_SIZE 0.06f             /* smaller: not embedded (the 112 x 112 crop would be mostly upscaled) */
#define FACE_CLOSER_SIZE 0.07f          /* smaller: the "Move a little closer" hint */               /* detection on a 120 x 160 copy (padded to 128 x 160) */

enum { FACE_SPEED = 0, FACE_SECURITY = 1 };

struct face_det {
    float x, y, w, h, score;            /* the frame's pixels */
    float pt[10];                       /* right eye, left eye (the person's), nose, right / left mouth corner: x, y */
};
struct face_sig {                       /* per detected frame: what liveness and enrolment read */
    float yaw;                          /* (nose - eye midpoint) along the eye line / eye distance: 0 straight on */
    float pitch;                        /* the same across the eye line (grows when the head bends down) */
    float eye;                          /* eye openness: the eyes' dark core against the skin below them (0..1) */
    float size;                         /* eye distance / frame width */
    float bright;                       /* the face box's mean grey (0..255) */
};

enum { FE_BUSY = 0, FE_IDLE, FE_DET, FE_EMB };

struct face_eng {
    const uint8_t *pak;
    struct fn_ctx yn, sf;
    const struct fn_plat *plat;
    uint32_t *frame;                    /* the frame being detected (a copy) */
    uint8_t *grey;                      /* its grey (eye signals) */
    unsigned fw, fh;
    int det_busy, emb_busy, det_ready, emb_ready;
    int nfaces;                         /* faces above the threshold in the last detection */
    struct face_det det;
    struct face_sig sig;
    float emb[FACE_DIM];
    uint32_t det_us, emb_us;            /* the last run's time (sum of its steps) */
    uint32_t step_us_max;               /* the longest step */
};

int face_eng_open(struct face_eng *e, const uint8_t *pak, size_t pak_len, const struct fn_plat *plat);
void face_eng_close(struct face_eng *e);
/* a new frame (XRGB, FACE_FRAME_W x FACE_FRAME_H): 0 taken, -1 a detection is still running (dropped) */
int face_eng_feed(struct face_eng *e, const uint32_t *px, unsigned w, unsigned h, unsigned stride);
/* embed the face of the frame just detected (call right after FE_DET, before the next feed): 0, -1 busy */
int face_eng_embed(struct face_eng *e);
/* one loop turn's work: FE_DET / FE_EMB when one finished (call again: the other may be ready too), FE_BUSY when
 * more is to do, FE_IDLE when nothing runs */
int face_eng_step(struct face_eng *e, uint32_t budget_us);

/* ---- pieces (exposed for the host test) ----------------------------------------------------------------------- */
/* YuNet's input from an XRGB frame: a box-filter down-scale by s, B G R floats, padded with 0 to dh x dw */
void face_det_input(float *dst, unsigned dh, unsigned dw, const uint32_t *px, unsigned w, unsigned h, unsigned stride,
                    unsigned s);
/* decode YuNet's outputs (OpenCV's FaceDetectorYN): the best face above thr in the detector's pixels; the count */
int face_det_decode(const struct fn_ctx *yn, float thr, struct face_det *best);
/* the 112 x 112 crop (ArcFace's 5-point template, a similarity fit, bilinear) into SFace's input (R G B, normalised) */
void face_align(float *dst, const uint32_t *px, unsigned w, unsigned h, unsigned stride, const struct face_det *d,
                float mean, float scale);
void face_signals(const struct face_det *d, const uint8_t *grey, unsigned w, unsigned h, struct face_sig *s);
float face_cos(const float *a, const float *b);
float face_sqrtf(float x);
float face_expf(float x);

/* ---- the template record --------------------------------------------------------------------------------------- */
struct face_tpl {
    uint8_t n;                          /* templates kept (<= FACE_NTPL) */
    uint8_t mode;                       /* FACE_SPEED / FACE_SECURITY at enrolment (the pref face.mode rules) */
    uint8_t rsv[2];
    float eye_ref;                      /* the enrolment's mean eye openness */
    int8_t v[FACE_NTPL][FACE_DIM];      /* unit vectors x 127 */
};
#define FACE_KEY 32U
#define FACE_SEAL_LEN (16U + sizeof(struct face_tpl) + 32U)   /* nonce, ciphertext, HMAC */
#define FACE_WRAP_LEN (16U + 4U + FACE_KEY + 32U)             /* salt, rounds, wrapped K, HMAC */
#define FACE_WRAP_ROUNDS 20000U         /* the passcode's own cost (passcode.h PASS_ROUNDS) */
float face_tpl_score(const struct face_tpl *t, const float *emb);      /* the best cosine against the templates */
void face_tpl_put(struct face_tpl *t, int i, const float *emb);
int face_tpl_seal(const struct face_tpl *t, const uint8_t key[FACE_KEY], const uint8_t nonce[16], uint8_t out[FACE_SEAL_LEN]);
int face_tpl_open(struct face_tpl *t, const uint8_t key[FACE_KEY], const uint8_t *in, unsigned n);   /* 0, -1 */
int face_key_wrap(const uint8_t key[FACE_KEY], const char *digits, const uint8_t salt[16], uint8_t out[FACE_WRAP_LEN]);
int face_key_unwrap(uint8_t key[FACE_KEY], const char *digits, const uint8_t *in, unsigned n);       /* 0, -1 */
void face_hmac(const uint8_t *key, unsigned klen, const uint8_t *m, unsigned n, uint8_t out[32]);

/* ---- one unlock attempt --------------------------------------------------------------------------------------- */
#define FACE_THR_SPEED 0.40f            /* SFace cosine; OpenCV's LFW threshold is 0.363 (see docs/FACE-UNLOCK.md) */
#define FACE_THR_SECURITY 0.50f
#define FACE_SPEED_MS 1500U             /* a face seen but no match for this long: a failed scan */
#define FACE_SECURITY_MS 3000U
#define FACE_YAW_TURN 0.10f             /* Security's turn: the yaw ratio's range over the attempt, and */
#define FACE_PARALLAX 0.035f            /* the nose off the plane of the eyes and mouth corners (a photo: 0) */
#define FACE_FAILS_MAX 5                /* failed scans in a row before the passcode is required */
enum { FS_RUN = 0, FS_MATCH, FS_NOMATCH, FS_NOFACE };
enum { FS_HINT_NONE = 0, FS_HINT_TURN, FS_HINT_CLOSER, FS_HINT_LIGHT };
struct face_scan {
    int mode;
    uint64_t t0, t_face;                /* ms: the start, the first face */
    int faces, embeds, matches;
    float best;
    float yaw_min, yaw_max;
    float ref[10];                      /* the first face's points (the parallax's reference) */
    float par_max;                      /* the largest nose parallax / eye distance seen */
    float eye_base, eye_min_after;      /* blink: the open baseline, the lowest since, then back up */
    int blink;
    int result, hint;
};
void face_scan_begin(struct face_scan *s, int mode, uint64_t now_ms);
void face_scan_det(struct face_scan *s, int found, const struct face_det *d, const struct face_sig *g, uint64_t now_ms);
/* the nose's distance from where a plane through the eyes and mouth corners puts it (a homography from ref), over the
 * eye distance: a flat picture gives ~0 however it is turned; a head turning gives ~0.05 per 10 degrees */
float face_parallax(const float ref[10], const float cur[10]);
void face_scan_emb(struct face_scan *s, float score, uint64_t now_ms);                          /* every embedding */
int face_scan_tick(struct face_scan *s, uint64_t now_ms);   /* FS_*: FS_RUN while it goes on */
int face_scan_live(const struct face_scan *s);              /* Security's liveness seen (a turn or a blink) */

/* ---- enrolment --------------------------------------------------------------------------------------------- */
enum { FB_CENTRE = 0, FB_LEFT, FB_RIGHT, FB_UP, FB_DOWN };
struct face_enrol {
    int have;                           /* bit per pose */
    float q[FACE_NTPL];                 /* each pose's kept quality */
    float pitch0, yaw0;                 /* the centre pose's pitch and yaw (the others are relative to them) */
    int centre_n;
    float eye_sum;
    int eye_n;
    struct face_tpl t;
    int want;                           /* the pose whose embedding is being made, or -1 */
    float pend_pitch, pend_yaw, pend_q, pend_eye;   /* that detection's signals */
};
void face_enrol_begin(struct face_enrol *en);
/* a detection: the pose it fills (FB_*) when it is a better one than kept (then embed it), else -1 */
int face_enrol_det(struct face_enrol *en, const struct face_sig *g, float score);
/* its embedding: 1 kept, 0 refused (not the same person as the centre pose: < 0.5) */
int face_enrol_emb(struct face_enrol *en, const float *emb);
int face_enrol_done(const struct face_enrol *en);           /* every pose */
int face_enrol_count(const struct face_enrol *en);

#endif
