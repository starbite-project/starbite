/* SPDX-License-Identifier: GPL-2.0-only
 * Face Unlock's service (face_svc.h): shell.h's sh_face_* on kern/face.c. No libc; the kernel and the host shell
 * build this file as it is.
 *
 * Kept (prefs, never exported): the blob "face.k" (the 256-bit template key K wrapped under the passcode), the blob
 * "face.t" (the templates sealed under K), the prefs face.on and face.mode. In RAM only: K and the opened templates,
 * from the first right passcode after a start; the failed scans in a row; when the passcode was last typed.
 * Logs: one line a scan / set-up (times, counts, the best score); never an embedding, a key or the digits. */
#include "shell.h"
#include "face.h"
#include "face_svc.h"

#define FS_BUDGET_US 30000U             /* the face's own work in one turn (the gate: no turn >= 100 ms) */
#define FS_PASS_MAX_MS (48ULL * 3600 * 1000)   /* the passcode again after 48 h without it (iOS's rule) */
#define FS_ENROL_MS 45000U              /* set-up gives up after this long */
#define BLOB_KEY "face.k"
#define BLOB_TPL "face.t"

static struct fsvc_plat P;
static struct {
    int inited;
    uint8_t key[FACE_KEY];
    int key_ok;
    struct face_tpl tpl;
    int tpl_ok;                         /* the templates are open in RAM */
    int enrolled;                       /* the sealed record exists (known without the key) */
    int fails;
    uint64_t pass_ms;                   /* when the passcode was last right (0: not since the start) */
    /* the job in hand */
    int job;                            /* J_* */
    int cam_on, eng_on;
    unsigned seq;
    uint64_t t0, t_first;               /* ms: the start, the first frame */
    struct face_eng eng;
    struct face_scan scan;
    struct face_enrol en;
    int result, hint;                   /* the last scan's SH_FS_*, its hint */
    int enrol_result;                   /* 0 running, 1 done, < 0 failed */
    int dets, embeds;
    uint32_t det_us, emb_us, step_max, poll_max;
    /* the deferred key work */
    int pend;                           /* 1 check, 2 set */
    char d_old[8], d_new[8];
    char why[64];
    int last_first;                     /* the camera test's first frame (ms) */
    int unlocked;                       /* the shell says the phone is unlocked: no pack kept */
    int told_loading;                   /* a wake before the pack was in: logged once a lock */
} F;
enum { J_NONE = 0, J_SCAN, J_ENROL, J_BENCH, J_CAMTEST };

/* the pack: 1 in memory (P.pak set), 0 loading, -1 not there */
static int pack(int start)
{
    if (P.pak) return 1;
    if (P.fake) return -1;
    if (!start) {
        int st = P.pack_state ? P.pack_state() : -1;
        return st < 0 ? -1 : st == 2 ? (pack(1) > 0 ? 1 : -1) : 0;
    }
    const uint8_t *p = 0;
    size_t n = 0;
    int r = P.pack_get ? P.pack_get(&p, &n) : -1;
    if (r > 0) {
        if (fn_pack_check(p, n, 0) < 0) return -1;     /* the loader checked its SHA-256; the header here */
        P.pak = p;
        P.pak_len = n;
    }
    return r;
}
static int avail(void) { return P.fake || pack(0) >= 0; }

static int tk_like_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a != *b; }   /* 0 equal */

/* ---- small helpers ----------------------------------------------------------------------------------------------- */
static void wipe(void *p, unsigned n) { for (unsigned i = 0; i < n; i++) ((volatile uint8_t *)p)[i] = 0; }
static void scpy(char *d, unsigned cap, const char *s)
{
    unsigned i = 0;
    while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}
struct lb { char b[160]; unsigned n; };
static void lb_s(struct lb *l, const char *s) { while (*s && l->n + 1 < sizeof l->b) l->b[l->n++] = *s++; l->b[l->n] = 0; }
static void lb_d(struct lb *l, uint64_t v)
{
    char t[24];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && l->n + 1 < sizeof l->b) l->b[l->n++] = t[--k];
    l->b[l->n] = 0;
}
static void lb_f2(struct lb *l, float v)        /* two decimals */
{
    if (v < 0) { lb_s(l, "-"); v = -v; }
    int c = (int)(v * 100 + 0.5f);
    lb_d(l, (uint64_t)(c / 100));
    lb_s(l, ".");
    if (c % 100 < 10) lb_s(l, "0");
    lb_d(l, (uint64_t)(c % 100));
}
static void logl(struct lb *l) { if (P.log) P.log(l->b); }
static void log1(const char *s) { struct lb l = { "", 0 }; lb_s(&l, "face: "); lb_s(&l, s); logl(&l); }

void fsvc_init(const struct fsvc_plat *p)
{
    P = *p;
    wipe(&F, sizeof F);
    F.inited = 1;
    uint8_t b[FACE_SEAL_LEN];
    F.enrolled = P.blob_get && P.blob_get(BLOB_TPL, b, sizeof b) == (int)FACE_SEAL_LEN;
    P.pak = 0;
    if (F.enrolled) (void)pack(1);       /* set up: the pack into memory now (the lock's first scan needs it) */
}

/* ---- the key and the record -------------------------------------------------------------------------------------- */
static int tpl_load(void)
{
    uint8_t b[FACE_SEAL_LEN];
    int n = P.blob_get ? P.blob_get(BLOB_TPL, b, sizeof b) : -1;
    F.enrolled = n == (int)FACE_SEAL_LEN;
    F.tpl_ok = F.enrolled && F.key_ok && face_tpl_open(&F.tpl, F.key, b, (unsigned)n) == 0;
    if (F.enrolled && F.key_ok && !F.tpl_ok) log1("the kept face did not open (another key): set Face Unlock up again");
    wipe(b, sizeof b);
    return F.tpl_ok;
}
static void forget_all(void)
{
    if (P.blob_put) { (void)P.blob_put(BLOB_TPL, 0, 0); (void)P.blob_put(BLOB_KEY, 0, 0); }
    if (P.pref_set_int) P.pref_set_int("face.on", 0);
    wipe(F.key, sizeof F.key);
    wipe(&F.tpl, sizeof F.tpl);
    F.key_ok = F.tpl_ok = F.enrolled = 0;
}
static int key_new(const char *digits)
{
    uint8_t salt[16], w[FACE_WRAP_LEN];
    P.random(F.key, FACE_KEY);
    P.random(salt, sizeof salt);
    face_key_wrap(F.key, digits, salt, w);
    int r = P.blob_put(BLOB_KEY, w, sizeof w);
    wipe(w, sizeof w);
    F.key_ok = r == 0;
    if (P.blob_put) (void)P.blob_put(BLOB_TPL, 0, 0);   /* templates under an older key are gone with it */
    F.enrolled = F.tpl_ok = 0;
    return r;
}
static void do_pending(void)
{
    uint64_t t0 = P.now_ms();
    if (F.pend == 1) {
        uint8_t w[FACE_WRAP_LEN];
        int n = P.blob_get(BLOB_KEY, w, sizeof w);
        if (n != (int)FACE_WRAP_LEN) (void)key_new(F.d_new);           /* the first time: a key of its own */
        else if (face_key_unwrap(F.key, F.d_new, w, (unsigned)n) == 0) F.key_ok = 1;
        else { log1("the wrapped key did not open with this passcode: a new one (set Face Unlock up again)"); (void)key_new(F.d_new); }
        wipe(w, sizeof w);
        (void)tpl_load();
    } else if (F.pend == 2) {
        if (!F.d_new[0]) forget_all();
        else {
            uint8_t w[FACE_WRAP_LEN], salt[16];
            int n = P.blob_get(BLOB_KEY, w, sizeof w);
            if (!F.key_ok && n == (int)FACE_WRAP_LEN && F.d_old[0] && face_key_unwrap(F.key, F.d_old, w, (unsigned)n) == 0)
                F.key_ok = 1;
            if (F.key_ok) {
                P.random(salt, sizeof salt);
                face_key_wrap(F.key, F.d_new, salt, w);
                if (P.blob_put(BLOB_KEY, w, sizeof w) < 0) log1("the rewrapped key was not kept");
                (void)tpl_load();
            } else
                (void)key_new(F.d_new);
            wipe(w, sizeof w);
        }
        F.pass_ms = P.now_ms();
    }
    F.pend = 0;
    wipe(F.d_old, sizeof F.d_old);
    wipe(F.d_new, sizeof F.d_new);
    struct lb l = { "", 0 };
    lb_s(&l, "face: key ");
    lb_s(&l, F.key_ok ? "ready" : "not ready");
    lb_s(&l, F.enrolled ? (F.tpl_ok ? ", the face opened" : ", the face sealed") : ", no face set up");
    lb_s(&l, " (");
    lb_d(&l, P.now_ms() - t0);
    lb_s(&l, " ms)");
    logl(&l);
}
void fsvc_passcode(const char *digits)
{
    if (!F.inited || !avail()) return;
    F.pass_ms = P.now_ms();
    F.fails = 0;
    if (F.key_ok && (F.tpl_ok || !F.enrolled)) return;   /* already open since this start */
    scpy(F.d_new, sizeof F.d_new, digits);
    F.pend = 1;
    if (P.arm) P.arm(20);
}
void fsvc_passcode_set(const char *old, const char *nw)
{
    if (!F.inited || !avail()) return;
    scpy(F.d_old, sizeof F.d_old, old);
    scpy(F.d_new, sizeof F.d_new, nw);
    F.pend = 2;
    F.fails = 0;
    if (P.arm) P.arm(20);
}

/* ---- the job: camera + engine ---------------------------------------------------------------------------------- */
static void release_pack(void)
{
    if (!P.pak && !(P.pack_state && P.pack_state() == 1)) return;
    P.pak = 0;
    P.pak_len = 0;
    if (P.pack_release) P.pack_release();
}
static void job_end(void)
{
    if (F.cam_on && P.cam_close) P.cam_close();
    if (F.eng_on) face_eng_close(&F.eng);
    F.cam_on = F.eng_on = 0;
    int was = F.job;
    F.job = J_NONE;
    if (F.unlocked && (was == J_ENROL || was == J_SCAN)) release_pack();   /* set-up ended in Settings: memory back */
}
static int job_begin(int job)
{
    if (F.job) job_end();
    F.dets = F.embeds = 0;
    F.det_us = F.emb_us = F.step_max = F.poll_max = 0;
    F.t0 = P.now_ms();
    F.t_first = 0;
    F.seq = 0;
    if (!P.fake) {
        if (!P.pak || face_eng_open(&F.eng, P.pak, P.pak_len, &P.fn) < 0) { scpy(F.why, sizeof F.why, "Not enough memory for Face Unlock"); return -3; }
        F.eng_on = 1;
    }
    if (job != J_BENCH) {
        F.t0 = P.now_ms();
        if (P.cam_open(FACE_FRAME_W, FACE_FRAME_H) < 0) {
            if (F.eng_on) face_eng_close(&F.eng);
            F.eng_on = 0;
            scpy(F.why, sizeof F.why, "The front camera is busy or did not start");
            return -4;
        }
        F.cam_on = 1;
    }
    F.job = job;
    if (P.arm) P.arm(1);
    return 0;
}

/* a frame in, and the engine's work for this turn; calls on_det / on_emb with what finished */
static void run_engine(void (*on_det)(void), void (*on_emb)(void), int may_feed)
{
    uint64_t t0 = P.fn.now_us();
    if (may_feed && F.cam_on) {
        unsigned seq = 0;
        const uint32_t *px = P.cam_frame(&seq);
        if (px && seq != F.seq && !F.eng.det_busy) {
            F.seq = seq;
            if (!F.t_first) F.t_first = P.now_ms();
            (void)face_eng_feed(&F.eng, px, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W);
        }
    }
    uint32_t used = (uint32_t)(P.fn.now_us() - t0);
    while (used < FS_BUDGET_US) {
        int ev = face_eng_step(&F.eng, FS_BUDGET_US - used);
        used = (uint32_t)(P.fn.now_us() - t0);
        if (ev == FE_DET) { F.dets++; F.det_us += F.eng.det_us; on_det(); }
        else if (ev == FE_EMB) { F.embeds++; F.emb_us += F.eng.emb_us; on_emb(); }
        else break;
    }
    if (used > F.step_max) F.step_max = used;
}

/* -- the scan */
/* round 2: the face's box (a little larger) to the camera's metering: the exposure is set on the face, not the room */
static void tell_roi(void)
{
    if (!P.cam_roi) return;
    if (!F.eng.nfaces) { P.cam_roi(0, 0, 0, 0, 0); return; }
    float x = F.eng.det.x + F.eng.det.w * 0.15f, y = F.eng.det.y + F.eng.det.h * 0.1f;
    float w = F.eng.det.w * 0.7f, h = F.eng.det.h * 0.8f;   /* the skin: no hair, no background */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w >= 4 && h >= 4) P.cam_roi(1, (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
}
/* round 2: frames before the exposure settles look nothing like the set-up's (9i: "never recognises"); they are
 * used once it has settled, or after 0.5 s from the first frame whatever it is (Speed stays quick) */
static int frame_usable(void)
{
    if (!P.cam_settled || P.cam_settled()) return 1;
    return F.t_first && P.now_ms() - F.t_first >= 500;
}
static void scan_det(void)
{
    tell_roi();
    if (!frame_usable()) return;
    face_scan_det(&F.scan, F.eng.nfaces, &F.eng.det, &F.eng.sig, P.now_ms());
    /* an embedding of this frame when none is running and the face is big and bright enough to be worth it */
    if (F.eng.nfaces && !F.eng.emb_busy && F.eng.sig.size >= FACE_MIN_SIZE && F.eng.sig.bright >= 20) (void)face_eng_embed(&F.eng);
}
static void scan_emb(void) { face_scan_emb(&F.scan, face_tpl_score(&F.tpl, F.eng.emb), P.now_ms()); }
static void scan_log(int r)
{
    struct lb l = { "", 0 };
    lb_s(&l, "face: scan (");
    lb_s(&l, F.scan.mode == FACE_SECURITY ? "Security" : "Speed");
    lb_s(&l, "): ");
    lb_s(&l, r == FS_MATCH ? "match" : r == FS_NOMATCH ? "no match" : r == FS_NOFACE ? "no face" : "stopped");
    lb_s(&l, " after ");
    lb_d(&l, P.now_ms() - F.t0);
    lb_s(&l, " ms; first frame ");
    lb_d(&l, F.t_first ? F.t_first - F.t0 : 0);
    lb_s(&l, " ms; detections ");
    lb_d(&l, (uint64_t)F.dets);
    lb_s(&l, " (");
    lb_d(&l, F.dets ? F.det_us / (uint32_t)F.dets / 1000 : 0);
    lb_s(&l, " ms), embeddings ");
    lb_d(&l, (uint64_t)F.embeds);
    lb_s(&l, " (");
    lb_d(&l, F.embeds ? F.emb_us / (uint32_t)F.embeds / 1000 : 0);
    lb_s(&l, " ms); best ");
    lb_f2(&l, F.scan.best);
    if (F.scan.mode == FACE_SECURITY) {
        lb_s(&l, F.scan.blink ? "; blink" : "; no blink");
        lb_s(&l, "; parallax ");
        lb_f2(&l, F.scan.par_max);
    }
    lb_s(&l, "; longest turn ");
    lb_d(&l, F.step_max / 1000);
    lb_s(&l, " ms; fails in a row ");
    lb_d(&l, (uint64_t)F.fails);
    logl(&l);
}
static void scan_tick(void)
{
    if (P.fake) {
        uint64_t e = P.now_ms() - F.t0;
        if (e >= 300) {
            F.result = P.fake == 2 ? SH_FS_FAIL : SH_FS_MATCH;
            if (F.result == SH_FS_FAIL) F.fails++;
            job_end();
        }
        return;
    }
    run_engine(scan_det, scan_emb, 1);
    int r = face_scan_tick(&F.scan, P.now_ms());
    F.hint = F.scan.hint == FS_HINT_TURN ? SH_FH_TURN : F.scan.hint == FS_HINT_CLOSER ? SH_FH_CLOSER
           : F.scan.hint == FS_HINT_LIGHT ? SH_FH_LIGHT : SH_FH_NONE;
    if (r == FS_RUN) return;
    if (r == FS_NOMATCH) F.fails++;
    if (r == FS_MATCH) F.fails = 0;
    F.result = r == FS_MATCH ? SH_FS_MATCH : r == FS_NOMATCH ? SH_FS_FAIL : SH_FS_NOFACE;
    scan_log(r);
    job_end();
}

/* -- set-up */
static void enrol_det(void)
{
    tell_roi();
    if (!F.eng.nfaces) { F.hint = SH_FH_NONE; return; }
    if (P.cam_settled && !P.cam_settled()) return;     /* the set-up only from settled frames */
    F.hint = F.eng.sig.size < FACE_CLOSER_SIZE ? SH_FH_CLOSER : F.eng.sig.bright < 35 ? SH_FH_LIGHT : SH_FH_NONE;
    if (!F.eng.emb_busy && face_enrol_det(&F.en, &F.eng.sig, F.eng.det.score) >= 0) {
        if (face_eng_embed(&F.eng) < 0) F.en.want = -1;
    }
}
static void enrol_emb(void) { (void)face_enrol_emb(&F.en, F.eng.emb); }
static void enrol_finish(void)
{
    uint8_t nonce[16], b[FACE_SEAL_LEN];
    F.en.t.mode = (uint8_t)P.pref_int("face.mode", FACE_SPEED);
    P.random(nonce, sizeof nonce);
    face_tpl_seal(&F.en.t, F.key, nonce, b);
    int r = P.blob_put(BLOB_TPL, b, sizeof b);
    wipe(b, sizeof b);
    if (r < 0) {
        F.enrol_result = -2;
        scpy(F.why, sizeof F.why, "The face could not be kept (the settings store is full)");
    } else {
        F.tpl = F.en.t;
        F.tpl_ok = F.enrolled = 1;
        F.fails = 0;
        P.pref_set_int("face.on", 1);
        F.enrol_result = 1;
    }
    wipe(&F.en, sizeof F.en);
}
static void enrol_tick(void)
{
    if (P.fake) {
        uint64_t e = P.now_ms() - F.t0;
        F.en.have = (int)((1u << (e / 700 > 5 ? 5 : e / 700)) - 1);
        if (e >= 3600) {
            F.enrolled = F.tpl_ok = 1;
            P.pref_set_int("face.on", 1);
            F.enrol_result = 1;
            job_end();
        }
        return;
    }
    run_engine(enrol_det, enrol_emb, 1);
    if (face_enrol_done(&F.en) && !F.eng.emb_busy) {
        struct lb l = { "", 0 };
        lb_s(&l, "face: set up in ");
        lb_d(&l, P.now_ms() - F.t0);
        lb_s(&l, " ms (detections ");
        lb_d(&l, (uint64_t)F.dets);
        lb_s(&l, ", embeddings ");
        lb_d(&l, (uint64_t)F.embeds);
        lb_s(&l, ", ");
        lb_d(&l, F.embeds ? F.emb_us / (uint32_t)F.embeds / 1000 : 0);
        lb_s(&l, " ms each; longest turn ");
        lb_d(&l, F.step_max / 1000);
        lb_s(&l, " ms)");
        logl(&l);
        enrol_finish();
        job_end();
        return;
    }
    if (P.now_ms() - F.t0 > FS_ENROL_MS) {
        F.enrol_result = -1;
        scpy(F.why, sizeof F.why, F.dets ? "Not every angle was seen. Try again in good light." : "No face was seen");
        log1(F.dets ? "set-up: timed out (not every pose)" : "set-up: timed out (no face)");
        job_end();
    }
}

/* -- the self test's bench: the networks on a made-up frame (a skin-toned oval with dark eyes; timing only) */
static uint32_t *bench_px;
static int bench_n;
static void bench_det(void)
{
    if (!F.eng.emb_busy) {               /* embed whatever was found; with no face, the centre as a face */
        if (!F.eng.nfaces) {
            static const float pt[10] = { 140, 200, 220, 200, 180, 250, 150, 300, 210, 300 };
            for (int i = 0; i < 10; i++) F.eng.det.pt[i] = pt[i];
            F.eng.nfaces = 1;
        }
        (void)face_eng_embed(&F.eng);
    }
}
static void bench_emb(void) { bench_n++; }
static void bench_tick(void)
{
    if (!F.eng.det_busy && !F.eng.emb_busy && bench_n < 3)
        (void)face_eng_feed(&F.eng, bench_px, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W);
    run_engine(bench_det, bench_emb, 0);
    if (bench_n < 3) return;
    struct lb l = { "", 0 };
    lb_s(&l, "face: bench: detection ");
    lb_d(&l, F.dets ? F.det_us / (uint32_t)F.dets / 1000 : 0);
    lb_s(&l, " ms, embedding ");
    lb_d(&l, F.embeds ? F.emb_us / (uint32_t)F.embeds / 1000 : 0);
    lb_s(&l, " ms (");
    lb_d(&l, (uint64_t)F.dets);
    lb_s(&l, " / ");
    lb_d(&l, (uint64_t)F.embeds);
    lb_s(&l, " runs); longest turn ");
    lb_d(&l, F.step_max / 1000);
    lb_s(&l, " ms; longest step ");
    lb_d(&l, F.eng.step_us_max / 1000);
    lb_s(&l, " ms; working memory ");
    lb_d(&l, (F.eng.yn.arena_bytes + F.eng.sf.arena_bytes + FACE_FRAME_W * FACE_FRAME_H * 5) >> 10);
    lb_s(&l, " KiB");
    logl(&l);
    job_end();
    P.fn.free(bench_px);
    bench_px = 0;
}
int fsvc_bench_start(void)
{
    if (pack(1) <= 0 || F.job) return 0;
    bench_px = P.fn.alloc((size_t)FACE_FRAME_W * FACE_FRAME_H * 4);
    if (!bench_px) return 0;
    for (unsigned y = 0; y < FACE_FRAME_H; y++)
        for (unsigned x = 0; x < FACE_FRAME_W; x++) {
            int dx = (int)x - 180, dy = (int)y - 250;
            int in = dx * dx * 16 + dy * dy * 9 < 110 * 110 * 16;
            int eye = ((x - 140) * (x - 140) + (y - 200) * (y - 200) < 80) || ((x - 220) * (x - 220) + (y - 200) * (y - 200) < 80);
            bench_px[y * FACE_FRAME_W + x] = eye ? 0xFF202020u : in ? 0xFFD8A888u : 0xFF404850u;
        }
    bench_n = 0;
    if (job_begin(J_BENCH) < 0) { P.fn.free(bench_px); bench_px = 0; return 0; }
    return 1;
}

/* -- the face test image: the camera with nobody enrolled (an empty template set: no match is possible) */
static uint32_t camtest_ms;
static void camtest_det(void) { tell_roi(); if (F.eng.nfaces && !F.eng.emb_busy) (void)face_eng_embed(&F.eng); }
static void camtest_emb(void) { }
static void camtest_tick(void)
{
    run_engine(camtest_det, camtest_emb, 1);
    if (P.now_ms() - F.t0 < camtest_ms) return;
    struct lb l = { "", 0 };
    lb_s(&l, "face: camera test: first frame after ");
    lb_d(&l, F.t_first ? F.t_first - F.t0 : 0);
    lb_s(&l, " ms; frames processed ");
    lb_d(&l, (uint64_t)F.dets);
    lb_s(&l, " in ");
    lb_d(&l, P.now_ms() - F.t0);
    lb_s(&l, " ms (detection ");
    lb_d(&l, F.dets ? F.det_us / (uint32_t)F.dets / 1000 : 0);
    lb_s(&l, " ms); faces embedded ");
    lb_d(&l, (uint64_t)F.embeds);
    lb_s(&l, " (");
    lb_d(&l, F.embeds ? F.emb_us / (uint32_t)F.embeds / 1000 : 0);
    lb_s(&l, " ms); longest turn ");
    lb_d(&l, F.step_max / 1000);
    lb_s(&l, " ms");
    logl(&l);
    F.last_first = F.t_first ? (int)(F.t_first - F.t0) : 0;
    job_end();
}
int fsvc_camera_test(unsigned ms)
{
    if (pack(1) <= 0 || F.job) return 0;
    camtest_ms = ms;
    return job_begin(J_CAMTEST) == 0;
}
int fsvc_result_first_frame_ms(void) { return F.last_first; }

int fsvc_busy(void) { return F.job != J_NONE || F.pend != 0; }
void fsvc_tick(void)
{
    if (F.pend) { do_pending(); if (F.job && P.arm) P.arm(10); return; }   /* the key's ~45 ms: a turn of its own */
    if (F.job == J_SCAN) scan_tick();
    else if (F.job == J_ENROL) enrol_tick();
    else if (F.job == J_BENCH) bench_tick();
    else if (F.job == J_CAMTEST) camtest_tick();
    if (F.job && P.arm) P.arm(10);
}

/* ---- shell.h ------------------------------------------------------------------------------------------------- */
static int ready(void)
{
    if (!avail()) { scpy(F.why, sizeof F.why, "Face Unlock's files are not on the card"); return 0; }
    if (!P.pass_on || !P.pass_on()) { scpy(F.why, sizeof F.why, "Set a passcode first"); return 0; }
    if (!P.fake && pack(F.enrolled && !F.unlocked) <= 0) { scpy(F.why, sizeof F.why, "Face Unlock is loading"); return 0; }   /* (unlocked: never loads) */
    if (!F.enrolled) { scpy(F.why, sizeof F.why, "Face Unlock is not set up"); return 0; }
    if (!P.pref_int("face.on", 1)) { scpy(F.why, sizeof F.why, "Unlock with Face is off"); return 0; }
    if (!F.key_ok || !F.tpl_ok || !F.pass_ms) { scpy(F.why, sizeof F.why, "Your passcode is required to enable Face Unlock"); return 0; }
    if (F.fails >= FACE_FAILS_MAX) { scpy(F.why, sizeof F.why, "Face not recognised. Enter your passcode."); return 0; }
    if (!P.fake && P.now_ms() - F.pass_ms > FS_PASS_MAX_MS) { scpy(F.why, sizeof F.why, "Your passcode is required (48 hours without it)"); return 0; }
    if (P.pass_wait && P.pass_wait() > 0) { scpy(F.why, sizeof F.why, "Your passcode is required"); return 0; }
    F.why[0] = 0;
    return 1;
}
int sh_face_status(void)
{
    if (!F.inited || !avail()) return 0;
    int s = SH_FACE_AVAIL;
    if (F.enrolled) s |= SH_FACE_ENROLLED;
    if (P.pref_int("face.on", 1)) s |= SH_FACE_ON;
    if (ready()) s |= SH_FACE_READY;
    return s;
}
void sh_face_prepare(void) { if (F.inited && !P.fake) (void)pack(1); }
const char *sh_face_why(void) { if (F.inited && !F.job) (void)ready(); return F.why; }
int sh_face_mode(void) { return P.pref_int && P.pref_int("face.mode", FACE_SPEED) == FACE_SECURITY ? SH_FACE_SECURITY : SH_FACE_SPEED; }
void sh_face_set_mode(int m) { if (P.pref_set_int) P.pref_set_int("face.mode", m == SH_FACE_SECURITY); }
void sh_face_set_on(int on) { if (P.pref_set_int) P.pref_set_int("face.on", on ? 1 : 0); }
int sh_face_scan(int start)
{
    if (!start) {
        if (F.job == J_SCAN) { if (!P.fake) scan_log(FS_RUN); job_end(); F.result = SH_FS_IDLE; }
        return 0;
    }
    if (F.job == J_SCAN) return 0;
    F.result = SH_FS_IDLE;
    F.hint = SH_FH_NONE;
    if (!F.inited || F.job || !ready()) {
        if (F.inited && !F.told_loading && !tk_like_streq(F.why, "Face Unlock is loading")) {
            F.told_loading = 1;
            log1("a wake before the pack was in memory: the passcode only this time");
        }
        return -1;
    }
    face_scan_begin(&F.scan, sh_face_mode() == SH_FACE_SECURITY ? FACE_SECURITY : FACE_SPEED, P.now_ms());
    int r = job_begin(J_SCAN);
    if (r == 0) F.result = SH_FS_RUN;
    return r;
}
int sh_face_poll(int *hint)
{
    if (hint) *hint = F.job == J_SCAN ? F.hint : SH_FH_NONE;
    return F.job == J_SCAN ? SH_FS_RUN : F.result;
}
int sh_face_enrol(int mode)
{
    if (!F.inited || !avail()) { scpy(F.why, sizeof F.why, "Face Unlock's files are not on the card"); return -1; }
    if (!P.pass_on || !P.pass_on()) { scpy(F.why, sizeof F.why, "Set a passcode first"); return -1; }
    int pk = P.fake ? 1 : pack(1);
    if (pk < 0) { scpy(F.why, sizeof F.why, "Face Unlock's files are not on the card"); return -1; }
    if (!pk) { scpy(F.why, sizeof F.why, "Loading Face Unlock..."); return -2; }
    if (!F.key_ok) { scpy(F.why, sizeof F.why, "Lock the phone and unlock it with your passcode first"); return -1; }
    if (F.job) job_end();
    sh_face_set_mode(mode);
    face_enrol_begin(&F.en);
    F.enrol_result = 0;
    F.hint = SH_FH_NONE;
    return job_begin(J_ENROL);
}
int sh_face_enrol_poll(int *poses, int *hint)
{
    if (poses) *poses = F.job == J_ENROL || F.enrol_result == 1 ? (F.enrol_result == 1 ? (1 << FACE_NTPL) - 1 : F.en.have) : 0;
    if (hint) *hint = F.job == J_ENROL ? F.hint : SH_FH_NONE;
    if (F.job == J_ENROL) return 0;
    return F.enrol_result ? F.enrol_result : -1;
}
const uint32_t *sh_face_frame(int *w, int *h, unsigned *seq)
{
    if ((F.job != J_ENROL && F.job != J_CAMTEST) || !F.cam_on) return 0;   /* (the face test's screen too) */
    unsigned s = 0;
    const uint32_t *px = P.cam_frame(&s);
    if (!px) return 0;
    if (w) *w = FACE_FRAME_W;
    if (h) *h = FACE_FRAME_H;
    if (seq) *seq = s;
    return px;
}
void sh_face_stop(void)
{
    if (F.job == J_SCAN && !P.fake) scan_log(FS_RUN);
    if (F.job == J_ENROL && !F.enrol_result) F.enrol_result = -1;
    if (F.job != J_BENCH) job_end();
    if (F.unlocked && !F.job) release_pack();        /* a set-up left (or never started) in Settings */
}
void sh_face_reset(void)
{
    if (F.job == J_SCAN || F.job == J_ENROL) job_end();
    if (P.blob_put) (void)P.blob_put(BLOB_TPL, 0, 0);
    if (P.pref_set_int) P.pref_set_int("face.on", 0);
    wipe(&F.tpl, sizeof F.tpl);
    F.tpl_ok = F.enrolled = 0;
    F.fails = 0;
    log1("the face was forgotten");
}

/* the host's stills: a face set up and the key open (the fake platform only) */
void fsvc_host_enrolled(void)
{
    if (!P.fake) return;
    F.key_ok = F.tpl_ok = F.enrolled = 1;
    F.pass_ms = P.now_ms() ? P.now_ms() : 1;
}

void sh_face_lock(int locked)
{
    if (!F.inited || P.fake) { F.unlocked = !locked; return; }
    F.unlocked = !locked;
    if (locked) {
        F.told_loading = 0;
        if (F.enrolled && P.pref_int("face.on", 1) && P.pass_on && P.pass_on()) (void)pack(1);
        return;
    }
    if (F.job == J_SCAN) { scan_log(FS_RUN); job_end(); }
    if (F.job != J_ENROL && F.job != J_BENCH && F.job != J_CAMTEST) release_pack();
}
