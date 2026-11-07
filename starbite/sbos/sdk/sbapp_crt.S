/* SPDX-License-Identifier: MIT
 * Starbite OS app runtime (docs/STARBITE-OS.md 2.3, 6.1): the entry trampoline the kernel erets into (x0 = the
 * event in the mailbox) and the system call stub. An app's handler returns into the trampoline, which gives
 * the CPU back with SYS_RETURN. */
.section .text.sbapp_entry, "ax"
.global _sbapp_entry
_sbapp_entry:
    bl app_event
    mov x8, #0
    svc #0
1:  b 1b

.text
/* int64_t sbos_call(nr, a, b, c, d, e) */
.global sbos_call
sbos_call:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    svc #0
    ret
