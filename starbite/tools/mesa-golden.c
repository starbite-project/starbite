#if 0
# Mesa golden words for Mali Bifrost v6 (Mali-G72) job descriptors.
# This file is also a bash script that builds and runs itself in the Lima VM:
#   limactl shell starbite -- bash <path-to>/starbite/tools/mesa-golden.c
# Default: link the prebuilt Mesa 25.0.7 libs (no ninja) and cross-check the
# pan_pack replica against the real GENX(pan_emit_fbd).
# GOLDEN_HEADER_ONLY=1 in the environment: pan_pack only, links nothing.
set -euo pipefail
SRC=$(readlink -f "$0")
B=$HOME/mesa/build
OUT=$HOME/mesa/starbite-lab
mkdir -p "$OUT"
FLAGS=$(python3 - "$B" <<"PY"
import json, shlex, sys
b = sys.argv[1]
for e in json.load(open(b + "/compile_commands.json")):
    if e["output"].endswith("libpan-arch-v6.a.p/pan_desc.c.o"):
        out, skip = [], False
        for x in shlex.split(e["command"])[1:]:
            if skip:
                skip = False
                continue
            if x in ("-o", "-c", "-MF", "-MQ"):
                skip = True
                continue
            if x == "-MD" or x.endswith(".c"):
                continue
            if x.startswith("-I") and not x[2:].startswith("/"):
                x = "-I" + b + "/" + x[2:]
            out.append(x)
        print(" ".join(shlex.quote(x) for x in out))
        break
PY
)
LIBS=(src/panfrost/lib/libpan-arch-v6.a src/panfrost/lib/libpan-format-v6.a
	src/panfrost/lib/libpanfrost_lib.a src/panfrost/compiler/libpanfrost_bifrost.a
	src/panfrost/util/libpanfrost_util.a src/panfrost/shared/libpanfrost_shared.a
	src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a
	src/util/libmesa_util_sse41.a src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a)
CC="gcc $FLAGS -Wno-error $SRC -o $OUT/mesa-golden"
if [ -n "${GOLDEN_HEADER_ONLY:-}" ]; then
	eval "$CC -DGOLDEN_HEADER_ONLY"
else
	L=""
	for l in "${LIBS[@]}"; do L="$L $B/$l"; done
	eval "$CC -Wl,--start-group $L -Wl,--end-group -lm -lpthread -lstdc++ -lz -lexpat -ldrm"
fi
"$OUT/mesa-golden" | tee "$OUT/mesa-golden.txt"
exit 0
#endif

/*
 * mesa-golden.c: Mesa-packed golden 32-bit words for a few Mali Bifrost v6
 * (Mali-G72) job descriptors, for a host unit test of a hand-written packer.
 *
 * Everything is packed with Mesa 25.0.7 genxml v6 (pan_pack /
 * pan_section_pack on the MALI_* structs from v6.xml) the way
 * src/panfrost/lib/pan_desc.c GENX(pan_emit_fbd) and
 * GENX(pan_emit_fragment_job_payload), and src/gallium/drivers/panfrost/
 * pan_jm.c GENX(jm_emit_fbds) / GENX(jm_emit_fragment_job), build them for
 * PAN_ARCH 6. Unless GOLDEN_HEADER_ONLY is defined, the framebuffer and
 * fragment job are additionally built by calling the real Mesa functions
 * (linked from the prebuilt ~/mesa/build libs) and must match byte for byte.
 *
 * It also replays the hand packing of xnu/src/starlte_gpu.c (sg_write_value,
 * sg_clear without tiler) and prints a word-by-word diff against Mesa.
 *
 * Output: C arrays, word i = little-endian bytes 4i..4i+3.
 */

#ifndef PAN_ARCH
#define PAN_ARCH 6
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genxml/gen_macros.h"
#include "drm-uapi/drm_fourcc.h"
#include "pan_desc.h"
#include "pan_props.h"
#include "pan_samples.h"
#include "pan_texture.h"
#include "util/format/u_formats.h"

#ifdef GOLDEN_HEADER_ONLY
/* Mesa's own sample-position LUT, compiled in instead of linked. */
#include "pan_samples.c"
#endif

#if PAN_ARCH != 6
#error "v6 (Bifrost, Mali-G72) only"
#endif

/* pan_props.c panfrost_model_list: MODEL(0x6221, 0, "G72", "THEx", ...) */
#define GPU_ID_G72 0x6221u

#define VA_WV_ZERO_OUT  0x02000100ull
#define VA_WV_IMM_OUT   0x02000300ull
#define VA_WV_IMM_NEXT  0x02000440ull
#define VA_WV_TS_OUT    0x02000308ull
#define VA_WV_CYC_OUT   0x02000310ull
#define VA_FBD          0x02001000ull
#define VA_SAMPLES      0x02001200ull
#define VA_RT_BASE      0x02002000ull
#define VA_RT2_BASE     0x04763F40ull
#define IMM_STARBITE    0x4554494252415453ull

#define FBD_WORDS ((MALI_FRAMEBUFFER_LENGTH + MALI_RENDER_TARGET_LENGTH) / 4)
#define SAMPLE_WORDS 33

static uint32_t
le32(const void *p)
{
   const uint8_t *b = p;
   return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
          (uint32_t)b[3] << 24;
}

static void
dump(const char *name, const void *buf, unsigned words)
{
   printf("static const uint32_t golden_%s[%u] = {", name, words);
   for (unsigned i = 0; i < words; i++)
      printf("%s0x%08x,", (i % 4) ? " " : "\n\t",
             le32((const uint8_t *)buf + 4 * i));
   printf("\n};\n");
}

/* ---- Write Value jobs (header + payload, MALI_WRITE_VALUE_JOB) ---- */

static void
mesa_write_value(uint8_t out[64], unsigned index, uint64_t next, uint64_t addr,
                 enum mali_write_value_type type, uint64_t imm)
{
   memset(out, 0, 64);
   pan_section_pack(out, WRITE_VALUE_JOB, HEADER, hdr) {
      hdr.type = MALI_JOB_TYPE_WRITE_VALUE;
      hdr.index = index;
      hdr.next = next;
   }
   pan_section_pack(out, WRITE_VALUE_JOB, PAYLOAD, p) {
      p.address = addr;
      p.type = type;
      p.immediate_value = imm;
   }
}

/* ---- Framebuffer pointer tag (end of GENX(pan_emit_fbd)) ---- */

static uint32_t
mesa_fbd_tag(bool has_zs_crc_ext, unsigned rt_count)
{
   struct mali_framebuffer_pointer_packed tag;
   pan_pack(&tag, FRAMEBUFFER_POINTER, cfg) {
      cfg.zs_crc_extension_present = has_zs_crc_ext;
      cfg.render_target_count = MAX2(rt_count, 1);
   }
   return tag.opaque[0];
}

/* ---- Fragment job (GENX(jm_emit_fragment_job), full-frame extent) ---- */

static void
mesa_fragment_job(uint8_t out[64], unsigned w, unsigned h, uint64_t fbd_ptr)
{
   memset(out, 0, 64);
   pan_section_pack(out, FRAGMENT_JOB, PAYLOAD, payload) {
      payload.bound_min_x = 0 >> MALI_TILE_SHIFT;
      payload.bound_min_y = 0 >> MALI_TILE_SHIFT;
      payload.bound_max_x = (w - 1) >> MALI_TILE_SHIFT;
      payload.bound_max_y = (h - 1) >> MALI_TILE_SHIFT;
      payload.framebuffer = fbd_ptr;
   }
   pan_section_pack(out, FRAGMENT_JOB, HEADER, header) {
      header.type = MALI_JOB_TYPE_FRAGMENT;
      header.index = 1;
   }
}

/* ---- Framebuffer + RT 0, replica of GENX(pan_emit_fbd) for one linear
 * single-sampled R8G8B8A8_UNORM colour target that is cleared, no ZS, no
 * CRC, no pre/post frame shaders, no tiler (clear-only batch). ---- */

/* pan_rt_init_format: panfrost_invert_swizzle(desc->swizzle) of
 * R8G8B8A8_UNORM (XYZW) is XYZW; panfrost_translate_swizzle_4 packs 3 bits
 * per channel. */
static unsigned
rt_swizzle_rgba(void)
{
   const unsigned char s[4] = {PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z,
                               PIPE_SWIZZLE_W};
   unsigned out = 0;
   for (unsigned i = 0; i < 4; ++i)
      out |= s[i] << (3 * i);
   return out;
}

static void
mesa_fbd_replica(uint8_t out[FBD_WORDS * 4], unsigned w, unsigned h,
                 uint64_t rt_base, unsigned row_stride, uint32_t clear0)
{
   /* GENX(pan_select_tile_size): budget (G72: 16384 / 2) >> log2(4 B/px)
    * clamped to panfrost_max_effective_tile_size(6) = 16x16; colour buffer
    * allocation = ALIGN_POT(4 * tile, 1024). */
   unsigned tile = panfrost_max_effective_tile_size(PAN_ARCH);
   unsigned cbuf = ALIGN_POT(4 * tile, 1024);

   memset(out, 0, FBD_WORDS * 4);
   pan_section_pack(out, FRAMEBUFFER, PARAMETERS, cfg) {
      cfg.sample_locations =
         VA_SAMPLES + panfrost_sample_positions_offset(pan_sample_pattern(1));
      cfg.pre_frame_0 = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
      cfg.pre_frame_1 = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
      cfg.post_frame = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
      cfg.frame_shader_dcds = 0;
      cfg.tiler = 0;
      cfg.width = w;
      cfg.height = h;
      cfg.bound_max_x = w - 1;
      cfg.bound_max_y = h - 1;
      cfg.effective_tile_size = tile;
      cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
      cfg.render_target_count = 1;
      cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;
      cfg.z_clear = 0.0f;
      cfg.s_clear = 0;
      cfg.color_buffer_allocation = cbuf;
      cfg.sample_count = 1;
      cfg.sample_pattern = pan_sample_pattern(1);
      cfg.z_write_enable = false;
      cfg.s_write_enable = false;
      cfg.has_zs_crc_extension = false;
   }
   pan_section_pack(out, FRAMEBUFFER, PADDING, padding)
      ;

   /* No ZS/CRC extension: RT 0 directly after the 128-byte Framebuffer. */
   struct mali_render_target_packed *rt =
      (struct mali_render_target_packed *)(out + MALI_FRAMEBUFFER_LENGTH);
   pan_pack(rt, RENDER_TARGET, cfg) {
      cfg.clean_pixel_write_enable = true; /* rts[0].clear */
      cfg.internal_buffer_offset = 0;
      cfg.clear.color_0 = clear0;
      cfg.clear.color_1 = 0;
      cfg.clear.color_2 = 0;
      cfg.clear.color_3 = 0;
      cfg.write_enable = true;
      cfg.dithering_enable = true;
      cfg.writeback_msaa = MALI_MSAA_SINGLE;
      cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
      cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
      cfg.swizzle = rt_swizzle_rgba();
      cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
      cfg.rgb.base = rt_base;
      cfg.rgb.row_stride = row_stride;
      cfg.rgb.surface_stride = 0;
   }
}

#ifndef GOLDEN_HEADER_ONLY
/* ---- The same frame through Mesa's real GENX(pan_emit_fbd) ---- */

struct real_fb {
   struct pan_image img;
   struct pan_image_view view;
   bool crc_valid;
   struct pan_fb_info fb;
};

static uint32_t
mesa_fbd_real(struct real_fb *r, uint8_t out[FBD_WORDS * 4], unsigned w,
              unsigned h, uint64_t rt_base, unsigned row_stride, uint32_t clear0)
{
   memset(r, 0, sizeof(*r));
   r->img.data.base = rt_base;
   r->img.layout.modifier = DRM_FORMAT_MOD_LINEAR;
   r->img.layout.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   r->img.layout.width = w;
   r->img.layout.height = h;
   r->img.layout.depth = 1;
   r->img.layout.nr_samples = 1;
   r->img.layout.dim = MALI_TEXTURE_DIMENSION_2D;
   r->img.layout.nr_slices = 1;
   r->img.layout.array_size = 1;
   struct pan_image_explicit_layout ex = {.offset = 0, .row_stride = row_stride};
   if (!pan_image_layout_init(PAN_ARCH, &r->img.layout, &ex)) {
      fprintf(stderr, "pan_image_layout_init rejected %ux%u stride %u\n", w, h,
              row_stride);
      exit(1);
   }

   r->view.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   r->view.dim = MALI_TEXTURE_DIMENSION_2D;
   r->view.swizzle[0] = PIPE_SWIZZLE_X;
   r->view.swizzle[1] = PIPE_SWIZZLE_Y;
   r->view.swizzle[2] = PIPE_SWIZZLE_Z;
   r->view.swizzle[3] = PIPE_SWIZZLE_W;
   r->view.planes[0] = &r->img;
   r->view.nr_samples = 1;

   const struct panfrost_model *model = panfrost_get_model(GPU_ID_G72, 0);
   if (!model) {
      fprintf(stderr, "no G72 in panfrost_model_list\n");
      exit(1);
   }

   struct pan_fb_info *fb = &r->fb;
   fb->width = w;
   fb->height = h;
   fb->extent.minx = 0;
   fb->extent.miny = 0;
   fb->extent.maxx = w - 1;
   fb->extent.maxy = h - 1;
   fb->nr_samples = 1;
   fb->rt_count = 1;
   fb->rts[0].view = &r->view;
   fb->rts[0].crc_valid = &r->crc_valid;
   fb->rts[0].clear = true;
   fb->rts[0].clear_value[0] = clear0;
   fb->bifrost.pre_post.modes[0] = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
   fb->bifrost.pre_post.modes[1] = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
   fb->bifrost.pre_post.modes[2] = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
   fb->tile_buf_budget = panfrost_query_optimal_tib_size(model);
   fb->sample_positions =
      VA_SAMPLES + panfrost_sample_positions_offset(pan_sample_pattern(1));
   GENX(pan_select_tile_size)(fb);

   struct pan_tiler_context tiler;
   struct pan_tls_info tls;
   memset(&tiler, 0, sizeof(tiler));
   memset(&tls, 0, sizeof(tls));
   memset(out, 0, FBD_WORDS * 4);
   return GENX(pan_emit_fbd)(fb, 0, &tls, &tiler, out);
}
#endif

/* ---- Replay of the XNU hand packing (xnu/src/starlte_gpu.c) ---- */

#define SG_VA          0x02000000ull
#define SG_FRAG_OFF    0x0400u
#define SG_FBD_OFF     0x1000u
#define SG_SAMPLES_OFF 0x1200u
#define SG_PIX_OFF     0x2000u

static uint8_t sg_data[SG_PIX_OFF];

static void
sg_put32(uint32_t off, uint32_t v)
{
   memcpy(sg_data + off, &v, 4);
}

static void
sg_put64(uint32_t off, uint64_t v)
{
   memcpy(sg_data + off, &v, 8);
}

/* sg_write_value() minus the run: index is always 1 and next always 0. */
static void
xnu_write_value(uint32_t job_off, uint32_t out_off, uint32_t type,
                uint64_t value)
{
   uint8_t *job = sg_data + job_off;
   memset(job, 0, 64);
   job[16] = 1 | (2 << 1);
   uint16_t index = 1;
   memcpy(job + 18, &index, 2);
   uint64_t addr = SG_VA + out_off;
   memcpy(job + 32, &addr, 8);
   memcpy(job + 40, &type, 4);
   memcpy(job + 48, &value, 8);
}

/* sg_clear() with with_tiler = false, minus the run. */
static void
xnu_clear(uint64_t pixels_va, uint32_t w, uint32_t h, uint32_t stride,
          uint32_t color)
{
   for (uint32_t off = SG_FRAG_OFF; off < SG_FRAG_OFF + 64; off += 4)
      sg_put32(off, 0);
   for (uint32_t off = SG_FBD_OFF; off < SG_PIX_OFF; off += 4)
      sg_put32(off, 0);
   sg_put32(SG_SAMPLES_OFF + 0 * 4, 128u | 128u << 16);
   sg_put32(SG_SAMPLES_OFF + 32 * 4, 128u | 128u << 16);
   uint64_t tiler = 0;
   sg_put64(SG_FBD_OFF + 16, SG_VA + SG_SAMPLES_OFF);
   sg_put32(SG_FBD_OFF + 32, (w - 1) | (h - 1) << 16);
   sg_put32(SG_FBD_OFF + 36, 0);
   sg_put32(SG_FBD_OFF + 40, (w - 1) | (h - 1) << 16);
   sg_put32(SG_FBD_OFF + 44, 2u << 6 | 8u << 9 | 1u << 24);
   sg_put32(SG_FBD_OFF + 48, 1u << 16);
   sg_put64(SG_FBD_OFF + 56, tiler);
   uint32_t rt = SG_FBD_OFF + 128;
   sg_put32(rt + 0, 1u << 26);
   sg_put32(rt + 4, 1u | 19u << 3 | 2u << 10 | 1u << 15 | 0x688u << 16 | 1u << 31);
   sg_put64(rt + 32, pixels_va);
   sg_put32(rt + 40, stride);
   sg_put32(rt + 48, color);
   sg_put32(SG_FRAG_OFF + 16, 1u | 9u << 1 | 1u << 16);
   sg_put32(SG_FRAG_OFF + 32, 0);
   sg_put32(SG_FRAG_OFF + 36, ((w - 1) >> 4) | ((h - 1) >> 4) << 16);
   sg_put64(SG_FRAG_OFF + 40, (SG_VA + SG_FBD_OFF) | 1);
}

static unsigned
xnu_diff(const char *name, const uint8_t *mesa, const uint8_t *xnu,
         unsigned words)
{
   unsigned n = 0;
   for (unsigned i = 0; i < words; i++) {
      uint32_t m = le32(mesa + 4 * i), x = le32(xnu + 4 * i);
      if (m != x) {
         printf("/* xnu-diff %-9s word %2u (+%3u): mesa 0x%08x xnu 0x%08x "
                "xor 0x%08x */\n",
                name, i, 4 * i, m, x, m ^ x);
         n++;
      }
   }
   if (!n)
      printf("/* xnu-diff %-9s identical (%u words) */\n", name, words);
   return n;
}

int
main(void)
{
   static uint8_t wv_zero[64], wv_imm[64], wv_ts[64], wv_cyc[64];
   static uint8_t frag[64], fbd[FBD_WORDS * 4], fbd2[FBD_WORDS * 4];
   uint8_t samples[SAMPLE_WORDS * 4];

   mesa_write_value(wv_zero, 1, 0, VA_WV_ZERO_OUT, MALI_WRITE_VALUE_TYPE_ZERO,
                    0);
   mesa_write_value(wv_imm, 2, VA_WV_IMM_NEXT, VA_WV_IMM_OUT,
                    MALI_WRITE_VALUE_TYPE_IMMEDIATE_64, IMM_STARBITE);
   mesa_write_value(wv_ts, 3, 0, VA_WV_TS_OUT,
                    MALI_WRITE_VALUE_TYPE_SYSTEM_TIMESTAMP, 0);
   mesa_write_value(wv_cyc, 4, 0, VA_WV_CYC_OUT,
                    MALI_WRITE_VALUE_TYPE_CYCLE_COUNTER, 0);

   uint32_t tag = mesa_fbd_tag(false, 1);
   mesa_fbd_replica(fbd, 64, 64, VA_RT_BASE, 256, 0xff00ff00u);
   mesa_fbd_replica(fbd2, 32, 32, VA_RT2_BASE, 5760, 0xff3366ccu);
   mesa_fragment_job(frag, 64, 64, VA_FBD | tag);

   /* Mesa uploads the whole LUT; the FBD points at the pattern's entry. */
   unsigned lut_size = panfrost_sample_positions_buffer_size();
   uint8_t *lut = calloc(1, lut_size);
   panfrost_upload_sample_positions(lut);
   unsigned soff = panfrost_sample_positions_offset(pan_sample_pattern(1));
   assert(soff + sizeof(samples) <= lut_size);
   memcpy(samples, lut + soff, sizeof(samples));

   const char *xcheck = "skipped (GOLDEN_HEADER_ONLY)";
#ifndef GOLDEN_HEADER_ONLY
   {
      static struct real_fb r;
      static uint8_t real_fbd[FBD_WORDS * 4], real_frag[64];
      uint32_t real_tag = mesa_fbd_real(&r, real_fbd, 64, 64, VA_RT_BASE, 256,
                                        0xff00ff00u);
      memset(real_frag, 0, sizeof(real_frag));
      GENX(pan_emit_fragment_job_payload)(&r.fb, VA_FBD | real_tag, real_frag);
      pan_section_pack(real_frag, FRAGMENT_JOB, HEADER, header) {
         header.type = MALI_JOB_TYPE_FRAGMENT;
         header.index = 1;
      }
      bool ok = real_tag == tag && r.fb.tile_size == 256 &&
                r.fb.cbuf_allocation == 1024 &&
                !memcmp(real_fbd, fbd, sizeof(fbd)) &&
                !memcmp(real_frag, frag, sizeof(frag));
      real_tag = mesa_fbd_real(&r, real_fbd, 32, 32, VA_RT2_BASE, 5760,
                               0xff3366ccu);
      ok = ok && real_tag == tag && !memcmp(real_fbd, fbd2, sizeof(fbd2));
      if (!ok) {
         fprintf(stderr, "MISMATCH: pan_pack replica != GENX(pan_emit_fbd)\n");
         return 1;
      }
      xcheck = "OK, replica == GENX(pan_emit_fbd) + "
               "GENX(pan_emit_fragment_job_payload), byte for byte";
   }
#endif

   printf("/* Mesa 25.0.7 genxml v6 (Mali-G72), generated by "
          "starbite/tools/mesa-golden.c */\n");
   printf("/* cross-check: %s */\n", xcheck);
   printf("#define GOLDEN_FBD_PTR_TAG 0x%xu /* MFBD, no ZS/CRC ext, 1 RT; "
          "fragment job framebuffer = 0x%llx */\n",
          tag, (unsigned long long)(VA_FBD | tag));
   printf("#define GOLDEN_RT0_OFFSET %uu /* RT 0 at FBD + %u (no ZS/CRC "
          "extension), %u bytes */\n",
          MALI_FRAMEBUFFER_LENGTH, MALI_FRAMEBUFFER_LENGTH,
          MALI_RENDER_TARGET_LENGTH);
   dump("wv_zero", wv_zero, 16);
   dump("wv_imm64", wv_imm, 16);
   dump("wv_ts", wv_ts, 16);
   dump("wv_cyc", wv_cyc, 16);
   dump("frag", frag, 16);
   dump("fbd", fbd, FBD_WORDS);
   dump("fbd2", fbd2, FBD_WORDS);
   dump("samples", samples, SAMPLE_WORDS);

   /* XNU replay: Write Value jobs at data +0 with out_off = addr - SG_VA. */
   static const struct {
      const char *name;
      const uint8_t *mesa;
      uint64_t addr;
      uint32_t type;
      uint64_t value;
   } wv[] = {
      {"wv_zero", wv_zero, VA_WV_ZERO_OUT, 3, 0},
      {"wv_imm64", wv_imm, VA_WV_IMM_OUT, 7, IMM_STARBITE},
      {"wv_ts", wv_ts, VA_WV_TS_OUT, 2, 0},
      {"wv_cyc", wv_cyc, VA_WV_CYC_OUT, 1, 0},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(wv); i++) {
      memset(sg_data, 0, sizeof(sg_data));
      xnu_write_value(0, (uint32_t)(wv[i].addr - SG_VA), wv[i].type,
                      wv[i].value);
      xnu_diff(wv[i].name, wv[i].mesa, sg_data, 16);
   }
   memset(sg_data, 0, sizeof(sg_data));
   xnu_clear(VA_RT_BASE, 64, 64, 256, 0xff00ff00u);
   xnu_diff("frag", frag, sg_data + SG_FRAG_OFF, 16);
   xnu_diff("fbd", fbd, sg_data + SG_FBD_OFF, FBD_WORDS);
   xnu_diff("samples", samples, sg_data + SG_SAMPLES_OFF, SAMPLE_WORDS);
   memset(sg_data, 0, sizeof(sg_data));
   xnu_clear(VA_RT2_BASE, 32, 32, 5760, 0xff3366ccu);
   xnu_diff("fbd2", fbd2, sg_data + SG_FBD_OFF, FBD_WORDS);

   free(lut);
   return 0;
}
