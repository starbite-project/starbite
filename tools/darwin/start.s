/*
 * Entry of the Darwin console's static init (tools/darwin/dsh.c).
 *
 * XNU enters a static executable (no dyld) at LC_UNIXTHREAD's pc with sp at
 * argc, aligned only to what the argument and apple strings happened to need.
 * Real hardware faults on a misaligned sp used as a base register (EL0 SP
 * alignment check -> SIGBUS; QEMU does not check, herolte NOTES 2026-10-15),
 * so align it before any C code runs. cstart(sp) never returns.
 */
	.text
	.globl	_start
	.p2align 2
_start:
	mov	x0, sp
	and	x1, x0, #~15
	mov	sp, x1
	mov	x29, #0
	mov	x30, #0
	bl	_cstart
1:	b	1b
