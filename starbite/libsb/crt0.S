/* SPDX-License-Identifier: GPL-2.0-only
 * libsb entry (docs/ENGINEER-OS.md section 3, "entry contract").
 *
 * An arm64 Image header (the loader's image_load reads text_offset and image_size; build.sh
 * patches image_size), then:
 *   1. mask interrupts, keep x0 (FDT), find where we run (adr _start), our own stack (.bss),
 *      FP/SIMD on (CPACR_EL1.FPEN; the loader gave back S-Boot's value);
 *   2. sb_mmu_enable (mmu.c): identity map, caches on. It is plain C that uses no pointer stored
 *      in data, so it is safe before relocation;
 *   3. sb_reloc (reloc.c): every R_AARCH64_RELATIVE entry of .rela.dyn, *(base + off) =
 *      base + addend. The image is linked at 0 as a PIE;
 *   4. VBAR_EL1 = sb_vectors; sb_start (sys.c) -> sb_main (the application).
 * Everything here is PC-relative (adr / adrp): no absolute address before step 3.
 */
.section .text.head, "ax"
.global _start
_start:
    b entry
    .long 0
    .quad 0x80000               /* text_offset: loaded at 0x92080000 by boot_bare */
    .quad 0                     /* image_size: build.sh writes the padded file size */
    .quad 0xa                   /* flags: little endian, 4 KiB pages, anywhere */
    .quad 0, 0, 0
    .byte 0x41, 0x52, 0x4d, 0x64 /* "ARMd" */
    .long 0
    /* Starbite extension at +64 (read by tools and the self test, not by the loader) */
    .ascii "SBOSIMG1"
    .long 1                     /* version */
    .long entry - _start        /* entry offset */
    .ascii "engineer"           /* image key (8 bytes) */
    .quad 0                     /* build.sh: CRC32 of the image with this field zero */

.text
entry:
    msr daifset, #15
    mov x19, x0                 /* FDT */
    adr x20, _start             /* runtime base */
    mrs x21, sctlr_el1
    mrs x22, cpacr_el1
    mrs x23, vbar_el1
    mrs x24, CurrentEL
    mrs x25, daif
    /* Our vectors first (PC-relative, valid before relocation): a fault in the MMU setup or the
     * relocation shows the crash screen instead of resetting silently through S-Boot's vectors. */
    adr x9, sb_vectors
    msr vbar_el1, x9
    isb
    adrp x9, sb_stack_top
    add x9, x9, :lo12:sb_stack_top
    mov sp, x9
    orr x9, x22, #(3 << 20)     /* FPEN: no FP/SIMD traps */
    msr cpacr_el1, x9
    isb
    bl sb_mmu_enable
    mov x0, x20
    adrp x1, __rela_start
    add x1, x1, :lo12:__rela_start
    adrp x2, __rela_end
    add x2, x2, :lo12:__rela_end
    ldr x3, [x20, #16]          /* image_size: every target must lie inside the image */
    bl sb_reloc_check
    mov x26, x0
    adr x9, sb_vectors
    msr vbar_el1, x9
    isb
    adrp x9, sb_boot
    add x9, x9, :lo12:sb_boot
    str x19, [x9, #0]           /* fdt */
    str x20, [x9, #8]           /* base */
    ldr x10, [x20, #16]
    str x10, [x9, #16]          /* image_size */
    str x26, [x9, #24]          /* relocs */
    str x24, [x9, #32]          /* el */
    str x21, [x9, #40]          /* sctlr at entry */
    str x22, [x9, #48]          /* cpacr at entry */
    str x23, [x9, #56]          /* vbar at entry */
    str x25, [x9, #64]          /* daif at entry */
    bl sb_start
1:  wfe
    b 1b

/* ---- vectors --------------------------------------------------------------------------------
 * Synchronous exceptions from EL1: inside a probe (sb_probe_depth > 0) they are recorded and
 * skipped; otherwise every register goes to sb_crash_regs and the crash screen (crash.c) takes
 * over on its own stack. SError is counted. IRQ / FIQ stay masked; if one arrives anyway it is
 * ignored. Anything from a lower EL is a crash. */
.balign 2048
.global sb_vectors
sb_vectors:
    .irp group, 0, 1
    .balign 0x80
    b vec_sync
    .balign 0x80
    eret
    .balign 0x80
    eret
    .balign 0x80
    b vec_serror
    .endr
    .irp group, 2, 3
    .balign 0x80
    b vec_lower
    .balign 0x80
    b vec_lower
    .balign 0x80
    b vec_lower
    .balign 0x80
    b vec_lower
    .endr

vec_sync:
    stp x0, x1, [sp, #-16]!
    adrp x1, sb_probe_depth
    ldr x0, [x1, :lo12:sb_probe_depth]
    cbz x0, vec_crash_pop
    adrp x1, sb_fault
    add x1, x1, :lo12:sb_fault
    mrs x0, esr_el1
    str x0, [x1]
    mrs x0, elr_el1
    str x0, [x1, #8]
    mrs x0, far_el1
    str x0, [x1, #16]
    ldr x0, [x1, #24]
    add x0, x0, #1
    str x0, [x1, #24]
    mrs x0, elr_el1
    add x0, x0, #4
    msr elr_el1, x0
    ldp x0, x1, [sp], #16
    eret

vec_serror:
    stp x0, x1, [sp, #-16]!
    adrp x1, sb_fault
    add x1, x1, :lo12:sb_fault
    mrs x0, esr_el1
    str x0, [x1, #32]
    ldr x0, [x1, #40]
    add x0, x0, #1
    str x0, [x1, #40]
    ldp x0, x1, [sp], #16
    eret

vec_crash_pop:
    ldp x0, x1, [sp], #16
vec_lower:
    /* x0..x30, sp, elr, spsr, esr, far into sb_crash_regs[36] (x0/x1 via the crash stack) */
    msr tpidr_el1, x0
    adrp x0, sb_crash_regs
    add x0, x0, :lo12:sb_crash_regs
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
    str x1, [x0, #8]
    mrs x1, tpidr_el1
    str x1, [x0]
    mov x1, sp
    str x1, [x0, #248]
    mrs x1, elr_el1
    str x1, [x0, #256]
    mrs x1, spsr_el1
    str x1, [x0, #264]
    mrs x1, esr_el1
    str x1, [x0, #272]
    mrs x1, far_el1
    str x1, [x0, #280]
    adrp x1, sb_crash_stack_top
    add x1, x1, :lo12:sb_crash_stack_top
    mov sp, x1
    mov x1, x0
    mov x0, xzr
    bl sb_crash_show
2:  wfe
    b 2b

/* ---- secondary cores ---------------------------------------------------------------------------
 * Target of PSCI CPU_ON (the core bring-up test, as em_vec.S em_core_entry): MMU off, records
 * MPIDR and CurrentEL in sb_core_box (slot = Aff2 * 4 + Aff1), then CPU_OFF; if CPU_OFF returns,
 * state 2 (parked) and WFE forever. The boot core invalidates the box before reading it. */
.global sb_core_entry
sb_core_entry:
    mrs x2, mpidr_el1
    ubfx x3, x2, #8, #2
    ubfx x4, x2, #16, #1
    add x0, x3, x4, lsl #2
    adrp x1, sb_core_box
    add x1, x1, :lo12:sb_core_box
    add x1, x1, x0, lsl #5
    str x2, [x1, #8]
    mrs x2, CurrentEL
    str x2, [x1, #16]
    mov x2, #1
    str x2, [x1]
    dsb sy
    mov x9, x1
    movz w0, #0x8400, lsl #16
    movk w0, #0x0002
    smc #0
    str x0, [x9, #24]
    mov x2, #2
    str x2, [x9]
    dsb sy
3:  wfe
    b 3b

/* ---- secondary cores as workers (smp.c) ------------------------------------------------------
 * Target of PSCI CPU_ON for the stress tests. MMU off on entry: sb_worker_boot (cleaned to DRAM
 * by the boot core before CPU_ON) gives the worker index, the boot core's MAIR / TCR / TTBR0 /
 * SCTLR and this core's stack (arena, write-back). The core turns its MMU and caches on with the
 * same tables (no set/way maintenance here: the A55s share their L3 with the boot core), installs
 * sb_worker_vectors, calls sb_worker_main(index), then PSCI CPU_OFF; if CPU_OFF returns, the slot
 * says PARKED and the core waits in WFE. ARMv8.0 instructions only (the Mongoose M3 cores). */
.global sb_worker_entry
sb_worker_entry:
    msr daifset, #15
    msr tpidr_el1, xzr
    /* x0: PSCI's context id, 0x5B5B5B00 | index when the firmware passes it. Then the core's own start record
     * sb_worker_rec[index] (its magic checked), marked "used" at +56; else the shared sb_worker_boot (a core that
     * boots late could take a newer core's shared record: the heater is careful in that case) */
    lsr x11, x0, #8
    movz x12, #0x5b5b
    movk x12, #0x5b, lsl #16
    cmp x11, x12
    b.ne 1f
    and x13, x0, #0xff
    cbz x13, 1f
    cmp x13, #8
    b.hs 1f
    adrp x9, sb_worker_rec
    add x9, x9, :lo12:sb_worker_rec
    add x9, x9, x13, lsl #6
    ldr x10, [x9, #48]
    movz x12, #0x4253
    movk x12, #0x4f57, lsl #16
    movk x12, #0x4b52, lsl #32
    movk x12, #0x5245, lsl #48
    cmp x10, x12
    b.ne 1f
    ldr x10, [x9]
    cmp x10, x13
    b.ne 1f
    movz x10, #0x4e57
    movk x10, #0x4f, lsl #16                /* "OWN" */
    str x10, [x9, #56]
    dsb sy
    b 2f
1:  adrp x9, sb_worker_boot
    add x9, x9, :lo12:sb_worker_boot
2:  ldr x19, [x9]
    adr x10, sb_worker_vectors
    msr vbar_el1, x10
    mrs x10, cpacr_el1
    orr x10, x10, #(3 << 20)
    msr cpacr_el1, x10
    ldr x10, [x9, #40]
    mov sp, x10
    ldr x10, [x9, #8]
    msr mair_el1, x10
    ldr x10, [x9, #16]
    msr tcr_el1, x10
    ldr x10, [x9, #24]
    msr ttbr0_el1, x10
    isb
    tlbi vmalle1
    ic iallu
    dsb sy
    isb
    ldr x10, [x9, #32]
    msr sctlr_el1, x10
    isb
    mov x0, x19
    bl sb_worker_main
    movz w0, #0x8400, lsl #16
    movk w0, #0x0002
    smc #0
    mrs x1, tpidr_el1
    cbz x1, 4f
    mov x2, #4                  /* SB_WK_PARKED */
    str x2, [x1]
    dsb sy
    dc civac, x1
    dsb sy
4:  wfe
    b 4b

/* Every exception on a worker: ESR / ELR / FAR into its slot (tpidr_el1), state FAULT, CPU_OFF. */
.balign 2048
.global sb_worker_vectors
sb_worker_vectors:
    .rept 16
    .balign 0x80
    b worker_fault
    .endr
worker_fault:
    mrs x0, tpidr_el1
    cbz x0, 5f
    mrs x1, esr_el1
    str x1, [x0, #40]
    mrs x1, elr_el1
    str x1, [x0, #48]
    mrs x1, far_el1
    str x1, [x0, #56]
    mov x1, #3                  /* SB_WK_FAULT */
    str x1, [x0]
    dsb sy
    dc civac, x0
    dsb sy
5:  movz w0, #0x8400, lsl #16
    movk w0, #0x0002
    smc #0
6:  wfe
    b 6b

/* ---- whole data cache by set/way (shim/starlte/xnu_boot.S star_dcache_all) -------------------
 * x0: 0 invalidate (DC ISW), 1 clean + invalidate (DC CISW). Leaf, x0-x12. */
.global sb_dcache_all
sb_dcache_all:
    mov x12, x0
    dmb sy
    mrs x0, clidr_el1
    and w3, w0, #0x07000000
    lsr w3, w3, #23
    cbz w3, 5f
    mov w10, #0
1:  add w2, w10, w10, lsr #1
    lsr w1, w0, w2
    and w1, w1, #7
    cmp w1, #2
    b.lt 4f
    msr csselr_el1, x10
    isb
    mrs x1, ccsidr_el1
    and w2, w1, #7
    add w2, w2, #4
    mov w4, #0x3ff
    and w4, w4, w1, lsr #3
    clz w5, w4
    mov w7, #0x7fff
    and w7, w7, w1, lsr #13
2:  mov w9, w4
3:  lsl w6, w9, w5
    orr w11, w10, w6
    lsl w6, w7, w2
    orr w11, w11, w6
    cbz x12, 6f
    dc cisw, x11
    b 7f
6:  dc isw, x11
7:  subs w9, w9, #1
    b.ge 3b
    subs w7, w7, #1
    b.ge 2b
4:  add w10, w10, #2
    cmp w3, w10
    b.gt 1b
5:  mov x10, #0
    msr csselr_el1, x10
    dsb sy
    isb
    ret

.section .data
.balign 64
.global sb_fault, sb_probe_depth, sb_core_box, sb_crash_regs
sb_fault:
    .quad 0, 0, 0, 0, 0, 0
sb_probe_depth:
    .quad 0
.balign 64
sb_core_box:
    .fill 8 * 4, 8, 0
sb_crash_regs:
    .fill 40, 8, 0
.balign 64
.global sb_worker_boot
sb_worker_boot:
    .fill 8, 8, 0
.global sb_worker_rec
.balign 64
sb_worker_rec:
    .fill 64, 8, 0

.section .bss
.balign 16
sb_stack:
    .fill 256 * 1024, 1, 0
.global sb_stack_top
sb_stack_top:
sb_crash_stack:
    .fill 32 * 1024, 1, 0
sb_crash_stack_top:
