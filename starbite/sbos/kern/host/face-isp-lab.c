/* SPDX-License-Identifier: GPL-2.0-only
 * Face Unlock round 2: the camera pipeline against face recognition, on the Mac.
 *   face-isp-lab synth FREF.bin [N]        LFW frames made into raw sensor frames (linear light, the lens's fall-off, a
 *                                          raw colour cast, a dim exposure, shot + read noise, the GRBG mosaic, black
 *                                          64), then v1 and v2: how far each picture's embedding is from the clean one
 *   face-isp-lab raw OUTDIR RAW.bin...     the phone's raw frames (tools/face/facedbg-pull.py): v1 and v2 pictures
 *                                          (PPM) and the face found in each, the embeddings' agreement across frames
 * Built by face-isp-lab.sh with the pack (FACE_PACK). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cam.h"
#include "face.h"

static void *al(size_t n) { void *p = NULL; if (posix_memalign(&p, 64, n)) return NULL; memset(p, 0, n); return p; }
static void fr(void *p) { free(p); }
static const struct fn_plat PLAT = { al, fr, NULL, NULL };
static uint8_t *pak;
static size_t pak_len;

static int embed(const uint32_t *px, float *emb, float *score)
{
    struct face_eng e;
    if (face_eng_open(&e, pak, pak_len, &PLAT) < 0) return -1;
    face_eng_feed(&e, px, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W);
    while (face_eng_step(&e, 0) == FE_BUSY) {}
    int ok = e.nfaces;
    if (score) *score = ok ? e.det.score : 0;
    if (ok) {
        face_eng_embed(&e);
        while (face_eng_step(&e, 0) != FE_EMB) {}
        memcpy(emb, e.emb, sizeof e.emb);
    }
    face_eng_close(&e);
    return ok;
}

/* v1 as the camera ran it in 9i: grey world on the raw, nearest cells, gamma 2 */
static void run_v1(const struct cam_raw *r, uint32_t *out)
{
    struct cam_stats s;
    struct cam_wb wb;
    cam_isp_stats(r, 16, &s);
    cam_isp_wb_grey(&s, 64, &wb);
    struct cam_out o = { out, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W, CAM_ROT270 };
    cam_isp_render(r, &wb, &o, 0, FACE_FRAME_H);
}
/* v2 as cam.c runs it: frames in a row, the white balance from the near-grey pixels (a quarter a frame) */
static void run_v2(const struct cam_raw *r, uint32_t *out, const struct cam_tune *tu, int frames)
{
    struct cam_wb wb = { 256, 256, 256 };
    struct cam_out o = { out, FACE_FRAME_W, FACE_FRAME_H, FACE_FRAME_W, CAM_ROT270 };
    for (int f = 0; f < frames; f++) {
        struct cam_pstats p;
        memset(&p, 0, sizeof p);
        cam_isp_process(r, &wb, tu, &o, 0, 0, cam_isp_rows(&o), &p);
        const uint64_t *sm = p.gn * 50 >= p.n && p.gn ? p.gsum : p.sum;
        uint32_t kr = (uint32_t)(sm[1] * 256 / (sm[0] ? sm[0] : 1)), kb = (uint32_t)(sm[1] * 256 / (sm[2] ? sm[2] : 1));
        kr = kr < 128 ? 128 : kr > 1024 ? 1024 : kr;
        kb = kb < 128 ? 128 : kb > 1024 ? 1024 : kb;
        if (f == 0) wb.r = kr, wb.b = kb;
        else wb.r = (3 * wb.r + kr) / 4, wb.b = (3 * wb.b + kb) / 4;
    }
}
static void ppm(const char *path, const uint32_t *px, unsigned w, unsigned h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b[3] = { px[i] >> 16, px[i] >> 8, px[i] }; fwrite(b, 1, 3, f); }
    fclose(f);
}
static float lin(float v) { v /= 255.0f; return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }
static unsigned rs = 12345;
static float gauss(void)
{
    float u = 0;
    for (int i = 0; i < 12; i++) { rs = rs * 1103515245u + 12345u; u += (float)((rs >> 8) & 0xFFFF) / 65536.0f; }
    return u - 6;
}
/* a portrait XRGB frame (360 x 480) into a raw sensor frame (1632 x 1224, the camera's mounting: ROT270) */
static void synth(const uint32_t *fr, uint16_t *raw, float level, float noise)
{
    const unsigned W = 1632, H = 1224, CW = 816, CH = 612;
    for (unsigned cy = 0; cy < CH; cy++)
        for (unsigned cx = 0; cx < CW; cx++) {
            /* ROT270: cell (cx, cy) shows output (ox, oy) with cx = cw - 1 - ry, cy = rx; scaled 816 x 612 -> 480 x 360 */
            float ry = (float)(CW - 1 - cx) * 480.0f / CW, rx = (float)cy * 360.0f / CH;
            unsigned ox = (unsigned)rx, oy = (unsigned)ry;
            if (ox > 359) ox = 359;
            if (oy > 479) oy = 479;
            uint32_t p = fr[oy * 360 + ox];
            float R = lin((float)(p >> 16 & 255)), G = lin((float)(p >> 8 & 255)), B = lin((float)(p & 255));
            float dx = ((float)cx - CW / 2.0f) / (CW / 2.0f), dy = ((float)cy - CH / 2.0f) / (CH / 2.0f);
            float vig = 1.0f - 0.3f * (dx * dx + dy * dy) / 2.0f;          /* 0.7 at the corners */
            float k[4] = { R * 0.55f, G, G, B * 0.75f };                    /* the raw's cast before white balance */
            /* GRBG: TL Gr, TR R, BL B, BR Gb */
            float v4[4] = { k[1], k[0], k[3], k[2] };
            for (int i = 0; i < 4; i++) {
                float e = v4[i] * vig * level * 959.0f;
                e += gauss() * sqrtf(noise * noise + 0.6f * e);              /* read noise + shot noise (in 10-bit DN) */
                int dn = 64 + (int)(e + 0.5f);
                dn = dn < 0 ? 0 : dn > 1023 ? 1023 : dn;
                raw[(size_t)(2 * cy + i / 2) * W + 2 * cx + (i & 1)] = (uint16_t)dn;
            }
        }
    (void)H;
}

static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = al((size_t)L);
    if (!b || fread(b, 1, (size_t)L, f) != (size_t)L) { fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)L;
    return b;
}

int main(int argc, char **argv)
{
    const char *pk = getenv("FACE_PACK");
    if (argc < 3 || !pk || !(pak = load(pk, &pak_len))) { fprintf(stderr, "usage: FACE_PACK=... face-isp-lab synth FREF [N] | raw OUT RAW...\n"); return 2; }
    struct cam_tune tu;
    cam_tune_default(&tu);
    uint16_t *raw = al((size_t)1632 * 1224 * 2);
    uint32_t *a = al(360 * 480 * 4), *b = al(360 * 480 * 4);
    struct cam_raw r = { (const uint8_t *)raw, 1632, 1224, 1632 * 2, CAM_RAW16, CAM_BAYER_GRBG, 64 };
    if (!strcmp(argv[1], "synth")) {
        size_t rl;
        uint8_t *ref = load(argv[2], &rl);
        if (!ref) return 2;
        uint32_t n;
        memcpy(&n, ref + 4, 4);
        int lim = argc > 3 ? atoi(argv[3]) : 30;
        const uint8_t *p = ref + 8;
        size_t yin = 160 * 128 * 3 * 4, yout = (20 * 16 + 10 * 8 + 5 * 4) * (1 + 1 + 4 + 10) * 4;
        static const struct { float level, noise; const char *name; } L[] = {
            { 0.45f, 2.0f, "bright room" }, { 0.12f, 3.0f, "dim room (gain)" }, { 0.05f, 4.0f, "dark (high gain)" } };
        for (int li = 0; li < 3; li++) {
            double c1 = 0, c2 = 0, d1 = 0, d2 = 0;
            int found1 = 0, found2 = 0, tot = 0;
            const uint8_t *q = p;
            for (uint32_t i = 0; i < n && tot < lim; i++) {
                q += 8 + yin + yout;
                const uint32_t *frame = (const uint32_t *)q;
                q += 360 * 480 * 4 + 15 * 4 + 112 * 112 * 3 + 128 * 4;
                float e0[FACE_DIM], e1[FACE_DIM], e2[FACE_DIM], s1 = 0, s2 = 0;
                if (embed(frame, e0, 0) <= 0) continue;
                tot++;
                synth(frame, raw, L[li].level, L[li].noise);
                run_v1(&r, a);
                run_v2(&r, b, &tu, 6);
                if (tot == 1) {
                    char nm[64];
                    snprintf(nm, sizeof nm, "/tmp/face-isp-lab-%d-v1.ppm", li); ppm(nm, a, 360, 480);
                    snprintf(nm, sizeof nm, "/tmp/face-isp-lab-%d-v2.ppm", li); ppm(nm, b, 360, 480);
                }
                if (embed(a, e1, &s1) > 0) { found1++; c1 += face_cos(e0, e1); d1 += s1; }
                if (embed(b, e2, &s2) > 0) { found2++; c2 += face_cos(e0, e2); d2 += s2; }
            }
            printf("%-18s v1: found %2d/%d, cosine to the clean face %.3f, detection %.2f | v2: found %2d/%d, cosine %.3f, detection %.2f\n",
                   L[li].name, found1, tot, found1 ? c1 / found1 : 0, found1 ? d1 / found1 : 0, found2, tot,
                   found2 ? c2 / found2 : 0, found2 ? d2 / found2 : 0);
        }
        return 0;
    }
    if (!strcmp(argv[1], "raw") && argc > 3) {
        int nf = argc - 3;
        float (*E)[2][FACE_DIM] = calloc((size_t)nf, sizeof *E);
        int *ok = calloc((size_t)nf * 2, sizeof(int));
        for (int i = 0; i < nf; i++) {
            size_t rl;
            uint8_t *rb = load(argv[3 + i], &rl);
            if (!rb || rl != (size_t)1632 * 1224 * 2) { printf("%s: not a raw frame\n", argv[3 + i]); continue; }
            memcpy(raw, rb, rl);
            fr(rb);
            struct cam_stats st;
            cam_isp_stats(&r, 4, &st);
            run_v1(&r, a);
            run_v2(&r, b, &tu, 6);
            char nm[512];
            snprintf(nm, sizeof nm, "%s/%02d-v1.ppm", argv[2], i); ppm(nm, a, 360, 480);
            snprintf(nm, sizeof nm, "%s/%02d-v2.ppm", argv[2], i); ppm(nm, b, 360, 480);
            float s1 = 0, s2 = 0;
            ok[2 * i] = embed(a, E[i][0], &s1) > 0;
            ok[2 * i + 1] = embed(b, E[i][1], &s2) > 0;
            printf("%s: raw means R %u Gr %u Gb %u B %u (black 64), max %u, clipped greens %u of %u | face v1 %s %.2f, v2 %s %.2f\n",
                   argv[3 + i], st.mean[0], st.mean[1], st.mean[2], st.mean[3], st.max, st.clip, 2 * st.n,
                   ok[2 * i] ? "yes" : "no", s1, ok[2 * i + 1] ? "yes" : "no", s2);
        }
        for (int v = 0; v < 2; v++) {
            double sum = 0;
            int m = 0;
            for (int i = 0; i < nf; i++)
                for (int j = i + 1; j < nf; j++)
                    if (ok[2 * i + v] && ok[2 * j + v]) { sum += face_cos(E[i][v], E[j][v]); m++; }
            printf("v%d: the frames' embeddings agree at %.3f on average (%d pairs; the same person: higher is better)\n", v + 1,
                   m ? sum / m : 0, m);
        }
        return 0;
    }
    return 2;
}
