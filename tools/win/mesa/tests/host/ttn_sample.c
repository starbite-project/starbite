/* SPDX-License-Identifier: MIT */
/* ttn_sample: host test for sb-patches/0114 (docs/WIN11-GPU.md 6.23). TGSI shaders using the D3D10 texture
 * opcodes, as d3d10umd emits them, through tgsi_to_nir and Panfrost's whole compile pipeline for the S9's
 * Mali-G72 (arch 6), natively in the lima VM. Checks that each compiles, and prints the NIR texture
 * instructions: separate texture and sampler indices, and the view swizzle applied.
 *   build: tools/win/mesa/tests/host/build-ttn-sample.sh */
#include <stdio.h>
#include <string.h>
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_text.h"
#include "nir/tgsi_to_nir.h"
#include "nir.h"
#include "nir_builder.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/model/pan_model.h"
#include "util/u_dynarray.h"

struct panfrost_sysvals;
bool panfrost_nir_lower_sysvals(nir_shader *s, unsigned arch, struct panfrost_sysvals *sysvals);
bool panfrost_nir_lower_res_indices(nir_shader *shader, uint64_t gpu_id);

static const char *shaders[][2] = {
   { "control: legacy TEX (the T14/T15 path)",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SAMP[0]\n"
     "DCL TEMP[0]\n"
     "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
     "  1: MOV OUT[0], TEMP[0]\n"
     "  2: END\n" },
   { "two textures, one sampler, view swizzle (Direct2D text)",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SAMP[0]\n"
     "DCL SVIEW[0], 2D, FLOAT\n"
     "DCL SVIEW[1], 2D, FLOAT\n"
     "DCL TEMP[0..1]\n"
     "  0: SAMPLE TEMP[0], IN[0], SVIEW[0], SAMP[0]\n"
     "  1: SAMPLE TEMP[1], IN[0], SVIEW[1].wwww, SAMP[0]\n"
     "  2: MUL OUT[0], TEMP[0], TEMP[1]\n"
     "  3: END\n" },
   { "texture 2 with sampler 1, B/L/D",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SAMP[1]\n"
     "DCL SVIEW[2], 2D, FLOAT\n"
     "DCL TEMP[0..3]\n"
     "IMM[0] FLT32 { 0.5, 1.0, 0.0, 0.0 }\n"
     "  0: SAMPLE_B TEMP[0], IN[0], SVIEW[2], SAMP[1], IMM[0].xxxx\n"
     "  1: SAMPLE_L TEMP[1], IN[0], SVIEW[2].zyxw, SAMP[1], IMM[0].yyyy\n"
     "  2: SAMPLE_D TEMP[2], IN[0], SVIEW[2], SAMP[1], IMM[0], IMM[0]\n"
     "  3: ADD TEMP[3], TEMP[0], TEMP[1]\n"
     "  4: ADD OUT[0], TEMP[3], TEMP[2]\n"
     "  5: END\n" },
   { "compare (SAMPLE_C, SAMPLE_C_LZ)",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SAMP[0]\n"
     "DCL SVIEW[0], 2D, FLOAT\n"
     "DCL TEMP[0..1]\n"
     "IMM[0] FLT32 { 0.5, 1.0, 0.0, 0.0 }\n"
     "  0: SAMPLE_C TEMP[0], IN[0], SVIEW[0].xxxx, SAMP[0], IMM[0].xxxx\n"
     "  1: SAMPLE_C_LZ TEMP[1], IN[0], SVIEW[0].xxxx, SAMP[0], IMM[0].xxxx\n"
     "  2: ADD OUT[0], TEMP[0], TEMP[1]\n"
     "  3: END\n" },
   { "control: legacy TXF",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SAMP[0]\n"
     "DCL TEMP[0]\n"
     "IMM[0] UINT32 { 3, 4, 0, 0 }\n"
     "  0: TXF TEMP[0], IMM[0], SAMP[0], 2D\n"
     "  1: MOV OUT[0], TEMP[0]\n"
     "  2: END\n" },
   { "load (SAMPLE_I, 2D)",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SVIEW[0], 2D, FLOAT\n"
     "DCL TEMP[0]\n"
     "IMM[0] UINT32 { 3, 4, 0, 0 }\n"
     "  0: SAMPLE_I TEMP[0], IMM[0], SVIEW[0]\n"
     "  1: MOV OUT[0], TEMP[0]\n"
     "  2: END\n" },
   { "size (SVIEWINFO, 2D): NIR only (texture sizes are system values the gallium driver lowers; legacy TXQ is the same)",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SVIEW[0], 2D, FLOAT\n"
     "DCL TEMP[0]\n"
     "IMM[0] UINT32 { 3, 4, 0, 0 }\n"
     "  0: SVIEWINFO TEMP[0], IMM[0].zzzz, SVIEW[0]\n"
     "  1: U2F OUT[0], TEMP[0]\n"
     "  2: END\n" },
   { "load from a buffer view (SAMPLE_I, BUFFER)",
     "FRAG\n"
     "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
     "DCL OUT[0], COLOR\n"
     "DCL SVIEW[1], BUFFER, FLOAT\n"
     "DCL TEMP[0]\n"
     "IMM[0] UINT32 { 3, 4, 0, 0 }\n"
     "  0: SAMPLE_I TEMP[0], IMM[0].xxxx, SVIEW[1]\n"
     "  1: MOV OUT[0], TEMP[0]\n"
     "  2: END\n" },
   { "flat integer varying (Direct2D's colour bits, sb-patches/0117)",
     "FRAG\n"
     "DCL IN[0], GENERIC[2], CONSTANT\n"
     "DCL OUT[0], COLOR\n"
     "DCL TEMP[0]\n"
     "IMM[0] UINT32 { 255, 8, 16, 0 }\n"
     "  0: AND TEMP[0].x, IN[0].xxxx, IMM[0].xxxx\n"
     "  1: USHR TEMP[0].y, IN[0].xxxx, IMM[0].yyyy\n"
     "  2: AND TEMP[0].y, TEMP[0].yyyy, IMM[0].xxxx\n"
     "  3: U2F TEMP[0].xy, TEMP[0].xyyy\n"
     "  4: MOV OUT[0], TEMP[0].xyxy\n"
     "  5: END\n" },
   { "control: vertex shader, legacy TXL: NIR only (this harness has no varying layout; the backend crashes on any VS)",
     "VERT\n"
     "DCL IN[0]\n"
     "DCL OUT[0], POSITION\n"
     "DCL SAMP[0]\n"
     "DCL TEMP[0]\n"
     "  0: TXL TEMP[0], IN[0], SAMP[0], 2D\n"
     "  1: ADD OUT[0], IN[0], TEMP[0]\n"
     "  2: END\n" },
   { "vertex shader fetch (SAMPLE becomes lod 0): NIR only",
     "VERT\n"
     "DCL IN[0]\n"
     "DCL OUT[0], POSITION\n"
     "DCL SAMP[0]\n"
     "DCL SVIEW[3], 2D, FLOAT\n"
     "DCL TEMP[0]\n"
     "  0: SAMPLE TEMP[0], IN[0], SVIEW[3], SAMP[0]\n"
     "  1: ADD OUT[0], IN[0], TEMP[0]\n"
     "  2: END\n" },
};

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type, false);
}

static int
print_tex(nir_shader *s)
{
   int n = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            printf("    tex op %d texture_index %u sampler_index %u shadow %d dim %d srcs %u\n", t->op,
                   t->texture_index, t->sampler_index, t->is_shadow, t->sampler_dim, t->num_srcs);
            n++;
         }
      }
   }
   return n;
}

int
main(void)
{
   const uint64_t gpu_id = 0x62210000ull;   /* Mali-G72 (S9: GPU_ID 0x62210010) */
   unsigned arch = pan_arch(gpu_id);
   int fails = 0;

   setvbuf(stdout, NULL, _IONBF, 0);
   glsl_type_singleton_init_or_ref();
   printf("arch %u\n", arch);
   for (unsigned i = 0; i < sizeof(shaders) / sizeof(shaders[0]); i++) {
      struct tgsi_token tokens[1024];
      printf("== %s\n", shaders[i][0]);
      if (!tgsi_text_translate(shaders[i][1], tokens, 1024)) {
         printf("  FAIL: TGSI text did not parse\n");
         fails++;
         continue;
      }
      mesa_shader_stage stage = strncmp(shaders[i][1], "VERT", 4) == 0 ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;
      nir_shader *s = tgsi_to_nir_noscreen(tokens, pan_get_nir_shader_compiler_options(arch, stage, false));
      nir_validate_shader(s, "ttn_sample");
      /* what tgsi_to_nir's ttn_finalize_nir does for Panfrost (no finalize_nir hook, integers, samplers as
       * indices) */
      NIR_PASS(_, s, nir_lower_continue_constructs);
      NIR_PASS(_, s, nir_lower_returns);
      NIR_PASS(_, s, nir_lower_vars_to_ssa);
      NIR_PASS(_, s, nir_lower_reg_intrinsics_to_ssa);
      NIR_PASS(_, s, nir_lower_global_vars_to_local);
      NIR_PASS(_, s, nir_split_var_copies);
      NIR_PASS(_, s, nir_lower_var_copies);
      NIR_PASS(_, s, nir_lower_system_values);
      NIR_PASS(_, s, nir_lower_compute_system_values, NULL);
      if (s->options->lower_uniforms_to_ubo)
         NIR_PASS(_, s, nir_lower_uniforms_to_ubo, false, false);
      NIR_PASS(_, s, nir_lower_samplers);
      NIR_PASS(_, s, nir_opt_dce);
      nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
      if (s->info.stage == MESA_SHADER_FRAGMENT &&
          s->info.outputs_written & BITFIELD_BIT(FRAG_RESULT_COLOR))
         NIR_PASS(_, s, nir_lower_fragcolor, 8);
      pan_preprocess_nir(s, gpu_id);
      NIR_PASS(_, s, nir_lower_indirect_derefs_to_if_else_trees, nir_var_shader_in | nir_var_shader_out, UINT32_MAX);
      NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out, glsl_type_size,
               nir_lower_io_use_interpolated_input_intrinsics);
      NIR_PASS(_, s, nir_opt_constant_folding);
      nir_foreach_function_impl(impl, s) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic == nir_intrinsic_load_input)
                  printf("    load_input base %u type 0x%x (before Panfrost's varying collection)\n",
                         nir_intrinsic_base(in), (unsigned)nir_intrinsic_dest_type(in));
            }
         }
      }
      printf("  textures_used 0x%x samplers_used 0x%x\n", (unsigned)s->info.textures_used[0],
             (unsigned)s->info.samplers_used[0]);
      print_tex(s);
      if (strstr(shaders[i][0], "NIR only")) {
         printf("  PASS (NIR): validated\n");
         ralloc_free(s);
         continue;
      }
      struct pan_compile_inputs inputs = { .gpu_id = gpu_id };
      struct pan_shader_info info = {0};
      NIR_PASS(_, s, panfrost_nir_lower_res_indices, gpu_id);
      pan_postprocess_nir(s, &inputs, &info);
      char sysvals[4096] = {0};
      NIR_PASS(_, s, panfrost_nir_lower_sysvals, arch, (struct panfrost_sysvals *)sysvals);
      inputs.fau.pushable_ubos = 1;
      inputs.fau.promote_immediates = true;
      struct util_dynarray bin = UTIL_DYNARRAY_INIT;
      pan_shader_compile(s, &inputs, &bin, &info);
      printf("  %s: %u bytes of Bifrost code, texture_count %u\n", bin.size ? "PASS" : "FAIL",
             (unsigned)bin.size, info.texture_count);
      if (!bin.size)
         fails++;
      util_dynarray_fini(&bin);
      ralloc_free(s);
   }
   printf("%d failure(s)\n", fails);
   return fails;
}
