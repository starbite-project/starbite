#if 0
# Starbite OS: the GPU compositor compute shaders (Mali-G72, Bifrost v6), compiled and packed by Mesa, as
# mesa-alujob.c. Run in the Lima VM:  limactl shell starbite -- bash <path-to>/starbite/tools/mesa-compjob.c <out.h>
# Writes a C header: each shader's Bifrost binary, RENDERER_STATE, the shared LOCAL_STORAGE, and one COMPUTE_JOB per
# dispatch size, at fixed GPU addresses in the data window (gpucomp.c's layout). Every shader reads its parameters
# from a fixed block in the data window (no push constants or UBOs: the packer here supports neither) and works on
# 0xAARRGGBB premultiplied pixels through global loads and stores (no texture units, tiler or fragment jobs).
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
eval "gcc $FLAGS -Wno-error $SRC -o $OUT/mesa-compjob -Wl,--start-group $L -Wl,--end-group -lm -lpthread -lstdc++ -lz -lexpat -ldrm"
"$OUT/mesa-compjob" > "$OUTH"
echo "mesa-compjob: wrote $OUTH" >&2
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

#define GPU_ID_G72 0x6221u
#define VA_BASE 0x02000000ull           /* SB_GPU_VA: the data window */
/* gpucomp's layout in the data window (offsets): shaders 8 KiB each, then the descriptors, then the parameter blocks */
#define O_SH(i) (0x1000u + (i) * 0x2000u)     /* five shaders: 0x1000 .. 0xb000 */
#define O_RSD(i) (0xb000u + (i) * 0x80u)
#define O_TLS 0xb800u
#define O_JOB(i) (0xc000u + (i) * 0x100u)
#define O_PAR(i) (0x10000u + (i) * 0x2000u)  /* FILL, COMP (64 quads of 80 bytes after 64), DOWN, UP; COMP2 last: */
#define PAR_COMP2_BYTES 0x4000u              /* 64 + 127 quads of 128 bytes */
#define WG 16u

enum { SH_FILL, SH_COMP, SH_DOWN, SH_UP, SH_COMP2, NSH };
static const char *const sh_name[NSH] = { "fill", "comp", "down", "up", "comp2" };

/* ---- helpers ------------------------------------------------------------------------------------------- */
/* Bifrost takes no 64-bit constants (drivers load addresses from memory; a constant address would be folded into
 * one): a parameter's address is its 32-bit constant plus a zero the compiler cannot fold (the workgroup id's top
 * bit), packed with a 32-bit high half. Every VA here is below 4 GiB. */
static nir_def *g_zero;
static nir_def *par_addr(nir_builder *b, unsigned sh, unsigned off)
{
   nir_def *lo = nir_iadd(b, nir_imm_int(b, (int)(uint32_t)(VA_BASE + O_PAR(sh) + off)), g_zero);
   return nir_pack_64_2x32_split(b, lo, nir_imm_int(b, 0));
}
static nir_def *ld32(nir_builder *b, nir_def *addr)
{
   return nir_build_load_global(b, 1, 32, addr, .align_mul = 4);
}
static nir_def *ldf(nir_builder *b, nir_def *addr)
{
   return ld32(b, addr);                /* float bits: the caller treats them as float */
}
static nir_def *ld64(nir_builder *b, nir_def *addr)
{
   nir_def *lo = ld32(b, addr), *hi = ld32(b, nir_iadd_imm(b, addr, 4));
   return nir_pack_64_2x32_split(b, lo, hi);
}
static nir_def *addr_at(nir_builder *b, nir_def *base, nir_def *index_px)
{
   return nir_iadd(b, base, nir_u2u64(b, nir_imul_imm(b, index_px, 4)));
}
/* 0xAARRGGBB -> vec4 (r, g, b, a) in 0..1 (premultiplied stays premultiplied) */
static nir_def *unpack(nir_builder *b, nir_def *p)
{
   nir_def *r = nir_iand_imm(b, nir_ushr_imm(b, p, 16), 0xff);
   nir_def *g = nir_iand_imm(b, nir_ushr_imm(b, p, 8), 0xff);
   nir_def *bl = nir_iand_imm(b, p, 0xff);
   nir_def *a = nir_ushr_imm(b, p, 24);
   return nir_fmul_imm(b, nir_u2f32(b, nir_vec4(b, r, g, bl, a)), 1.0 / 255.0);
}
static nir_def *pack(nir_builder *b, nir_def *c)
{
   nir_def *u = nir_f2u32(b, nir_fadd_imm(b, nir_fmul_imm(b, nir_fsat(b, c), 255.0), 0.5));
   nir_def *r = nir_ishl_imm(b, nir_channel(b, u, 0), 16), *g = nir_ishl_imm(b, nir_channel(b, u, 1), 8);
   nir_def *a = nir_ishl_imm(b, nir_channel(b, u, 3), 24);
   return nir_ior(b, nir_ior(b, r, g), nir_ior(b, nir_channel(b, u, 2), a));
}
/* the thread's pixel (x, y) in a 16 x 16 workgroup grid */
static void thread_xy(nir_builder *b, nir_def **x, nir_def **y)
{
   nir_def *lid = nir_load_local_invocation_id(b), *wg = nir_load_workgroup_id(b);
   g_zero = nir_iand_imm(b, nir_channel(b, wg, 0), 0x80000000);
   *x = nir_iadd(b, nir_imul_imm(b, nir_channel(b, wg, 0), WG), nir_channel(b, lid, 0));
   *y = nir_iadd(b, nir_imul_imm(b, nir_channel(b, wg, 1), WG), nir_channel(b, lid, 1));
}
static nir_def *clampi(nir_builder *b, nir_def *v, nir_def *hi)
{
   return nir_imax(b, nir_imm_int(b, 0), nir_imin(b, v, nir_iadd_imm(b, hi, -1)));
}
static nir_def *fetch(nir_builder *b, nir_def *base, nir_def *stride, nir_def *w, nir_def *h, nir_def *ix, nir_def *iy)
{
   nir_def *cx = clampi(b, ix, w), *cy = clampi(b, iy, h);
   return unpack(b, ld32(b, addr_at(b, base, nir_iadd(b, nir_imul(b, cy, stride), cx))));
}

/* ---- FILL: [0..7] dst, [8] stride px, [12] w, [16] h, [20] colour ------------------------------------------ */
static void build_fill(nir_builder *b)
{
   nir_def *x, *y;
   thread_xy(b, &x, &y);
   nir_def *w = ld32(b, par_addr(b, SH_FILL, 12)), *h = ld32(b, par_addr(b, SH_FILL, 16));
   nir_push_if(b, nir_iand(b, nir_ult(b, x, w), nir_ult(b, y, h)));
   {
      nir_def *dst = ld64(b, par_addr(b, SH_FILL, 0)), *stride = ld32(b, par_addr(b, SH_FILL, 8));
      nir_def *col = ld32(b, par_addr(b, SH_FILL, 20));
      nir_build_store_global(b, col, addr_at(b, dst, nir_iadd(b, nir_imul(b, y, stride), x)), .align_mul = 4,
                             .write_mask = 1);
   }
   nir_pop_if(b, NULL);
}

/* ---- COMP: [0..7] dst, [8] stride, [12] w, [16] h, [20] n quads, [24] background; quads from 64, 80 bytes each:
 *   +0 src (64-bit), +8 stride px, +12 w, +16 h, +20 flags (1 solid, 2 bilinear), +24 solid colour, +28 alpha f,
 *   +32 dx0 f, +36 dy0 f (the destination point of the source's origin), +40 isx f, +44 isy f (source px per
 *   destination px), +48 clip x0, +52 y0, +56 x1, +60 y1 (floats, destination px), +64 radius f
 * Bottom to top, src-over, a rounded clip with a 1 px antialiased edge. ------------------------------------------ */
static void build_comp(nir_builder *b)
{
   nir_def *x, *y;
   thread_xy(b, &x, &y);
   nir_def *w = ld32(b, par_addr(b, SH_COMP, 12)), *h = ld32(b, par_addr(b, SH_COMP, 16));
   nir_push_if(b, nir_iand(b, nir_ult(b, x, w), nir_ult(b, y, h)));
   {
      nir_def *fx = nir_fadd_imm(b, nir_u2f32(b, x), 0.5), *fy = nir_fadd_imm(b, nir_u2f32(b, y), 0.5);
      nir_def *n = ld32(b, par_addr(b, SH_COMP, 20));
      nir_variable *acc = nir_local_variable_create(b->impl, glsl_vec4_type(), "acc");
      nir_variable *ii = nir_local_variable_create(b->impl, glsl_uint_type(), "i");
      nir_store_var(b, acc, unpack(b, ld32(b, par_addr(b, SH_COMP, 24))), 0xf);
      nir_store_var(b, ii, nir_imm_int(b, 0), 1);
      nir_loop *loop = nir_push_loop(b);
      {
         nir_def *i = nir_load_var(b, ii);
         nir_push_if(b, nir_uge(b, i, n));
         nir_jump(b, nir_jump_break);
         nir_pop_if(b, NULL);
         nir_store_var(b, ii, nir_iadd_imm(b, i, 1), 1);
         nir_def *q = nir_iadd(b, par_addr(b, SH_COMP, 64), nir_u2u64(b, nir_imul_imm(b, i, 80)));
         nir_def *cx0 = ldf(b, nir_iadd_imm(b, q, 48)), *cy0 = ldf(b, nir_iadd_imm(b, q, 52));
         nir_def *cx1 = ldf(b, nir_iadd_imm(b, q, 56)), *cy1 = ldf(b, nir_iadd_imm(b, q, 60));
         nir_def *in = nir_iand(b, nir_iand(b, nir_fge(b, fx, cx0), nir_flt(b, fx, cx1)),
                                nir_iand(b, nir_fge(b, fy, cy0), nir_flt(b, fy, cy1)));
         nir_push_if(b, in);
         {
            /* the rounded clip: a rounded rect's signed distance, coverage over its 1 px edge */
            nir_def *rad = ldf(b, nir_iadd_imm(b, q, 64));
            nir_def *hx = nir_fmul_imm(b, nir_fsub(b, cx1, cx0), 0.5), *hy = nir_fmul_imm(b, nir_fsub(b, cy1, cy0), 0.5);
            nir_def *px = nir_fabs(b, nir_fsub(b, fx, nir_fadd(b, cx0, hx)));
            nir_def *py = nir_fabs(b, nir_fsub(b, fy, nir_fadd(b, cy0, hy)));
            nir_def *qx = nir_fsub(b, px, nir_fsub(b, hx, rad)), *qy = nir_fsub(b, py, nir_fsub(b, hy, rad));
            nir_def *mx = nir_fmax(b, qx, nir_imm_float(b, 0)), *my = nir_fmax(b, qy, nir_imm_float(b, 0));
            nir_def *d = nir_fsub(b, nir_fadd(b, nir_fsqrt(b, nir_fadd(b, nir_fmul(b, mx, mx), nir_fmul(b, my, my))),
                                              nir_fmin(b, nir_fmax(b, qx, qy), nir_imm_float(b, 0))), rad);
            nir_def *cov = nir_bcsel(b, nir_flt_imm(b, rad, 0.01), nir_imm_float(b, 1),
                                     nir_fsat(b, nir_fsub(b, nir_imm_float(b, 0.5), d)));
            nir_def *flags = ld32(b, nir_iadd_imm(b, q, 20));
            nir_def *alpha = nir_fmul(b, ldf(b, nir_iadd_imm(b, q, 28)), cov);
            nir_variable *src = nir_local_variable_create(b->impl, glsl_vec4_type(), "src");
            nir_variable *hit = nir_local_variable_create(b->impl, glsl_bool_type(), "hit");
            nir_store_var(b, src, nir_imm_vec4(b, 0, 0, 0, 0), 0xf);
            nir_store_var(b, hit, nir_imm_true(b), 1);
            nir_push_if(b, nir_i2b(b, nir_iand_imm(b, flags, 1)));
            {
               nir_store_var(b, src, unpack(b, ld32(b, nir_iadd_imm(b, q, 24))), 0xf);
            }
            nir_push_else(b, NULL);
            {
               nir_def *base = ld64(b, q), *stride = ld32(b, nir_iadd_imm(b, q, 8));
               nir_def *sw = ld32(b, nir_iadd_imm(b, q, 12)), *sh = ld32(b, nir_iadd_imm(b, q, 16));
               nir_def *u = nir_fmul(b, nir_fsub(b, fx, ldf(b, nir_iadd_imm(b, q, 32))), ldf(b, nir_iadd_imm(b, q, 40)));
               nir_def *v = nir_fmul(b, nir_fsub(b, fy, ldf(b, nir_iadd_imm(b, q, 36))), ldf(b, nir_iadd_imm(b, q, 44)));
               nir_def *inside = nir_iand(b, nir_iand(b, nir_fge_imm(b, u, 0), nir_fge_imm(b, v, 0)),
                                          nir_iand(b, nir_flt(b, u, nir_u2f32(b, sw)), nir_flt(b, v, nir_u2f32(b, sh))));
               nir_store_var(b, hit, inside, 1);
               nir_push_if(b, inside);
               {
                  nir_push_if(b, nir_i2b(b, nir_iand_imm(b, flags, 2)));
                  {
                     /* bilinear: the four texels around (u - .5, v - .5), clamped to the edges */
                     nir_def *uu = nir_fadd_imm(b, u, -0.5), *vv = nir_fadd_imm(b, v, -0.5);
                     nir_def *u0 = nir_ffloor(b, uu), *v0 = nir_ffloor(b, vv);
                     nir_def *tu = nir_fsub(b, uu, u0), *tv = nir_fsub(b, vv, v0);
                     nir_def *iu = nir_f2i32(b, u0), *iv = nir_f2i32(b, v0);
                     nir_def *t00 = fetch(b, base, stride, sw, sh, iu, iv);
                     nir_def *t10 = fetch(b, base, stride, sw, sh, nir_iadd_imm(b, iu, 1), iv);
                     nir_def *t01 = fetch(b, base, stride, sw, sh, iu, nir_iadd_imm(b, iv, 1));
                     nir_def *t11 = fetch(b, base, stride, sw, sh, nir_iadd_imm(b, iu, 1), nir_iadd_imm(b, iv, 1));
                     nir_def *top = nir_flrp(b, t00, t10, tu), *bot = nir_flrp(b, t01, t11, tu);
                     nir_store_var(b, src, nir_flrp(b, top, bot, tv), 0xf);
                  }
                  nir_push_else(b, NULL);
                  {
                     nir_def *iu = nir_f2i32(b, u), *iv = nir_f2i32(b, v);
                     nir_store_var(b, src, unpack(b, ld32(b, addr_at(b, base, nir_iadd(b, nir_imul(b, iv, stride), iu)))),
                                   0xf);
                  }
                  nir_pop_if(b, NULL);
               }
               nir_pop_if(b, NULL);
            }
            nir_pop_if(b, NULL);
            nir_push_if(b, nir_load_var(b, hit));
            {
               nir_def *s = nir_fmul(b, nir_load_var(b, src), alpha);
               nir_def *a = nir_load_var(b, acc);
               nir_def *k = nir_fsub(b, nir_imm_float(b, 1), nir_channel(b, s, 3));
               nir_store_var(b, acc, nir_ffma(b, a, k, s), 0xf);
            }
            nir_pop_if(b, NULL);
         }
         nir_pop_if(b, NULL);
      }
      nir_pop_loop(b, loop);
      nir_def *dst = ld64(b, par_addr(b, SH_COMP, 0)), *stride = ld32(b, par_addr(b, SH_COMP, 8));
      nir_build_store_global(b, pack(b, nir_load_var(b, acc)), addr_at(b, dst, nir_iadd(b, nir_imul(b, y, stride), x)),
                             .align_mul = 4, .write_mask = 1);
   }
   nir_pop_if(b, NULL);
}

/* ---- COMP2: the toolkit's draw items (sbtk tk_comp.c draw_item), 128-byte quads from 64:
 *   +0 src, +8 stride px, +12 w, +16 h, +20 flags (1 solid, 2 bilinear, 4 glass: the backdrop through the 8-bit
 *   mask first, 8 fade, 16 3-slice vertical, 32 3-slice horizontal), +24 solid, +28 alpha, +32 dx0, +36 dy0,
 *   +40 isx, +44 isy, +48 clip x0, +52 y0, +56 x1, +60 y1, +64 radius, +68 fade top0, +72 top1, +76 bottom0,
 *   +80 bottom1 (screen px), +84 the slice's drawn length (source px), +88 s0, +92 s1, +96 mask (64-bit),
 *   +104 mask stride (bytes), +108 backdrop (64-bit), +116 backdrop stride px, +120 backdrop scale (backdrop px per
 *   screen px; nearest, as bw_row), +124 backdrop w | h << 16 ------------------------------------------------- */
static nir_def *slice_coord(nir_builder *b, nir_def *v, nir_def *len, nir_def *s0, nir_def *s1, nir_def *srch)
{
   nir_def *mid = nir_fsub(b, nir_fsub(b, len, s0), s1), *smid = nir_fsub(b, nir_fsub(b, srch, s0), s1);
   nir_def *k = nir_bcsel(b, nir_flt_imm(b, mid, 1e-3), nir_imm_float(b, 0), nir_fdiv(b, smid, mid));
   nir_def *m = nir_fadd(b, s0, nir_fmul(b, nir_fsub(b, v, s0), k));
   nir_def *tail = nir_fsub(b, srch, nir_fsub(b, len, v));
   return nir_bcsel(b, nir_flt(b, v, s0), v, nir_bcsel(b, nir_fge(b, v, nir_fsub(b, len, s1)), tail, m));
}

static void build_comp2(nir_builder *b)
{
   nir_def *x, *y;
   thread_xy(b, &x, &y);
   nir_def *w = ld32(b, par_addr(b, SH_COMP2, 12)), *h = ld32(b, par_addr(b, SH_COMP2, 16));
   nir_push_if(b, nir_iand(b, nir_ult(b, x, w), nir_ult(b, y, h)));
   {
      nir_def *fx = nir_fadd_imm(b, nir_u2f32(b, x), 0.5), *fy = nir_fadd_imm(b, nir_u2f32(b, y), 0.5);
      nir_def *n = ld32(b, par_addr(b, SH_COMP2, 20));
      nir_variable *acc = nir_local_variable_create(b->impl, glsl_vec4_type(), "acc");
      nir_variable *ii = nir_local_variable_create(b->impl, glsl_uint_type(), "i");
      nir_store_var(b, acc, unpack(b, ld32(b, par_addr(b, SH_COMP2, 24))), 0xf);
      nir_store_var(b, ii, nir_imm_int(b, 0), 1);
      nir_loop *loop = nir_push_loop(b);
      {
         nir_def *i = nir_load_var(b, ii);
         nir_push_if(b, nir_uge(b, i, n));
         nir_jump(b, nir_jump_break);
         nir_pop_if(b, NULL);
         nir_store_var(b, ii, nir_iadd_imm(b, i, 1), 1);
         nir_def *q = nir_iadd(b, par_addr(b, SH_COMP2, 64), nir_u2u64(b, nir_imul_imm(b, i, 128)));
         nir_def *cx0 = ldf(b, nir_iadd_imm(b, q, 48)), *cy0 = ldf(b, nir_iadd_imm(b, q, 52));
         nir_def *cx1 = ldf(b, nir_iadd_imm(b, q, 56)), *cy1 = ldf(b, nir_iadd_imm(b, q, 60));
         nir_def *in = nir_iand(b, nir_iand(b, nir_fge(b, fx, cx0), nir_flt(b, fx, cx1)),
                                nir_iand(b, nir_fge(b, fy, cy0), nir_flt(b, fy, cy1)));
         nir_push_if(b, in);
         {
            nir_def *rad = ldf(b, nir_iadd_imm(b, q, 64));
            nir_def *hx = nir_fmul_imm(b, nir_fsub(b, cx1, cx0), 0.5), *hy = nir_fmul_imm(b, nir_fsub(b, cy1, cy0), 0.5);
            nir_def *px = nir_fabs(b, nir_fsub(b, fx, nir_fadd(b, cx0, hx)));
            nir_def *py = nir_fabs(b, nir_fsub(b, fy, nir_fadd(b, cy0, hy)));
            nir_def *qx = nir_fsub(b, px, nir_fsub(b, hx, rad)), *qy = nir_fsub(b, py, nir_fsub(b, hy, rad));
            nir_def *mx = nir_fmax(b, qx, nir_imm_float(b, 0)), *my = nir_fmax(b, qy, nir_imm_float(b, 0));
            nir_def *d = nir_fsub(b, nir_fadd(b, nir_fsqrt(b, nir_fadd(b, nir_fmul(b, mx, mx), nir_fmul(b, my, my))),
                                              nir_fmin(b, nir_fmax(b, qx, qy), nir_imm_float(b, 0))), rad);
            nir_def *cov = nir_bcsel(b, nir_flt_imm(b, rad, 0.01), nir_imm_float(b, 1),
                                     nir_fsat(b, nir_fsub(b, nir_imm_float(b, 0.5), d)));
            nir_def *flags = ld32(b, nir_iadd_imm(b, q, 20));
            /* the fade (mask-image inside a clip): by the screen's y, as fade_at */
            nir_def *ft0 = ldf(b, nir_iadd_imm(b, q, 68)), *ft1 = ldf(b, nir_iadd_imm(b, q, 72));
            nir_def *fb0 = ldf(b, nir_iadd_imm(b, q, 76)), *fb1 = ldf(b, nir_iadd_imm(b, q, 80));
            nir_def *ftop = nir_bcsel(b, nir_flt(b, fy, ft1),
                                      nir_bcsel(b, nir_flt(b, ft0, ft1), nir_fsat(b, nir_fdiv(b, nir_fsub(b, fy, ft0), nir_fsub(b, ft1, ft0))),
                                                nir_bcsel(b, nir_flt(b, fy, ft0), nir_imm_float(b, 0), nir_imm_float(b, 1))),
                                      nir_imm_float(b, 1));
            nir_def *fbot = nir_bcsel(b, nir_flt(b, fb0, fy),
                                      nir_bcsel(b, nir_flt(b, fb0, fb1), nir_fsat(b, nir_fdiv(b, nir_fsub(b, fb1, fy), nir_fsub(b, fb1, fb0))),
                                                nir_bcsel(b, nir_flt(b, fb1, fy), nir_imm_float(b, 0), nir_imm_float(b, 1))),
                                      nir_imm_float(b, 1));
            nir_def *fade = nir_bcsel(b, nir_i2b(b, nir_iand_imm(b, flags, 8)), nir_fmul(b, ftop, fbot), nir_imm_float(b, 1));
            nir_def *alpha = nir_fmul(b, nir_fmul(b, ldf(b, nir_iadd_imm(b, q, 28)), cov), fade);
            nir_variable *src = nir_local_variable_create(b->impl, glsl_vec4_type(), "src");
            nir_variable *hit = nir_local_variable_create(b->impl, glsl_bool_type(), "hit");
            nir_store_var(b, src, nir_imm_vec4(b, 0, 0, 0, 0), 0xf);
            /* the source's own extent (a solid's too: its w x h): (u, v) in source px, a 3-slice mapped */
            nir_def *base = ld64(b, q), *stride = ld32(b, nir_iadd_imm(b, q, 8));
            nir_def *sw = ld32(b, nir_iadd_imm(b, q, 12)), *sh = ld32(b, nir_iadd_imm(b, q, 16));
            nir_def *u = nir_fmul(b, nir_fsub(b, fx, ldf(b, nir_iadd_imm(b, q, 32))), ldf(b, nir_iadd_imm(b, q, 40)));
            nir_def *v = nir_fmul(b, nir_fsub(b, fy, ldf(b, nir_iadd_imm(b, q, 36))), ldf(b, nir_iadd_imm(b, q, 44)));
            nir_def *len = ldf(b, nir_iadd_imm(b, q, 84)), *s0 = ldf(b, nir_iadd_imm(b, q, 88)), *s1 = ldf(b, nir_iadd_imm(b, q, 92));
            v = nir_bcsel(b, nir_i2b(b, nir_iand_imm(b, flags, 16)), slice_coord(b, v, len, s0, s1, nir_u2f32(b, sh)), v);
            u = nir_bcsel(b, nir_i2b(b, nir_iand_imm(b, flags, 32)), slice_coord(b, u, len, s0, s1, nir_u2f32(b, sw)), u);
            nir_def *inside = nir_iand(b, nir_iand(b, nir_fge_imm(b, u, 0), nir_fge_imm(b, v, 0)),
                                       nir_iand(b, nir_flt(b, u, nir_u2f32(b, sw)), nir_flt(b, v, nir_u2f32(b, sh))));
            nir_store_var(b, hit, inside, 1);
            nir_push_if(b, nir_i2b(b, nir_iand_imm(b, flags, 1)));
            {
               nir_store_var(b, src, unpack(b, ld32(b, nir_iadd_imm(b, q, 24))), 0xf);
            }
            nir_push_else(b, NULL);
            {
               nir_push_if(b, inside);
               {
                  nir_def *iu = nir_f2i32(b, u), *iv = nir_f2i32(b, v);
                  /* glass: the backdrop (nearest, its own scale) through the mask (nearest, the bitmap's px) */
                  nir_push_if(b, nir_i2b(b, nir_iand_imm(b, flags, 4)));
                  {
                     nir_def *mb = ld64(b, nir_iadd_imm(b, q, 96)), *ms = ld32(b, nir_iadd_imm(b, q, 104));
                     nir_def *off = nir_iadd(b, nir_imul(b, iv, ms), iu);
                     nir_def *word = ld32(b, nir_iadd(b, mb, nir_u2u64(b, nir_iand_imm(b, off, ~3))));
                     nir_def *m = nir_iand_imm(b, nir_ushr(b, word, nir_ishl_imm(b, nir_iand_imm(b, off, 3), 3)), 0xff);
                     nir_def *wv = nir_fmul(b, nir_fmul_imm(b, nir_u2f32(b, m), 1.0 / 255.0), alpha);
                     nir_def *bb = ld64(b, nir_iadd_imm(b, q, 108)), *bs = ld32(b, nir_iadd_imm(b, q, 116));
                     nir_def *bk = ldf(b, nir_iadd_imm(b, q, 120)), *bwh = ld32(b, nir_iadd_imm(b, q, 124));
                     nir_def *bx = nir_f2i32(b, nir_fmul(b, nir_fadd_imm(b, fx, -0.5), bk));
                     nir_def *by = nir_f2i32(b, nir_fmul(b, nir_fadd_imm(b, fy, -0.5), bk));
                     nir_def *bc = fetch(b, bb, bs, nir_iand_imm(b, bwh, 0xffff), nir_ushr_imm(b, bwh, 16), bx, by);
                     nir_def *a = nir_load_var(b, acc);
                     nir_store_var(b, acc, nir_flrp(b, a, bc, wv), 0xf);
                  }
                  nir_pop_if(b, NULL);
                  nir_push_if(b, nir_i2b(b, nir_iand_imm(b, flags, 2)));
                  {
                     nir_def *uu = nir_fadd_imm(b, u, -0.5), *vv = nir_fadd_imm(b, v, -0.5);
                     nir_def *u0 = nir_ffloor(b, uu), *v0 = nir_ffloor(b, vv);
                     nir_def *tu = nir_fsub(b, uu, u0), *tv = nir_fsub(b, vv, v0);
                     nir_def *ju = nir_f2i32(b, u0), *jv = nir_f2i32(b, v0);
                     nir_def *t00 = fetch(b, base, stride, sw, sh, ju, jv);
                     nir_def *t10 = fetch(b, base, stride, sw, sh, nir_iadd_imm(b, ju, 1), jv);
                     nir_def *t01 = fetch(b, base, stride, sw, sh, ju, nir_iadd_imm(b, jv, 1));
                     nir_def *t11 = fetch(b, base, stride, sw, sh, nir_iadd_imm(b, ju, 1), nir_iadd_imm(b, jv, 1));
                     nir_def *top = nir_flrp(b, t00, t10, tu), *bot = nir_flrp(b, t01, t11, tu);
                     nir_store_var(b, src, nir_flrp(b, top, bot, tv), 0xf);
                  }
                  nir_push_else(b, NULL);
                  {
                     nir_store_var(b, src, unpack(b, ld32(b, addr_at(b, base, nir_iadd(b, nir_imul(b, iv, stride), iu)))), 0xf);
                  }
                  nir_pop_if(b, NULL);
               }
               nir_pop_if(b, NULL);
            }
            nir_pop_if(b, NULL);
            nir_push_if(b, nir_load_var(b, hit));
            {
               nir_def *s = nir_fmul(b, nir_load_var(b, src), alpha);
               nir_def *a = nir_load_var(b, acc);
               nir_store_var(b, acc, nir_ffma(b, a, nir_fsub(b, nir_imm_float(b, 1), nir_channel(b, s, 3)), s), 0xf);
            }
            nir_pop_if(b, NULL);
         }
         nir_pop_if(b, NULL);
      }
      nir_pop_loop(b, loop);
      nir_def *dst = ld64(b, par_addr(b, SH_COMP2, 0)), *stride = ld32(b, par_addr(b, SH_COMP2, 8));
      nir_build_store_global(b, pack(b, nir_load_var(b, acc)), addr_at(b, dst, nir_iadd(b, nir_imul(b, y, stride), x)),
                             .align_mul = 4, .write_mask = 1);
   }
   nir_pop_if(b, NULL);
}

/* ---- DOWN / UP (the dual filter's passes): [0..7] src, [8] src stride, [12] src w, [16] src h, [24..31] dst,
 * [32] dst stride, [36] dst w, [40] dst h ---------------------------------------------------------------------- */
static void build_down(nir_builder *b)
{
   nir_def *x, *y;
   thread_xy(b, &x, &y);
   nir_def *dw = ld32(b, par_addr(b, SH_DOWN, 36)), *dh = ld32(b, par_addr(b, SH_DOWN, 40));
   nir_push_if(b, nir_iand(b, nir_ult(b, x, dw), nir_ult(b, y, dh)));
   {
      nir_def *src = ld64(b, par_addr(b, SH_DOWN, 0)), *ss = ld32(b, par_addr(b, SH_DOWN, 8));
      nir_def *sw = ld32(b, par_addr(b, SH_DOWN, 12)), *sh = ld32(b, par_addr(b, SH_DOWN, 16));
      nir_def *sx = nir_ishl_imm(b, x, 1), *sy = nir_ishl_imm(b, y, 1);
      /* the centre's 2 x 2 (weight 4/8) and the four outer diagonal texels (1/8 each) */
      nir_def *c = nir_fadd(b, nir_fadd(b, fetch(b, src, ss, sw, sh, sx, sy), fetch(b, src, ss, sw, sh, nir_iadd_imm(b, sx, 1), sy)),
                            nir_fadd(b, fetch(b, src, ss, sw, sh, sx, nir_iadd_imm(b, sy, 1)),
                                     fetch(b, src, ss, sw, sh, nir_iadd_imm(b, sx, 1), nir_iadd_imm(b, sy, 1))));
      nir_def *o = nir_fadd(b, nir_fadd(b, fetch(b, src, ss, sw, sh, nir_iadd_imm(b, sx, -1), nir_iadd_imm(b, sy, -1)),
                                        fetch(b, src, ss, sw, sh, nir_iadd_imm(b, sx, 2), nir_iadd_imm(b, sy, -1))),
                            nir_fadd(b, fetch(b, src, ss, sw, sh, nir_iadd_imm(b, sx, -1), nir_iadd_imm(b, sy, 2)),
                                     fetch(b, src, ss, sw, sh, nir_iadd_imm(b, sx, 2), nir_iadd_imm(b, sy, 2))));
      nir_def *r = nir_fadd(b, nir_fmul_imm(b, c, 1.0 / 8.0), nir_fmul_imm(b, o, 1.0 / 8.0));
      nir_def *dst = ld64(b, par_addr(b, SH_DOWN, 24)), *ds = ld32(b, par_addr(b, SH_DOWN, 32));
      nir_build_store_global(b, pack(b, r), addr_at(b, dst, nir_iadd(b, nir_imul(b, y, ds), x)), .align_mul = 4,
                             .write_mask = 1);
   }
   nir_pop_if(b, NULL);
}

static void build_up(nir_builder *b)
{
   nir_def *x, *y;
   thread_xy(b, &x, &y);
   nir_def *dw = ld32(b, par_addr(b, SH_UP, 36)), *dh = ld32(b, par_addr(b, SH_UP, 40));
   nir_push_if(b, nir_iand(b, nir_ult(b, x, dw), nir_ult(b, y, dh)));
   {
      nir_def *src = ld64(b, par_addr(b, SH_UP, 0)), *ss = ld32(b, par_addr(b, SH_UP, 8));
      nir_def *sw = ld32(b, par_addr(b, SH_UP, 12)), *sh = ld32(b, par_addr(b, SH_UP, 16));
      /* the dual filter's up pass: the four edge neighbours (1/6) and the four diagonals (1/12) around the source
       * point of this pixel (texel units of the half-size source) */
      nir_def *u = nir_fadd_imm(b, nir_fmul_imm(b, nir_u2f32(b, x), 0.5), -0.25);
      nir_def *v = nir_fadd_imm(b, nir_fmul_imm(b, nir_u2f32(b, y), 0.5), -0.25);
      nir_def *iu = nir_f2i32(b, nir_ffloor(b, nir_fadd_imm(b, u, 0.5))), *iv = nir_f2i32(b, nir_ffloor(b, nir_fadd_imm(b, v, 0.5)));
      nir_def *e = nir_fadd(b, nir_fadd(b, fetch(b, src, ss, sw, sh, nir_iadd_imm(b, iu, -1), iv), fetch(b, src, ss, sw, sh, nir_iadd_imm(b, iu, 1), iv)),
                            nir_fadd(b, fetch(b, src, ss, sw, sh, iu, nir_iadd_imm(b, iv, -1)), fetch(b, src, ss, sw, sh, iu, nir_iadd_imm(b, iv, 1))));
      nir_def *dg = nir_fadd(b, nir_fadd(b, fetch(b, src, ss, sw, sh, nir_iadd_imm(b, iu, -1), nir_iadd_imm(b, iv, -1)),
                                         fetch(b, src, ss, sw, sh, nir_iadd_imm(b, iu, 1), nir_iadd_imm(b, iv, -1))),
                             nir_fadd(b, fetch(b, src, ss, sw, sh, nir_iadd_imm(b, iu, -1), nir_iadd_imm(b, iv, 1)),
                                      fetch(b, src, ss, sw, sh, nir_iadd_imm(b, iu, 1), nir_iadd_imm(b, iv, 1))));
      nir_def *r = nir_fadd(b, nir_fmul_imm(b, e, 1.0 / 6.0), nir_fmul_imm(b, dg, 1.0 / 12.0));
      nir_def *dst = ld64(b, par_addr(b, SH_UP, 24)), *ds = ld32(b, par_addr(b, SH_UP, 32));
      nir_build_store_global(b, pack(b, r), addr_at(b, dst, nir_iadd(b, nir_imul(b, y, ds), x)), .align_mul = 4,
                             .write_mask = 1);
   }
   nir_pop_if(b, NULL);
}

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

/* the dispatches: a shader and its destination size (16 x 16 workgroups over it) */
static const struct { unsigned sh, w, h; const char *name; } jobs[] = {
   { SH_FILL, 1440, 2960, "fill_full" },
   { SH_COMP, 1440, 2960, "comp_full" },
   { SH_DOWN, 720, 1480, "down_half" },
   { SH_DOWN, 360, 740, "down_quarter" },
   { SH_UP, 720, 1480, "up_half" },
   { SH_UP, 1440, 2960, "up_full" },
   { SH_COMP2, 1440, 2960, "comp2_full" },
};
#define NJOB (sizeof(jobs) / sizeof(jobs[0]))

int main(void)
{
   glsl_type_singleton_init_or_ref();
   const nir_shader_compiler_options *opt = GENX(pan_shader_get_compiler_options)();
   printf("/* Generated by starbite/tools/mesa-compjob.c with Mesa 25.0.7 (Bifrost v6, GPU %#x): do not edit. */\n",
          GPU_ID_G72);
   printf("#define GPUCOMP_VA_BASE 0x%llxull\n#define GPUCOMP_WG %uu\n", (unsigned long long)VA_BASE, WG);
   for (unsigned i = 0; i < NSH; ++i)
      printf("#define GPUCOMP_O_PAR_%s 0x%xu\n", sh_name[i], O_PAR(i));
   printf("#define GPUCOMP_O_TLS 0x%xu\n", O_TLS);
   for (unsigned i = 0; i < NSH; ++i) {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opt, "starbite-%s", sh_name[i]);
      b.shader->info.workgroup_size[0] = WG;
      b.shader->info.workgroup_size[1] = WG;
      b.shader->info.workgroup_size[2] = 1;
      if (i == SH_FILL) build_fill(&b);
      else if (i == SH_COMP) build_comp(&b);
      else if (i == SH_DOWN) build_down(&b);
      else if (i == SH_UP) build_up(&b);
      else build_comp2(&b);
      nir_shader *s = b.shader;
      NIR_PASS(_, s, nir_lower_vars_to_ssa);
      NIR_PASS(_, s, nir_lower_compute_system_values, NULL);
      /* as a driver's front end does before the backend: scalar ALU and constants (Bifrost's load_const is scalar) */
      NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);
      NIR_PASS(_, s, nir_lower_load_const_to_scalar);
      NIR_PASS(_, s, nir_copy_prop);
      NIR_PASS(_, s, nir_opt_dce);
      pan_shader_preprocess(s, GPU_ID_G72);
      NIR_PASS(_, s, nir_lower_load_const_to_scalar);
      struct panfrost_compile_inputs in = {.gpu_id = GPU_ID_G72};
      struct util_dynarray bin;
      util_dynarray_init(&bin, NULL);
      struct pan_shader_info info;
      GENX(pan_shader_compile)(s, &in, &bin, &info);
      if (getenv("COMPJOB_DISASM"))
         disassemble_bifrost(stderr, bin.data, bin.size, false);
      if (info.tls_size || info.wls_size || info.ubo_count || info.push.count) {
         fprintf(stderr, "mesa-compjob: %s needs TLS %u WLS %u UBOs %u push %u: not supported here\n", sh_name[i],
                 info.tls_size, info.wls_size, info.ubo_count, info.push.count);
         return 1;
      }
      if (bin.size > 0x2000u) {
         fprintf(stderr, "mesa-compjob: %s: %u bytes, more than 8 KiB\n", sh_name[i], bin.size);
         return 1;
      }
      struct mali_renderer_state_packed rsd;
      pan_pack(&rsd, RENDERER_STATE, cfg) {
         pan_shader_prepare_rsd(&info, VA_BASE + O_SH(i), &cfg);
      }
      char nm[64];
      snprintf(nm, sizeof nm, "gpucomp_sh_%s", sh_name[i]);
      words(nm, bin.data, bin.size);
      printf("#define GPUCOMP_O_SH_%s 0x%xu\n#define GPUCOMP_O_RSD_%s 0x%xu\n/* %s: %u bytes, %u work registers */\n",
             sh_name[i], O_SH(i), sh_name[i], O_RSD(i), sh_name[i], bin.size, info.work_reg_count);
      snprintf(nm, sizeof nm, "gpucomp_rsd_%s", sh_name[i]);
      words(nm, &rsd, sizeof(rsd));
      fprintf(stderr, "mesa-compjob: %s: %u bytes, %u work registers\n", sh_name[i], bin.size, info.work_reg_count);
   }
   struct mali_local_storage_packed tls;
   pan_pack(&tls, LOCAL_STORAGE, cfg) {
      cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
   }
   words("gpucomp_tls", &tls, sizeof(tls));
   printf("#define GPUCOMP_JOB_BYTES %uu\n", (unsigned)MALI_COMPUTE_JOB_LENGTH);
   for (unsigned j = 0; j < NJOB; ++j) {
      struct mali_compute_job_packed job;
      memset(&job, 0, sizeof(job));
      pan_section_pack(&job, COMPUTE_JOB, HEADER, cfg) {
         cfg.type = MALI_JOB_TYPE_COMPUTE;
         cfg.index = 1;
      }
      unsigned nx = DIV_ROUND_UP(jobs[j].w, WG), ny = DIV_ROUND_UP(jobs[j].h, WG);
      panfrost_pack_work_groups_compute(pan_section_ptr(&job, COMPUTE_JOB, INVOCATION), nx, ny, 1, WG, WG, 1, false,
                                        false);
      pan_section_pack(&job, COMPUTE_JOB, PARAMETERS, cfg) {
         cfg.job_task_split = util_logbase2_ceil(WG + 1) + util_logbase2_ceil(WG + 1) + util_logbase2_ceil(1 + 1);
      }
      pan_section_pack(&job, COMPUTE_JOB, DRAW, cfg) {
         cfg.state = VA_BASE + O_RSD(jobs[j].sh);
         cfg.thread_storage = VA_BASE + O_TLS;
      }
      char nm[64];
      snprintf(nm, sizeof nm, "gpucomp_job_%s", jobs[j].name);
      words(nm, &job, sizeof(job));
      printf("#define GPUCOMP_O_JOB_%s 0x%xu\n#define GPUCOMP_JOB_%s_W %uu\n#define GPUCOMP_JOB_%s_H %uu\n", jobs[j].name,
             O_JOB(j), jobs[j].name, jobs[j].w, jobs[j].name, jobs[j].h);
   }
   return 0;
}
