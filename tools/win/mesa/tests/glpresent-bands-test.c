/* Host test for sb-patches/0146's band-split present copy (src/gallium/targets/wgl/wgl_sb_present.h in the Mesa
 * tree, branch sb-glp2). Build natively, e.g. in the VM, once with the 16-byte loads and once with memcpy rows:
 *   for d in "" -DSB_PRESENT_NO_SIMD "-fsanitize=address,undefined" -fsanitize=thread; do  # tsan: run under setarch -R
 *     cc -O2 -std=c11 -pthread -DHAVE_ENDIAN_H $d -I ~/sbmesa/src-glp2/src -I ~/sbmesa/build-glp2-x64/src \
 *        -I ~/sbmesa/src-glp2/src/gallium/targets/wgl tools/win/mesa/tests/glpresent-bands-test.c \
 *        -o /tmp/glpresent-bands-test && /tmp/glpresent-bands-test; done
 * (build-glp2-x64/src only for the generated util/format/u_format_gen.h.) 2026-11-12 in the VM (aarch64): all four
 * passed, 5,560 copy cases, 300 pool frames.
 * Checks:
 *  - thread and band counts; the band rows of every height 1..3000 for 1..16 bands: in order, no gap, no overlap,
 *    sizes within one row;
 *  - the copy and the R/B swap, bands run in reverse and in shuffled order, for odd and tiny widths and heights,
 *    source strides larger than the row (padding never copied), a destination with guard bytes around every row;
 *  - the pool protocol of wgl.c (bands handed out under a lock with their job, presenter takes bands and waits) on
 *    4 pthreads, 300 frames with new buffers each frame, every frame checked byte for byte.
 * Then times one thread against four at 2220x1080 (host memory is cached: shows the split's overhead, not the
 * write-combined speed-up). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "wgl_sb_present.h"

static int fails;
#define CHECK(c)                                                                                                   \
   do {                                                                                                            \
      if (!(c)) {                                                                                                  \
         if (fails < 20)                                                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
         fails++;                                                                                                  \
      }                                                                                                            \
   } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng; }

static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint32_t swap_rb(uint32_t p)
{
   return (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
}

static void test_counts(void)
{
   CHECK(sb_present_thread_count(0) == 1);
   CHECK(sb_present_thread_count(1) == 1);
   CHECK(sb_present_thread_count(3) == 3);
   CHECK(sb_present_thread_count(4) == 4);
   CHECK(sb_present_thread_count(8) == 4);
   CHECK(sb_present_band_count(1080, 4) == 8);
   CHECK(sb_present_band_count(256, 4) == 8);
   CHECK(sb_present_band_count(100, 4) == 6);  /* 16-row minimum */
   CHECK(sb_present_band_count(15, 4) == 1);
   CHECK(sb_present_band_count(1, 1) == 1);
   CHECK(sb_present_parallel_size(512, 256));
   CHECK(!sb_present_parallel_size(511, 256));
   CHECK(!sb_present_parallel_size(512, 255));
   CHECK(sb_present_parallel_size(2220, 1080));
}

static void test_band_rows(void)
{
   for (unsigned h = 1; h <= 3000; h++) {
      for (unsigned n = 1; n <= 16 && n <= h; n++) {
         unsigned prev = 0, lo = ~0u, hi = 0;
         for (unsigned i = 0; i < n; i++) {
            unsigned y0, y1;
            sb_present_band_rows(h, n, i, &y0, &y1);
            CHECK(y0 == prev);
            CHECK(y1 > y0);
            prev = y1;
            if (y1 - y0 < lo)
               lo = y1 - y0;
            if (y1 - y0 > hi)
               hi = y1 - y0;
         }
         CHECK(prev == h);
         CHECK(hi - lo <= 1);
      }
   }
}

/* One frame through the bands in the given order, then every byte checked. */
#define GUARD 0xA5
static void copy_case(unsigned w, unsigned h, size_t src_stride, size_t dst_pad, enum sb_present_kind kind,
                      unsigned bands, int order)
{
   size_t row = (size_t)w * 4, dst_stride = row + dst_pad;
   uint8_t *src = malloc(src_stride * h + 64);
   uint8_t *dst = malloc(dst_stride * h + 128);
   for (size_t i = 0; i < src_stride * h + 64; i++)
      src[i] = (uint8_t)rnd();
   memset(dst, GUARD, dst_stride * h + 128);
   uint8_t *d0 = dst + 64; /* guard bytes before the first row too */

   struct sb_present_job job = { d0, dst_stride, src, src_stride, w, h, bands, kind };
   unsigned perm[64];
   for (unsigned i = 0; i < bands; i++)
      perm[i] = order == 0 ? i : order == 1 ? bands - 1 - i : i;
   if (order == 2)
      for (unsigned i = bands - 1; i > 0; i--) {
         unsigned j = rnd() % (i + 1), t = perm[i];
         perm[i] = perm[j];
         perm[j] = t;
      }
   for (unsigned i = 0; i < bands; i++)
      sb_present_run_band(&job, perm[i]);

   int bad = 0;
   for (size_t i = 0; i < 64; i++)
      bad |= dst[i] != GUARD;
   for (unsigned y = 0; y < h; y++) {
      const uint32_t *s = (const uint32_t *)(src + (size_t)y * src_stride);
      const uint8_t *drow = d0 + (size_t)y * dst_stride;
      for (unsigned x = 0; x < w; x++) {
         uint32_t want = kind == SB_PRESENT_RGBX ? swap_rb(s[x]) : s[x], got;
         memcpy(&got, drow + (size_t)x * 4, 4);
         bad |= got != want;
      }
      for (size_t i = row; i < dst_stride; i++)
         bad |= drow[i] != GUARD;
   }
   for (size_t i = 64 + dst_stride * h; i < dst_stride * h + 128; i++)
      bad |= dst[i] != GUARD;
   if (bad)
      printf("FAIL copy %ux%u src_stride %zu dst_pad %zu kind %d bands %u order %d\n", w, h, src_stride, dst_pad,
             kind, bands, order);
   fails += bad;
   free(src);
   free(dst);
}

static void test_copies(void)
{
   static const unsigned ws[] = { 1, 3, 4, 5, 15, 16, 17, 31, 63, 64, 65, 511, 512, 513, 1080, 2220 };
   static const unsigned hs[] = { 1, 2, 15, 16, 17, 255, 256, 257, 1081 };
   unsigned cases = 0;
   for (unsigned a = 0; a < sizeof(ws) / sizeof(ws[0]); a++) {
      for (unsigned b = 0; b < sizeof(hs) / sizeof(hs[0]); b++) {
         unsigned w = ws[a], h = hs[b];
         size_t row = (size_t)w * 4;
         size_t strides[] = { row, row + 4, (row + 63) & ~(size_t)63, row + 60, row * 2 + 12 };
         for (unsigned s = 0; s < 5; s++) {
            if (s == 2 && strides[2] == row)
               continue;
            for (int kind = SB_PRESENT_BGRX; kind <= SB_PRESENT_RGBX; kind++) {
               unsigned nb[] = { 1, sb_present_band_count(h, 4), sb_present_band_count(h, 3), h < 7 ? h : 7 };
               for (unsigned k = 0; k < 4; k++, cases++)
                  copy_case(w, h, strides[s], (s & 1) ? 12 : 0, kind, nb[k], (int)(cases % 3));
            }
         }
      }
   }
   /* the frame this is for: 2220x1080, Panfrost's 64-byte-aligned stride (8896), all band counts up to 16 */
   for (unsigned n = 1; n <= 16; n++, cases++)
      for (int order = 0; order < 3; order++)
         copy_case(2220, 1080, 8896, 0, SB_PRESENT_BGRX, n, order);
   printf("copy cases: %u\n", cases);
}

/* wgl.c's pool, with pthreads instead of SRWLOCK/CONDITION_VARIABLE. */
static struct {
   pthread_mutex_t lock;
   pthread_cond_t work, done;
   struct sb_present_job job;
   unsigned next, pending;
   int quit;
} pool = { .lock = PTHREAD_MUTEX_INITIALIZER, .work = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER };

static void *pool_main(void *arg)
{
   (void)arg;
   pthread_mutex_lock(&pool.lock);
   for (;;) {
      while (pool.next >= pool.job.bands && !pool.quit)
         pthread_cond_wait(&pool.work, &pool.lock);
      if (pool.quit)
         break;
      unsigned band = pool.next++;
      struct sb_present_job job = pool.job;
      pthread_mutex_unlock(&pool.lock);
      sb_present_run_band(&job, band);
      pthread_mutex_lock(&pool.lock);
      if (--pool.pending == 0)
         pthread_cond_signal(&pool.done);
   }
   pthread_mutex_unlock(&pool.lock);
   return NULL;
}

static void pool_run(const struct sb_present_job *job)
{
   pthread_mutex_lock(&pool.lock);
   pool.job = *job;
   pool.next = 0;
   pool.pending = job->bands;
   pthread_mutex_unlock(&pool.lock);
   pthread_cond_broadcast(&pool.work);
   pthread_mutex_lock(&pool.lock);
   while (pool.next < pool.job.bands) {
      unsigned band = pool.next++;
      pthread_mutex_unlock(&pool.lock);
      sb_present_run_band(job, band);
      pthread_mutex_lock(&pool.lock);
      pool.pending--;
   }
   while (pool.pending)
      pthread_cond_wait(&pool.done, &pool.lock);
   pthread_mutex_unlock(&pool.lock);
}

static void test_pool(void)
{
   pthread_t t[3];
   for (int i = 0; i < 3; i++)
      pthread_create(&t[i], NULL, pool_main, NULL);
   int bad_frames = 0;
   for (int f = 0; f < 300; f++) {
      unsigned w = 512 + rnd() % 1800, h = 256 + rnd() % 900;
      size_t src_stride = ((size_t)w * 4 + 63) & ~(size_t)63, row = (size_t)w * 4;
      uint8_t *src = malloc(src_stride * h), *dst = malloc(row * h);
      for (size_t i = 0; i < src_stride * h; i += 4) {
         uint32_t v = rnd();
         memcpy(src + i, &v, 4);
      }
      memset(dst, GUARD, row * h);
      enum sb_present_kind kind = (f % 5 == 4) ? SB_PRESENT_RGBX : SB_PRESENT_BGRX;
      struct sb_present_job job = { dst, row, src, src_stride, w, h, sb_present_band_count(h, 4), kind };
      pool_run(&job);
      /* the presenter would unmap here: free the source to catch a late reader under ASan/valgrind */
      int bad = 0;
      for (unsigned y = 0; y < h && !bad; y++) {
         const uint32_t *s = (const uint32_t *)(src + (size_t)y * src_stride);
         const uint32_t *d = (const uint32_t *)(dst + (size_t)y * row);
         for (unsigned x = 0; x < w; x++)
            bad |= d[x] != (kind == SB_PRESENT_RGBX ? swap_rb(s[x]) : s[x]);
      }
      bad_frames += bad;
      free(src);
      free(dst);
   }
   CHECK(bad_frames == 0);
   pthread_mutex_lock(&pool.lock);
   pool.quit = 1;
   pthread_mutex_unlock(&pool.lock);
   pthread_cond_broadcast(&pool.work);
   for (int i = 0; i < 3; i++)
      pthread_join(t[i], NULL);
   printf("pool: 300 frames, %d bad\n", bad_frames);
}

static void time_copy(void)
{
   unsigned w = 2220, h = 1080;
   size_t stride = 8896, row = (size_t)w * 4;
   uint8_t *src = malloc(stride * h), *dst = malloc(row * h);
   memset(src, 7, stride * h);
   pthread_t t[3];
   pool.quit = 0;
   pool.job.bands = 0;
   for (int i = 0; i < 3; i++)
      pthread_create(&t[i], NULL, pool_main, NULL);
   for (int threads = 1; threads <= 4; threads += 3) {
      struct sb_present_job job = { dst, row, src, stride, w, h, sb_present_band_count(h, threads), SB_PRESENT_BGRX };
      double best = 1e9;
      for (int r = 0; r < 30; r++) {
         double t0 = now_ms();
         if (threads == 1)
            for (unsigned b = 0; b < job.bands; b++)
               sb_present_run_band(&job, b);
         else
            pool_run(&job);
         double dt = now_ms() - t0;
         if (dt < best)
            best = dt;
      }
      printf("host 2220x1080 copy, %d thread(s), %u bands: best %.2f ms (%.0f MB/s)\n", threads, job.bands, best,
             row * h / best / 1e3);
   }
   pthread_mutex_lock(&pool.lock);
   pool.quit = 1;
   pthread_mutex_unlock(&pool.lock);
   pthread_cond_broadcast(&pool.work);
   for (int i = 0; i < 3; i++)
      pthread_join(t[i], NULL);
   free(src);
   free(dst);
}

int main(void)
{
   printf("loads: %s\n", SB_PRESENT_SIMD);
   test_counts();
   test_band_rows();
   test_copies();
   test_pool();
   time_copy();
   printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
   return fails != 0;
}
