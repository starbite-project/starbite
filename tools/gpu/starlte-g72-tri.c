/*
 * Project Starbite: the iOS kernel's first Mali-G72 draw, built offline with
 * Mesa 25.0.7 (MIT) so every descriptor is packed by Mesa's own v6 genxml
 * pack macros and the fragment shader comes from Mesa's Bifrost compiler.
 *
 * One tiler job (no vertex job: screen-space positions straight into the
 * tiler, as Mesa's fb preload does on Midgard) and one fragment job that
 * clears a 512x512 render target on the panel to dark grey and shades a
 * magenta triangle. Output: a C header with the descriptor/shader blob and
 * its GPU addresses, for xnu/src/starlte_gpu.c. Both job chains are run
 * through pandecode first.
 *
 * Built and run in the lima VM by tools/gpu/build-g72-tri.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir_builder.h"
#include "panfrost/lib/pan_shader.h"
#include "panfrost/lib/pan_encoder.h"
#include "panfrost/lib/pan_util.h"
#include "panfrost/lib/pan_format.h"
#include "panfrost/lib/wrap.h"
#include "panfrost/compiler/bifrost/disassemble.h"
#include "util/u_dynarray.h"
#include "genxml/gen_macros.h"

#define GPU_ID          0x6221u         /* Mali-G72 product ID (GPU_ID >> 16; r0p1 here) */

/* Must match xnu/src/starlte_gpu.c. */
#define BLOB_VA         0x02008000ull   /* SG_VA + data offset 0x8000 */
#define BLOB_BYTES      0x2000u
#define HEAP_VA         0x10000000ull
#define HEAP_BYTES      0x01000000u     /* 16 MiB, pre-mapped (no growable heap) */
#define PANEL_VA        0x04000000ull
#define PANEL_STRIDE    5760u
#define RT_X            464u            /* (1440 - 512) / 2 */
#define RT_Y            1216u           /* about (2960 - 512) / 2, 16-aligned */
#define FB_W            512u
#define FB_H            512u

/* Blob layout (offsets from BLOB_VA). */
#define O_TILER_JOB     0x000u
#define O_FRAG_JOB      0x100u
#define O_FBD           0x200u          /* Framebuffer 128 B + one RT 64 B */
#define O_TILER_CTX     0x300u
#define O_TILER_HEAP    0x3c0u
#define O_RSD           0x400u          /* Renderer State 64 B + Blend 16 B */
#define O_VIEWPORT      0x480u
#define O_TLS           0x4c0u
#define O_POS           0x500u
#define O_SAMPLES       0x540u          /* 256 B */
#define O_SHADER        0x800u

static uint8_t blob[BLOB_BYTES];

static void *
at(unsigned off)
{
   return blob + off;
}

static uint32_t
fbits(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

static void
compile_fs(struct util_dynarray *bin, struct pan_shader_info *info)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(), "starbite_tri");
   nir_def *magenta = nir_imm_vec4(&b, 1.0, 0.0, 1.0, 1.0);
   nir_store_output(&b, magenta, nir_imm_int(&b, 0), .base = 0,
                    .src_type = nir_type_float32,
                    .io_semantics.location = FRAG_RESULT_DATA0,
                    .io_semantics.num_slots = 1, .write_mask = 0xf);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = GPU_ID,
      .is_blit = true,          /* no ATEST: Z/S forced early, as Mesa's preload */
      .no_idvs = true,
   };
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   pan_shader_preprocess(b.shader, inputs.gpu_id);
   util_dynarray_init(bin, NULL);
   GENX(pan_shader_compile)(b.shader, &inputs, bin, info);
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s OUT.h\n", argv[0]);
      return 1;
   }
   glsl_type_singleton_init_or_ref();

   struct util_dynarray bin;
   struct pan_shader_info info;
   memset(&info, 0, sizeof(info));
   compile_fs(&bin, &info);
   if (O_SHADER + bin.size > BLOB_BYTES) {
      fprintf(stderr, "shader too big: %u\n", bin.size);
      return 1;
   }
   memcpy(at(O_SHADER), bin.data, bin.size);
   fprintf(stderr, "fragment shader: %u bytes, work regs %u, preload %#llx\n", bin.size,
           info.work_reg_count, (unsigned long long)info.preload);
   /* The listing: BIFROST_MESA_DEBUG=shaders (the disassembler trips on the
    * prefetch padding after the last clause). */

   /* Positions: screen space within the 512x512 render target, w = 1. */
   float pos[] = {
      256, 48, 0, 1,
      464, 464, 0, 1,
      48, 464, 0, 1,
   };
   memcpy(at(O_POS), pos, sizeof(pos));

   /* Single-sampled positions (pan_samples.c): sample 0 and origin. */
   uint16_t *sp = at(O_SAMPLES);
   sp[0] = sp[1] = 128;
   sp[64] = sp[65] = 128;

   pan_cast_and_pack(at(O_TLS), LOCAL_STORAGE, cfg) {
      cfg.tls_size = 0;
   }

   pan_cast_and_pack(at(O_VIEWPORT), VIEWPORT, cfg) {
      cfg.scissor_minimum_x = 0;
      cfg.scissor_minimum_y = 0;
      cfg.scissor_maximum_x = FB_W - 1;
      cfg.scissor_maximum_y = FB_H - 1;
   }

   /* Renderer State + Blend, as pan_preload_emit_rsd / _blend (colour only). */
   pan_cast_and_pack(at(O_RSD), RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(&info, BLOB_VA + O_SHADER, &cfg);
      cfg.multisample_misc.sample_mask = 0xFFFF;
      cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
      cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
      cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.mask = 0xFF;
      cfg.stencil_back = cfg.stencil_front;
      cfg.properties.zs_update_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.allow_forward_pixel_to_kill = true;
      cfg.properties.allow_forward_pixel_to_be_killed = true;
   }
   pan_cast_and_pack(at(O_RSD + pan_size(RENDERER_STATE)), BLEND, cfg) {
      cfg.round_to_fb_precision = true;
      cfg.srgb = false;
      cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
      cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.color_mask = 0xf;
      cfg.internal.fixed_function.num_comps = 4;
      cfg.internal.fixed_function.conversion.memory_format =
         GENX(panfrost_dithered_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM, false);
      cfg.internal.fixed_function.conversion.register_format =
         MALI_REGISTER_FILE_FORMAT_F32;
      cfg.internal.fixed_function.rt = 0;
   }

   /* Tiler heap and context, as jm_emit_tiler_desc(). */
   pan_cast_and_pack(at(O_TILER_HEAP), TILER_HEAP, heap) {
      heap.size = HEAP_BYTES;
      heap.base = HEAP_VA;
      heap.bottom = HEAP_VA;
      heap.top = HEAP_VA + HEAP_BYTES;
   }
   unsigned max_levels = (0x809 >> 8) & 0xf;   /* TILER_FEATURES */
   pan_cast_and_pack(at(O_TILER_CTX), TILER_CONTEXT, tiler) {
      tiler.hierarchy_mask = pan_select_tiler_hierarchy_mask(FB_W, FB_H, max_levels);
      tiler.fb_width = FB_W;
      tiler.fb_height = FB_H;
      tiler.heap = BLOB_VA + O_TILER_HEAP;
      tiler.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
   }

   /* Tiler job: header, invocation (3 vertices), primitive, size, tiler, draw. */
   void *tj = at(O_TILER_JOB);
   pan_section_pack(tj, TILER_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_TILER;
      cfg.index = 1;
   }
   panfrost_pack_work_groups_compute(pan_section_ptr(tj, TILER_JOB, INVOCATION), 1, 3, 1, 1,
                                     1, 1, true, false);
   pan_section_pack(tj, TILER_JOB, PRIMITIVE, cfg) {
      cfg.draw_mode = MALI_DRAW_MODE_TRIANGLES;
      cfg.index_count = 3;
      cfg.job_task_split = 6;
   }
   pan_section_pack(tj, TILER_JOB, PRIMITIVE_SIZE, cfg) {
      cfg.constant = 1.0f;
   }
   pan_section_pack(tj, TILER_JOB, TILER, cfg) {
      cfg.address = BLOB_VA + O_TILER_CTX;
   }
   pan_section_pack(tj, TILER_JOB, PADDING, cfg)
      ;
   pan_section_pack(tj, TILER_JOB, DRAW, cfg) {
      cfg.position = BLOB_VA + O_POS;
      cfg.state = BLOB_VA + O_RSD;
      cfg.viewport = BLOB_VA + O_VIEWPORT;
      cfg.thread_storage = BLOB_VA + O_TLS;
   }

   /* Framebuffer: as GENX(pan_emit_fbd) for one RGBA8 linear target with a
    * clear, 16x16 tiles, 1 KiB colour buffer, the tiler context above. */
   void *fbd = at(O_FBD);
   pan_section_pack(fbd, FRAMEBUFFER, PARAMETERS, cfg) {
      cfg.sample_locations = BLOB_VA + O_SAMPLES;
      cfg.tiler = BLOB_VA + O_TILER_CTX;
      cfg.width = FB_W;
      cfg.height = FB_H;
      cfg.bound_max_x = FB_W - 1;
      cfg.bound_max_y = FB_H - 1;
      cfg.effective_tile_size = 16 * 16;
      cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
      cfg.render_target_count = 1;
      cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;
      cfg.color_buffer_allocation = 1024;
      cfg.sample_count = 1;
      cfg.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
   }
   pan_section_pack(fbd, FRAMEBUFFER, PADDING, cfg)
      ;
   pan_cast_and_pack(at(O_FBD + pan_size(FRAMEBUFFER)), RENDER_TARGET, cfg) {
      cfg.clean_pixel_write_enable = true;
      cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
      cfg.write_enable = true;
      cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
      cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
      cfg.dithering_enable = true;
      cfg.swizzle = 0x688;      /* R G B A */
      cfg.rgb.base = PANEL_VA + (uint64_t)RT_Y * PANEL_STRIDE + RT_X * 4u;
      cfg.rgb.row_stride = PANEL_STRIDE;
      cfg.clear.color_0 = 0xff262626;   /* dark grey */
   }

   void *fj = at(O_FRAG_JOB);
   pan_section_pack(fj, FRAGMENT_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_FRAGMENT;
      cfg.index = 1;
   }
   pan_section_pack(fj, FRAGMENT_JOB, PAYLOAD, cfg) {
      cfg.bound_max_x = (FB_W - 1) >> MALI_TILE_SHIFT;
      cfg.bound_max_y = (FB_H - 1) >> MALI_TILE_SHIFT;
      cfg.framebuffer = (BLOB_VA + O_FBD) | 1;  /* MFBD, 1 RT, no ZS/CRC */
   }

   /* Check both chains with pandecode (blob, heap and panel as dummies). */
   struct pandecode_context *ctx = pandecode_create_context(true);
   static uint8_t heap_dummy[0x10000], panel_dummy[0x10000];
   pandecode_inject_mmap(ctx, BLOB_VA, blob, BLOB_BYTES, "starbite blob");
   pandecode_inject_mmap(ctx, HEAP_VA, heap_dummy, sizeof(heap_dummy), "heap");
   pandecode_inject_mmap(ctx, PANEL_VA, panel_dummy, sizeof(panel_dummy), "panel");
   fprintf(stderr, "==== tiler chain ====\n");
   pandecode_jc(ctx, BLOB_VA + O_TILER_JOB, GPU_ID);
   fprintf(stderr, "==== fragment chain ====\n");
   pandecode_jc(ctx, BLOB_VA + O_FRAG_JOB, GPU_ID);

   FILE *f = fopen(argv[1], "w");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fprintf(f, "/* Generated by tools/gpu/starlte-g72-tri.c (Mesa 25.0.7, MIT). Do not edit. */\n");
   fprintf(f, "#define SG_TRI_BLOB_VA     0x%llxull\n", (unsigned long long)BLOB_VA);
   fprintf(f, "#define SG_TRI_HEAP_VA     0x%llxull\n", (unsigned long long)HEAP_VA);
   fprintf(f, "#define SG_TRI_HEAP_BYTES  0x%xu\n", HEAP_BYTES);
   fprintf(f, "#define SG_TRI_TILER_JOB   0x%xu\n", O_TILER_JOB);
   fprintf(f, "#define SG_TRI_FRAG_JOB    0x%xu\n", O_FRAG_JOB);
   fprintf(f, "#define SG_TRI_RT_X        %uu\n", RT_X);
   fprintf(f, "#define SG_TRI_RT_Y        %uu\n", RT_Y);
   fprintf(f, "#define SG_TRI_FB_W        %uu\n", FB_W);
   fprintf(f, "#define SG_TRI_FB_H        %uu\n", FB_H);
   fprintf(f, "#define SG_TRI_COLOR       0xff%02x%02x%02xu\n", 255, 0, 255);
   fprintf(f, "#define SG_TRI_CLEAR       0xff262626u\n");
   unsigned words = (O_SHADER + bin.size + 3) / 4;
   fprintf(f, "#define SG_TRI_BLOB_WORDS  %uu\n", words);
   fprintf(f, "static const uint32_t sg_tri_blob[SG_TRI_BLOB_WORDS] = {");
   for (unsigned i = 0; i < words; i++) {
      uint32_t w;
      memcpy(&w, blob + i * 4, 4);
      fprintf(f, "%s0x%08x,", i % 6 ? " " : "\n\t", w);
   }
   fprintf(f, "\n};\n");
   fclose(f);
   fprintf(stderr, "wrote %s (%u words)\n", argv[1], words);
   (void)fbits;
   return 0;
}
