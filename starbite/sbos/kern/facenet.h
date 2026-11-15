/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS Face Unlock: the small CNN runtime for the two networks in the face pack (tools/face-pack.py):
 * YuNet (faces + 5 points; MIT) and SFace (a 128-D embedding; Apache-2.0). Pure C, no libc, no hardware: the kernel,
 * the host shell and kern/host/face-test.c build this file as it is.
 *
 * Tensors are NHWC float32 with the channels padded to a multiple of 4 (the padding stays 0). Ops: a full 3 x 3 conv
 * (the first layer), 1 x 1 convs (a GEMM on int8 weights with one scale per output channel; each job dequantises its
 * 8-channel tile into a small pooled buffer), 3 x 3 depthwise convs, 2 x 2 max pooling, nearest 2x upsampling + add, a
 * fully connected layer on 4-bit weights (a scale per 64 inputs); Relu / PRelu fused. Batch norms are folded by the packer.
 *
 * A run is cut into pieces: fn_run(ctx, budget_us) runs ops until the budget is spent (an op is never cut), so no
 * loop turn takes long (the self-test gate: no turn >= 100 ms). The platform gives memory, the clock and an optional
 * parallel-for over the other cores (kern_next.c's band workers); without it everything runs on the calling core.
 */
#ifndef SBOS_FACENET_H
#define SBOS_FACENET_H
#include <stdint.h>
#include <stddef.h>

#define FN_MAGIC "SBFACE1"
#define FN_NET_YUNET 0
#define FN_NET_SFACE 1
#define FN_MAX_OPS 64
#define FN_MAX_SLOTS 16

enum { FN_OP_CONV3 = 1, FN_OP_PW, FN_OP_DW, FN_OP_POOL, FN_OP_UPADD, FN_OP_FC };
enum { FN_ACT_NONE = 0, FN_ACT_RELU, FN_ACT_PRELU };

#define FN_FC_GROUP 64             /* the 4-bit fully connected layer: one scale per 64 inputs */
#define FN_POOL 8                  /* 1 x 1 layers: dequantised weight tiles in flight (>= the cores) */
struct fn_op {                     /* 36 bytes, as face-pack.py packs it */
    uint8_t type, act, stride, wbits;   /* wbits (FC): 4 = 4-bit weights with group scales, else int8 */
    uint16_t in, in2, out, cin, cout, cinp, coutp, hw;
    uint32_t w, ws, b, a;          /* offsets in the pack: weights, int8 scales, bias, PRelu slopes (0: none) */
};
struct fn_net {                    /* 96 bytes; the ops follow */
    uint32_t nops, nslots, nout, bgr;   /* bgr: 1 the input is B, G, R (YuNet), 0 R, G, B (SFace) */
    float mean, scale;             /* the input's x -> (x - mean) * scale (0..255 values) */
    uint16_t outs[16], outc[16];   /* the outputs' slots and real channel counts */
    uint8_t pad_[8];
};

struct fn_plat {
    void *(*alloc)(size_t n);                                   /* zeroed, >= 16-byte aligned, or 0 */
    void (*free)(void *p);
    void (*par)(void (*fn)(void *arg, int i), void *arg, int n);   /* may be 0: a plain loop */
    uint64_t (*now_us)(void);                                   /* may be 0: no budget (run to the end) */
};

struct fn_ctx {
    const uint8_t *pak;
    const struct fn_net *net;
    const struct fn_op *ops;
    const struct fn_plat *plat;
    unsigned h, w;                 /* the input's size */
    unsigned next;                 /* the next op to run; == nops: done */
    float *slot[FN_MAX_SLOTS];
    unsigned sh[FN_MAX_SLOTS], sw[FN_MAX_SLOTS];   /* what each slot holds now */
    float *pool[FN_POOL];          /* 1 x 1 layers: a job's dequantised weight tile (cinp x 8 floats) */
    volatile uint32_t pool_busy[FN_POOL];
    void *arena;
    size_t arena_bytes;
    uint32_t op_us_max;            /* the longest op so far (budget planning) */
    uint64_t macs;                 /* multiply-adds of the whole net at this size */
};

/* the pack: 0 good; < 0 not a pack, a version this code does not know, or its digest is wrong (check_digest: run
 * the SHA-256 over the whole pack once, ~30 ms on the A55) */
int fn_pack_check(const uint8_t *pak, size_t len, int check_digest);
/* a run of net (FN_NET_*) at h x w (YuNet: multiples of 32; SFace: 112 x 112): 0, or < 0 (no memory, bad pack) */
int fn_open(struct fn_ctx *c, const uint8_t *pak, int net, unsigned h, unsigned w, const struct fn_plat *plat);
float *fn_input(struct fn_ctx *c);                 /* [h][w][4] float; fill, then fn_restart */
void fn_restart(struct fn_ctx *c);                 /* the next fn_run starts at the first op */
/* run ops until done or budget_us spent (0: to the end); 1 done, 0 more to do */
int fn_run(struct fn_ctx *c, uint32_t budget_us);
/* output i (the pack's order): its data, size and padded channel count */
const float *fn_output(const struct fn_ctx *c, int i, unsigned *h, unsigned *w, unsigned *cp);
void fn_close(struct fn_ctx *c);

#endif
