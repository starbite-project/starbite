// M3RunStub: start an M3 core in the boot CPU's UEFI context (docs/WIN11-BIGCORES.md stage B3a).
// SPDX-License-Identifier: GPL-2.0-only
//
// Assembled by gen-stubs.py into M3RunStub.h; M3Run.c copies it into a page below 4 GiB and fills
// the literal (the context's address; Samsung's EL3 doesn't deliver the PSCI context id).
// Entered by PSCI CPU_ON: MMU and caches off, DAIF masked, SPSel 1.
//
// The page starts with a vector table of our own (VBAR needs 2 KiB alignment): every exception
// stores ESR, ELR and FAR in the context, Done = 3, CPU_OFF. So a fault never leaves the M3 inside
// UEFI's handlers, in boot-services memory that Windows reuses later. The entry is at +0x800.
// 1. CurrentEL and MPIDR into the context; anything but EL1 -> Done = 2, CPU_OFF (the UEFI
//    context is EL1's and would make no sense elsewhere).
// 2. Local TLB and I-cache invalidated; VBAR = our vectors; MAIR, TCR, TTBR0, CPACR from the
//    context (the boot CPU's, UEFI's identity map), then SCTLR (MMU and caches on, its value).
// 3. SP from the context (a stack of its own), then Fn (Context) in C.
// 4. Done = 1, DSB, then PSCI CPU_OFF; WFE forever if that returns.
// The C side (M3Run.c) keeps the layout in M3_RUN_CTX; offsets checked there.

        .equ    CTX_SCTLR, 0x00
        .equ    CTX_TCR,   0x08
        .equ    CTX_TTBR0, 0x10
        .equ    CTX_MAIR,  0x18
        .equ    CTX_VBAR,  0x20
        .equ    CTX_CPACR, 0x28
        .equ    CTX_SP,    0x30
        .equ    CTX_FN,    0x38
        .equ    CTX_EL,    0x40
        .equ    CTX_MPIDR, 0x48
        .equ    CTX_DONE,  0x50
        .equ    CTX_ESR,   0x58
        .equ    CTX_ELR,   0x60
        .equ    CTX_FAR,   0x68

        .text
        .balign 2048
vectors:
        .rept   16
        b       vfail
        .balign 128
        .endr

        .global m3run_stub
m3run_stub:
        adr     x1, lit
        ldr     x19, [x1]               // context
        mrs     x0, CurrentEL
        lsr     x0, x0, #2
        str     x0, [x19, #CTX_EL]
        mrs     x2, mpidr_el1
        str     x2, [x19, #CTX_MPIDR]
        cmp     x0, #1
        b.ne    2f

        tlbi    vmalle1
        dsb     nsh
        ic      iallu
        dsb     nsh
        isb
        ldr     x0, [x19, #CTX_MAIR]
        msr     mair_el1, x0
        ldr     x0, [x19, #CTX_TCR]
        msr     tcr_el1, x0
        ldr     x0, [x19, #CTX_TTBR0]
        msr     ttbr0_el1, x0
        adr     x0, vectors
        msr     vbar_el1, x0
        ldr     x0, [x19, #CTX_CPACR]
        msr     cpacr_el1, x0
        isb
        ldr     x0, [x19, #CTX_SCTLR]
        msr     sctlr_el1, x0
        isb
        ldr     x0, [x19, #CTX_SP]
        mov     sp, x0
        mov     x0, x19
        ldr     x1, [x19, #CTX_FN]
        blr     x1
        mov     w0, #1
        b       3f

2:      mov     w0, #2                  // not EL1: nothing touched
        b       3f
vfail:  adr     x1, lit                 // any exception
        ldr     x19, [x1]
        mrs     x0, esr_el1
        str     x0, [x19, #CTX_ESR]
        mrs     x0, elr_el1
        str     x0, [x19, #CTX_ELR]
        mrs     x0, far_el1
        str     x0, [x19, #CTX_FAR]
        mov     w0, #3
3:      str     w0, [x19, #CTX_DONE]
        dsb     sy
        mov     w0, #0x0002             // PSCI CPU_OFF 0x84000002
        movk    w0, #0x8400, lsl #16
        smc     #0
4:      wfe
        b       4b

        .balign 8
lit:    .quad   0                       // context
