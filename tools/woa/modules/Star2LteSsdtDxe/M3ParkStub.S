// M3ParkStub: the parking loop of an M3 core (docs/WIN11-BIGCORES.md stage B2).
// SPDX-License-Identifier: GPL-2.0-only
//
// Assembled by gen-m3park-stub.py into M3ParkStub.h (a byte array); M3Park (M3Probe.c) copies it to
// mailbox + 0x800 (the firmware half of an ACPI parking-protocol mailbox) and fills the three
// literals at its end. Entered by PSCI CPU_ON: EL1 (Samsung's uH owns EL2), MMU and caches off
// (every data access is Device memory: aligned accesses only), DAIF masked. No stack.
//
// Mailbox (ACPI 6.4, 5.2.12.14): +0x000 CpuId (u32), +0x008 JumpAddress (u64), +0x010..0x7FF the
// OS's, +0x800..0xFFF the firmware's (this code, then MPIDR at +0xFF8 as the arrival mark).
// Loop: WFE, woken every 2^16 counter ticks by the generic timer's event stream (~2.5 ms at
// 26 MHz) or by any SEV; every 1024 wakes the seconds since parking are written as 8 hex digits
// into this core's "=M3 park" line in the zone-0 ring (the loader's `fastboot oem log dmesg`
// shows them after the next restart); then the mailbox: a JumpAddress != 0 with CpuId == our
// UID clears JumpAddress and jumps there with x0 = the mailbox (parking protocol). In B2 no OS
// writes these mailboxes (the M3s are not in the MADT), so the core just stays parked.
// Counters: CNTVCT_EL0 (the virtual count, never trapped to EL2), CNTFRQ_EL0.

        .text
        .global m3park_stub
        .balign 4
m3park_stub:
        adr     x20, lit
        ldr     x19, [x20, #0]          // mailbox page
        ldr     x21, [x20, #8]          // 8 hex digits in zone 0, 0 = none
        ldr     w24, [x20, #16]         // our ACPI processor UID
        mrs     x0, mpidr_el1
        str     x0, [x19, #0xff8]       // arrival mark
        dsb     sy
        mrs     x22, cntvct_el0         // t0
        mrs     x25, cntfrq_el0
        mrs     x0, cntkctl_el1         // event stream: EVNTEN = 1, EVNTDIR = 0, EVNTI = 15
        bic     x0, x0, #0xfc
        orr     x0, x0, #0xf0           // EVNTI = 15
        orr     x0, x0, #0x4            // EVNTEN
        msr     cntkctl_el1, x0
        isb
        mov     w23, #0
1:      wfe
        add     w23, w23, #1
        tst     w23, #0x3ff
        b.ne    3f
        cbz     x21, 3f
        mrs     x0, cntvct_el0
        sub     x0, x0, x22
        udiv    x0, x0, x25             // seconds parked (CNTFRQ 0 gives 0, no trap)
        mov     x1, #7
2:      and     x2, x0, #0xf
        add     x3, x2, #0x30           // '0' + d
        add     x4, x2, #0x37           // 'A' - 10 + d
        cmp     x2, #10
        csel    x2, x3, x4, lo
        strb    w2, [x21, x1]
        lsr     x0, x0, #4
        subs    x1, x1, #1
        b.pl    2b
        dsb     sy
3:      ldr     x1, [x19, #8]           // JumpAddress
        cbz     x1, 1b
        ldr     w0, [x19, #0]           // CpuId
        cmp     w0, w24
        b.ne    1b
        str     xzr, [x19, #8]          // taken
        dsb     sy
        mov     x0, x19
        br      x1

        .balign 8
lit:    .quad   0                       // mailbox page
        .quad   0                       // hex digits address
        .quad   0                       // UID
