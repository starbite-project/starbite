// M3ParkStub: the parking loop of an M3 core, with diagnostics (docs/WIN11-BIGCORES.md stages B2/B2b).
// SPDX-License-Identifier: GPL-2.0-only
//
// Assembled by gen-stubs.py into M3ParkStub.h (a byte array); M3Park (M3Probe.c) copies it to
// mailbox + 0x800 (the firmware half of an ACPI parking-protocol mailbox) and fills the literals at
// its end. Entered by PSCI CPU_ON: MMU and caches off (every data access is Device memory: aligned
// accesses only, bytes are fine), DAIF masked. No stack; x30 only for the leaf `hex`.
//
// Mailbox (ACPI 6.4, 5.2.12.14): +0x000 CpuId (u32), +0x008 JumpAddress (u64), +0x010..0x7FF the
// OS's, +0x800..0xFFF the firmware's (this code, then MPIDR at +0xFF8 as the arrival mark).
//
// B2 parked all three cores but their uptime digits stayed 0. Suspects: CNTFRQ_EL0 never set on a
// freshly powered M3 (udiv by 0 gives 0, no trap), no event stream so WFE never returns, EL2 (uH)
// trapping WFE, or the core dying after its arrival mark. So B2b writes raw values into the
// core's zone-0 line, every field 0 until written (the loader's `fastboot oem log dmesg` shows them):
//   el    CurrentEL >> 2, at entry
//   frq   CNTFRQ_EL0 as read
//   kctl  CNTKCTL_EL1 read back after enabling the event stream (EVNTEN bit 2, EVNTI 15 in 7:4)
//   spin  rounds done of phase A: 8 rounds of 2^26 YIELD iterations, no WFE (about a second)
//   wake  phase B: WFE wake-ups, written every 256
//   t     (CNTVCT_EL0 - t0) >> 16, written after every spin round and with every wake update
// Phase B also serves the parking protocol after every wake-up: JumpAddress != 0 and CpuId == our
// UID -> JumpAddress = 0, jump with x0 = the mailbox (for B4; in B2b no OS writes these mailboxes).

        .text
        .global m3park_stub
        .balign 4

        .macro  tick
        mrs     x0, cntvct_el0
        sub     x0, x0, x22
        lsr     x0, x0, #16
        ldr     x1, [x20, #56]
        mov     x2, #8
        bl      hex
        .endm

m3park_stub:
        adr     x20, lit
        ldr     x19, [x20, #0]          // mailbox page
        ldr     w24, [x20, #8]          // our ACPI processor UID
        mrs     x0, mpidr_el1
        str     x0, [x19, #0xff8]       // arrival mark
        dsb     sy
        mrs     x22, cntvct_el0         // t0
        mrs     x0, CurrentEL
        lsr     x0, x0, #2
        ldr     x1, [x20, #16]          // el
        mov     x2, #1
        bl      hex
        mrs     x0, cntfrq_el0
        ldr     x1, [x20, #24]          // frq
        mov     x2, #8
        bl      hex
        mrs     x0, cntkctl_el1         // event stream: EVNTEN = 1, EVNTDIR = 0, EVNTI = 15
        bic     x0, x0, #0xfc
        orr     x0, x0, #0xf0
        orr     x0, x0, #0x4
        msr     cntkctl_el1, x0
        isb
        mrs     x0, cntkctl_el1
        ldr     x1, [x20, #32]          // kctl
        mov     x2, #8
        bl      hex

        mov     x26, #0                 // phase A: bounded spin, no WFE
4:      mov     x3, #0x4000000
5:      yield
        subs    x3, x3, #1
        b.ne    5b
        add     x26, x26, #1
        mov     x0, x26
        ldr     x1, [x20, #40]          // spin
        mov     x2, #8
        bl      hex
        tick
        cmp     x26, #8
        b.lo    4b

        mov     x23, #0                 // phase B: WFE
1:      wfe
        add     x23, x23, #1
        tst     x23, #0xff
        b.ne    3f
        mov     x0, x23
        ldr     x1, [x20, #48]          // wake
        mov     x2, #8
        bl      hex
        tick
3:      ldr     x1, [x19, #8]           // JumpAddress
        cbz     x1, 1b
        ldr     w0, [x19, #0]           // CpuId
        cmp     w0, w24
        b.ne    1b
        str     xzr, [x19, #8]          // taken
        dsb     sy
        mov     x0, x19
        br      x1

// hex: x0 = value, x1 = where (0: skip), x2 = digits (low digits of x0, most significant first).
// Clobbers x0, x3..x6.
hex:    cbz     x1, 9f
        sub     x3, x2, #1
8:      and     x4, x0, #0xf
        add     x5, x4, #0x30           // '0' + d
        add     x6, x4, #0x37           // 'A' - 10 + d
        cmp     x4, #10
        csel    x4, x5, x6, lo
        strb    w4, [x1, x3]
        lsr     x0, x0, #4
        subs    x3, x3, #1
        b.pl    8b
        dsb     sy
9:      ret

        .balign 8
lit:    .quad   0                       // +0  mailbox page
        .quad   0                       // +8  UID
        .quad   0                       // +16 el digits
        .quad   0                       // +24 frq digits
        .quad   0                       // +32 kctl digits
        .quad   0                       // +40 spin digits
        .quad   0                       // +48 wake digits
        .quad   0                       // +56 t digits
