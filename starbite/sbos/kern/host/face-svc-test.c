/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of Face Unlock's service (ui/face_svc.c: shell.h's sh_face_*) with recorded frames: the camera is a fake
 * fed with kern/host/face-ref.py's frames (LFW faces in a 360 x 480 portrait frame): real head poses are photos of
 * the same person turned (FACE_REF_MANY=George_W_Bush,Colin_Powell,Tony_Blair face-ref.py ... 50).
 *   FACE_PACK=build/face/sbface.pak FACE_REF=fref.bin sh starbite/sbos/kern/host/face-svc-test.sh [-v]
 * The flow: no passcode -> set-up refused; a passcode -> the key; set-up (Speed) through five poses; unlock with
 * another photo of the same person; someone else fails, 5 times -> the passcode; a restart -> sealed until the
 * passcode; a passcode change keeps the face; Security wants a turn (a still picture never unlocks); removing the
 * passcode forgets everything. And: the longest turn, the memory, nothing in the clear in the store. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shell.h"
#include "face.h"
#include "face_svc.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); }       \
        else if (verbose) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); }                                    \
    } while (0)

/* ---- the fake platform -------------------------------------------------------------------------------------------- */
static uint64_t real_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static uint64_t fake_ms = 1000;
static uint64_t now_ms(void) { return fake_ms; }
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
static struct { char n[20]; uint8_t p[1024]; int len; } BL[8];
static int blob_get(const char *n, void *p, unsigned cap)
{
    for (int i = 0; i < 8; i++)
        if (BL[i].len > 0 && !strcmp(BL[i].n, n)) { int k = BL[i].len < (int)cap ? BL[i].len : (int)cap; memcpy(p, BL[i].p, (size_t)k); return k; }
    return -1;
}
static int blob_put(const char *n, const void *p, unsigned len)
{
    for (int i = 0; i < 8; i++)
        if (BL[i].len > 0 && !strcmp(BL[i].n, n)) { BL[i].len = 0; break; }
    if (!len) return 0;
    for (int i = 0; i < 8; i++)
        if (BL[i].len <= 0) { snprintf(BL[i].n, sizeof BL[i].n, "%s", n); memcpy(BL[i].p, p, len); BL[i].len = (int)len; return 0; }
    return -1;
}
static struct { char k[20]; int v; } PR[8];
static int npr;
static int pref_int(const char *k, int def) { for (int i = 0; i < npr; i++) if (!strcmp(PR[i].k, k)) return PR[i].v; return def; }
static void pref_set_int(const char *k, int v)
{
    for (int i = 0; i < npr; i++) if (!strcmp(PR[i].k, k)) { PR[i].v = v; return; }
    snprintf(PR[npr].k, sizeof PR[npr].k, "%s", k);
    PR[npr++].v = v;
}
static unsigned rnd = 12345;
static void rnd_bytes(uint8_t *p, unsigned n) { for (unsigned i = 0; i < n; i++) { rnd = rnd * 1103515245u + 12345u; p[i] = (uint8_t)(rnd >> 16); } }
static int pass_is_on;
static int pass_on(void) { return pass_is_on; }
static int pass_wait(void) { return 0; }
static char lastlog[256];
static void logf_(const char *l) { snprintf(lastlog, sizeof lastlog, "%s", l); if (verbose) printf("     log: %s\n", l); }

/* the camera: a scene function gives the frame for the fake time since the camera opened */
static int cam_is_on;
static uint64_t cam_t0;
static uint32_t frame[FACE_FRAME_W * FACE_FRAME_H];
static const uint32_t *(*scene)(uint64_t t_ms, unsigned *seq);
static int cam_open(unsigned w, unsigned h) { if (w != FACE_FRAME_W || h != FACE_FRAME_H || cam_is_on) return -1; cam_is_on = 1; cam_t0 = fake_ms; return 0; }
static const uint32_t *cam_frame(unsigned *seq)
{
    if (!cam_is_on) return 0;
    uint64_t e = fake_ms - cam_t0;
    if (e < 100) return 0;                          /* the sensor's start (~100 ms, to be measured on the phone) */
    return scene(e - 100, seq);
}
static void cam_close(void) { cam_is_on = 0; }

static struct fsvc_plat PL;
static uint8_t *pak;
static size_t pak_len;
static int pk_in, pk_gets, pk_frees;              /* the pack "in memory" (the phone: the heap), loads, frees */
static int pk_get(const uint8_t **p, size_t *n) { if (!pk_in) { pk_in = 1; pk_gets++; } *p = pak; *n = pak_len; return 1; }
static int pk_state(void) { return pk_in ? 2 : 0; }
static void pk_release(void) { if (pk_in) pk_frees++; pk_in = 0; }
static void restart(void)                           /* a restart: RAM gone, the store kept */
{
    memset(&PL, 0, sizeof PL);
    PL.pack_get = pk_get; PL.pack_state = pk_state; PL.pack_release = pk_release;
    pk_in = 0;
    PL.fn.alloc = h_alloc; PL.fn.free = h_free; PL.fn.now_us = real_us;
    PL.now_ms = now_ms;
    PL.cam_open = cam_open; PL.cam_frame = cam_frame; PL.cam_close = cam_close;
    PL.blob_get = blob_get; PL.blob_put = blob_put; PL.pref_int = pref_int; PL.pref_set_int = pref_set_int;
    PL.random = rnd_bytes; PL.pass_on = pass_on; PL.pass_wait = pass_wait; PL.log = logf_;
    fsvc_init(&PL);
}
/* run the service's turns while it is busy (the fake clock: 10 ms a turn); the longest turn in real time */
static uint64_t turn_max_us;
static void run_ms(uint64_t ms)
{
    uint64_t end = fake_ms + ms;
    while (fake_ms < end) {
        if (fsvc_busy()) {
            uint64_t a = real_us();
            fsvc_tick();
            uint64_t d = real_us() - a;
            if (d > turn_max_us) turn_max_us = d;
        }
        fake_ms += 10;
    }
}

/* ---- the recorded frames ---------------------------------------------------------------------------------------- */
struct rec { uint32_t pid, k; const uint32_t *frame; const float *cv; };
static struct rec R[200];
static int nr;
static const struct rec *cur;                       /* the scene's person */
static const struct rec *PP[5];                    /* the set-up's photo for each pose (real head poses) */
static const struct rec *TURN[2];                   /* the Security turn: two photos of the head turned apart */
static int pose;                                    /* 0 the person's photo; 5 the set-up's poses in turn; -1 turning */
static const uint32_t *scene_person(uint64_t t, unsigned *seq)
{
    *seq = (unsigned)(t / 33) + 1;
    if (pose == 5) return PP[(t / 1200) % 5]->frame;
    if (pose < 0) return TURN[(t / 400) % 2]->frame;
    return cur->frame;
}
static float dim = 1;                               /* the scene's brightness (the 9i dark-room case) */
static const uint32_t *scene_dim(uint64_t t, unsigned *seq)
{
    const uint32_t *f = scene_person(t, seq);
    static uint32_t d[FACE_FRAME_W * FACE_FRAME_H];
    for (size_t i = 0; i < (size_t)FACE_FRAME_W * FACE_FRAME_H; i++) {
        uint32_t v = f[i];
        d[i] = 0xFF000000u | (uint32_t)((v >> 16 & 255) * dim) << 16 | (uint32_t)((v >> 8 & 255) * dim) << 8 | (uint32_t)((v & 255) * dim);
    }
    return d;
}
static const uint32_t *scene_empty(uint64_t t, unsigned *seq)
{
    *seq = (unsigned)(t / 33) + 1;
    for (int i = 0; i < (int)(FACE_FRAME_W * FACE_FRAME_H); i++) frame[i] = 0xFF505860u;
    return frame;
}
static int contains(const void *hay, size_t n, const void *needle, size_t m)
{
    for (size_t i = 0; i + m <= n; i++) if (!memcmp((const uint8_t *)hay + i, needle, m)) return 1;
    return 0;
}

/* a scan from wake: the result and the fake ms it took */
static int scan(const struct rec *who, int p, uint64_t *took)
{
    cur = who;
    pose = p;
    scene = who ? scene_person : scene_empty;
    uint64_t t0 = fake_ms;
    if (sh_face_scan(1) < 0) return -1;
    int r = SH_FS_RUN;
    while ((r = sh_face_poll(0)) == SH_FS_RUN && fake_ms - t0 < 10000) run_ms(10);
    if (took) *took = fake_ms - t0;
    return r;
}

static int sig_of(const uint32_t *fr, struct face_sig *g, float *emb)
{
    struct face_eng e;
    if (face_eng_open(&e, pak, pak_len, &PL.fn) < 0) return 0;
    face_eng_feed(&e, fr, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W);
    while (face_eng_step(&e, 0) == FE_BUSY) {}
    int n = e.nfaces;
    *g = e.sig;
    if (n && emb) {
        face_eng_embed(&e);
        while (face_eng_step(&e, 0) != FE_EMB) {}
        memcpy(emb, e.emb, sizeof e.emb);
    }
    face_eng_close(&e);
    return n;
}
/* person pid's photos: a straight one as the centre, and for each other pose a photo face.c's set-up sees as that pose
 * (relative to the centre) and as the same person (cosine >= 0.55); a second straight photo for the unlock */
static int pick_poses(uint32_t pid, const struct rec **second)
{
    static struct face_sig sg[200];
    static float em[200][FACE_DIM];
    static int ok[200];
    for (int i = 0; i < nr; i++) ok[i] = R[i].pid == pid && sig_of(R[i].frame, &sg[i], em[i]) && sg[i].size >= FACE_CLOSER_SIZE;
    for (int c = 0; c < nr; c++) {
        if (!ok[c] || sg[c].yaw <= -0.05f || sg[c].yaw >= 0.05f) continue;
        PP[0] = &R[c];
        int got = 1;
        for (int q = 1; q < 5; q++) {
            PP[q] = 0;
            for (int i = 0; i < nr && !PP[q]; i++) {
                if (!ok[i] || i == c || face_cos(em[c], em[i]) < 0.55f) continue;
                float dy = sg[i].yaw - sg[c].yaw, dp = sg[i].pitch - sg[c].pitch;
                int fit = q == FB_LEFT ? dy < -0.14f : q == FB_RIGHT ? dy > 0.14f
                        : fabsf(dy) < 0.06f && (q == FB_UP ? dp < -0.09f : dp > 0.09f);
                if (fit) PP[q] = &R[i];
            }
            got += PP[q] != 0;
        }
        if (got < 5) continue;
        *second = 0;
        for (int i = 0; i < nr && !*second; i++)
            if (ok[i] && i != c && fabsf(sg[i].yaw) < 0.08f && PP[1] != &R[i] && PP[2] != &R[i] && PP[3] != &R[i] && PP[4] != &R[i])
                *second = &R[i];
        TURN[0] = PP[1];
        TURN[1] = PP[2];
        if (*second) {
            if (verbose)
                printf("     A = LFW person %u: centre photo %u (yaw %.2f); left %u (%+.2f) right %u (%+.2f) up %u (%+.2f) down %u (%+.2f)\n",
                       pid, PP[0]->k, sg[c].yaw, PP[1]->k, sg[PP[1] - R].yaw - sg[c].yaw, PP[2]->k, sg[PP[2] - R].yaw - sg[c].yaw,
                       PP[3]->k, sg[PP[3] - R].pitch - sg[c].pitch, PP[4]->k, sg[PP[4] - R].pitch - sg[c].pitch);
            return 1;
        }
    }
    return 0;
}
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    const char *pk = getenv("FACE_PACK"), *rf = getenv("FACE_REF");
    FILE *f = pk ? fopen(pk, "rb") : NULL;
    if (!f || !rf) { printf("face-svc-test: needs FACE_PACK and FACE_REF: SKIPPED\n"); return 0; }
    fseek(f, 0, SEEK_END); pak_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    if (posix_memalign((void **)&pak, 64, pak_len) || fread(pak, 1, pak_len, f) != pak_len) return 2;
    fclose(f);
    f = fopen(rf, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); size_t rl = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *ref = malloc(rl);
    if (!ref || fread(ref, 1, rl, f) != rl) return 2;
    fclose(f);
    uint32_t n;
    memcpy(&n, ref + 4, 4);
    const uint8_t *p = ref + 8;
    size_t yin = 160 * 128 * 3 * 4, yout = (20 * 16 + 10 * 8 + 5 * 4) * (1 + 1 + 4 + 10) * 4;
    for (uint32_t i = 0; i < n && nr < 200; i++) {
        memcpy(&R[nr].pid, p, 4); memcpy(&R[nr].k, p + 4, 4);
        p += 8 + yin + yout;
        R[nr].frame = (const uint32_t *)p;
        p += FACE_FRAME_W * FACE_FRAME_H * 4;
        R[nr].cv = (const float *)p;
        p += 15 * 4 + 112 * 112 * 3 + 128 * 4;
        if (R[nr].cv[14] > 0) nr++;
    }
    /* A: a person whose photos give every pose (FACE_REF from face-ref.py with FACE_REF_MANY); B: another person */
    restart();
    const struct rec *A0 = 0, *A1 = 0, *B0 = 0;
    for (int i = 0; i < nr && !A0; i++)
        if ((i == 0 || R[i].pid != R[i - 1].pid) && pick_poses(R[i].pid, &A1)) A0 = PP[0];
    for (int i = 0; i < nr && A0 && !B0; i++)
        if (R[i].pid != A0->pid && R[i].cv[14] > 0) B0 = &R[i];
    if (!A0 || !B0) { printf("face-svc-test: no person in FACE_REF gives all five poses (make it with FACE_REF_MANY)\n"); return 2; }

    restart();
    CHECK(sh_face_status() & SH_FACE_AVAIL, "the pack is there: available");
    CHECK(sh_face_enrol(SH_FACE_SPEED) < 0 && !strcmp(sh_face_why(), "Set a passcode first"), "no passcode: set-up refused (%s)", sh_face_why());
    pass_is_on = 1;
    fsvc_passcode_set("", "246810");
    run_ms(50);
    uint8_t probe[200];
    CHECK(blob_get("face.k", probe, sizeof probe) == (int)FACE_WRAP_LEN, "a passcode: the wrapped key is kept");

    /* set-up (Speed) with person A turning through the poses */
    cur = A0; pose = 5; scene = scene_person;          /* the set-up: the five poses' photos, 1.2 s each */
    CHECK(sh_face_enrol(SH_FACE_SPEED) == 0, "set-up starts");
    int r = 0, poses = 0;
    uint64_t t0 = fake_ms;
    int seen = 0;
    while ((r = sh_face_enrol_poll(&poses, 0)) == 0 && fake_ms - t0 < 50000) { seen |= poses; run_ms(10); }
    if (r != 1) printf("     the poses seen: %d\n", seen);
    CHECK(r == 1, "set-up done through five poses in %llu ms (poses %d; %s)", (unsigned long long)(fake_ms - t0), poses, lastlog);
    CHECK(!cam_is_on, "the camera is off after the set-up");
    uint8_t sealed[1024];
    int sl = blob_get("face.t", sealed, sizeof sealed);
    CHECK(sl == (int)FACE_SEAL_LEN, "the sealed templates are kept (%d bytes)", sl);
    CHECK(sh_face_status() & SH_FACE_READY, "ready right after the set-up");

    uint64_t took = 0;
    r = scan(A1, 0, &took);
    CHECK(r == SH_FS_MATCH, "Speed: another photo of A unlocks (%llu ms of fake time; %s)", (unsigned long long)took, lastlog);
    r = scan(A0, 0, &took);
    CHECK(r == SH_FS_MATCH, "Speed: A's set-up photo unlocks (%llu ms)", (unsigned long long)took);
    {   /* 9i's dark room: the face's mean grey ~15 (scans wanted >= 20, set-up nothing): it must still unlock */
        dim = 0.12f;
        cur = A1; pose = 0; scene = scene_dim;
        uint64_t t0d = fake_ms;
        int rr = sh_face_scan(1) < 0 ? -1 : SH_FS_RUN;
        while (rr == SH_FS_RUN && (rr = sh_face_poll(0)) == SH_FS_RUN && fake_ms - t0d < 10000) run_ms(10);
        CHECK(rr == SH_FS_MATCH, "a dim face (9i's dark room, x0.12) still unlocks (%s)", lastlog);
        dim = 1;
    }
    r = scan(0, 0, &took);
    CHECK(r == SH_FS_NOFACE, "no face: 'no face', not a failure (%llu ms)", (unsigned long long)took);
    for (int i = 0; i < FACE_FAILS_MAX; i++) {
        r = scan(B0, 0, &took);
        CHECK(r == SH_FS_FAIL, "someone else (B): a failed scan %d (%llu ms; %s)", i + 1, (unsigned long long)took, lastlog);
    }
    CHECK(!(sh_face_status() & SH_FACE_READY) && sh_face_scan(1) < 0, "5 failed scans: the passcode is required (%s)", sh_face_why());
    fsvc_passcode("246810");
    run_ms(50);
    CHECK(sh_face_status() & SH_FACE_READY, "the passcode: ready again");

    /* a restart */
    restart();
    CHECK((sh_face_status() & SH_FACE_ENROLLED) && !(sh_face_status() & SH_FACE_READY), "after a restart: set up, but sealed (%s)", sh_face_why());
    CHECK(scan(A1, 0, 0) < 0, "after a restart: no scan before the passcode");
    fsvc_passcode("246810");
    run_ms(50);
    CHECK(scan(A1, 0, 0) == SH_FS_MATCH, "after the passcode: A unlocks again");

    /* a passcode change, then a restart: the face opens with the new passcode */
    fsvc_passcode_set("246810", "135790");
    run_ms(50);
    restart();
    fsvc_passcode("135790");
    run_ms(50);
    CHECK(sh_face_status() & SH_FACE_READY, "a changed passcode keeps the face (rewrapped key)");

    /* the pack only while locked (Track A: home and apps get the heap) */
    int g0 = pk_gets, f0 = pk_frees;
    sh_face_lock(0);
    CHECK(!pk_in && pk_frees == f0 + 1, "unlocked: the pack is given back");
    CHECK(scan(A1, 0, 0) < 0, "unlocked: no scan (and no pack)");
    sh_face_lock(1);
    CHECK(pk_in && pk_gets == g0 + 1, "locked again: the pack is loaded again");
    sh_face_lock(0);
    cur = A0; pose = 5; scene = scene_person;
    CHECK(sh_face_enrol(SH_FACE_SPEED) == 0 && pk_in, "set-up while unlocked: the pack comes for it");
    sh_face_stop();
    CHECK(!pk_in, "set-up cancelled while unlocked: the pack goes again");
    sh_face_lock(1);
    fsvc_passcode("135790");
    run_ms(50);

    /* Security */
    sh_face_set_mode(SH_FACE_SECURITY);
    r = scan(A1, 0, &took);
    CHECK(r == SH_FS_FAIL, "Security: A held still (a photo) does not unlock (%s)", lastlog);
    fsvc_passcode("135790");
    run_ms(50);
    r = scan(A1, -1, &took);
    CHECK(r == SH_FS_MATCH, "Security: A turning the head unlocks (%llu ms; %s)", (unsigned long long)took, lastlog);
    sh_face_set_mode(SH_FACE_SPEED);

    /* the store holds nothing in the clear */
    struct face_eng e;
    face_eng_open(&e, pak, pak_len, &PL.fn);
    face_eng_feed(&e, A0->frame, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W);
    while (face_eng_step(&e, 0) == FE_BUSY) {}
    face_eng_embed(&e);
    while (face_eng_step(&e, 0) != FE_EMB) {}
    struct face_tpl t;
    memset(&t, 0, sizeof t);
    face_tpl_put(&t, 0, e.emb);
    face_eng_close(&e);
    sl = blob_get("face.t", sealed, sizeof sealed);
    CHECK(sl > 0 && !contains(sealed, (size_t)sl, t.v[0], 12) && !contains(probe, sizeof probe, "246810", 6), "no template bytes or digits in the store");

    /* removing the passcode forgets the face */
    fsvc_passcode_set("135790", "");
    pass_is_on = 0;
    run_ms(50);
    CHECK(blob_get("face.t", sealed, sizeof sealed) < 0 && blob_get("face.k", sealed, sizeof sealed) < 0 && !(sh_face_status() & SH_FACE_ENROLLED),
          "the passcode removed: the face and the key are gone");

    printf("face-svc-test: the longest turn on this host %.1f ms (budget 30 ms of face work + the frame copy); heap peak %.1f MiB; now %zu bytes\n",
           turn_max_us / 1000.0, heap_peak / 1048576.0, heap_now);
    CHECK(heap_now == 0, "all memory given back");
    printf("face-svc-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
