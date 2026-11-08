/* smptest: QEMU virt entry at 0x80080000 (EL1, MMU off), as the loader is entered */
.section .text.head, "ax"
.global _start
_start:
    msr daifset, #15
    adr x1, stack_top
    mov sp, x1
    mrs x1, cpacr_el1
    orr x1, x1, #(3 << 20)
    msr cpacr_el1, x1
    isb
    bl smptest_main
1:  wfe
    b 1b
/* mmu.c calls this to invalidate the data cache by set/way before the MMU goes on (QEMU: no-op) */
.global star_dcache_all
star_dcache_all:
    ret
.section .bss
.balign 16
stack:
    .space 65536
stack_top:
