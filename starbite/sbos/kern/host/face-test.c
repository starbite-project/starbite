/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of Face Unlock (kern/facenet.c, kern/face.c).
 *   sh starbite/sbos/kern/host/face-test.sh [-v]      (exit status 0 = all passed)
 * Always: the record's crypto (seal / open / tamper / wrong key; wrap / unwrap / wrong digits), templates, the unlock
 * rules (Speed, Security with its liveness, timeouts, hints), enrolment, the liveness geometry (a real head turning
 * against a photo turned), the pack's own checks.
 * With the pack (build/face/sbface.pak) and FACE_REF (kern/host/face-ref.py's file): the runtime against onnxruntime
 * (YuNet's 12 outputs, SFace on OpenCV's crops), the whole pipeline against OpenCV (points, embeddings), the
 * genuine / impostor scores at both thresholds, the time per net (and the A55 estimate), the turn budget. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "face.h"
#include "passcode.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        } else if (verbose) {                                                                                          \
            printf("ok   ");                                                                                           \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static size_t heap_now, heap_peak;
static void *h_alloc(size_t n)
{
    size_t *p = NULL;
    if (posix_memalign((void **)&p, 64, n + 64)) return NULL;
    memset(p, 0, n + 64);
    p[0] = n;
    heap_now += n;
    if (heap_now > heap_peak) heap_peak = heap_now;
    return (uint8_t *)p + 64;
}
static void h_free(void *q)
{
    if (!q) return;
    size_t *p = (size_t *)((uint8_t *)q - 64);
    heap_now -= p[0];
    free(p);
}
static const struct fn_plat PLAT = { h_alloc, h_free, NULL, now_us };

/* ---- crypto ---------------------------------------------------------------------------------------------------- */
static void test_crypto(void)
{
    /* HMAC-SHA256: RFC 4231 test case 2 */
    uint8_t mac[32];
    face_hmac((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, mac);
    static const uint8_t want[32] = { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95,
                                      0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9,
                                      0x64, 0xec, 0x38, 0x43 };
    CHECK(!memcmp(mac, want, 32), "HMAC-SHA256 matches RFC 4231 case 2");

    struct face_tpl t, u;
    memset(&t, 0, sizeof t);
    t.n = FACE_NTPL;
    t.mode = FACE_SECURITY;
    t.eye_ref = 0.3f;
    for (int i = 0; i < FACE_NTPL; i++)
        for (int k = 0; k < FACE_DIM; k++) t.v[i][k] = (int8_t)((i * 37 + k * 11) % 200 - 100);
    uint8_t key[32], key2[32], nonce[16], sealed[FACE_SEAL_LEN];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)(i * 7 + 1); key2[i] = key[i]; }
    key2[5] ^= 1;
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(200 - i);
    face_tpl_seal(&t, key, nonce, sealed);
    CHECK(memmem(sealed, sizeof sealed, t.v[0], 16) == NULL, "the sealed record does not hold a template in clear");
    CHECK(face_tpl_open(&u, key, sealed, sizeof sealed) == 0 && !memcmp(&u, &t, sizeof t), "seal / open round trip");
    CHECK(face_tpl_open(&u, key2, sealed, sizeof sealed) < 0, "a wrong key is refused");
    int tamper_ok = 1;
    for (unsigned b = 0; b < sizeof sealed; b += 37) {
        sealed[b] ^= 0x10;
        if (face_tpl_open(&u, key, sealed, sizeof sealed) == 0) tamper_ok = 0;
        sealed[b] ^= 0x10;
    }
    CHECK(tamper_ok, "any changed byte is refused (HMAC)");
    CHECK(face_tpl_open(&u, key, sealed, sizeof sealed - 1) < 0, "a short record is refused");

    uint8_t salt[16], wrapped[FACE_WRAP_LEN], back[32];
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)(i * 13);
    uint64_t t0 = now_us();
    face_key_wrap(key, "482916", salt, wrapped);
    uint64_t dt = now_us() - t0;
    CHECK(face_key_unwrap(back, "482916", wrapped, sizeof wrapped) == 0 && !memcmp(back, key, 32), "wrap / unwrap with the digits");
    CHECK(face_key_unwrap(back, "482917", wrapped, sizeof wrapped) < 0, "the wrong digits are refused");
    CHECK(face_key_unwrap(back, "4829", wrapped, sizeof wrapped) < 0, "a prefix of the digits is refused");
    CHECK(memmem(wrapped, sizeof wrapped, key, 8) == NULL && memmem(wrapped, sizeof wrapped, "482916", 6) == NULL,
          "neither the key nor the digits are in the wrapped record");
    wrapped[30] ^= 1;
    CHECK(face_key_unwrap(back, "482916", wrapped, sizeof wrapped) < 0, "a changed wrapped key is refused");
    if (verbose) printf("     the key's derivation: %.1f ms on this host (~%.0f ms on the A55: x2.3, passcode-test's ratio)\n", dt / 1000.0, dt / 1000.0 * 2.3);
}

/* ---- templates and the unlock rules ------------------------------------------------------------------------- */
static void unitv(float *v, unsigned seed)
{
    float n = 0;
    for (int k = 0; k < FACE_DIM; k++) {
        seed = seed * 1103515245u + 12345u;
        v[k] = (float)((seed >> 8) & 0xFFFF) / 65536.0f - 0.5f;
        n += v[k] * v[k];
    }
    n = 1.0f / sqrtf(n);
    for (int k = 0; k < FACE_DIM; k++) v[k] *= n;
}
/* b: a unit vector with cosine c to a */
static void at_cos(const float *a, float c, unsigned seed, float *b)
{
    float r[FACE_DIM], d = 0, n = 0;
    unitv(r, seed);
    for (int k = 0; k < FACE_DIM; k++) d += r[k] * a[k];
    for (int k = 0; k < FACE_DIM; k++) { r[k] -= d * a[k]; n += r[k] * r[k]; }
    n = 1.0f / sqrtf(n);
    float s = sqrtf(1 - c * c);
    for (int k = 0; k < FACE_DIM; k++) b[k] = c * a[k] + s * r[k] * n;
}

static void test_tpl(void)
{
    struct face_tpl t;
    memset(&t, 0, sizeof t);
    float a[FACE_DIM], b[FACE_DIM];
    unitv(a, 1);
    face_tpl_put(&t, 0, a);
    t.n = 1;
    CHECK(fabsf(face_tpl_score(&t, a) - 1) < 0.002f, "an int8 template scores itself ~1 (%.4f)", face_tpl_score(&t, a));
    at_cos(a, 0.45f, 9, b);
    CHECK(fabsf(face_tpl_score(&t, b) - 0.45f) < 0.01f, "int8 keeps a cosine of 0.45 within 0.01 (%.4f)", face_tpl_score(&t, b));
}

/* ---- the liveness geometry: a head (3D points) against a photo of it (a plane), both turned ------------------- */
static const float HEAD[5][3] = { { -32, -10, 0 }, { 32, -10, 0 }, { 0, 22, 22 }, { -24, 50, 6 }, { 24, 50, 6 } };
static void project(const float p[5][3], float yaw_deg, int flat, float out[10])
{
    float a = yaw_deg * 3.14159265f / 180, c = cosf(a), s = sinf(a);
    for (int i = 0; i < 5; i++) {
        float x = p[i][0], y = p[i][1], z = flat ? 0 : p[i][2];
        float xr = c * x + s * z, zr = -s * x + c * z;
        float dist = 300 - zr;                          /* mm from the camera, the face 30 cm away */
        float f = 400;                                  /* pixels: the 360-wide frame's focal length (~48 degrees) */
        out[2 * i] = 180 + f * xr / dist;
        out[2 * i + 1] = 240 + f * y / dist;
    }
}
static uint8_t GREY[360 * 480];
static unsigned noise_seed = 1;
static float noise(float amp)
{
    noise_seed = noise_seed * 1103515245u + 12345u;
    return amp * ((float)((noise_seed >> 8) & 0xFFFF) / 32768.0f - 1);
}
/* a detection of the head (flat 0) or of a photo of it (flat 1) turned yaw degrees, points jittered by amp px */
static void mk(float yaw, int flat, float amp, float eye, struct face_det *d, struct face_sig *g)
{
    memset(d, 0, sizeof *d);
    d->w = d->h = 100;
    project(HEAD, yaw, flat, d->pt);
    for (int i = 0; i < 10; i++) d->pt[i] += noise(amp);
    face_signals(d, GREY, 360, 480, g);
    g->eye = eye;
    g->size = 0.2f;
    g->bright = 120;
}
static struct face_sig sig(float yaw, float eye)
{
    struct face_sig g = { yaw, 0.6f, eye, 0.2f, 120 };
    return g;
}

static void test_scan(void)
{
    struct face_scan s;
    struct face_det d;
    struct face_sig g;
    mk(0, 0, 0, 0.3f, &d, &g);
    /* Speed: the first embedding over 0.40 unlocks */
    face_scan_begin(&s, FACE_SPEED, 1000);
    face_scan_det(&s, 1, &d, &g, 1100);
    face_scan_emb(&s, 0.38f, 1200);
    CHECK(face_scan_tick(&s, 1200) == FS_RUN, "Speed: 0.38 is not a match yet");
    face_scan_emb(&s, 0.47f, 1300);
    CHECK(face_scan_tick(&s, 1300) == FS_MATCH, "Speed: 0.47 unlocks");
    /* Speed: someone else: fails after 1.5 s with a face */
    face_scan_begin(&s, FACE_SPEED, 0);
    for (uint64_t t = 100; t < 3000; t += 100) {
        face_scan_det(&s, 1, &d, &g, t);
        face_scan_emb(&s, 0.1f, t);
        if (face_scan_tick(&s, t) != FS_RUN) break;
    }
    CHECK(s.result == FS_NOMATCH && s.t_face == 100, "Speed: another face is a failed scan after 1.5 s");
    /* no face at all: FS_NOFACE (not counted as a failure) */
    face_scan_begin(&s, FACE_SPEED, 0);
    int r = FS_RUN;
    for (uint64_t t = 0; t < 4000 && r == FS_RUN; t += 100) {
        face_scan_det(&s, 0, &d, &g, t);
        r = face_scan_tick(&s, t);
    }
    CHECK(r == FS_NOFACE, "no face: FS_NOFACE");

    /* Security: a still face that matches (a photo held still) never unlocks */
    face_scan_begin(&s, FACE_SECURITY, 0);
    r = FS_RUN;
    for (uint64_t t = 33; t < 5000 && r == FS_RUN; t += 33) {
        struct face_sig h;
        mk(0, 1, 0.7f, 0.3f, &d, &h);
        face_scan_det(&s, 1, &d, &h, t);
        if (t % 132 == 0) face_scan_emb(&s, 0.7f, t);
        r = face_scan_tick(&s, t);
        if (t == 1518) CHECK(s.hint == FS_HINT_TURN, "Security: a still match asks for a slight turn");
    }
    CHECK(r == FS_NOMATCH, "Security: matching but no turn or blink: no unlock");
    /* Security: a photo of the person turned +-30 degrees (the yaw ratio moves a little, the nose stays on the plane) */
    face_scan_begin(&s, FACE_SECURITY, 0);
    r = FS_RUN;
    for (uint64_t t = 33; t < 5000 && r == FS_RUN; t += 33) {
        struct face_sig h;
        mk(30.0f * sinf((float)t / 300.0f), 1, 0.7f, 0.3f, &d, &h);
        face_scan_det(&s, 1, &d, &h, t);
        if (t % 132 == 0) face_scan_emb(&s, 0.7f, t);
        r = face_scan_tick(&s, t);
    }
    CHECK(r == FS_NOMATCH, "Security: a matching photo turned +-30 degrees does not unlock (parallax max %.3f)", s.par_max);
    /* Security: matching + a real head turning ~10 degrees */
    face_scan_begin(&s, FACE_SECURITY, 0);
    r = FS_RUN;
    uint64_t when = 0;
    for (uint64_t t = 33; t < 5000 && r == FS_RUN; t += 33) {
        float yaw = t < 600 ? 0 : t < 900 ? 5.0f : 10.0f;
        struct face_sig h;
        mk(yaw, 0, 0.7f, 0.3f, &d, &h);
        face_scan_det(&s, 1, &d, &h, t);
        if (t % 132 == 0) face_scan_emb(&s, 0.62f, t);
        r = face_scan_tick(&s, t);
        when = t;
    }
    CHECK(r == FS_MATCH && when < 1200, "Security: a match plus a slight head turn unlocks (at %llu ms)", (unsigned long long)when);
    /* Security: matching + a blink */
    face_scan_begin(&s, FACE_SECURITY, 0);
    r = FS_RUN;
    for (uint64_t t = 33; t < 5000 && r == FS_RUN; t += 33) {
        float eye = (t >= 400 && t < 520) ? 0.08f : 0.30f;
        struct face_sig h;
        mk(0, 0, 0.7f, eye, &d, &h);
        face_scan_det(&s, 1, &d, &h, t);
        if (t % 132 == 0) face_scan_emb(&s, 0.62f, t);
        r = face_scan_tick(&s, t);
        when = t;
    }
    CHECK(r == FS_MATCH && s.blink, "Security: a match plus a blink unlocks (at %llu ms)", (unsigned long long)when);
    /* Security: a head turn by someone else (0.45 < 0.50) does not unlock */
    face_scan_begin(&s, FACE_SECURITY, 0);
    r = FS_RUN;
    for (uint64_t t = 33; t < 6000 && r == FS_RUN; t += 33) {
        struct face_sig h;
        mk(t < 600 ? 0 : 15.0f, 0, 0.7f, 0.3f, &d, &h);
        face_scan_det(&s, 1, &d, &h, t);
        if (t % 132 == 0) face_scan_emb(&s, 0.45f, t);
        r = face_scan_tick(&s, t);
    }
    CHECK(r == FS_NOMATCH, "Security: 0.45 (a Speed match) is not enough in Security");
    /* hints */
    face_scan_begin(&s, FACE_SPEED, 0);
    struct face_sig far = g;
    far.size = 0.05f;
    face_scan_det(&s, 1, &d, &far, 10);
    CHECK(s.hint == FS_HINT_CLOSER, "a small face asks to come closer");
    struct face_sig dark = g;
    dark.bright = 20;
    face_scan_det(&s, 1, &d, &dark, 20);
    CHECK(s.hint == FS_HINT_LIGHT, "a dark face asks for light");
}

static void test_geometry(void)
{
    float lo = 9, hi = -9, plo = 9, phi = -9, par_head = 0, par_photo = 0, par_noise = 0;
    struct face_det d0, f0, d;
    struct face_sig g;
    mk(0, 0, 0, 0.3f, &d0, &g);
    mk(0, 1, 0, 0.3f, &f0, &g);                          /* the photo's own reference */
    for (float yaw = -12; yaw <= 12.01f; yaw += 3) {
        mk(yaw, 0, 0, 0.3f, &d, &g);
        if (g.yaw < lo) lo = g.yaw;
        if (g.yaw > hi) hi = g.yaw;
        float p = face_parallax(d0.pt, d.pt);
        if (p > par_head) par_head = p;
        mk(yaw * 3, 1, 0, 0.3f, &d, &g);                 /* the photo turned three times as far */
        if (g.yaw < plo) plo = g.yaw;
        if (g.yaw > phi) phi = g.yaw;
        p = face_parallax(f0.pt, d.pt);
        if (p > par_photo) par_photo = p;
    }
    struct face_det n0;
    mk(0, 1, 0.7f, 0.3f, &n0, &g);
    for (int i = 0; i < 90; i++) {                       /* 3 s of a still photo, the points jittered +-0.7 px */
        mk(0, 1, 0.7f, 0.3f, &d, &g);
        float p = face_parallax(n0.pt, d.pt);
        if (p > par_noise) par_noise = p;
    }
    CHECK(hi - lo >= FACE_YAW_TURN, "a real head turned +-12 degrees: yaw ratio range %.3f >= %.2f", hi - lo, FACE_YAW_TURN);
    CHECK(par_head >= FACE_PARALLAX, "a real head turned up to 12 degrees: nose parallax %.3f >= %.3f", par_head, FACE_PARALLAX);
    CHECK(par_photo < 0.005f, "a photo turned up to 36 degrees: parallax %.4f (~0); yaw range %.3f", par_photo, phi - plo);
    CHECK(par_noise < FACE_PARALLAX, "a still photo with +-0.7 px point noise: parallax max %.3f < %.3f", par_noise, FACE_PARALLAX);
}

/* ---- enrolment ------------------------------------------------------------------------------------------------ */
static void test_enrol(void)
{
    struct face_enrol en;
    face_enrol_begin(&en);
    float me[FACE_DIM], v[FACE_DIM], other[FACE_DIM];
    unitv(me, 42);
    struct face_sig g = sig(0.15f, 0.3f);
    CHECK(face_enrol_det(&en, &g, 0.9f) < 0, "the first pose must be the centre one");
    g = sig(0.01f, 0.3f);
    g.size = 0.05f;
    CHECK(face_enrol_det(&en, &g, 0.9f) < 0, "a face too far away is not taken");
    g.size = 0.2f;
    CHECK(face_enrol_det(&en, &g, 0.9f) == FB_CENTRE, "a straight face fills the centre");
    CHECK(face_enrol_det(&en, &g, 0.95f) < 0, "one embedding at a time");
    /* round 2: the centre wants 3 embeddings (averaged), the others 2; each a noisy look at the same face */
    at_cos(me, 0.75f, 900, v);
    CHECK(face_enrol_emb(&en, v) == 1 && face_enrol_count(&en) == 0 && face_enrol_seen(&en) == 1, "the centre's first look is kept (not full yet)");
    for (int k = 1; k < FACE_ENROL_CENTRE_N; k++) {
        CHECK(face_enrol_det(&en, &g, 0.9f) == FB_CENTRE, "the centre wants look %d", k + 1);
        at_cos(me, 0.75f, 900 + (unsigned)k, v);
        CHECK(face_enrol_emb(&en, v) == 1, "the centre's look %d kept", k + 1);
    }
    CHECK(face_enrol_count(&en) == 1 && face_enrol_det(&en, &g, 0.9f) < 0, "the centre is full after %d looks", FACE_ENROL_CENTRE_N);
    {   /* the mean of three looks at 0.75 is nearer the face than any one look */
        float c[FACE_DIM];
        for (int k = 0; k < FACE_DIM; k++) c[k] = (float)en.t.v[FB_CENTRE][k];
        CHECK(face_cos(c, me) > 0.85f, "the averaged centre is at %.3f to the face (each look 0.75)", face_cos(c, me));
    }
    struct { float yaw, pitch; int pose; } P[] = { { -0.2f, 0.6f, FB_LEFT }, { 0.2f, 0.6f, FB_RIGHT },
                                                   { 0.0f, 0.45f, FB_UP }, { 0.0f, 0.75f, FB_DOWN } };
    for (int i = 0; i < 4; i++) {
        struct face_sig h = sig(P[i].yaw, 0.3f);
        h.pitch = P[i].pitch;
        for (int k = 0; k < FACE_ENROL_POSE_N; k++) {
            int p = face_enrol_det(&en, &h, 0.9f);
            CHECK(p == P[i].pose, "pose %d is recognised (%d)", P[i].pose, p);
            if (i == 1 && k == 0) {
                unitv(other, 77);
                CHECK(face_enrol_emb(&en, other) == 0, "another person's face is refused mid-enrolment");
                p = face_enrol_det(&en, &h, 0.9f);
            }
            at_cos(me, 0.72f, 100 + (unsigned)(i * 10 + k), v);
            CHECK(face_enrol_emb(&en, v) == 1, "pose %d look %d kept", p, k + 1);
        }
    }
    CHECK(face_enrol_done(&en), "all five poses: done");
    at_cos(me, 0.75f, 555, v);                     /* a new look as good as the set-up's */
    CHECK(face_tpl_score(&en.t, v) >= 0.6f, "a new look matches the record (%.3f >= 0.6)", face_tpl_score(&en.t, v));
}

/* ---- the models (needs the pack and the reference file) ----------------------------------------------------- */
static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = NULL;
    if (posix_memalign((void **)&p, 64, (size_t)n) || fread(p, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}
static float sigmoidf(float x) { return 1 / (1 + expf(-x)); }

static void test_models(const char *pakpath, const char *refpath)
{
    size_t plen = 0, rlen = 0;
    uint8_t *pak = load(pakpath, &plen);
    if (!pak) { printf("face-test: no pack at %s: the model checks are SKIPPED\n", pakpath); return; }
    uint64_t t0 = now_us();
    CHECK(fn_pack_check(pak, plen, 1) == 0, "the pack's header and SHA-256 (%.1f ms)", (now_us() - t0) / 1000.0);
    pak[70000] ^= 1;
    CHECK(fn_pack_check(pak, plen, 1) == -3, "a changed byte in the pack is caught");
    pak[70000] ^= 1;
    uint8_t *ref = refpath ? load(refpath, &rlen) : NULL;
    if (!ref || rlen < 8 || memcmp(ref, "FREF", 4)) {
        printf("face-test: no FACE_REF file: the runtime / pipeline comparisons are SKIPPED\n");
        free(pak);
        return;
    }
    uint32_t nrec;
    memcpy(&nrec, ref + 4, 4);
    const uint8_t *p = ref + 8;
    struct face_eng e;
    CHECK(face_eng_open(&e, pak, plen, &PLAT) == 0, "the engine opens (%zu KiB in use)", heap_now >> 10);
    if (verbose) printf("     YuNet %u x %u: %.1f M multiply-adds; SFace: %.1f M\n", e.yn.w, e.yn.h, e.yn.macs / 1e6, e.sf.macs / 1e6);
    struct fn_ctx sfx;
    CHECK(fn_open(&sfx, pak, FN_NET_SFACE, 112, 112, &PLAT) == 0, "a second SFace context (for OpenCV's crops)");
    float (*embs)[FACE_DIM] = calloc(nrec, sizeof *embs);
    uint32_t *pid = calloc(nrec, 4);
    int *ok = calloc(nrec, sizeof(int));
    double yerr = 0, ptmax = 0, ptsum = 0, cos_rt_min = 1, cos_pipe_min = 1, cos_pipe_sum = 0;
    int ncmp = 0, nfound = 0, nref = 0;
    uint64_t det_us = 0, emb_us = 0, step_max = 0;
    int ndet = 0, nemb = 0;
    unsigned counts[12];
    {   /* the outputs' element counts (YuNet at 128 x 160 below the frame's 1/3) */
        unsigned st[3] = { 8, 16, 32 }, ch[4] = { 1, 1, 4, 10 };
        for (int k = 0; k < 12; k++) counts[k] = (e.yn.h / st[k % 3]) * (e.yn.w / st[k % 3]) * ch[k / 3];
    }
    for (uint32_t r = 0; r < nrec; r++) {
        memcpy(&pid[r], p, 4);
        p += 8;
        const float *yin = (const float *)p;
        p += (size_t)e.yn.h * e.yn.w * 3 * 4;
        const float *yo[12];
        for (int k = 0; k < 12; k++) { yo[k] = (const float *)p; p += counts[k] * 4; }
        const uint32_t *frame = (const uint32_t *)p;
        p += FACE_FRAME_W * FACE_FRAME_H * 4;
        const float *cvbest = (const float *)p;
        p += 15 * 4;
        const uint8_t *crop = p;
        p += 112 * 112 * 3;
        const float *remb = (const float *)p;
        p += 128 * 4;

        /* 1. YuNet: the runtime against onnxruntime on the same input */
        float *in = fn_input(&e.yn);
        for (size_t i = 0; i < (size_t)e.yn.h * e.yn.w; i++) {
            in[4 * i] = yin[3 * i]; in[4 * i + 1] = yin[3 * i + 1]; in[4 * i + 2] = yin[3 * i + 2]; in[4 * i + 3] = 0;
        }
        fn_restart(&e.yn);
        fn_run(&e.yn, 0);
        for (int k = 0; k < 12; k++) {
            unsigned cp;
            const float *o = fn_output(&e.yn, k, NULL, NULL, &cp);
            unsigned ch = k < 6 ? 1 : k < 9 ? 4 : 10, n = counts[k] / ch;
            for (unsigned i = 0; i < n; i++)
                for (unsigned c = 0; c < ch; c++) {
                    float v = o[i * cp + c], want = yo[k][i * ch + c];
                    if (k < 6) v = sigmoidf(v);
                    double d = fabs(v - want);
                    if (d > yerr) yerr = d;
                }
        }
        /* 2. SFace: the runtime on OpenCV's crop against onnxruntime */
        if (cvbest[14] > 0) {
            nref++;
            float *si = fn_input(&sfx);
            for (int i = 0; i < 112 * 112; i++) {      /* the crop is B G R; SFace wants R G B */
                si[4 * i] = (crop[3 * i + 2] - 127.5f) * 0.0078125f;
                si[4 * i + 1] = (crop[3 * i + 1] - 127.5f) * 0.0078125f;
                si[4 * i + 2] = (crop[3 * i] - 127.5f) * 0.0078125f;
                si[4 * i + 3] = 0;
            }
            fn_restart(&sfx);
            fn_run(&sfx, 0);
            float c = face_cos(fn_output(&sfx, 0, NULL, NULL, NULL), remb);
            if (c < cos_rt_min) cos_rt_min = c;
        }
        /* 3. the whole pipeline on the frame, in 20 ms steps, against OpenCV's detection and embedding */
        face_eng_feed(&e, frame, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W);
        int ev;
        uint64_t a = now_us();
        while ((ev = face_eng_step(&e, 20000)) == FE_BUSY) {}
        det_us += now_us() - a;
        ndet++;
        if (ev == FE_DET && e.nfaces) {
            nfound++;
            if (cvbest[14] > 0) {
                double m = 0;
                for (int i = 0; i < 10; i++) {
                    double d = fabs(e.det.pt[i] - cvbest[4 + i]);
                    if (d > m) m = d;
                }
                ptsum += m;
                if (m > ptmax) ptmax = m;
                ncmp++;
            }
            face_eng_embed(&e);
            a = now_us();
            while ((ev = face_eng_step(&e, 20000)) == FE_BUSY) {}
            emb_us += now_us() - a;
            nemb++;
            if (ev == FE_EMB) {
                memcpy(embs[r], e.emb, sizeof e.emb);
                ok[r] = 1;
                if (cvbest[14] > 0) {
                    float c = face_cos(e.emb, remb);
                    cos_pipe_sum += c;
                    if (c < cos_pipe_min) cos_pipe_min = c;
                }
            }
        }
        if (e.step_us_max > step_max) step_max = e.step_us_max;
    }
    CHECK(yerr < 2e-3, "YuNet: the runtime against onnxruntime: max abs difference %.2g over %u inputs", yerr, nrec);
    CHECK(nref > 0 && cos_rt_min > 0.995, "SFace (int8 weights): cosine to onnxruntime's fp32 >= 0.995 on OpenCV's crops (min %.5f, %d faces)", cos_rt_min, nref);
    CHECK(nfound >= (int)nrec * 9 / 10, "the pipeline finds a face in %d of %u frames", nfound, nrec);
    CHECK(ncmp > 0 && ptsum / ncmp < 1.0 && ptmax < 3.0, "the 5 points against OpenCV's FaceDetectorYN: mean worst %.2f px, max %.2f px", ncmp ? ptsum / ncmp : 0, ptmax);
    CHECK(ncmp > 0 && cos_pipe_min > 0.97, "the pipeline's embedding against OpenCV + onnxruntime: mean %.4f, min %.4f", ncmp ? cos_pipe_sum / ncmp : 0, cos_pipe_min);

    /* 4. genuine (same person) and impostor scores on the pipeline's own embeddings */
    int g_n = 0, g_lo40 = 0, g_lo50 = 0, i_n = 0, i_hi40 = 0, i_hi50 = 0;
    float i_max = -1, g_min = 2;
    for (uint32_t a2 = 0; a2 < nrec; a2++)
        for (uint32_t b2 = a2 + 1; b2 < nrec; b2++) {
            if (!ok[a2] || !ok[b2]) continue;
            float c = face_cos(embs[a2], embs[b2]);
            if (pid[a2] == pid[b2]) { g_n++; g_lo40 += c < FACE_THR_SPEED; g_lo50 += c < FACE_THR_SECURITY; if (c < g_min) g_min = c; }
            else { i_n++; i_hi40 += c >= FACE_THR_SPEED; i_hi50 += c >= FACE_THR_SECURITY; if (c > i_max) i_max = c; }
        }
    printf("face-test: LFW pairs: genuine %d (under 0.40: %d, under 0.50: %d, lowest %.3f), impostor %d (0.40 or more: %d, 0.50 or more: %d, highest %.3f)\n",
           g_n, g_lo40, g_lo50, g_min, i_n, i_hi40, i_hi50, i_max);
    CHECK(i_n > 0 && i_hi50 == 0, "no impostor pair reaches Security's 0.50");
    CHECK(i_n > 0 && i_hi40 * 200 <= i_n, "impostors at Speed's 0.40: %d of %d (<= 0.5 %%)", i_hi40, i_n);

    /* 5. time and memory */
    double yd = ndet ? det_us / 1000.0 / ndet : 0, se = nemb ? emb_us / 1000.0 / nemb : 0;
    printf("face-test: host: detection %.2f ms, embedding %.2f ms (one core); the longest step %.2f ms; heap peak %.1f MiB\n",
           yd, se, step_max / 1000.0, heap_peak / 1048576.0);
    CHECK(heap_peak < 24u << 20, "the engine's memory: peak %.1f MiB (< 24)", heap_peak / 1048576.0);
    face_eng_close(&e);
    fn_close(&sfx);
    CHECK(heap_now == 0, "everything given back (%zu bytes left)", heap_now);
    free(embs); free(pid); free(ok); free(ref); free(pak);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) verbose = 1;
    test_crypto();
    test_tpl();
    test_scan();
    test_geometry();
    test_enrol();
    const char *pk = getenv("FACE_PACK");
    test_models(pk ? pk : "build/face/sbface.pak", getenv("FACE_REF"));
    printf("face-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
