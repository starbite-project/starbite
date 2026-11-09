#if 0
# Starbite OS 1.2-alpha15: an arithmetic-bound Mali-G72 compute job for the GPU heat, compiled and packed by Mesa.
# This file is also a bash script that builds and runs itself in the Lima VM (the Mesa 25.0.7 build mesa-golden uses):
#   limactl shell starbite -- bash <path-to>/starbite/tools/mesa-alujob.c <out.h>
# It writes a C header: the Bifrost binary of a compute shader (an FMA loop per thread, one store at the end), and
# Mesa's own v6 RENDERER_STATE, LOCAL_STORAGE and COMPUTE_JOB words at fixed GPU addresses inside gpuheat's region.
set -euo pipefail
SRC=$(readlink -f "$0")
OUTH=${1:-/dev/stdout}
B=$HOME/mesa/build
OUT=$HOME/mesa/starbite-lab
mkdir -p "$OUT"
FLAGS=$(python3 - "$B" <<"PY"
import json, shlex, sys
b = sys.argv[1]
for e in json.load(open(b + "/compile_commands.json")):
    if e["output"].endswith("libpan-arch-v6.a.p/pan_shader.c.o"):
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
	src/panfrost/compiler/libpanfrost_bifrost_disasm.a src/panfrost/compiler/valhall/libpanfrost_valhall_disasm.a src/panfrost/midgard/libpanfrost_midgard_disasm.a
	src/panfrost/util/libpanfrost_util.a src/panfrost/shared/libpanfrost_shared.a
	src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a
	src/util/libmesa_util_sse41.a src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a)
L=""
for l in "${LIBS[@]}"; do [ -f "$B/$l" ] && L="$L $B/$l"; done
eval "gcc $FLAGS -Wno-error $SRC -o $OUT/mesa-alujob -Wl,--start-group $L -Wl,--end-group -lm -lpthread -lstdc++ -lz -lexpat -ldrm"
"$OUT/mesa-alujob" > "$OUTH"
echo "mesa-alujob: wrote $OUTH" >&2
exit 0
#endif

#ifndef PAN_ARCH
#define PAN_ARCH 6
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genxml/gen_macros.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include "pan_encoder.h"
#include "pan_shader.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
void disassemble_bifrost(FILE *fp, const void *code, size_t size, bool verbose);

#define GPU_ID_G72 0x6221u          /* the product id (pan_props.c MODEL 0x6221 "G72"), as mesa-golden: pan_arch() = id >> 12 */
/* gpuheat's GPU addresses (SB_GPU_VA 0x02000000 + offset): its clear job uses 0xC000 (samples), 0x10000 (FBD),
 * 0x20000.. (jobs), 0x100000 (the render target, 4 MiB); the compute job's shader, state and storage below 0xC000,
 * its output in the render target's first MiB, its chain where the clear jobs go */
#define VA_BASE 0x02000000ull
#define O_SHADER 0x1000u
#define O_RSD 0x6000u
#define O_TLS 0x6100u
#define O_OUT 0x100000u
#define WG_SIZE 256u
#define WG_COUNT 1024u
#define ITER 256u                   /* loop turns; 16 vec4 FMAs each: 64 fp32 FMAs */
#define FMA_A 0.99999f
#define FMA_B 0.00001f

static void words(const char *name, const void *p, unsigned bytes)
{
   const uint8_t *b = p;
   printf("static const uint32_t %s[%u] = {", name, (bytes + 3) / 4);
   for (unsigned i = 0; i < bytes; i += 4) {
      uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < bytes; ++k)
         w |= (uint32_t)b[i + k] << (8 * k);
      printf("%s0x%08xu,", i % 32 ? " " : "\n    ", w);
   }
   printf("\n};\n");
}

int main(void)
{
   glsl_type_singleton_init_or_ref();
   const nir_shader_compiler_options *opt = GENX(pan_shader_get_compiler_options)();
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opt, "starbite-alu");
   b.shader->info.workgroup_size[0] = WG_SIZE;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_def *lid = nir_load_local_invocation_index(&b);
   nir_def *wg = nir_channel(&b, nir_load_workgroup_id(&b), 0);
   nir_def *idx = nir_iadd(&b, nir_imul_imm(&b, wg, WG_SIZE), lid);
   nir_def *slot = nir_iand_imm(&b, idx, 0xffff);
   /* the start depends on the output slot only, so every writer of a slot stores the same value */
   nir_def *x0 = nir_fadd_imm(&b, nir_fmul_imm(&b, nir_u2f32(&b, slot), 1e-7), 1.0);
   nir_def *init = nir_vec4(&b, x0, nir_fadd_imm(&b, x0, 0.25), nir_fadd_imm(&b, x0, 0.5), nir_fadd_imm(&b, x0, 0.75));
   nir_variable *vv = nir_local_variable_create(b.impl, glsl_vec4_type(), "v");
   nir_variable *ii = nir_local_variable_create(b.impl, glsl_uint_type(), "i");
   nir_store_var(&b, vv, init, 0xf);
   nir_store_var(&b, ii, nir_imm_int(&b, 0), 1);
   nir_loop *loop = nir_push_loop(&b);
   {
      nir_def *i = nir_load_var(&b, ii);
      nir_push_if(&b, nir_uge_imm(&b, i, ITER));
      nir_jump(&b, nir_jump_break);
      nir_pop_if(&b, NULL);
      nir_def *v = nir_load_var(&b, vv);
      nir_def *a = nir_imm_vec4(&b, FMA_A, FMA_A, FMA_A, FMA_A);
      nir_def *c = nir_imm_vec4(&b, FMA_B, FMA_B, FMA_B, FMA_B);
      for (unsigned k = 0; k < 16; ++k)
         v = nir_ffma(&b, v, a, c);
      nir_store_var(&b, vv, v, 0xf);
      nir_store_var(&b, ii, nir_iadd_imm(&b, i, 1), 1);
   }
   nir_pop_loop(&b, loop);
   nir_def *addr = nir_iadd(&b, nir_imm_int64(&b, (int64_t)(VA_BASE + O_OUT)),
                            nir_u2u64(&b, nir_imul_imm(&b, slot, 16)));
   nir_build_store_global(&b, nir_load_var(&b, vv), addr, .align_mul = 16, .write_mask = 0xf);

   nir_shader *s = b.shader;
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   NIR_PASS(_, s, nir_lower_compute_system_values, NULL);
   pan_shader_preprocess(s, GPU_ID_G72);

   struct panfrost_compile_inputs in = {.gpu_id = GPU_ID_G72};
   struct util_dynarray bin;
   util_dynarray_init(&bin, NULL);
   struct pan_shader_info info;
   GENX(pan_shader_compile)(s, &in, &bin, &info);
   if (getenv("ALUJOB_DISASM"))
      disassemble_bifrost(stderr, bin.data, bin.size, false);
   if (info.tls_size || info.wls_size || info.ubo_count || info.push.count) {
      fprintf(stderr, "mesa-alujob: the shader needs TLS %u WLS %u UBOs %u push %u: not supported here\n",
              info.tls_size, info.wls_size, info.ubo_count, info.push.count);
      return 1;
   }
   if (bin.size > O_RSD - O_SHADER) {
      fprintf(stderr, "mesa-alujob: binary %u bytes is too big\n", bin.size);
      return 1;
   }

   struct mali_renderer_state_packed rsd;
   pan_pack(&rsd, RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(&info, VA_BASE + O_SHADER, &cfg);
   }
   struct mali_local_storage_packed tls;
   pan_pack(&tls, LOCAL_STORAGE, cfg) {
      cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
   }
   struct mali_compute_job_packed job;
   memset(&job, 0, sizeof(job));
   pan_section_pack(&job, COMPUTE_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_COMPUTE;
      cfg.index = 1;
   }
   panfrost_pack_work_groups_compute(pan_section_ptr(&job, COMPUTE_JOB, INVOCATION), WG_COUNT, 1, 1, WG_SIZE, 1, 1,
                                     false, false);
   pan_section_pack(&job, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = util_logbase2_ceil(WG_SIZE + 1) + util_logbase2_ceil(1 + 1) + util_logbase2_ceil(1 + 1);
   }
   pan_section_pack(&job, COMPUTE_JOB, DRAW, cfg) {
      cfg.state = VA_BASE + O_RSD;
      cfg.thread_storage = VA_BASE + O_TLS;
   }

   printf("/* Generated by starbite/tools/mesa-alujob.c with Mesa 25.0.7 (Bifrost v6, GPU %#x): do not edit. */\n",
          GPU_ID_G72);
   printf("#define GPUALU_O_SHADER 0x%xu\n#define GPUALU_O_RSD 0x%xu\n#define GPUALU_O_TLS 0x%xu\n", O_SHADER, O_RSD,
          O_TLS);
   printf("#define GPUALU_O_OUT 0x%xu\n#define GPUALU_WG_SIZE %uu\n#define GPUALU_WG_COUNT %uu\n#define GPUALU_ITER %uu\n",
          O_OUT, WG_SIZE, WG_COUNT, ITER);
   printf("#define GPUALU_FMA_A %.9gf\n#define GPUALU_FMA_B %.9gf\n", FMA_A, FMA_B);
   printf("#define GPUALU_JOB_BYTES %uu\n#define GPUALU_JOB_TYPE %uu\n", (unsigned)MALI_COMPUTE_JOB_LENGTH,
          (unsigned)MALI_JOB_TYPE_COMPUTE);
   printf("#define GPUALU_WORK_REGS %uu\n", info.work_reg_count);
   printf("/* FMAs a job: %llu */\n", (unsigned long long)WG_SIZE * WG_COUNT * ITER * 64ull);
   words("gpualu_shader", bin.data, bin.size);
   printf("#define GPUALU_SHADER_BYTES %uu\n", bin.size);
   words("gpualu_rsd", &rsd, sizeof(rsd));
   words("gpualu_tls", &tls, sizeof(tls));
   words("gpualu_job", &job, sizeof(job));
   return 0;
}
