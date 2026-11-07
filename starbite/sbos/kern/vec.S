/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS vectors and the EL0 frame switch (docs/STARBITE-OS.md 2.3).
 *
 * Exceptions from EL1 go to libsb's own vectors (probes, the crash screen): the kernel runs with IRQs
 * masked, so only synchronous exceptions and SError arrive there. Every exception from EL0 (an app) saves the
 * app's whole frame into *sbos_cur_frame and returns from sbos_app_resume() with the reason:
 *   1 synchronous (an svc or a fault: ESR in the frame), 2 IRQ, 3 FIQ, 4 SError, 5 AArch32 (never valid)
 * Frame layout (struct aframe, kern/kapp.h): x0..x30 at 0, sp_el0 248, elr 256, spsr 264, esr 272, far 280,
 * q0..q31 at 288, fpsr 800, fpcr 808 (816 bytes, 16-aligned).
 * The kernel context (callee-saved x19..x30, sp, d8..d15) lives in sbos_kctx.
 */
.text

.macro LIBSB off
    .balign 0x80
    b sb_vectors + \off
.endm

.macro LOWER reason
    .balign 0x80
    stp x0, x1, [sp, #-16]!
    mov x1, #\reason
    b el0_save
.endm

.balign 2048
.global sbos_vectors
sbos_vectors:
    LIBSB 0x000
    LIBSB 0x080
    LIBSB 0x100
    LIBSB 0x180
    LIBSB 0x200
    LIBSB 0x280
    LIBSB 0x300
    LIBSB 0x380
    LOWER 1
    LOWER 2
    LOWER 3
    LOWER 4
    LOWER 5
    LOWER 5
    LOWER 5
    LOWER 5

/* x0, x1 of the app on the stack, x1 = reason */
el0_save:
    adrp x0, sbos_cur_frame
    ldr x0, [x0, :lo12:sbos_cur_frame]
    stp x2, x3, [x0, #16]
    stp x4, x5, [x0, #32]
    stp x6, x7, [x0, #48]
    stp x8, x9, [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x16, x17, [x0, #128]
    stp x18, x19, [x0, #144]
    stp x20, x21, [x0, #160]
    stp x22, x23, [x0, #176]
    stp x24, x25, [x0, #192]
    stp x26, x27, [x0, #208]
    stp x28, x29, [x0, #224]
    str x30, [x0, #240]
    mov x19, x1                         /* the reason */
    ldp x2, x3, [sp], #16               /* the app's x0, x1 */
    stp x2, x3, [x0, #0]
    mrs x2, sp_el0
    str x2, [x0, #248]
    mrs x2, elr_el1
    str x2, [x0, #256]
    mrs x2, spsr_el1
    str x2, [x0, #264]
    mrs x2, esr_el1
    str x2, [x0, #272]
    mrs x2, far_el1
    str x2, [x0, #280]
    add x1, x0, #288
    stp q0, q1, [x1], #32
    stp q2, q3, [x1], #32
    stp q4, q5, [x1], #32
    stp q6, q7, [x1], #32
    stp q8, q9, [x1], #32
    stp q10, q11, [x1], #32
    stp q12, q13, [x1], #32
    stp q14, q15, [x1], #32
    stp q16, q17, [x1], #32
    stp q18, q19, [x1], #32
    stp q20, q21, [x1], #32
    stp q22, q23, [x1], #32
    stp q24, q25, [x1], #32
    stp q26, q27, [x1], #32
    stp q28, q29, [x1], #32
    stp q30, q31, [x1], #32
    mrs x2, fpsr
    mrs x3, fpcr
    stp x2, x3, [x1]                    /* x1 = frame + 800 after the q stores */
    mov x0, x19
    /* back to the kernel: restore the context sbos_app_resume saved and return from it with x0 */
    adrp x9, sbos_kctx
    add x9, x9, :lo12:sbos_kctx
    ldp x19, x20, [x9, #0]
    ldp x21, x22, [x9, #16]
    ldp x23, x24, [x9, #32]
    ldp x25, x26, [x9, #48]
    ldp x27, x28, [x9, #64]
    ldp x29, x30, [x9, #80]
    ldr x10, [x9, #96]
    mov sp, x10
    ldp d8, d9, [x9, #112]
    ldp d10, d11, [x9, #128]
    ldp d12, d13, [x9, #144]
    ldp d14, d15, [x9, #160]
    ret

/* uint64_t sbos_app_resume(struct aframe *f): run the app's frame at EL0 until it leaves EL0; the reason. */
.global sbos_app_resume
sbos_app_resume:
    adrp x9, sbos_kctx
    add x9, x9, :lo12:sbos_kctx
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    stp x29, x30, [x9, #80]
    mov x10, sp
    str x10, [x9, #96]
    stp d8, d9, [x9, #112]
    stp d10, d11, [x9, #128]
    stp d12, d13, [x9, #144]
    stp d14, d15, [x9, #160]
    adrp x1, sbos_cur_frame
    str x0, [x1, :lo12:sbos_cur_frame]
    ldr x1, [x0, #248]
    msr sp_el0, x1
    ldr x1, [x0, #256]
    msr elr_el1, x1
    ldr x1, [x0, #264]
    msr spsr_el1, x1
    add x1, x0, #288
    ldp q0, q1, [x1], #32
    ldp q2, q3, [x1], #32
    ldp q4, q5, [x1], #32
    ldp q6, q7, [x1], #32
    ldp q8, q9, [x1], #32
    ldp q10, q11, [x1], #32
    ldp q12, q13, [x1], #32
    ldp q14, q15, [x1], #32
    ldp q16, q17, [x1], #32
    ldp q18, q19, [x1], #32
    ldp q20, q21, [x1], #32
    ldp q22, q23, [x1], #32
    ldp q24, q25, [x1], #32
    ldp q26, q27, [x1], #32
    ldp q28, q29, [x1], #32
    ldp q30, q31, [x1], #32
    ldp x2, x3, [x1]                    /* frame + 800 */
    msr fpsr, x2
    msr fpcr, x3
    msr tpidr_el0, xzr
    msr tpidrro_el0, xzr
    ldp x2, x3, [x0, #16]
    ldp x4, x5, [x0, #32]
    ldp x6, x7, [x0, #48]
    ldp x8, x9, [x0, #64]
    ldp x10, x11, [x0, #80]
    ldp x12, x13, [x0, #96]
    ldp x14, x15, [x0, #112]
    ldp x16, x17, [x0, #128]
    ldp x18, x19, [x0, #144]
    ldp x20, x21, [x0, #160]
    ldp x22, x23, [x0, #176]
    ldp x24, x25, [x0, #192]
    ldp x26, x27, [x0, #208]
    ldp x28, x29, [x0, #224]
    ldr x30, [x0, #240]
    ldp x0, x1, [x0, #0]
    eret
    dsb nsh
    isb

/* Pin SPSel to 1 keeping the current stack (S-Boot may have left SPSel = 0, in which case crt0's stack is
 * SP_EL0). Called first thing from sb_main, with interrupts masked. */
.global sbos_spsel1
sbos_spsel1:
    mov x0, sp
    msr spsel, #1
    mov sp, x0
    ret

.bss
.balign 16
.global sbos_kctx
sbos_kctx:
    .space 176
.global sbos_cur_frame
.balign 8
sbos_cur_frame:
    .space 8
