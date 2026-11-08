/* SPDX-License-Identifier: GPL-2.0-only
 * sbsim: the QEMU stand-in for Starbite OS (docs/STARBITE-OS-LINUX-M3.md 7). Entered by QEMU's -kernel on
 * CPU 0 at EL1 with the MMU off; the other seven CPUs wait in PSCI OFF. MMU and caches stay off (every
 * access is Device-nGnRnE: built with -mstrict-align), interrupts stay masked and are polled from GICC_IAR,
 * as Starbite OS does on the phone. */
    .section .text.start, "ax"
    .global _start
_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #0xffffff
    cbnz    x1, park
    ldr     x1, =__stack_top
    mov     sp, x1
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:  msr     daifset, #0xf
    bl      sbsim_main
park:
    wfe
    b       park

    .text
/* x0 = function id, x1..x3 = arguments; PSCI result in x0 */
    .global sbsim_hvc
sbsim_hvc:
    hvc     #0
    ret

/* AArch64 semihosting: w0 = operation, x1 = parameter block */
    .global sbsim_semi
sbsim_semi:
    hlt     #0xf000
    ret
