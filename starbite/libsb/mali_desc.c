/* SPDX-License-Identifier: GPL-2.0-only
 * Mali Bifrost (v6, Mali-G72) job descriptors, packed by hand: the layouts of Mesa's genxml v6.xml
 * (MIT) as Starbite's iOS port used them on this phone (xnu/src/starlte_gpu.c sg_write_value,
 * sg_clear: WRITE_VALUE jobs and clear-only fragment jobs ran and verified on the S9). Pure: the
 * host unit test (starbite/tools/unit-test.c) compares every word with what Mesa 25.0.7's own
 * pan_pack / GENX(pan_emit_fbd) produce for the same fields (starbite/tools/mesa-golden.c).
 *
 * Job Header (32 bytes): word 0 exception status, word 1 first incomplete task, words 2-3 fault
 * pointer, word 4 [0] 64-bit descriptor, [7:1] type, [8] barrier, [31:16] index, word 5 the two
 * dependencies, words 6-7 the next job. Payloads follow at +32.
 */
#include "sb.h"

void mali_job_header(uint32_t *w, unsigned type, unsigned index, uint64_t next)
{
    for (unsigned i = 0; i < 8; ++i)
        w[i] = 0;
    w[4] = 1U | (type & 0x7fU) << 1 | (index & 0xffffU) << 16;
    w[6] = (uint32_t)next;
    w[7] = (uint32_t)(next >> 32);
}

/* Write Value Job: header, then the payload: address (64), type (32), immediate (64). */
void mali_write_value(uint32_t w[16], unsigned index, uint64_t next, uint64_t addr, unsigned type, uint64_t imm)
{
    mali_job_header(w, MALI_JOB_WRITE_VALUE, index, next);
    w[8] = (uint32_t)addr;
    w[9] = (uint32_t)(addr >> 32);
    w[10] = type;
    w[11] = 0;
    w[12] = (uint32_t)imm;
    w[13] = (uint32_t)(imm >> 32);
    w[14] = w[15] = 0;
}

/* Fragment Job: header, then bound min (tiles), bound max (tiles of 16 x 16), the framebuffer
 * pointer tagged MFBD with one render target and no ZS/CRC extension (tag 1, as Mesa's
 * jm_emit_fbds). The frame is w x h pixels from tile (0, 0). */
void mali_fragment(uint32_t w[16], unsigned index, uint64_t next, unsigned wpx, unsigned hpx, uint64_t fbd)
{
    mali_job_header(w, MALI_JOB_FRAGMENT, index, next);
    w[8] = 0;
    w[9] = ((wpx - 1U) >> 4) | ((hpx - 1U) >> 4) << 16;
    w[10] = (uint32_t)(fbd | MALI_FBD_TAG);
    w[11] = (uint32_t)(fbd >> 32);
    for (unsigned i = 12; i < 16; ++i)
        w[i] = 0;
}

/* A clear-only framebuffer (no draws: tiler pointer 0, no frame shaders): Framebuffer Parameters
 * (64 bytes) + 64 bytes of padding, then render target 0 at +128 (64 bytes). w x h pixels, one
 * sample (sample positions at samples), 16 x 16 tiles (effective tile size 256, colour buffer
 * allocation 1 KiB, the G72's 16 KiB tile buffer), tie-break MINUS_180_IN_0_OUT, Z D24; RT:
 * internal R8G8B8A8, write enable, writeback R8G8B8A8, LINEAR, dithering, swizzle RGBA, clean
 * pixel write, base, row stride, clear colour word 0 (the packed pixel). */
void mali_fbd(uint32_t w[48], uint64_t samples, unsigned wpx, unsigned hpx, uint64_t rt_base, uint32_t stride,
              uint32_t clear)
{
    for (unsigned i = 0; i < 48; ++i)
        w[i] = 0;
    w[4] = (uint32_t)samples;
    w[5] = (uint32_t)(samples >> 32);
    w[8] = (wpx - 1U) | (hpx - 1U) << 16;
    w[9] = 0;
    w[10] = (wpx - 1U) | (hpx - 1U) << 16;
    w[11] = 2U << 6 | 8U << 9 | 1U << 24;
    w[12] = 1U << 16;
    w[32] = 1U << 26;
    w[33] = 1U | 19U << 3 | 2U << 10 | 1U << 15 | 0x688U << 16 | 1U << 31;
    w[40] = (uint32_t)rt_base;
    w[41] = (uint32_t)(rt_base >> 32);
    w[42] = stride;
    w[44] = clear;
}

/* Mesa's single-sample position table: sample 0 and the origin (word 32) at the pixel centre,
 * 128/256 px in x and y. */
void mali_samples(uint32_t w[33])
{
    for (unsigned i = 0; i < 33; ++i)
        w[i] = 0;
    w[0] = 128U | 128U << 16;
    w[32] = 128U | 128U << 16;
}
