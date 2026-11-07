/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS apps inside the kernel (app.c, sys.c, vec.S, hw.c). */
#pragma once
#include <stdint.h>
#include "kern.h"

/* the frame vec.S saves and restores; offsets fixed (vec.S) */
struct aframe {
    uint64_t x[31];                 /* 0 */
    uint64_t sp, elr, spsr, esr, far;   /* 248 .. 280 */
    uint64_t q[64];                 /* 288: q0..q31 as pairs of u64 */
    uint64_t fpsr, fpcr;            /* 800, 808 */
} __attribute__((aligned(16)));
_Static_assert(sizeof(struct aframe) == 816, "struct aframe is vec.S's layout");

#define APP_QLEN 16
struct kapp {
    uintptr_t slot;                 /* 0 on the host */
    const struct sbapp_header *hdr;
    void (*host_entry)(const struct sbos_event *);    /* the host build's app function */
    char id[24], name[24];
    uint32_t perms, color, icon;
    uint64_t heap, heap_end;
    int dead;                       /* stopped (fault or budget): no more events */
    int running;                    /* a handler frame exists */
    uint64_t used_ms, t_resume;     /* the handler's time so far */
    struct aframe fr;
    struct sbos_event q[APP_QLEN];
    unsigned qh, qt;
    unsigned timers;                /* active timer count */
    uint64_t log_window, log_n;
    uint8_t state[STATE_REC_MAX];
    uint32_t state_n;
};
extern struct kapp apps[APP_MAX];
uint64_t hw_app_resume(struct aframe *f);
int user_str(int a, uint64_t p, char *out, unsigned cap);
int user_buf(int a, uint64_t p, uint64_t n);
int user_out(int a, uint64_t p, const void *src, uint64_t n);
int user_in(int a, uint64_t p, void *dst, uint64_t n);
