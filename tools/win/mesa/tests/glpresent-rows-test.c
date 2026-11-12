/* Host test for sb-patches/0145's present helpers (src/gallium/targets/wgl/wgl_sb_present.h in the Mesa tree).
 * Build natively, e.g. in the VM:
 *   cc -O2 -std=c11 -DHAVE_ENDIAN_H -I ~/sbmesa/src-glp/src -I ~/sbmesa/build-glp-a64/src -I ~/sbmesa/src-glp/src/gallium/targets/wgl \
 *      tools/win/mesa/tests/glpresent-rows-test.c -o /tmp/glpresent-rows-test && /tmp/glpresent-rows-test
 * Checks: the format classes, the in-place DIB width, that GDI reading the mapping as a stride/4-wide DIB sees the
 * same pixels the old malloc+memcpy path handed it, the row copy, and the RGBA->BGRA swap against the 0136 loop.
 * Then times the CPU half of the old and new paths at 2220x1080 (host memory: cached, so no write-combined cost). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "wgl_sb_present.h"

static int fails;
#define CHECK(c)                                                                                                   \
   do {                                                                                                            \
      if (!(c)) {                                                                                                  \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
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

/* What GDI reads for pixel (x, y) of a top-down 32-bit DIB `dib_width` pixels wide. */
static uint32_t dib_pixel(const void *bits, unsigned dib_width, unsigned x, unsigned y)
{
   return ((const uint32_t *)bits)[(size_t)y * dib_width + x];
}

/* 0136's present, CPU half: malloc, per-row copy or swap, (StretchDIBits), free. Returns the dense frame. */
static uint32_t *old_present(const uint8_t *map, size_t stride, unsigned w, unsigned h, int rgba)
{
   uint32_t *bits = malloc((size_t)w * h * 4);
   for (unsigned y = 0; y < h; y++) {
      const uint32_t *src = (const uint32_t *)(map + (size_t)y * stride);
      uint32_t *dst = bits + (size_t)y * w;
      if (rgba) {
         for (unsigned x = 0; x < w; x++) {
            uint32_t p = src[x];
            dst[x] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
         }
      } else {
         memcpy(dst, src, (size_t)w * 4);
      }
   }
   return bits;
}

static void test_case(unsigned w, unsigned h, size_t stride)
{
   uint8_t *map = malloc(stride * h);
   for (size_t i = 0; i < stride * h; i++)
      map[i] = (uint8_t)rnd();

   /* BGRA: GDI reads the mapping in place. */
   unsigned dw = sb_present_dib_width(stride, w);
   CHECK(dw == stride / 4);
   uint32_t *ref = old_present(map, stride, w, h, 0);
   int bad = 0;
   for (unsigned y = 0; y < h && !bad; y++)
      for (unsigned x = 0; x < w; x++)
         if (dib_pixel(map, dw, x, y) != ref[(size_t)y * w + x]) { bad = 1; break; }
   CHECK(!bad);

   /* BGRA through the kept buffer (the fallback when GDI refuses the mapping). */
   uint32_t *buf = malloc((size_t)w * h * 4);
   sb_present_copy_rows(buf, (size_t)w * 4, map, stride, (size_t)w * 4, h);
   CHECK(memcmp(buf, ref, (size_t)w * h * 4) == 0);
   free(ref);

   /* RGBA: the swap, byte by byte, and against 0136's loop. */
   ref = old_present(map, stride, w, h, 1);
   sb_present_rgba_to_bgra_rows(buf, (size_t)w * 4, map, stride, w, h);
   CHECK(memcmp(buf, ref, (size_t)w * h * 4) == 0);
   bad = 0;
   for (unsigned y = 0; y < h && !bad; y++)
      for (unsigned x = 0; x < w; x++) {
         const uint8_t *s = map + y * stride + x * 4, *d = (const uint8_t *)buf + ((size_t)y * w + x) * 4;
         if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0] || d[3] != s[3]) { bad = 1; break; }
      }
   CHECK(!bad);
   free(ref);
   free(buf);
   free(map);
}

int main(void)
{
   CHECK(sb_present_kind(PIPE_FORMAT_B8G8R8A8_UNORM) == SB_PRESENT_BGRX);
   CHECK(sb_present_kind(PIPE_FORMAT_B8G8R8X8_UNORM) == SB_PRESENT_BGRX);
   CHECK(sb_present_kind(PIPE_FORMAT_B8G8R8A8_SRGB) == SB_PRESENT_BGRX);
   CHECK(sb_present_kind(PIPE_FORMAT_R8G8B8A8_UNORM) == SB_PRESENT_RGBX);
   CHECK(sb_present_kind(PIPE_FORMAT_R8G8B8X8_SRGB) == SB_PRESENT_RGBX);
   CHECK(sb_present_kind(PIPE_FORMAT_A8R8G8B8_UNORM) == SB_PRESENT_CONVERT);
   CHECK(sb_present_kind(PIPE_FORMAT_X8R8G8B8_UNORM) == SB_PRESENT_CONVERT);
   CHECK(sb_present_kind(PIPE_FORMAT_R10G10B10A2_UNORM) == SB_PRESENT_CONVERT);
   CHECK(sb_present_kind(PIPE_FORMAT_R16G16B16A16_FLOAT) == SB_PRESENT_CONVERT);

   CHECK(sb_present_dib_width(8896, 2220) == 2224); /* Panfrost's 64-byte-aligned linear rows at GD's width */
   CHECK(sb_present_dib_width(8880, 2220) == 2220);
   CHECK(sb_present_dib_width(1024, 256) == 256);
   CHECK(sb_present_dib_width(8882, 2220) == 0);    /* not whole pixels */
   CHECK(sb_present_dib_width(100, 30) == 0);       /* narrower than the image */

   test_case(256, 256, 1024);
   test_case(2220, 1080, 8896);
   test_case(17, 5, 128);
   test_case(1, 1, 64);

   /* CPU half at 2220x1080, best of 20 (host memory, cached). */
   const unsigned w = 2220, h = 1080;
   const size_t stride = 8896;
   uint8_t *map = malloc(stride * h);
   memset(map, 0x5a, stride * h);
   uint32_t *kept = malloc((size_t)w * h * 4);
   double t_old = 1e9, t_swz = 1e9, t_kept = 1e9;
   volatile uint32_t sink = 0;
   for (int i = 0; i < 20; i++) {
      double t0 = now_ms();
      uint32_t *b = old_present(map, stride, w, h, 0);
      sink += b[i];
      free(b);
      double t1 = now_ms();
      b = old_present(map, stride, w, h, 1);
      sink += b[i];
      free(b);
      double t2 = now_ms();
      sb_present_copy_rows(kept, (size_t)w * 4, map, stride, (size_t)w * 4, h);
      sink += kept[i];
      double t3 = now_ms();
      if (t1 - t0 < t_old) t_old = t1 - t0;
      if (t2 - t1 < t_swz) t_swz = t2 - t1;
      if (t3 - t2 < t_kept) t_kept = t3 - t2;
   }
   printf("2220x1080 CPU half (host, cached memory, best of 20):\n");
   printf("  0136 BGRA: malloc+copy+free %.2f ms\n", t_old);
   printf("  0136 RGBA: malloc+swap+free %.2f ms\n", t_swz);
   printf("  0145 fallback: copy into kept buffer %.2f ms\n", t_kept);
   printf("  0145 BGRA direct: 0 bytes touched by the ICD\n");

   free(kept);
   free(map);
   printf(fails ? "FAILED: %d\n" : "all checks passed\n", fails);
   return fails != 0;
}
