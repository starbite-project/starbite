/* SPDX-License-Identifier: GPL-2.0-only
 * The M3 stub of the guest probe (probe.c, docs/STARBITE-OS-LINUX-M3.md 10, P1): what Linux's first
 * instructions on an M3 core will need, without Linux. Copied by the A55 to the guest's Image address in the
 * carve-out (0x8_8008_0000) and entered by PSCI CPU_ON, directly or through the sbshm trampoline. MMU off, EL1,
 * position independent; every access an aligned word (Device memory). The A55 patches the literals at the end.
 *
 * It records x0 (PSCI's context id, or the trampoline's DTB literal), MPIDR, CurrentEL, its GIC CPU-interface bit
 * (GICD_ITARGETSR0 byte 0, banked), CNTFRQ, SCTLR, then turns its GIC CPU interface on, enables its own virtual
 * timer PPI 27 (banked) as a 2 s timeout and waits in WFI for the host's doorbell SPI. It acks the interrupt that
 * woke it, rings the host's doorbell SPI (GICD_ISPENDR) unless it was the timeout, turns its CPU interface off and
 * calls PSCI CPU_OFF. Block (64-bit words): 0 state (1 waiting, 2 done, 3 CPU_OFF returned), 1 MPIDR,
 * 2 CurrentEL, 3 x0, 4 cpuif, 5 INTID that woke it, 6 CNTFRQ, 7 WFIs, 8 CNTVCT at entry, 9 SCTLR,
 * 10 CNTVCT at the end, 11 CPU_OFF's return, 12 breadcrumb 2 (= the stub's crumb address).
 * Breadcrumb 1: m3stub_crumb (the last quad of the stub) = its own address, written by the first instruction.
 */
    .section .rodata.m3stub, "a"
    .balign 8
    .global m3stub_start, m3stub_lit, m3stub_crumb, m3stub_end
m3stub_start:
    adr     x8, m3stub_crumb
    str     x8, [x8]                /* breadcrumb 1: the stub's first instruction ran (its own address) */
    adr     x9, m3stub_lit
    ldr     x10, [x9, #0]           /* the result block */
    ldr     x11, [x9, #8]           /* GICD */
    ldr     x12, [x9, #16]          /* GICC */
    ldr     w13, [x9, #24]          /* the host's doorbell INTID (0: none) */
    ldr     x14, [x9, #32]          /* timeout, counter ticks */
    str     x8, [x10, #96]          /* breadcrumb 2: the literals and the block are reachable */
    str     x0, [x10, #24]
    mrs     x1, cntvct_el0
    str     x1, [x10, #64]
    mrs     x1, mpidr_el1
    str     x1, [x10, #8]
    mrs     x1, CurrentEL
    str     x1, [x10, #16]
    ldrb    w1, [x11, #0x800]
    str     x1, [x10, #32]
    mrs     x1, cntfrq_el0
    str     x1, [x10, #48]
    mrs     x1, sctlr_el1
    str     x1, [x10, #72]
    mov     w1, #0xff
    str     w1, [x12, #4]           /* GICC_PMR */
    mov     w1, #1
    str     w1, [x12, #0]           /* GICC_CTLR: group 1 on */
    mov     w1, #0x8000000
    str     w1, [x11, #0x100]       /* GICD_ISENABLER0 (banked): PPI 27, the virtual timer */
    mrs     x1, cntvct_el0
    add     x1, x1, x14
    msr     cntv_cval_el0, x1
    mov     x1, #1
    msr     cntv_ctl_el0, x1
    isb
    dsb     sy
    str     x1, [x10, #0]           /* state 1: checked in, waiting */
    dsb     sy
    mov     x15, #0
1:  wfi
    add     x15, x15, #1
    ldr     w2, [x12, #0x0c]        /* GICC_IAR */
    and     w3, w2, #0x3ff
    cmp     w3, #1020
    b.hs    1b                      /* spurious: wait again (the timer bounds it) */
    str     w2, [x12, #0x10]        /* GICC_EOIR */
    str     x3, [x10, #40]
    str     x15, [x10, #56]
    msr     cntv_ctl_el0, xzr
    cmp     w3, #27
    b.eq    2f                      /* the timeout: no doorbell came */
    cbz     w13, 2f
    lsr     w4, w13, #5
    lsl     w4, w4, #2
    add     x4, x11, x4
    add     x4, x4, #0x200          /* GICD_ISPENDR(n / 32) */
    and     w5, w13, #31
    mov     w6, #1
    lsl     w6, w6, w5
    str     w6, [x4]
2:  mov     w1, #0x8000000
    str     w1, [x11, #0x180]       /* GICD_ICENABLER0 (banked): PPI 27 off */
    str     wzr, [x12, #0]          /* GICC_CTLR off */
    mrs     x1, cntvct_el0
    str     x1, [x10, #80]
    dsb     sy
    mov     x1, #2
    str     x1, [x10, #0]           /* state 2: done, CPU_OFF next */
    dsb     sy
    movz    w0, #0x8400, lsl #16
    orr     w0, w0, #2              /* PSCI CPU_OFF 0x84000002 (the S9 DT's cpu_off) */
#ifdef M3STUB_HVC
    hvc     #0                      /* sbsim on QEMU: PSCI behind HVC */
#else
    smc     #0
#endif
    str     x0, [x10, #88]
    mov     x1, #3
    str     x1, [x10, #0]           /* state 3: CPU_OFF returned */
    dsb     sy
3:  wfe
    b       3b
    .balign 8
m3stub_lit:
    .quad   0, 0, 0, 0, 0
m3stub_crumb:
    .quad   0
m3stub_end:
