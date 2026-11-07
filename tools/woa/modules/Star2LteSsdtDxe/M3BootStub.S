// M3BootStub: M3 core 1 takes over the boot CPU at ExitBootServices (docs/WIN11-BIGCORES.md B3).
// SPDX-License-Identifier: GPL-2.0-only
//
// Assembled by gen-stubs.py into M3BootStub.h; M3Boot.c copies it into a boot-services code page
// and fills the literal (the context). A55 core 0 saved its whole EL1 state into the context
// (M3BootHandoff in M3Boot.c), cleaned it to the point of coherency and CPU_ON'd this entry; it
// now waits on Context.State without using its stack.
//
// Entered by PSCI CPU_ON: MMU and caches off, DAIF masked, SPSel 1.
// 1. CurrentEL, MPIDR into the context; not EL1 -> CPU_OFF (the A55 times out and stays the boot CPU).
// 2. VBAR = our vectors (any exception before the claim: ESR/ELR/FAR into the context, CPU_OFF);
//    MAIR, TCR, TTBR0/1, AMAIR, CPACR, CONTEXTIDR, CNTKCTL, TPIDRs; SCTLR (MMU and caches on).
// 3. Claim: State 0 -> 1 with exclusives (cacheable now, coherent with the A55). State 2 means the
//    A55 gave up waiting (it continues as the boot CPU): CPU_OFF, nothing else touched.
// 4. Claimed: UEFI's VBAR, FPCR/FPSR, q0-q31, SP_EL1/SP_EL0/SPSel, NZCV, x19-x30, DAIF; return 1
//    to M3BootHandoff's caller. From here the M3 is the boot CPU and A55 core 0 powers off.

        .equ    CTX_X19,     0x000
        .equ    CTX_SP_EL0,  0x060
        .equ    CTX_SP_EL1,  0x068
        .equ    CTX_SPSEL,   0x070
        .equ    CTX_DAIF,    0x078
        .equ    CTX_NZCV,    0x080
        .equ    CTX_FPCR,    0x088
        .equ    CTX_FPSR,    0x090
        .equ    CTX_SCTLR,   0x098
        .equ    CTX_TCR,     0x0a0
        .equ    CTX_TTBR0,   0x0a8
        .equ    CTX_TTBR1,   0x0b0
        .equ    CTX_MAIR,    0x0b8
        .equ    CTX_AMAIR,   0x0c0
        .equ    CTX_VBAR,    0x0c8
        .equ    CTX_CPACR,   0x0d0
        .equ    CTX_CTXID,   0x0d8
        .equ    CTX_TPIDR0,  0x0e0
        .equ    CTX_TPIDR1,  0x0e8
        .equ    CTX_TPIDRRO, 0x0f0
        .equ    CTX_CNTKCTL, 0x0f8
        .equ    CTX_STATE,   0x100
        .equ    CTX_STAGE,   0x104
        .equ    CTX_EL,      0x108
        .equ    CTX_MPIDR,   0x110
        .equ    CTX_ESR,     0x128
        .equ    CTX_ELR,     0x130
        .equ    CTX_FAR,     0x138
        .equ    CTX_Q0,      0x140

        .text
        .balign 2048
vectors:
        .rept   16
        b       vfail
        .balign 128
        .endr

        .global m3boot_stub
m3boot_stub:
        adr     x1, lit
        ldr     x19, [x1]               // context
        mov     w0, #1                  // Stage 1: running, MMU off (before any context load)
        str     w0, [x19, #CTX_STAGE]
        dsb     sy
        mrs     x0, CurrentEL
        lsr     x0, x0, #2
        str     x0, [x19, #CTX_EL]
        mrs     x2, mpidr_el1
        str     x2, [x19, #CTX_MPIDR]
        cmp     x0, #1
        b.ne    off

        tlbi    vmalle1
        dsb     nsh
        ic      iallu
        dsb     nsh
        isb
        adr     x0, vectors
        msr     vbar_el1, x0
        ldr     x0, [x19, #CTX_MAIR]
        msr     mair_el1, x0
        ldr     x0, [x19, #CTX_TCR]
        msr     tcr_el1, x0
        ldr     x0, [x19, #CTX_TTBR0]
        msr     ttbr0_el1, x0
        ldr     x0, [x19, #CTX_TTBR1]
        msr     ttbr1_el1, x0
        ldr     x0, [x19, #CTX_AMAIR]
        msr     amair_el1, x0
        ldr     x0, [x19, #CTX_CPACR]
        msr     cpacr_el1, x0
        ldr     x0, [x19, #CTX_CTXID]
        msr     contextidr_el1, x0
        ldr     x0, [x19, #CTX_CNTKCTL]
        msr     cntkctl_el1, x0
        ldr     x0, [x19, #CTX_TPIDR0]
        msr     tpidr_el0, x0
        ldr     x0, [x19, #CTX_TPIDR1]
        msr     tpidr_el1, x0
        ldr     x0, [x19, #CTX_TPIDRRO]
        msr     tpidrro_el0, x0
        isb
        ldr     x0, [x19, #CTX_SCTLR]
        msr     sctlr_el1, x0
        isb
        mov     w0, #2                  // Stage 2: MMU and caches on
        str     w0, [x19, #CTX_STAGE]

        add     x2, x19, #CTX_STATE     // claim: 0 -> 1
1:      ldaxr   w0, [x2]
        cbnz    w0, off                 // 2: the A55 gave up and stays the boot CPU
        mov     w1, #1
        stlxr   w3, w1, [x2]
        cbnz    w3, 1b
        dmb     ish
        mov     w0, #3                  // Stage 3: claimed
        str     w0, [x19, #CTX_STAGE]

        ldr     x0, [x19, #CTX_VBAR]    // claimed: the rest of the boot CPU's state
        msr     vbar_el1, x0
        ldr     x0, [x19, #CTX_FPCR]
        msr     fpcr, x0
        ldr     x0, [x19, #CTX_FPSR]
        msr     fpsr, x0
        add     x1, x19, #CTX_Q0
        ldp     q0, q1, [x1, #0x000]
        ldp     q2, q3, [x1, #0x020]
        ldp     q4, q5, [x1, #0x040]
        ldp     q6, q7, [x1, #0x060]
        ldp     q8, q9, [x1, #0x080]
        ldp     q10, q11, [x1, #0x0a0]
        ldp     q12, q13, [x1, #0x0c0]
        ldp     q14, q15, [x1, #0x0e0]
        ldp     q16, q17, [x1, #0x100]
        ldp     q18, q19, [x1, #0x120]
        ldp     q20, q21, [x1, #0x140]
        ldp     q22, q23, [x1, #0x160]
        ldp     q24, q25, [x1, #0x180]
        ldp     q26, q27, [x1, #0x1a0]
        ldp     q28, q29, [x1, #0x1c0]
        ldp     q30, q31, [x1, #0x1e0]
        msr     spsel, #1
        ldr     x0, [x19, #CTX_SP_EL1]
        mov     sp, x0
        ldr     x0, [x19, #CTX_SP_EL0]
        msr     sp_el0, x0
        ldr     x0, [x19, #CTX_SPSEL]
        msr     spsel, x0
        isb
        ldr     x0, [x19, #CTX_NZCV]
        msr     nzcv, x0
        ldp     x20, x21, [x19, #CTX_X19 + 0x08]
        ldp     x22, x23, [x19, #CTX_X19 + 0x18]
        ldp     x24, x25, [x19, #CTX_X19 + 0x28]
        ldp     x26, x27, [x19, #CTX_X19 + 0x38]
        ldp     x28, x29, [x19, #CTX_X19 + 0x48]
        ldr     x30, [x19, #CTX_X19 + 0x58]
        ldr     x0, [x19, #CTX_DAIF]
        msr     daif, x0
        mov     x0, #1                  // M3BootHandoff returns 1: running on the M3
        ldr     x19, [x19, #CTX_X19]
        ret

vfail:  adr     x1, lit                 // an exception before the claim
        ldr     x19, [x1]
        mrs     x0, esr_el1
        str     x0, [x19, #CTX_ESR]
        mrs     x0, elr_el1
        str     x0, [x19, #CTX_ELR]
        mrs     x0, far_el1
        str     x0, [x19, #CTX_FAR]
        dsb     sy
off:    mov     w0, #0x0002             // PSCI CPU_OFF 0x84000002
        movk    w0, #0x8400, lsl #16
        smc     #0
2:      wfe
        b       2b

        .balign 8
lit:    .quad   0                       // context
